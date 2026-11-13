#include "SnapshotBuilders.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adm::core::debug {
namespace {

constexpr std::array<const char*, 16> kCiaNames = {
    "PRA",   "PRB",    "DDRA",  "DDRB",   "TALO", "TAHI", "TBLO", "TBHI",
    "TODLO", "TODMID", "TODHI", "UNUSED", "SDR",  "ICR",  "CRA",  "CRB"};

const char* KindName(TimelineKind kind)
{
    switch (kind) {
        case TimelineKind::Cpu:
            return "cpu";
        case TimelineKind::Dma:
            return "dma";
        case TimelineKind::Idle:
            return "idle";
        case TimelineKind::Copper:
            return "copper";
        case TimelineKind::Blit:
            return "blit";
        case TimelineKind::Break:
            return "break";
    }
    return "unknown";
}

} // namespace

CpuRegisterSnapshot CpuRegisterSnapshotBuilder::Build(const CpuRawState& raw, bool trace)
{
    CpuRegisterSnapshot out;
    out.d = raw.d;
    out.a = raw.a;
    out.pc = raw.pc;
    out.sr = raw.sr;
    out.trace = trace;
    out.supervisor = (raw.sr & 0x2000) != 0;
    out.x = (raw.sr & 0x10) != 0;
    out.n = (raw.sr & 0x08) != 0;
    out.z = (raw.sr & 0x04) != 0;
    out.v = (raw.sr & 0x02) != 0;
    out.c = (raw.sr & 0x01) != 0;
    out.usp = raw.usp;
    out.ssp = raw.ssp;
    return out;
}

std::vector<RegisterRow>
CustomRegisterSnapshotBuilder::Build(const std::array<uint16_t, 256>& words,
                                     const std::vector<RegisterDefinition>& definitions,
                                     uint32_t baseAddress)
{
    std::vector<RegisterRow> out;
    out.reserve(definitions.size());
    for (const auto& def : definitions) {
        if ((def.offset & 1) != 0 || def.offset >= words.size() * 2)
            continue;
        out.push_back({baseAddress + def.offset, def.offset, words[def.offset / 2],
                       def.volatileMask, 2, def.name, def.access, def.description});
    }
    return out;
}

CiaRegisterSnapshot CiaRegisterSnapshotBuilder::Build(const CiaRawState& raw)
{
    CiaRegisterSnapshot out;
    for (size_t index = 0; index < kCiaNames.size(); ++index) {
        const uint16_t offset = static_cast<uint16_t>(index * 0x100);
        // CIAA is selected on odd addresses and CIAB on even addresses in the Amiga map.
        out.a.push_back({0xbfe001U + offset, static_cast<uint16_t>(index), raw.a[index], 0xff, 1,
                         kCiaNames[index], "rw", "CIA-A register"});
        out.b.push_back({0xbfd000U + offset, static_cast<uint16_t>(index), raw.b[index], 0xff, 1,
                         kCiaNames[index], "rw", "CIA-B register"});
    }
    return out;
}

std::vector<CopperRow> CopperTraceSnapshotBuilder::Build(const std::vector<CopperRawEvent>& events,
                                                         uint32_t frameStart, uint32_t frameEnd)
{
    std::vector<CopperRow> out;
    for (const auto& event : events) {
        if (event.absoluteCycle < frameStart || event.absoluteCycle > frameEnd)
            continue;
        CopperRow row;
        row.lRef = event.absoluteCycle - frameStart;
        row.address = event.address;
        row.word1 = event.word1;
        row.word2 = event.word2;
        row.list = event.list;
        if ((event.word1 & 1) == 0) {
            row.operation = "MOVE";
            row.registerOffset = event.word1 & 0x1fe;
        } else if ((event.word2 & 1) != 0) {
            row.operation = "SKIP";
        } else {
            row.operation = "WAIT";
        }
        out.push_back(row);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& a, const auto& b) { return a.lRef < b.lRef; });
    return out;
}

DmaStatsSnapshot DmaStatsCalculator::Build(const std::vector<DmaRawSlot>& slots, uint16_t hposCount,
                                           uint16_t vposCount, std::optional<uint32_t> breakLRef)
{
    DmaStatsSnapshot out;
    out.maxSlots = static_cast<uint32_t>(hposCount) * vposCount;
    out.partial = breakLRef.has_value();
    out.scanlines.resize(vposCount);
    for (const auto& slot : slots) {
        if (slot.vpos >= vposCount || slot.hpos >= hposCount)
            continue;
        const uint32_t lRef = static_cast<uint32_t>(slot.vpos) * hposCount + slot.hpos;
        if (breakLRef && lRef > *breakLRef)
            continue;
        const size_t channel = static_cast<size_t>(slot.channel);
        if (channel >= out.counts.size())
            continue;
        ++out.counts[channel];
        ++out.scanlines[slot.vpos][channel];
        ++out.recordedSlots;
    }
    return out;
}

