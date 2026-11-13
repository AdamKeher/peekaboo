#include "BlitProfiler.h"

#include "AmigaEmulator.h"
#include "Constants.h"

namespace adm::core {

// PAL frame timing (see adm::core::constants::beam for the shared definitions).
static constexpr int kCCKPerLine = static_cast<int>(constants::beam::CCKPerLine);
static constexpr int kTotalCCK = static_cast<int>(constants::beam::TotalCCK);

BlitRecord BlitProfiler::Enrich(const AmigaEmulator::BlitEvent& ev)
{
    BlitRecord rec{};
    rec.raw = ev;

    // L-reference: beam-relative CCK offset = vpos * 228 + hpos (same formula as CPU/Copper traces)
    rec.lRef = static_cast<uint32_t>(ev.vpos) * static_cast<uint32_t>(kCCKPerLine) +
               static_cast<uint32_t>(ev.hpos);

    // Decoded size: the profiler stores vblitsize/hblitsize already decoded by UAE
    rec.rows = ev.vblitsize ? ev.vblitsize : 1;
    rec.wordsPerRow = ev.hblitsize ? ev.hblitsize : 1;

    // Channel usage from BLTCON0[11:8]
    const uint8_t useBits = static_cast<uint8_t>((ev.bltcon0 >> 8) & 0xF);
    rec.useA = (useBits & 0x8) != 0;
    rec.useB = (useBits & 0x4) != 0;
    rec.useC = (useBits & 0x2) != 0;
    rec.useD = (useBits & 0x1) != 0;
    rec.minterm = static_cast<uint8_t>(ev.bltcon0 & 0xFF);
    rec.lineMode = (ev.bltcon1 & 0x1) != 0;

    // Carried straight through from the core: the id the provenance map stamped chip RAM with.
    rec.blitId = ev.blitId;

    // DMA cycle count: exact hardware measurement
    rec.dmaCycles = ev.end_cycle - ev.start_cycle;

    // Fill timing details
    rec.startLine = ev.vpos;
    rec.startClock = ev.hpos; // CCK units (0-226, or 0-227 on long-frame lines)
    rec.startCycle = rec.lRef;
    rec.endCycle = rec.startCycle + rec.dmaCycles;
    rec.endLine = static_cast<uint16_t>(rec.endCycle / kCCKPerLine);
    rec.endClock = static_cast<uint16_t>(rec.endCycle % kCCKPerLine);

    return rec;
}

void BlitProfiler::Update(AmigaEmulator* emu)
{
    // emu is null while the emulator isn't running — same contract the EmulatorView forwarders
    // used to provide. Taking the core type directly is what lets this file live in adm_core.
    int count = emu ? emu->GetBlitEvents(m_rawBuf, AmigaEmulator::kMaxBlitEvents) : 0;
    if (count == 0) {
        m_currentFrame.clear();
        return;
    }

    m_currentFrame.clear();
    m_currentFrame.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        m_currentFrame.push_back(Enrich(m_rawBuf[i]));

    // Drain each blit's destination words out of the core ring before the next frame's blits
    // recycle it. Only runs while capture is on — every dataWords is 0 otherwise.
    for (auto& rec : m_currentFrame) {
        if (rec.raw.dataWords == 0)
            continue;
        auto words = AcquireWordBuffer(rec.raw.dataWords);
        const int got = emu->GetBlitData(rec.raw.dataOff, rec.raw.dataWords, words->data());
        if (got <= 0)
            continue;
        words->resize(static_cast<size_t>(got));
        rec.destTruncated = static_cast<uint32_t>(got) < rec.raw.dataWords;
        rec.destWords = std::move(words);
    }

    ++m_frameVersion;
}

std::shared_ptr<std::vector<AmigaEmulator::BlitWord>> BlitProfiler::AcquireWordBuffer(size_t words)
{
    // With capture on, Update() used to make_shared a fresh vector per blit per frame (up to
    // kMaxBlitEvents). Buffers whose records nobody retained (use_count == 1) are reused; a
    // buffer still referenced by the UI is left alone and replaced in the pool.
    for (auto& buf : m_wordPool) {
        if (buf.use_count() == 1) {
            buf->clear();
            buf->resize(words);
            return buf;
        }
    }
    auto fresh = std::make_shared<std::vector<AmigaEmulator::BlitWord>>(words);
    if (m_wordPool.size() < AmigaEmulator::kMaxBlitEvents)
        m_wordPool.push_back(fresh);
    return fresh;
}

void BlitProfiler::Clear()
{
    m_currentFrame.clear();
    ++m_frameVersion;
}

BlitProfiler::FrameStats BlitProfiler::ComputeStats() const
{
    FrameStats s{};
    s.totalBlits = static_cast<int>(m_currentFrame.size());
    for (const auto& rec : m_currentFrame) {
        if (rec.raw.fromCopper)
            ++s.copperBlits;
        else
            ++s.cpuBlits;
        s.totalDmaCycles += rec.dmaCycles;
    }
    s.busPercent = static_cast<float>(s.totalDmaCycles) / static_cast<float>(kTotalCCK) * 100.0f;
    return s;
}

} // namespace adm::core
