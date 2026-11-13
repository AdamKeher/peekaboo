# Emulator validation phases 1–5

This directory contains the deterministic target protocol and the checked-in target artifacts used
by the validation framework.

Build the target fixtures with:

```powershell
./scripts/build_amiga_validation.ps1
```

That command also regenerates `expected.json` from source constants and formulas via
`scripts/generate_amiga_validation_expected.ps1`; it never reads emulator output.

Build and run the host-side framework with:

```powershell
cmake --build build/default
./scripts/run_emulator_validation.ps1 -Model a500-pal -Filter 1-5
```

The default `emulator_validation_tests` CTest executable is deliberately a **framework fixture**.
It validates mailbox lifecycle, manifests, field-aware comparisons, deterministic hashing, reports,
and capability-based skips. It advertises no emulator capabilities, so target/core scenarios are
reported as `SKIP`; fixture data can never become a false hardware pass.

The checked target includes CPU-register, Copper split, A-XOR-B blitter, and alignment checkpoint
labels. An `AmigaEmulator` adapter is available through the explicit `-Real` script switch. It
relocates ELF symbols through the active AmigaDOS HUNK list, controls the big-endian mailbox, arms
symbol breakpoints, and captures CPU, Copper, blitter, DMA/timeline, CIA, and save-state data.

The real lane is not registered as a blocking CTest lane yet. PUAE 1.18.3 can deadlock after a
host-driven PC breakpoint is armed in this headless runner; forcing the process down can also race
its audio callback. Until breakpoint delivery is fixed in the core, use the fixture lane in CI and
run `./scripts/run_emulator_validation.ps1 -Real` only for core development.

Generated artifacts include `result.json`, `junit.xml`, `report.html`, and per-case
expected/raw/displayed JSON. Golden updates are rejected by the fixture lane: a real renderer must
record core provenance and a reviewable diff before that switch can be enabled.

See `SKIPPED_CAPABILITIES.md` for the exact real-lane gaps and what each requires.