FrameTimelineSnapshot FrameTimelineBuilder::Build(uint32_t frameStart, uint16_t lines,
                                                  uint16_t cyclesPerLine,
                                                  const std::vector<TimelineRawEvent>& events,
                                                  std::optional<uint32_t> breakCycle)
{
    FrameTimelineSnapshot out;
    out.frameStart = frameStart;
    out.lines = lines;
    out.cyclesPerLine = cyclesPerLine;
    out.frameCycles = static_cast<uint32_t>(lines) * cyclesPerLine;
    out.partial = breakCycle.has_value();
    out.visibleCycles =
        breakCycle ? std::min(*breakCycle - std::min(*breakCycle, frameStart), out.frameCycles)
                   : out.frameCycles;

    const uint64_t frameEnd = static_cast<uint64_t>(frameStart) + out.visibleCycles;
    for (const auto& event : events) {
        const uint64_t end = event.absoluteEnd > event.absoluteStart
                                 ? event.absoluteEnd
                                 : static_cast<uint64_t>(event.absoluteStart) + 1;
        if (end <= frameStart || event.absoluteStart >= frameEnd)
            continue;
        TimelineSpan span;
        span.kind = event.kind;
        span.openStart = event.absoluteStart < frameStart;
        span.openEnd = end > frameEnd;
        span.startLRef = event.absoluteStart <= frameStart ? 0 : event.absoluteStart - frameStart;
        span.endLRef = static_cast<uint32_t>(std::min<uint64_t>(end, frameEnd) - frameStart);
        span.id = event.id;
        span.label = event.label;
        out.spans.push_back(std::move(span));
    }
    std::stable_sort(out.spans.begin(), out.spans.end(), [](const auto& a, const auto& b) {
        if (a.startLRef != b.startLRef)
            return a.startLRef < b.startLRef;
        return a.endLRef < b.endLRef;
    });
    return out;
}

const TimelineSpan* FrameTimelineBuilder::HitTest(const FrameTimelineSnapshot& snapshot,
                                                  TimelineKind kind, uint32_t lRef)
{
    // Later/shorter spans win when events overlap, matching the visual top-most event.
    const TimelineSpan* best = nullptr;
    for (const auto& span : snapshot.spans) {
        if (span.kind != kind || lRef < span.startLRef || lRef >= span.endLRef)
            continue;
        if (!best || span.startLRef > best->startLRef ||
            (span.startLRef == best->startLRef && span.endLRef < best->endLRef))
            best = &span;
    }
    return best;
}

double FrameTimelineBuilder::Percent(const FrameTimelineSnapshot& snapshot, uint32_t lRef)
{
    return snapshot.frameCycles == 0
               ? 0.0
               : 100.0 * std::min(lRef, snapshot.frameCycles) / snapshot.frameCycles;
}

double FrameTimelineBuilder::Microseconds(const FrameTimelineSnapshot& snapshot, uint32_t lRef,
                                          double colorClockHz)
{
    if (colorClockHz <= 0.0)
        return 0.0;
    return 1'000'000.0 * std::min(lRef, snapshot.frameCycles) / colorClockHz;
}

DemoTimingSnapshot DemoTimingCalculator::Build(const std::vector<uint32_t>& frameCycles,
                                               const std::vector<uint32_t>& idleCycles,
                                               double colorClockHz)
{
    DemoTimingSnapshot out;
    if (frameCycles.empty() || frameCycles.size() != idleCycles.size() || colorClockHz <= 0.0)
        return out;
    for (size_t i = 0; i < frameCycles.size(); ++i) {
        out.totalCycles += frameCycles[i];
        out.idleCycles += std::min(idleCycles[i], frameCycles[i]);
    }
    if (out.totalCycles == 0)
        return out;
    out.available = true;
    out.fps = colorClockHz * frameCycles.size() / out.totalCycles;
    out.idlePercent = 100.0 * out.idleCycles / out.totalCycles;
    return out;
}

void to_json(nlohmann::json& out, const CpuRegisterSnapshot& value)
{
    out = {{"d", value.d},    {"a", value.a},         {"pc", value.pc},
           {"sr", value.sr},  {"trace", value.trace}, {"supervisor", value.supervisor},
           {"x", value.x},    {"n", value.n},         {"z", value.z},
           {"v", value.v},    {"c", value.c},         {"usp", value.usp},
           {"ssp", value.ssp}};
}

void to_json(nlohmann::json& out, const RegisterRow& value)
{
    out = {{"address", value.address}, {"offset", value.offset},
           {"value", value.value},     {"mask", value.mask},
           {"size", value.size},       {"name", value.name},
           {"access", value.access},   {"description", value.description}};
}

void to_json(nlohmann::json& out, const CiaRegisterSnapshot& value)
{
    out = {{"a", value.a}, {"b", value.b}};
}

void to_json(nlohmann::json& out, const CopperRow& value)
{
    out = {{"lRef", value.lRef},
           {"address", value.address},
           {"word1", value.word1},
           {"word2", value.word2},
           {"list", value.list},
           {"operation", value.operation},
           {"registerOffset", value.registerOffset}};
}

void to_json(nlohmann::json& out, const DmaStatsSnapshot& value)
{
    out = {{"counts", value.counts},
           {"scanlines", value.scanlines},
           {"maxSlots", value.maxSlots},
           {"recordedSlots", value.recordedSlots},
           {"partial", value.partial}};
}

void to_json(nlohmann::json& out, const TimelineSpan& value)
{
    out = {{"kind", KindName(value.kind)}, {"startLRef", value.startLRef},
           {"endLRef", value.endLRef},     {"id", value.id},
           {"label", value.label},         {"openStart", value.openStart},
           {"openEnd", value.openEnd}};
}

void to_json(nlohmann::json& out, const FrameTimelineSnapshot& value)
{
    out = {{"frameStart", value.frameStart},
           {"frameCycles", value.frameCycles},
           {"lines", value.lines},
           {"cyclesPerLine", value.cyclesPerLine},
           {"partial", value.partial},
           {"visibleCycles", value.visibleCycles},
           {"spans", value.spans}};
}

void to_json(nlohmann::json& out, const DemoTimingSnapshot& value)
{
    out = {{"available", value.available},
           {"fps", value.fps},
           {"idlePercent", value.idlePercent},
           {"totalCycles", value.totalCycles},
           {"idleCycles", value.idleCycles}};
}

} // namespace adm::core::debug
