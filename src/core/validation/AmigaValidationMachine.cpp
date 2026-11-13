#include "AmigaValidationMachine.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "core/AmigaEmulator.h"
#include "core/SegListResolver.h"
#include "core/debug/SnapshotBuilders.h"

namespace adm::validation {
namespace {

AmigaModel ProfileModel(const std::string& profile)
{
    if (profile.starts_with("a1200"))
        return AmigaModel::A1200;
    if (profile.starts_with("a600"))
        return AmigaModel::A600;
    return AmigaModel::A500;
}

core::debug::DmaChannel DmaChannelFor(int type)
{
    switch (type) {
        case AmigaEmulator::kDmaRecordRefresh:
            return core::debug::DmaChannel::Refresh;
        case AmigaEmulator::kDmaRecordCpu:
            return core::debug::DmaChannel::Cpu;
        case AmigaEmulator::kDmaRecordCopper:
            return core::debug::DmaChannel::Copper;
        case AmigaEmulator::kDmaRecordAudio:
            return core::debug::DmaChannel::Audio;
        case AmigaEmulator::kDmaRecordBlitter:
            return core::debug::DmaChannel::Blitter;
        case AmigaEmulator::kDmaRecordBitplane:
            return core::debug::DmaChannel::Bitplane;
        case AmigaEmulator::kDmaRecordSprite:
            return core::debug::DmaChannel::Sprite;
        case AmigaEmulator::kDmaRecordDisk:
            return core::debug::DmaChannel::Disk;
        case AmigaEmulator::kDmaRecordNone:
            return core::debug::DmaChannel::Idle;
        default:
            return core::debug::DmaChannel::Other;
    }
}

std::string CopperOperation(const core::debug::CopperRow& row)
{
    if (row.operation != "MOVE")
        return row.operation;
    std::ostringstream out;
    if (row.registerOffset == 0x180)
        out << "MOVE COLOR00 $" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
            << row.word2;
    else
        out << "MOVE $" << std::uppercase << std::hex << std::setw(3) << std::setfill('0')
            << row.registerOffset << " $" << std::setw(4) << row.word2;
    return out.str();
}

} // namespace

AmigaValidationMachine::AmigaValidationMachine(AmigaValidationOptions options)
    : m_options(std::move(options)), m_emulator(std::make_unique<AmigaEmulator>())
{
}

AmigaValidationMachine::~AmigaValidationMachine() = default;

bool AmigaValidationMachine::Initialize(std::string& error)
{
    if (!m_symbols.Load(m_options.elfPath, error))
        return false;
    // Silence the host playback device: Launch() ends in LoadADF(), which calls InitAudio()
    // and opens a real one. A validation run is not something anyone wants to listen to, and
    // nothing this harness checks comes from the device - see the note in SmokeMain.cpp.
    m_emulator->SetMuted(true);
    m_emulator->SetSystemDirectory(std::filesystem::absolute(m_options.systemDirectory).string());
    m_emulator->SelectModelPreset(ProfileModel(m_options.profile));
    m_emulator->SetUserVariable("puae_video_standard",
                                m_options.profile.ends_with("ntsc") ? "NTSC" : "PAL");
    m_emulator->SetBartmanDebugSupport(true);
    // LoadCore only null-checks the libretro entry points; the adm_* symbols are dereferenced
    // unconditionally per frame, so preflight the full required set for a clear error message.
    const std::string corePath = std::filesystem::absolute(m_options.corePath).string();
    if (!AmigaEmulator::VerifyCore(corePath, error))
        return false;
    if (!m_emulator->LoadCore(corePath)) {
        error = "failed to load patched PUAE core: " + m_options.corePath.string();
        return false;
    }
    if (!Launch(error))
        return false;
    m_firstReset = true;
    return true;
}

bool AmigaValidationMachine::Launch(std::string& error)
{
    if (!m_emulator->RunHostExecutable(std::filesystem::absolute(m_options.executablePath).string(),
                                       error))
        return false;
    // Full CPU and DMA tracing during Workbench boot is prohibitively expensive and not part of
    // any target checkpoint. The checkpoint breakpoint still uses UAE's lightweight debug trace.
    m_emulator->SetCpuTraceEnabled(false);
    m_emulator->SetDebugDma(0);
    m_armedAddress.reset();
    m_breakHit = false;
    m_breakStatus = static_cast<int>(AmigaEmulator::OneShotBreakpointStatus::Idle);
    m_breakStopPc = 0;
    m_breakExecuted = 0;
    m_lastError.clear();
    return true;
}

std::vector<RuntimeSegment> AmigaValidationMachine::RuntimeSegments() const
{
    const auto readByte = [this](uint32_t address, bool& ok) {
        uint8_t value = 0;
        ok = const_cast<AmigaValidationMachine*>(this)->ReadMemory(address,
                                                                   std::span<uint8_t>(&value, 1));
        return value;
    };
    const auto readLong = [this](uint32_t address, bool& ok) {
        std::array<uint8_t, 4> bytes{};
        ok = const_cast<AmigaValidationMachine*>(this)->ReadMemory(address, bytes);
        return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
               (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
    };
    const auto amigaSegments = core::SegListResolver::ResolveActiveProcess(readLong, readByte);
    std::vector<RuntimeSegment> result;
    result.reserve(amigaSegments.size());
    for (size_t index = 0; index < amigaSegments.size(); ++index)
        result.push_back({static_cast<uint16_t>(index + 1), 0, amigaSegments[index].base,
                          amigaSegments[index].size});
    return result;
}

std::optional<uint32_t> AmigaValidationMachine::ResolveSymbol(const std::string& name) const
{
    const auto segments = RuntimeSegments();
    return m_symbols.ResolveRuntime(name, segments);
}

std::string AmigaValidationMachine::NormalizeAddress(uint32_t address) const
{
    const auto segments = RuntimeSegments();
    return m_symbols.NormalizeAddress(address, segments);
}

uint32_t AmigaValidationMachine::ResolveAddress(uint32_t address) const
{
    if (address != kResolvedMailboxAddress)
        return address;
    return ResolveSymbol("ptv_mailbox").value_or(kResolvedMailboxAddress);
}

bool AmigaValidationMachine::ReadMemory(uint32_t address, std::span<uint8_t> output)
{
    address = ResolveAddress(address);
    if (address == kResolvedMailboxAddress)
        return false;
    for (const auto& region : m_emulator->GetMemoryRegions()) {
        if (!region.hostPtr || address < region.amigaAddr)
            continue;
        const uint64_t offset = static_cast<uint64_t>(address) - region.amigaAddr;
        if (offset <= region.size && output.size() <= region.size - offset) {
            std::copy_n(region.hostPtr + offset, output.size(), output.data());
            return true;
        }
    }
    return false;
}

bool AmigaValidationMachine::WriteMemory(uint32_t address, std::span<const uint8_t> input)
{
    address = ResolveAddress(address);
    if (address == kResolvedMailboxAddress)
        return false;
    for (const auto& region : m_emulator->GetMemoryRegions()) {
        if (!region.hostPtr || !region.writable || address < region.amigaAddr)
            continue;
        const uint64_t offset = static_cast<uint64_t>(address) - region.amigaAddr;
        if (offset <= region.size && input.size() <= region.size - offset) {
            std::copy(input.begin(), input.end(), region.hostPtr + offset);
            return true;
        }
    }
    return false;
}

bool AmigaValidationMachine::PrepareCheckpoint(const std::string& symbol, std::string& error)
{
    ReleaseCheckpoint();
    const auto address = ResolveSymbol(symbol);
    if (!address) {
        error = "cannot relocate checkpoint symbol '" + symbol + "' into the active HUNK";
        return false;
    }
    // A waiting target reaches a checkpoint within a few instructions after the mailbox
    // command. This deliberately generous ceiling still guarantees retro_run() returns.
    constexpr uint32_t kCheckpointInstructionBudget = 2'000'000;
    if (!m_emulator->ArmOneShotBreakpoint(*address, kCheckpointInstructionBudget)) {
        error = "patched PUAE core does not provide the bounded one-shot checkpoint API";
        return false;
    }
    m_armedAddress = address;
    m_breakHit = false;
    m_breakStatus = static_cast<int>(AmigaEmulator::OneShotBreakpointStatus::Armed);
    m_breakStopPc = 0;
    m_breakExecuted = 0;
    return true;
}

bool AmigaValidationMachine::CheckpointReached(std::string& error)
{
    if (!m_breakHit || !m_armedAddress) {
        if (m_breakStatus ==
            static_cast<int>(AmigaEmulator::OneShotBreakpointStatus::BudgetExpired)) {
            std::ostringstream message;
            message << "checkpoint instruction budget expired after " << m_breakExecuted
                    << " instructions at PC $" << std::hex << std::setw(8) << std::setfill('0')
                    << m_breakStopPc;
            error = message.str();
        } else {
            error = "guest published the checkpoint without stopping on its symbol";
        }
        return false;
    }
    AmigaEmulator::CpuState state{};
    if (!m_emulator->GetCpuState(state) || state.pc != *m_armedAddress) {
        error = "checkpoint breakpoint stopped at the wrong PC";
        return false;
    }
    return true;
}

void AmigaValidationMachine::ReleaseCheckpoint()
{
    m_emulator->CancelOneShotBreakpoint();
    m_armedAddress.reset();
    m_breakHit = false;
    m_breakStatus = static_cast<int>(AmigaEmulator::OneShotBreakpointStatus::Idle);
    m_breakStopPc = 0;
    m_breakExecuted = 0;
}

void AmigaValidationMachine::StepFrame()
{
    m_emulator->StepOneFrame(false);
    m_breakStatus =
        static_cast<int>(m_emulator->GetOneShotBreakpointStatus(&m_breakStopPc, &m_breakExecuted));
    m_breakHit = m_breakStatus == static_cast<int>(AmigaEmulator::OneShotBreakpointStatus::Hit);
}

nlohmann::json AmigaValidationMachine::CaptureCpu(bool displayed) const
{
    AmigaEmulator::CpuState state{};
    if (!m_emulator->GetCpuState(state))
        return nlohmann::json::object();
    if (!displayed)
        return {{"d0", state.d[0]},
                {"d1", state.d[1]},
                {"sr", state.sr},
                {"pc", NormalizeAddress(state.pc)}};
    core::debug::CpuRawState raw;
    std::copy(std::begin(state.d), std::end(state.d), raw.d.begin());
    std::copy(std::begin(state.a), std::end(state.a), raw.a.begin());
    raw.pc = state.pc;
    raw.sr = state.sr;
    const auto view = core::debug::CpuRegisterSnapshotBuilder::Build(raw, true);
    return {
        {"d0", view.d[0]}, {"d1", view.d[1]}, {"sr", view.sr}, {"pc", NormalizeAddress(view.pc)}};
}

nlohmann::json AmigaValidationMachine::CaptureCopper(bool displayed) const
{
    std::array<AmigaEmulator::CopperExecEvent, AmigaEmulator::kMaxCopperExecEvents> events{};
    const int count = m_emulator->GetCopperExecEvents(events.data(), events.size());
    std::vector<core::debug::CopperRawEvent> rawEvents;
    rawEvents.reserve(std::max(0, count));
    for (int index = 0; index < count; ++index) {
        const auto& event = events[index];
        rawEvents.push_back({event.absolute_cycle, event.addr, event.word1, event.word2, event.vpos,
                             event.hpos, event.list});
    }
    const auto rows = core::debug::CopperTraceSnapshotBuilder::Build(
        rawEvents, m_emulator->GetFrameStartCycle(),
        m_emulator->IsPartialFrame() ? m_emulator->GetBreakCycle()
                                     : m_emulator->GetFrameStartCycle() + 100000);
    nlohmann::json operations = nlohmann::json::array();
    std::set<uint8_t> lists;
    for (const auto& row : rows) {
        operations.push_back(displayed
                                 ? CopperOperation(row)
                                 : ((row.word1 & 1) == 0 ? CopperOperation(row) : row.operation));
        if (row.list != 0)
            lists.insert(row.list);
    }
    return {{"operations", operations}, {"lists", lists}};
}

nlohmann::json AmigaValidationMachine::CaptureBlitter(bool) const
{
    std::array<AmigaEmulator::BlitEvent, AmigaEmulator::kMaxBlitEvents> events{};
    const int count = m_emulator->GetBlitEvents(events.data(), events.size());
    if (count <= 0)
        return nlohmann::json::object();
    const auto& event = events[count - 1];
    uint16_t result = 0;
    if (const auto address = ResolveSymbol("blit_d")) {
        std::array<uint8_t, 2> bytes{};
        if (const_cast<AmigaValidationMachine*>(this)->ReadMemory(*address, bytes))
            result = static_cast<uint16_t>((bytes[0] << 8) | bytes[1]);
    }
    return {{"minterm", event.bltcon0 & 0xff},
            {"result", result},
            {"trigger", event.fromCopper ? "copper" : "cpu"}};
}

nlohmann::json AmigaValidationMachine::CaptureTimeline(bool displayed) const
{
    AmigaEmulator::DmaSlotRecord* record = nullptr;
    int hposCount = 0;
    int vposCount = 0;
    const bool hasDma = m_emulator->IsPartialFrame()
                            ? m_emulator->GetDmaRecordLive(&record, hposCount, vposCount)
                            : m_emulator->GetDmaRecord(&record, hposCount, vposCount);
    std::vector<core::debug::DmaRawSlot> slots;
    if (hasDma && record) {
        slots.reserve(static_cast<size_t>(hposCount) * vposCount);
        for (int line = 0; line < vposCount; ++line)
            for (int hpos = 0; hpos < hposCount; ++hpos) {
                const auto& slot = record[line * hposCount + hpos];
                slots.push_back({static_cast<uint16_t>(line), static_cast<uint16_t>(hpos),
                                 DmaChannelFor(slot.type)});
            }
    }
    const auto breakLRef = m_emulator->IsPartialFrame()
                               ? std::optional<uint32_t>(m_emulator->GetBreakFrameOffset())
                               : std::nullopt;
    const auto dma = core::debug::DmaStatsCalculator::Build(
        slots, static_cast<uint16_t>(std::max(0, hposCount)),
        static_cast<uint16_t>(std::max(0, vposCount)), breakLRef);
    std::array<AmigaEmulator::IdleEvent, AmigaEmulator::kMaxIdleEvents> idle{};
    const int idleCount = m_emulator->GetIdleEvents(idle.data(), idle.size());
    nlohmann::json idleSpans = nlohmann::json::array();
    std::optional<uint32_t> start;
    for (int index = 0; index < idleCount; ++index) {
        const uint32_t lref = idle[index].cycle - m_emulator->GetFrameStartCycle();
        if (idle[index].start)
            start = lref;
        else if (start) {
            idleSpans.push_back({*start, lref});
            start.reset();
        }
    }
    const uint32_t frameCycles = static_cast<uint32_t>(std::max(0, hposCount)) *
                                 static_cast<uint32_t>(std::max(0, vposCount));
    return {{"frameCycles", frameCycles},
            {"idleSpans", idleSpans},
            {"dmaTotal", displayed ? dma.recordedSlots : slots.size()},
            {"breakLRef", breakLRef.value_or(frameCycles)}};
}

nlohmann::json AmigaValidationMachine::CaptureCia(bool displayed) const
{
    core::CIAState state{};
    if (!m_emulator->GetCIARegisters(state))
        return nlohmann::json::object();
    if (!displayed)
        return {{"ciaaCra", state.a.cra}, {"snapshotDestructive", false}};
    core::debug::CiaRawState raw;
    std::memcpy(raw.a.data(), &state.a, raw.a.size());
    std::memcpy(raw.b.data(), &state.b, raw.b.size());
    const auto view = core::debug::CiaRegisterSnapshotBuilder::Build(raw);
    const auto found = std::find_if(view.a.begin(), view.a.end(),
                                    [](const auto& row) { return row.name == "CRA"; });
    return {{"ciaaCra", found == view.a.end() ? 0 : found->value}, {"snapshotDestructive", false}};
}

nlohmann::json AmigaValidationMachine::CaptureRaw(uint16_t scenario, uint16_t)
{
    switch (scenario) {
        case 1:
            return CaptureCpu(false);
        case 2:
            return CaptureCopper(false);
        case 3:
            return CaptureBlitter(false);
        case 4:
            return CaptureTimeline(false);
        case 5:
            return CaptureCia(false);
        default:
            return nlohmann::json::object();
    }
}

nlohmann::json AmigaValidationMachine::CaptureDisplayed(uint16_t scenario, uint16_t)
{
    switch (scenario) {
        case 1:
            return CaptureCpu(true);
        case 2:
            return CaptureCopper(true);
        case 3:
            return CaptureBlitter(true);
        case 4:
            return CaptureTimeline(true);
        case 5:
            return CaptureCia(true);
        default:
            return nlohmann::json::object();
    }
}

std::set<std::string> AmigaValidationMachine::Capabilities() const
{
    std::set<std::string> result = {
        "mailbox", "symbols", "cpu", "copper", "blitter", "save-state", "capability-negotiation",
        "matrix"};
    if (m_emulator->HasDmaDebug()) {
        result.insert("dma");
        result.insert("timeline");
        result.insert("partial-frame");
    }
    if (m_emulator->HasIdleProfiling())
        result.insert("idle");
    core::CIAState cia{};
    if (m_emulator->GetCIARegisters(cia))
        result.insert("cia");
    return result;
}

bool AmigaValidationMachine::SaveState(std::vector<uint8_t>& output)
{
    output = m_emulator->SaveState();
    return !output.empty();
}

bool AmigaValidationMachine::LoadState(std::span<const uint8_t> input)
{
    return m_emulator->LoadState(std::vector<uint8_t>(input.begin(), input.end()));
}

void AmigaValidationMachine::Reset()
{
    ReleaseCheckpoint();
    if (m_firstReset) {
        m_firstReset = false;
        return;
    }
    if (!m_emulator->ReinitCore()) {
        m_lastError = "failed to reinitialize PUAE core";
        return;
    }
    std::string error;
    if (!Launch(error))
        m_lastError = error;
}

const std::string& AmigaValidationMachine::CoreVersion() const
{
    return m_emulator->GetCoreVersion();
}

std::string AmigaValidationMachine::RelocationDiagnostic() const
{
    auto* self = const_cast<AmigaValidationMachine*>(this);
    const auto readLong = [self](uint32_t address) {
        std::array<uint8_t, 4> bytes{};
        if (!self->ReadMemory(address, bytes))
            return 0U;
        return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
               (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
    };
    AmigaEmulator::CpuState state{};
    m_emulator->GetCpuState(state);
    const uint32_t execBase = readLong(4);
    const uint32_t task = execBase ? readLong(execBase + 276) : 0;
    uint8_t nodeType = 0;
    if (task)
        self->ReadMemory(task + 8, std::span<uint8_t>(&nodeType, 1));
    const uint32_t cliBptr = task ? readLong(task + 172) : 0;
    const uint32_t moduleBptr = cliBptr ? readLong((cliBptr << 2) + 60) : 0;
    const auto segments = RuntimeSegments();
    std::ostringstream out;
    out << "pc=$" << std::hex << state.pc << " execBase=$" << execBase << " task=$" << task
        << " nodeType=" << std::dec << static_cast<int>(nodeType) << " cliBptr=$" << std::hex
        << cliBptr << " moduleBptr=$" << moduleBptr << " segments=" << std::dec << segments.size();
    if (!m_lastError.empty())
        out << " lastError='" << m_lastError << "'";
    return out.str();
}

} // namespace adm::validation
