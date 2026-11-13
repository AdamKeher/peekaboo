#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "core/debug/SnapshotBuilders.h"
#include "core/validation/ElfSymbolResolver.h"
#include "core/validation/PtvProtocol.h"
#include "core/validation/SemanticComparator.h"
#include "core/validation/ValidationSuite.h"

using namespace adm::core::debug;
using namespace adm::validation;

TEST(PtvProtocolTest, RoundTripsBigEndianMailbox)
{
    PtvMailbox expected;
    expected.buildId = 0x10203040;
    expected.commandSeq = 7;
    expected.command = PtvCommand::Run;
    expected.scenario = 3;
    expected.checkpointSeq = 9;
    expected.checkpoint = 2;
    expected.status = PtvStatus::Pass;
    expected.payloadAddr = 0x123456;
    expected.payloadSize = 4;
    expected.payloadCrc32 = 0xdeadbeef;
    const auto bytes = EncodeMailbox(expected);
    EXPECT_EQ(bytes[0], 'P');
    EXPECT_EQ(bytes[12], 0);
    EXPECT_EQ(bytes[15], 7);
    const auto decoded = DecodeMailbox(bytes);
    ASSERT_TRUE(decoded.ok) << decoded.error;
    EXPECT_EQ(decoded.mailbox.buildId, expected.buildId);
    EXPECT_EQ(decoded.mailbox.command, PtvCommand::Run);
    EXPECT_EQ(decoded.mailbox.payloadCrc32, 0xdeadbeefU);
}

TEST(PtvProtocolTest, RejectsBadMagicAndCalculatesKnownCrc)
{
    auto bytes = EncodeMailbox(PtvMailbox{});
    bytes[0] = 0;
    EXPECT_FALSE(DecodeMailbox(bytes).ok);
    const std::array<uint8_t, 9> input = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(Crc32(input), 0xcbf43926U);
}

namespace {

class ContinueAckMachine final : public IValidationMachine
{
public:
    ContinueAckMachine() : memory(128)
    {
        PtvMailbox mailbox;
        mailbox.buildId = 7;
        mailbox.status = PtvStatus::Ready;
        const auto bytes = EncodeMailbox(mailbox);
        std::copy(bytes.begin(), bytes.end(), memory.begin());
    }

    bool ReadMemory(uint32_t address, std::span<uint8_t> output) override
    {
        if (address > memory.size() || output.size() > memory.size() - address)
            return false;
        std::copy_n(memory.begin() + address, output.size(), output.begin());
        return true;
    }
    bool WriteMemory(uint32_t address, std::span<const uint8_t> input) override
    {
        if (address > memory.size() || input.size() > memory.size() - address)
            return false;
        std::copy(input.begin(), input.end(), memory.begin() + address);
        return true;
    }
    void StepFrame() override
    {
        std::array<uint8_t, kPtvMailboxSize> bytes{};
        ReadMemory(0, bytes);
        auto decoded = DecodeMailbox(bytes);
        if (!decoded.ok || decoded.mailbox.command != PtvCommand::Continue)
            return;
        ++continueAcks;
        decoded.mailbox.command = PtvCommand::None;
        decoded.mailbox.status = PtvStatus::Ready;
        const auto encoded = EncodeMailbox(decoded.mailbox);
        WriteMemory(0, encoded);
    }
    bool PrepareCheckpoint(const std::string&, std::string&) override { return true; }
    bool CheckpointReached(std::string&) override { return true; }
    void ReleaseCheckpoint() override { released = true; }
    nlohmann::json CaptureRaw(uint16_t, uint16_t) override { return {}; }
    nlohmann::json CaptureDisplayed(uint16_t, uint16_t) override { return {}; }
    std::set<std::string> Capabilities() const override { return {}; }
    bool SaveState(std::vector<uint8_t>&) override { return false; }
    bool LoadState(std::span<const uint8_t>) override { return false; }
    void Reset() override {}

    std::vector<uint8_t> memory;
    int continueAcks = 0;
    bool released = false;
};

} // namespace

TEST(CheckpointRunnerTest, WaitsForGuestToAcknowledgeContinue)
{
    ContinueAckMachine machine;
    CheckpointRunner runner(machine, {0, 7, 3});
    std::string error;
    ASSERT_TRUE(runner.WaitForReady(error)) << error;
    ASSERT_TRUE(runner.Continue(error)) << error;
    EXPECT_TRUE(machine.released);
    EXPECT_EQ(machine.continueAcks, 1);

    std::array<uint8_t, kPtvMailboxSize> bytes{};
    ASSERT_TRUE(machine.ReadMemory(0, bytes));
    const auto decoded = DecodeMailbox(bytes);
    ASSERT_TRUE(decoded.ok) << decoded.error;
    EXPECT_EQ(decoded.mailbox.command, PtvCommand::None);
    EXPECT_EQ(decoded.mailbox.status, PtvStatus::Ready);
}

