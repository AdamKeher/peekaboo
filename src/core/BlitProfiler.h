#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "AmigaEmulator.h"

namespace adm::core {

// Enriched blit record computed from a raw BlitEvent.
struct BlitRecord
{
    AmigaEmulator::BlitEvent raw; // Raw hardware capture

    // L-reference: beam-relative timestamp (Hardware Cycle Offset) in CCK units.
    // Formula: vpos * 228 + hpos  (hpos is in CCK units 0-226, or 0-227 on long-frame lines)
    // PAL range: 0 .. 71363  (313 lines × 228 CCK/line)
    uint32_t lRef;

    // Decoded size (from blt_info.vblitsize / hblitsize or bltsize OCS format)
    uint16_t rows;        // Height in rows
    uint16_t wordsPerRow; // Width in words

    // Channel usage decoded from BLTCON0[11:8]
    bool useA, useB, useC, useD;
    uint8_t minterm; // Logical function (BLTCON0[7:0])
    bool lineMode;   // BLTCON1 bit 0 = line-draw mode

    // Predicted DMA cycle count
    uint32_t dmaCycles;

    // Provenance id allocated by the core at BLTSIZE; joins onto
    // AmigaEmulator::BlitVisSpan::blitId so the fetch-time overlay can isolate this blit.
    // 0 when the loaded DLL predates the blitter-vis patch.
    uint32_t blitId;

    // Timing details
    uint16_t startLine, startClock;
    uint32_t startCycle;
    uint16_t endLine, endClock;
    uint32_t endCycle;

    // The words this blit actually wrote through its D channel, oldest first, each with its
    // own destination address. Null unless destination capture was on when the blit ran;
    // shared rather than copied because records are handed around by value (scrubber, MCP
    // snapshots) and a full-screen clear is 20 KB of words.
    std::shared_ptr<const std::vector<AmigaEmulator::BlitWord>> destWords;

    // The core recorded more D writes than came back — either the per-blit capture cap or the
    // ring recycling the window before the frame ended. What did arrive is still the truth,
    // it is just the start of the blit rather than all of it.
    bool destTruncated = false;
};

class BlitProfiler
{
public:
    // Call after each EmulatorView::Draw() (which calls retro_run() internally).
    // emu may be null (emulator not running) — the frame is then cleared.
    void Update(AmigaEmulator* emu);
    void Clear();

    const std::vector<BlitRecord>& GetCurrentFrame() const { return m_currentFrame; }

    // Returns a monotonically-increasing counter incremented each time new data arrives.
    uint32_t GetFrameVersion() const { return m_frameVersion; }

    struct FrameStats
    {
        int totalBlits;
        int cpuBlits;
        int copperBlits;
        uint32_t totalDmaCycles;
        float busPercent; // totalDmaCycles / 71364 * 100
    };
    FrameStats ComputeStats() const;

private:
    std::vector<BlitRecord> m_currentFrame;
    AmigaEmulator::BlitEvent m_rawBuf[AmigaEmulator::kMaxBlitEvents];
    uint32_t m_frameVersion = 0;
    // Reused destination-word buffers (see AcquireWordBuffer).
    std::vector<std::shared_ptr<std::vector<AmigaEmulator::BlitWord>>> m_wordPool;

    std::shared_ptr<std::vector<AmigaEmulator::BlitWord>> AcquireWordBuffer(size_t words);

    static BlitRecord Enrich(const AmigaEmulator::BlitEvent& ev);
};

} // namespace adm::core
