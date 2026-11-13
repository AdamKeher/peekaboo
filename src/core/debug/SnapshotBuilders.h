#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm::core::debug {

struct CpuRawState
{
    std::array<uint32_t, 8> d{};
    std::array<uint32_t, 8> a{};
    uint32_t pc = 0;
    uint16_t sr = 0;
    std::optional<uint32_t> usp;
    std::optional<uint32_t> ssp;
};

struct CpuRegisterSnapshot
{
    std::array<uint32_t, 8> d{};
    std::array<uint32_t, 8> a{};
    uint32_t pc = 0;
    uint16_t sr = 0;
    bool trace = false;
    bool supervisor = false;
    bool x = false, n = false, z = false, v = false, c = false;
    std::optional<uint32_t> usp;
    std::optional<uint32_t> ssp;
};

class CpuRegisterSnapshotBuilder
{
public:
    static CpuRegisterSnapshot Build(const CpuRawState& raw, bool trace);
};

struct RegisterDefinition
{
    uint16_t offset = 0;
    std::string name;
    std::string access;
    std::string description;
    uint16_t volatileMask = 0xffff;
};

struct RegisterRow
{
    uint32_t address = 0;
    uint16_t offset = 0;
    uint16_t value = 0;
    uint16_t mask = 0xffff;
    uint8_t size = 2;
    std::string name;
    std::string access;
    std::string description;
};

class CustomRegisterSnapshotBuilder
{
public:
    static std::vector<RegisterRow> Build(const std::array<uint16_t, 256>& words,
                                          const std::vector<RegisterDefinition>& definitions,
                                          uint32_t baseAddress = 0xdff000);
};

struct CiaRawState
{
    std::array<uint8_t, 16> a{};
    std::array<uint8_t, 16> b{};
};

struct CiaRegisterSnapshot
{
    std::vector<RegisterRow> a;
    std::vector<RegisterRow> b;
};

class CiaRegisterSnapshotBuilder
{
public:
    static CiaRegisterSnapshot Build(const CiaRawState& raw);
};

struct CopperRawEvent
{
    uint32_t absoluteCycle = 0;
    uint32_t address = 0;
    uint16_t word1 = 0;
    uint16_t word2 = 0;
    uint16_t vpos = 0;
    uint16_t hpos = 0;
    uint8_t list = 0;
};

struct CopperRow
{
    uint32_t lRef = 0;
    uint32_t address = 0;
    uint16_t word1 = 0;
    uint16_t word2 = 0;
    uint8_t list = 0;
    std::string operation;
    uint16_t registerOffset = 0;
};

class CopperTraceSnapshotBuilder
{
public:
    static std::vector<CopperRow> Build(const std::vector<CopperRawEvent>& events,
                                        uint32_t frameStart, uint32_t frameEnd);
};

enum class DmaChannel : uint8_t {
    Idle,
    Refresh,
    Cpu,
    Copper,
    Audio,
    Blitter,
    Bitplane,
    Sprite,
    Disk,
    Other
};

struct DmaRawSlot
{
    uint16_t vpos = 0;
    uint16_t hpos = 0;
    DmaChannel channel = DmaChannel::Idle;
};

struct DmaStatsSnapshot
{
    std::array<uint32_t, 10> counts{};
    std::vector<std::array<uint16_t, 10>> scanlines;
    uint32_t maxSlots = 0;
    uint32_t recordedSlots = 0;
    bool partial = false;
};

class DmaStatsCalculator
{
public:
    static DmaStatsSnapshot Build(const std::vector<DmaRawSlot>& slots, uint16_t hposCount,
                                  uint16_t vposCount, std::optional<uint32_t> breakLRef = {});
};

enum class TimelineKind : uint8_t { Cpu, Dma, Idle, Copper, Blit, Break };

struct TimelineRawEvent
{
    TimelineKind kind = TimelineKind::Cpu;
    uint32_t absoluteStart = 0;
    uint32_t absoluteEnd = 0;
    uint32_t id = 0;
    std::string label;
};

struct TimelineSpan
{
    TimelineKind kind = TimelineKind::Cpu;
    uint32_t startLRef = 0;
    uint32_t endLRef = 0;
    uint32_t id = 0;
    std::string label;
    bool openStart = false;
    bool openEnd = false;
};

struct FrameTimelineSnapshot
{
    uint32_t frameStart = 0;
    uint32_t frameCycles = 0;
    uint16_t lines = 0;
    uint16_t cyclesPerLine = 0;
    bool partial = false;
    uint32_t visibleCycles = 0;
    std::vector<TimelineSpan> spans;
};

class FrameTimelineBuilder
{
public:
    static FrameTimelineSnapshot Build(uint32_t frameStart, uint16_t lines, uint16_t cyclesPerLine,
                                       const std::vector<TimelineRawEvent>& events,
                                       std::optional<uint32_t> breakCycle = {});
    static const TimelineSpan* HitTest(const FrameTimelineSnapshot& snapshot, TimelineKind kind,
                                       uint32_t lRef);
    static double Percent(const FrameTimelineSnapshot& snapshot, uint32_t lRef);
    static double Microseconds(const FrameTimelineSnapshot& snapshot, uint32_t lRef,
                               double colorClockHz);
};

struct DemoTimingSnapshot
{
    bool available = false;
    double fps = 0.0;
    double idlePercent = 0.0;
    uint64_t totalCycles = 0;
    uint64_t idleCycles = 0;
};

class DemoTimingCalculator
{
public:
    static DemoTimingSnapshot Build(const std::vector<uint32_t>& frameCycles,
                                    const std::vector<uint32_t>& idleCycles, double colorClockHz);
};

void to_json(nlohmann::json& out, const CpuRegisterSnapshot& value);
void to_json(nlohmann::json& out, const RegisterRow& value);
void to_json(nlohmann::json& out, const CiaRegisterSnapshot& value);
void to_json(nlohmann::json& out, const CopperRow& value);
void to_json(nlohmann::json& out, const DmaStatsSnapshot& value);
void to_json(nlohmann::json& out, const TimelineSpan& value);
void to_json(nlohmann::json& out, const FrameTimelineSnapshot& value);
void to_json(nlohmann::json& out, const DemoTimingSnapshot& value);

} // namespace adm::core::debug
