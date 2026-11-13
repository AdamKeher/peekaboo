#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace adm::core {

// Shared-mode WASAPI output written straight from the emulation thread: no ring, no callback
// thread - WinUAE's push mode (od-win32/sounddep/sound.cpp, finish_sound_buffer_wasapi_push).
//
// Whatever is queued in the device buffer is the whole latency AND the whole cushion. A
// callback-driven device (miniaudio) keeps its own buffer full on top of whatever cushion the
// ring in front of it holds: measured on a 10 ms-period device, ~20 ms of queue that covers no
// producer gap at all. Writing blocks into the device buffer as the core delivers them removes
// that layer.
//
// Windows only. Elsewhere Open() fails and the caller falls back to miniaudio.
class WasapiPushOutput
{
public:
    WasapiPushOutput();
    ~WasapiPushOutput();
    WasapiPushOutput(const WasapiPushOutput&) = delete;
    WasapiPushOutput& operator=(const WasapiPushOutput&) = delete;

    // Opens the default render device for 16-bit stereo at `sampleRate`. Windows converts to the
    // device's own format and rate. `bufferMs` bounds how much can ever be queued.
    bool Open(uint32_t sampleRate, uint32_t bufferMs, std::string& error);
    void Close();
    bool IsOpen() const;

    // Frames written and not yet taken by the audio engine. False when the device was lost.
    bool Queued(uint32_t& frames);
    // Appends interleaved stereo frames (silence when `frames` is null), clipped to the free
    // space. Returns the frames written; false when the device was lost.
    bool Write(const int16_t* frames, uint32_t count, uint32_t& written);
    // Drops everything queued.
    void Flush();

    uint32_t BufferFrames() const;
    // The audio engine's period in stream frames: it takes this much at a time, so the queue has
    // to hold at least this at every engine pass.
    uint32_t EnginePeriodFrames() const;
    // The device's own mix rate.
    uint32_t DeviceSampleRate() const;
    // True when the default render device is no longer the one this stream was opened on, so
    // the caller can reopen and follow it (miniaudio reroutes by itself).
    bool DefaultDeviceChanged();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace adm::core