TEST(SemanticComparatorTest, SupportsAllSemanticModes)
{
    nlohmann::json rules = {{"exact", {{"mode", "exact"}, {"value", 7}}},
                            {"masked", {{"mode", "masked"}, {"value", 0xa0}, {"mask", 0xf0}}},
                            {"range", {{"mode", "range"}, {"min", 4.0}, {"max", 6.0}}},
                            {"sequence", {{"mode", "sequence"}, {"value", {1, 3}}}},
                            {"set", {{"mode", "set"}, {"value", {"a", "b"}}}},
                            {"image", {{"mode", "image"}, {"hash", "pixels"}}}};
    nlohmann::json actual = {{"exact", 7},        {"masked", 0xaf},
                             {"range", 5.0},      {"sequence", {0, 1, 2, 3}},
                             {"set", {"b", "a"}}, {"image", "pixels"}};
    SemanticComparator comparator;
    EXPECT_TRUE(comparator.Compare(rules, actual, FailureLayer::GuestToRaw).Passed());
    actual["masked"] = 0x1f;
    const auto failed = comparator.CompareLayers(rules, actual, actual);
    ASSERT_FALSE(failed.Passed());
    EXPECT_EQ(failed.failures.front().path, "$.masked");
    EXPECT_EQ(failed.failures.front().layer, FailureLayer::GuestToRaw);
}

TEST(CpuRegisterSnapshotBuilderTest, DecodesStatusAndPreservesAllBits)
{
    CpuRawState raw;
    for (size_t i = 0; i < raw.d.size(); ++i) {
        raw.d[i] = 0x11111111U * static_cast<uint32_t>(i + 1);
        raw.a[i] = 0x01020304U + static_cast<uint32_t>(i);
    }
    raw.pc = 0x12345678;
    raw.sr = 0x201b;
    const auto snapshot = CpuRegisterSnapshotBuilder::Build(raw, true);
    EXPECT_EQ(snapshot.d, raw.d);
    EXPECT_EQ(snapshot.a, raw.a);
    EXPECT_TRUE(snapshot.supervisor);
    EXPECT_TRUE(snapshot.x);
    EXPECT_TRUE(snapshot.n);
    EXPECT_FALSE(snapshot.z);
    EXPECT_TRUE(snapshot.v);
    EXPECT_TRUE(snapshot.c);
    EXPECT_TRUE(snapshot.trace);
}

TEST(RegisterSnapshotBuilderTest, BuildsStableCustomAndCiaRows)
{
    std::array<uint16_t, 256> words{};
    words[0xc0] = 0x0abc;
    const auto rows = CustomRegisterSnapshotBuilder::Build(
        words, {{0x180, "COLOR00", "rw", "background", 0x0fff}});
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].address, 0xdff180U);
    EXPECT_EQ(rows[0].value, 0x0abc);
    EXPECT_EQ(rows[0].mask, 0x0fff);

    CiaRawState cia;
    cia.a[14] = 0x11;
    const auto ciaRows = CiaRegisterSnapshotBuilder::Build(cia);
    ASSERT_EQ(ciaRows.a.size(), 16U);
    EXPECT_EQ(ciaRows.a[14].name, "CRA");
    EXPECT_EQ(ciaRows.a[14].value, 0x11);
    EXPECT_EQ(ciaRows.a[0].address, 0xbfe001U);
    EXPECT_EQ(ciaRows.b[0].address, 0xbfd000U);
}

TEST(CopperSnapshotBuilderTest, DecodesAndSortsExecutedInstructions)
{
    std::vector<CopperRawEvent> events = {{120, 4, 0x2c01, 0xfffe, 44, 0, 1},
                                          {110, 0, 0x0180, 0x000f, 0, 0, 1},
                                          {90, 0, 0x0180, 0x0f00, 0, 0, 1}};
    const auto rows = CopperTraceSnapshotBuilder::Build(events, 100, 200);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].lRef, 10U);
    EXPECT_EQ(rows[0].operation, "MOVE");
    EXPECT_EQ(rows[1].operation, "WAIT");
}

TEST(DmaStatsCalculatorTest, UsesGeometryAndClipsPartialFrames)
{
    std::vector<DmaRawSlot> slots = {{0, 0, DmaChannel::Refresh},
                                     {0, 1, DmaChannel::Cpu},
                                     {1, 0, DmaChannel::Copper},
                                     {1, 1, DmaChannel::Blitter}};
    const auto full = DmaStatsCalculator::Build(slots, 2, 2);
    EXPECT_EQ(full.maxSlots, 4U);
    EXPECT_EQ(full.recordedSlots, 4U);
    const auto partial = DmaStatsCalculator::Build(slots, 2, 2, 2);
    EXPECT_TRUE(partial.partial);
    EXPECT_EQ(partial.recordedSlots, 3U);
}

