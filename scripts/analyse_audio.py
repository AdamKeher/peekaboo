#!/usr/bin/env python3
"""Analyse WAV files captured by Peekaboo's audio debug taps.

Peekaboo records emulator audio at two points (see AudioRecorder.h):

  core    what PuAE synthesised, before Peekaboo's ring buffer
  device  what miniaudio handed the sound card

Comparing the two localises a fault: clean at the core tap but corrupt at the device tap means the
host audio path is to blame, not the emulated program.

The metrics here are deliberately *analytic* rather than a golden-WAV diff. A golden diff is
defeated by legitimate differences in resampling and Paula filter emulation between emulators,
which would drown any real signal. These measurements instead look for the specific shapes that
corruption takes:

  held runs      consecutive identical frames. When Peekaboo's ring buffer runs dry the device
                 callback repeats the last sample as DC — audible as crunch rather than a gap, and
                 the single most diagnostic metric in the set.
  discontinuity  sample-to-sample jumps far outside the signal's own distribution: clicks, and the
                 step at the start/end of a held run.
  dropouts       windows whose RMS collapses relative to the rest of the capture.
  tone           dominant frequency and total harmonic distortion, for the pure-tone test targets
                 whose expected output is known analytically.

Usage:
    python scripts/analyse_audio.py capture.wav
    python scripts/analyse_audio.py core.wav core.device.wav      # side-by-side
    python scripts/analyse_audio.py capture.wav --expect-tone 440 --json out.json
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import wave

import numpy as np


# --- WAV loading ------------------------------------------------------------------------------


def load_wav(path):
    """Return (samples float32 [frames, channels], sample_rate).

    The stdlib `wave` module refuses IEEE-float WAVs, which is exactly what the device tap writes,
    so float files are parsed directly.
    """
    with open(path, "rb") as handle:
        head = handle.read(44)
    if len(head) < 44 or head[0:4] != b"RIFF" or head[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")

    audio_format = struct.unpack_from("<H", head, 20)[0]
    channels = struct.unpack_from("<H", head, 22)[0]
    rate = struct.unpack_from("<I", head, 24)[0]
    bits = struct.unpack_from("<H", head, 34)[0]

    if audio_format == 3 and bits == 32:
        raw = np.fromfile(path, dtype="<f4", offset=44)
    elif audio_format == 1 and bits == 16:
        with wave.open(path, "rb") as wav:
            rate = wav.getframerate()
            channels = wav.getnchannels()
            raw = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2")
        raw = raw.astype(np.float32) / 32768.0
    else:
        raise ValueError(f"{path}: unsupported format {audio_format} / {bits} bits")

    if channels < 1:
        raise ValueError(f"{path}: bad channel count {channels}")
    usable = (len(raw) // channels) * channels
    return raw[:usable].reshape(-1, channels).astype(np.float32), rate


def load_sidecar(path):
    """Peekaboo writes '<wav>.json' alongside each capture with per-block accounting."""
    sidecar = path + ".json"
    if not os.path.exists(sidecar):
        return None
    try:
        with open(sidecar, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return None


# --- Metrics ----------------------------------------------------------------------------------


def held_runs(mono, silence_floor=0.002):
    """Lengths of runs of consecutive identical samples, ignoring silence.

    A run of length N means N+1 identical frames in a row. Genuine Amiga audio does repeat samples
    — Paula's rate is far below 44.1 kHz — so short runs are normal and only long ones indicate the
    underrun fill. The caller decides the length threshold; we return the whole distribution.

    Runs of (near) silence are excluded outright: digital silence is legitimately a constant value
    and would otherwise swamp the metric, whereas the underrun fill holds whatever the last sample
    happened to be, which is audible precisely because it is not zero.
    """
    if len(mono) < 2:
        return np.zeros(0, dtype=np.int64)

    same = np.diff(mono) == 0.0
    if not same.any():
        return np.zeros(0, dtype=np.int64)

    # Run-length encode the boolean 'identical to previous sample' array.
    padded = np.concatenate(([False], same, [False]))
    edges = np.flatnonzero(np.diff(padded.astype(np.int8)))
    starts = edges[0::2]
    lengths = (edges[1::2] - starts).astype(np.int64)

    # `starts[i]` indexes into the diff array, so the repeated value is mono[starts[i]].
    audible = np.abs(mono[starts]) > silence_floor
    return lengths[audible]


def analyse(path, expect_tone=None, held_threshold=8, click_rate=10.0):
    samples, rate = load_wav(path)
    frames, channels = samples.shape
    mono = samples.mean(axis=1)

    result = {
        "path": path,
        "sampleRate": rate,
        "channels": channels,
        "frames": int(frames),
        "seconds": frames / rate if rate else 0.0,
    }

    if frames < 2:
        result["error"] = "capture is too short to analyse"
        return result

    # Level.
    peak = float(np.max(np.abs(mono)))
    rms = float(np.sqrt(np.mean(mono**2)))
    result["peak"] = peak
    result["rms"] = rms
    result["dcOffset"] = float(np.mean(mono))
    result["silent"] = peak < 1e-6

    # Held runs — the underrun-fill signature.
    runs = held_runs(mono)
    long_runs = runs[runs >= held_threshold]
    result["held"] = {
        "threshold": held_threshold,
        "runs": int(len(runs)),
        "longRuns": int(len(long_runs)),
        "maxRun": int(runs.max()) if len(runs) else 0,
        "framesInLongRuns": int(long_runs.sum()) if len(long_runs) else 0,
        "percentOfStream": float(long_runs.sum()) / frames * 100.0 if len(long_runs) else 0.0,
    }

    # Discontinuities: jumps far outside the signal's own step distribution. Using a high
    # percentile as the reference keeps this scale-free, so it works on quiet and loud captures
    # alike without a hand-tuned absolute threshold.
    diffs = np.abs(np.diff(mono))
    if peak > 1e-6:
        reference = float(np.percentile(diffs, 99.9))
        limit = max(reference * 4.0, peak * 0.25)
        spikes = np.flatnonzero(diffs > limit)
        result["discontinuities"] = {
            "count": int(len(spikes)),
            "perSecond": float(len(spikes)) / (frames / rate) if rate and frames else 0.0,
            "threshold": limit,
            "maxJump": float(diffs.max()),
        }
    else:
        result["discontinuities"] = {"count": 0, "perSecond": 0.0, "threshold": 0.0, "maxJump": 0.0}

    # Dropouts: 10 ms windows whose RMS collapses against the capture's median.
    window = max(1, rate // 100)
    usable = (frames // window) * window
    if usable >= window * 4:
        windows = mono[:usable].reshape(-1, window)
        window_rms = np.sqrt(np.mean(windows**2, axis=1))
        median = float(np.median(window_rms))
        quiet = int(np.count_nonzero(window_rms < median * 0.1)) if median > 1e-9 else 0
        result["envelope"] = {
            "windowMs": window * 1000.0 / rate,
            "windows": int(len(window_rms)),
            "medianRms": median,
            "minRms": float(window_rms.min()),
            "maxRms": float(window_rms.max()),
            "quietWindows": quiet,
            "quietPercent": quiet / len(window_rms) * 100.0,
        }

    # Spectrum. For the pure-tone targets the expected fundamental is known analytically, so the
    # measured frequency and the energy outside it are a complete pass/fail on their own.
    spectrum_len = min(frames, 1 << 18)
    windowed = mono[:spectrum_len] * np.hanning(spectrum_len)
    magnitude = np.abs(np.fft.rfft(windowed))
    freqs = np.fft.rfftfreq(spectrum_len, 1.0 / rate)
    if magnitude.size > 1 and magnitude[1:].max() > 0:
        peak_bin = int(np.argmax(magnitude[1:])) + 1
        dominant = float(freqs[peak_bin])
        total_energy = float(np.sum(magnitude**2))
        # Energy within +-2 bins of the fundamental.
        lo, hi = max(0, peak_bin - 2), min(len(magnitude), peak_bin + 3)
        fundamental_energy = float(np.sum(magnitude[lo:hi] ** 2))
        result["spectrum"] = {
            "dominantHz": dominant,
            "fundamentalEnergyPercent": fundamental_energy / total_energy * 100.0
            if total_energy > 0
            else 0.0,
        }
        if expect_tone:
            error = abs(dominant - expect_tone) / expect_tone * 100.0
            result["spectrum"]["expectedHz"] = expect_tone
            result["spectrum"]["errorPercent"] = error

    sidecar = load_sidecar(path)
    if sidecar:
        blocks = sidecar.get("blocks", [])
        result["capture"] = {
            "blockCount": sidecar.get("blockCount", 0),
            "stagingDropped": sidecar.get("stagingDropped", 0),
            "heldFrames": sum(b.get("held", 0) for b in blocks),
            "droppedFrames": sum(b.get("dropped", 0) for b in blocks),
            "blocksWithHeld": sum(1 for b in blocks if b.get("held", 0)),
        }
        sizes = [b.get("frames", 0) for b in blocks]
        levels = [b.get("ring", 0) for b in blocks]
        if sizes:
            result["capture"]["blockFrames"] = {
                "min": min(sizes),
                "max": max(sizes),
                "mean": sum(sizes) / len(sizes),
            }
        if any(levels):
            result["capture"]["ringLevel"] = {
                "min": min(levels),
                "max": max(levels),
                "mean": sum(levels) / len(levels),
            }

    result["verdict"] = verdict(result, click_rate)
    return result


def verdict(result, click_rate=10.0):
    """Reduce the measurements to a pass/fail plus the reasons, so the fix-iterate loop is one
    command rather than a judgement call."""
    problems = []

    if result.get("silent"):
        problems.append("capture is silent")

    held = result.get("held", {})
    if held.get("percentOfStream", 0.0) > 0.5:
        problems.append(
            f"{held['percentOfStream']:.1f}% of the stream is repeated samples "
            f"(longest run {held['maxRun']} frames): the ring buffer is starving and the "
            f"underrun fill is holding the last sample as DC"
        )
    elif held.get("maxRun", 0) >= 64:
        problems.append(f"longest repeated-sample run is {held['maxRun']} frames")

    disc = result.get("discontinuities", {})
    if disc.get("perSecond", 0.0) > click_rate:
        problems.append(
            f"{disc['perSecond']:.0f} discontinuities/sec, audible as clicks. Note that sharp "
            f"note attacks are legitimate, so treat this as advisory next to the held-run figure."
        )

    # Music legitimately contains quiet passages, and they show up identically at both taps, so
    # this only indicates a fault when it is extreme. Compare the figure between taps to tell a
    # real dropout from the material.
    env = result.get("envelope", {})
    if env.get("quietPercent", 0.0) > 60.0:
        problems.append(
            f"{env['quietPercent']:.1f}% of windows are near-silent; compare against the other "
            f"tap before treating this as dropouts rather than quiet material"
        )

    spec = result.get("spectrum", {})
    if "errorPercent" in spec and spec["errorPercent"] > 0.5:
        problems.append(
            f"dominant tone {spec['dominantHz']:.1f} Hz vs expected "
            f"{spec['expectedHz']:.1f} Hz ({spec['errorPercent']:.2f}% off)"
        )

    capture = result.get("capture", {})
    if capture.get("stagingDropped", 0) > 0:
        problems.append(
            f"{capture['stagingDropped']} frames lost inside the recorder: this capture is "
            f"unreliable, not evidence"
        )

    return {"pass": not problems, "problems": problems}


# --- Reporting --------------------------------------------------------------------------------


def report(result):
    print(f"\n{result['path']}")
    print("-" * max(len(result["path"]), 60))

    if "error" in result:
        print(f"  ERROR: {result['error']}")
        return

    print(
        f"  {result['frames']} frames, {result['seconds']:.2f}s, "
        f"{result['sampleRate']} Hz, {result['channels']} ch"
    )
    print(f"  peak {result['peak']:.4f}   rms {result['rms']:.4f}   dc {result['dcOffset']:+.5f}")

    held = result["held"]
    print(
        f"  held runs        {held['longRuns']} runs >= {held['threshold']} frames, "
        f"longest {held['maxRun']}, {held['percentOfStream']:.2f}% of stream"
    )

    disc = result["discontinuities"]
    print(f"  discontinuities  {disc['count']} ({disc['perSecond']:.1f}/s)")

    if "envelope" in result:
        env = result["envelope"]
        print(
            f"  envelope         median rms {env['medianRms']:.4f}, "
            f"{env['quietWindows']} quiet windows ({env['quietPercent']:.1f}%)"
        )

    if "spectrum" in result:
        spec = result["spectrum"]
        line = (
            f"  spectrum         dominant {spec['dominantHz']:.1f} Hz, "
            f"{spec['fundamentalEnergyPercent']:.1f}% of energy at fundamental"
        )
        if "errorPercent" in spec:
            line += f", {spec['errorPercent']:.2f}% off expected"
        print(line)

    if "capture" in result:
        cap = result["capture"]
        print(
            f"  capture          {cap['blockCount']} blocks, "
            f"{cap['heldFrames']} held, {cap['droppedFrames']} dropped, "
            f"{cap['stagingDropped']} lost in recorder"
        )
        if "blockFrames" in cap:
            bf = cap["blockFrames"]
            print(
                f"  block size       min {bf['min']}, mean {bf['mean']:.1f}, max {bf['max']}"
                "   (producer jitter)"
            )
        if "ringLevel" in cap:
            rl = cap["ringLevel"]
            print(f"  ring level       min {rl['min']}, mean {rl['mean']:.0f}, max {rl['max']}")

    v = result["verdict"]
    if v["pass"]:
        print("  VERDICT          clean")
    else:
        print("  VERDICT          PROBLEMS")
        for problem in v["problems"]:
            print(f"                   - {problem}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("wav", nargs="+", help="WAV file(s) captured by AUDREC")
    parser.add_argument(
        "--expect-tone",
        type=float,
        default=None,
        help="expected fundamental in Hz, for the pure-tone test targets",
    )
    parser.add_argument(
        "--held-threshold",
        type=int,
        default=8,
        help="run length (frames) above which repeated samples count as an underrun fill",
    )
    parser.add_argument(
        "--click-rate",
        type=float,
        default=10.0,
        help="discontinuities/sec above which the capture is flagged (advisory; note attacks count)",
    )
    parser.add_argument("--json", default=None, help="write the full results to this JSON file")
    args = parser.parse_args()

    results = []
    for path in args.wav:
        try:
            result = analyse(path, args.expect_tone, args.held_threshold, args.click_rate)
        except (OSError, ValueError) as exc:
            result = {"path": path, "error": str(exc), "verdict": {"pass": False,
                                                                   "problems": [str(exc)]}}
        results.append(result)
        report(result)

    if len(results) > 1:
        print("\nSummary")
        print("-" * 60)
        for result in results:
            state = "clean" if result.get("verdict", {}).get("pass") else "PROBLEMS"
            held = result.get("held", {}).get("percentOfStream", 0.0)
            print(f"  {state:9} held {held:5.2f}%   {os.path.basename(result['path'])}")

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(results, handle, indent=2)
        print(f"\nWrote {args.json}")

    return 0 if all(r.get("verdict", {}).get("pass") for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