TEST(FrameTimelineBuilderTest, NormalizesClipsAndHitTestsOnOneAxis)
{
    std::vector<TimelineRawEvent> events = {{TimelineKind::Idle, 90, 120, 1, "open start"},
                                            {TimelineKind::Blit, 130, 160, 2, "outer"},
                                            {TimelineKind::Blit, 140, 150, 3, "inner"},
                                            {TimelineKind::Copper, 220, 221, 4, "future"}};
    const auto timeline = FrameTimelineBuilder::Build(100, 10, 10, events, 180);
    EXPECT_TRUE(timeline.partial);
    EXPECT_EQ(timeline.visibleCycles, 80U);
    ASSERT_EQ(timeline.spans.size(), 3U);
    EXPECT_TRUE(timeline.spans[0].openStart);
    EXPECT_EQ(timeline.spans[0].startLRef, 0U);
    const auto* hit = FrameTimelineBuilder::HitTest(timeline, TimelineKind::Blit, 45);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->id, 3U);
    EXPECT_DOUBLE_EQ(FrameTimelineBuilder::Percent(timeline, 50), 50.0);
}

TEST(DemoTimingCalculatorTest, UsesCapturedIntervalsForFpsAndIdle)
{
    const auto timing = DemoTimingCalculator::Build({100, 100}, {25, 50}, 5000.0);
    EXPECT_TRUE(timing.available);
    EXPECT_DOUBLE_EQ(timing.fps, 50.0);
    EXPECT_DOUBLE_EQ(timing.idlePercent, 37.5);
    EXPECT_FALSE(DemoTimingCalculator::Build({}, {}, 5000.0).available);
}

TEST(ExpectedManifestTest, RejectsDuplicateScenarioIds)
{
    nlohmann::json manifest = {{"schemaVersion", 1}, {"buildId", 1}};
    manifest["scenarios"] = nlohmann::json::array(
        {{{"id", 1}, {"phase", 1}, {"name", "a"}, {"checkpoints", nlohmann::json::array()}},
         {{"id", 1}, {"phase", 2}, {"name", "b"}, {"checkpoints", nlohmann::json::array()}}});
    EXPECT_THROW(ExpectedManifest::Parse(manifest), std::runtime_error);
}

TEST(ElfSymbolResolverTest, ResolvesMailboxAndNormalizesRelocatedAddresses)
{
    const auto path = std::filesystem::path(__FILE__).parent_path() / "amiga_validation" /
                      "fixtures" / "peekaboo_validation.elf";
    ElfSymbolResolver resolver;
    std::string error;
    ASSERT_TRUE(resolver.Load(path, error)) << error;
    const auto* mailbox = resolver.Find("ptv_mailbox");
    ASSERT_NE(mailbox, nullptr);
    const RuntimeSegment segment{mailbox->section, mailbox->address, 0x20000, 0x1000};
    const auto runtime = resolver.ResolveRuntime("ptv_mailbox", {&segment, 1});
    ASSERT_TRUE(runtime.has_value());
    EXPECT_EQ(*runtime, 0x20000U);
    EXPECT_EQ(resolver.NormalizeAddress(0x20008, {&segment, 1}), "ptv_mailbox+8");
}

TEST(ValidationReportTest, WritesMachineReadableAndSideBySideArtifacts)
{
    ValidationRunResult result;
    result.profile = "a500-pal";
    result.coreVersion = "test-core";
    result.buildId = 7;
    ValidationCaseResult item;
    item.id = "P1-1-1";
    item.name = "report seam";
    item.phase = 1;
    item.status = ValidationCaseResult::Status::Fail;
    item.reason = "different";
    item.expected = {{"register", 1}};
    item.raw = {{"register", 2}};
    item.displayed = {{"register", 3}};
    result.cases.push_back(item);

    const auto directory = std::filesystem::temp_directory_path() / "adm-validation-report-test";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::string error;
    ASSERT_TRUE(ValidationReportWriter::Write(directory, result, error)) << error;
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / "result.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / "junit.xml"));
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / "report.html"));
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / item.id / "expected.json"));

    std::ifstream input(directory / "result.json");
    nlohmann::json json;
    input >> json;
    EXPECT_EQ(json["cases"][0]["raw"]["register"], 2);
    EXPECT_EQ(json["cases"][0]["displayed"]["register"], 3);
    std::filesystem::remove_all(directory, ec);
}
