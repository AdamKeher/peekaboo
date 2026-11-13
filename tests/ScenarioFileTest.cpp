#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "core/FrameInputCodec.h"
#include "core/ScenarioFile.h"

using namespace adm::core;

namespace {

FrameInput MakeInput(uint16_t seed)
{
    FrameInput in;
    in.joypad[0] = seed;
    in.joypad[1] = static_cast<uint16_t>(seed ^ 0xAAAA);
    in.mouseDeltaX = static_cast<int16_t>(-static_cast<int>(seed % 97));
    in.mouseDeltaY = static_cast<int16_t>(seed % 41);
    in.mouseButtons = static_cast<uint8_t>(seed & 0x07);
    in.keysDown = {static_cast<uint16_t>(seed % 13), static_cast<uint16_t>(seed % 29 + 40)};
    in.keyEvents.push_back({static_cast<uint16_t>(seed % 7), (seed & 1) != 0});
    in.keyEvents.push_back({static_cast<uint16_t>(seed % 11), (seed & 2) != 0});
    return in;
}

bool SameInput(const FrameInput& a, const FrameInput& b)
{
    if (a.joypad[0] != b.joypad[0] || a.joypad[1] != b.joypad[1])
        return false;
    if (a.mouseDeltaX != b.mouseDeltaX || a.mouseDeltaY != b.mouseDeltaY)
        return false;
    if (a.mouseButtons != b.mouseButtons)
        return false;
    if (a.keysDown != b.keysDown)
        return false;
    if (a.keyEvents.size() != b.keyEvents.size())
        return false;
    for (size_t i = 0; i < a.keyEvents.size(); ++i)
        if (a.keyEvents[i].key != b.keyEvents[i].key || a.keyEvents[i].down != b.keyEvents[i].down)
            return false;
    return true;
}

ScenarioFile MakeScenario(size_t frames, bool withGolden)
{
    ScenarioFile scn;
    scn.manifest.created = "2026-09-04T19:40:00Z";
    scn.manifest.coreVersion = "1.80.0+2b0ed42";
    scn.manifest.appCommit = "fe64b62";
    scn.manifest.model = "a500";
    scn.manifest.videoStandard = "PAL";
    scn.manifest.videoResolution = "Hires";
    scn.manifest.userVariables["puae_video_standard"] = "PAL";
    scn.manifest.notes = "round trip";
    scn.manifest.media.push_back({0, "peekaboo_validation.adf", 901120, "0123456789abcdef"});

    for (size_t i = 0; i < frames; ++i)
        scn.input.push_back(MakeInput(static_cast<uint16_t>(i * 7 + 1)));

    if (withGolden) {
        for (size_t i = 0; i < frames; ++i)
            scn.golden.frames.push_back(
                {0x1000ull + i, 0x2000ull + i, static_cast<uint32_t>(882 + (i % 3))});
        scn.golden.ram.push_back({0, 0xdeadbeefull});
        scn.golden.ram.push_back({frames / 2, 0xfeedfaceull});
        scn.golden.keyframes.push_back({0, {0x89, 'P', 'N', 'G', 1, 2, 3}});
        scn.golden.keyframes.push_back({frames / 2, {0x89, 'P', 'N', 'G', 4, 5, 6}});
    }
    return scn;
}

} // namespace

TEST(FrameInputCodecTest, RoundTripsEveryField)
{
    const FrameInput original = MakeInput(1234);
    std::vector<uint8_t> bytes;
    WriteFrameInput(bytes, original);

    ByteReader r(bytes.data(), bytes.size());
    FrameInput restored;
    ASSERT_TRUE(ReadFrameInput(r, restored));
    EXPECT_EQ(r.Remaining(), 0u);
    EXPECT_TRUE(SameInput(original, restored));
}

TEST(FrameInputCodecTest, RoundTripsAnEmptyFrame)
{
    const FrameInput original;
    std::vector<uint8_t> bytes;
    WriteFrameInput(bytes, original);

    ByteReader r(bytes.data(), bytes.size());
    FrameInput restored;
    ASSERT_TRUE(ReadFrameInput(r, restored));
    EXPECT_TRUE(SameInput(original, restored));
}

TEST(FrameInputCodecTest, RejectsATruncatedRecord)
{
    std::vector<uint8_t> bytes;
    WriteFrameInput(bytes, MakeInput(99));
    bytes.resize(bytes.size() - 1);

    ByteReader r(bytes.data(), bytes.size());
    FrameInput restored;
    EXPECT_FALSE(ReadFrameInput(r, restored));
}

// A corrupt count must not be believed: reserving on it is how a malformed file turns into
// an allocation the size of the count field.
TEST(FrameInputCodecTest, RejectsAnImplausibleKeyCount)
{
    std::vector<uint8_t> bytes;
    WriteFrameInput(bytes, MakeInput(5));
    // keysDown count sits right after joypad(4) + mouse deltas(4) + buttons(1).
    bytes[9] = 0xFF;
    bytes[10] = 0xFF;
    bytes[11] = 0xFF;
    bytes[12] = 0x7F;

    ByteReader r(bytes.data(), bytes.size());
    FrameInput restored;
    EXPECT_FALSE(ReadFrameInput(r, restored));
}

TEST(ScenarioFileTest, RoundTripsManifestInputAndGolden)
{
    const ScenarioFile original = MakeScenario(12, true);
    const std::vector<uint8_t> bytes = original.Serialize();

    ScenarioFile restored;
    std::string error;
    ASSERT_TRUE(restored.Deserialize(bytes.data(), bytes.size(), error)) << error;

    EXPECT_EQ(restored.manifest.coreVersion, "1.80.0+2b0ed42");
    EXPECT_EQ(restored.manifest.model, "a500");
    EXPECT_EQ(restored.manifest.videoStandard, "PAL");
    EXPECT_EQ(restored.manifest.videoResolution, "Hires");
    EXPECT_EQ(restored.manifest.notes, "round trip");
    EXPECT_EQ(restored.manifest.userVariables.at("puae_video_standard"), "PAL");
    ASSERT_EQ(restored.manifest.media.size(), 1u);
    EXPECT_EQ(restored.manifest.media[0].md5, "0123456789abcdef");
    EXPECT_EQ(restored.manifest.media[0].size, 901120u);

    ASSERT_EQ(restored.input.size(), original.input.size());
    for (size_t i = 0; i < original.input.size(); ++i)
        EXPECT_TRUE(SameInput(original.input[i], restored.input[i])) << "frame " << i;

    ASSERT_EQ(restored.golden.frames.size(), original.golden.frames.size());
    for (size_t i = 0; i < original.golden.frames.size(); ++i)
        EXPECT_EQ(restored.golden.frames[i], original.golden.frames[i]) << "frame " << i;
    EXPECT_EQ(restored.golden.ram.size(), 2u);
    EXPECT_EQ(restored.golden.ram[1].digest, 0xfeedfaceull);
    ASSERT_EQ(restored.golden.keyframes.size(), 2u);
    EXPECT_EQ(restored.golden.keyframes[1].png, original.golden.keyframes[1].png);
}

// The frame count in the manifest is written from the input stream, not from whatever the
// caller left in the struct - the two can never disagree in a file this writes.
TEST(ScenarioFileTest, FrameCountComesFromTheInputStream)
{
    ScenarioFile scn = MakeScenario(5, false);
    scn.manifest.frameCount = 999;
    const std::vector<uint8_t> bytes = scn.Serialize();

    ScenarioFile restored;
    std::string error;
    ASSERT_TRUE(restored.Deserialize(bytes.data(), bytes.size(), error)) << error;
    EXPECT_EQ(restored.manifest.frameCount, 5u);
    EXPECT_EQ(restored.input.size(), 5u);
}

TEST(ScenarioFileTest, ColdBootScenarioCarriesNoSeed)
{
    ScenarioFile scn = MakeScenario(3, false);
    scn.manifest.coldBoot = true;
    const std::vector<uint8_t> bytes = scn.Serialize();

    ScenarioFile restored;
    std::string error;
    ASSERT_TRUE(restored.Deserialize(bytes.data(), bytes.size(), error)) << error;
    EXPECT_TRUE(restored.manifest.coldBoot);
    EXPECT_TRUE(restored.seedState.empty());
}

TEST(ScenarioFileTest, SeededScenarioRoundTripsTheState)
{
    ScenarioFile scn = MakeScenario(3, false);
    scn.manifest.coldBoot = false;
    scn.seedState.assign(4096, 0x5A);
    scn.seedState[0] = 0x01;
    scn.seedState.back() = 0x02;
    const std::vector<uint8_t> bytes = scn.Serialize();

    ScenarioFile restored;
    std::string error;
    ASSERT_TRUE(restored.Deserialize(bytes.data(), bytes.size(), error)) << error;
    EXPECT_FALSE(restored.manifest.coldBoot);
    ASSERT_EQ(restored.seedState.size(), 4096u);
    EXPECT_EQ(restored.seedState[0], 0x01);
    EXPECT_EQ(restored.seedState.back(), 0x02);
}

TEST(ScenarioFileTest, RejectsForeignAndTruncatedFiles)
{
    ScenarioFile scn;
    std::string error;

    const std::vector<uint8_t> notAScenario = {'P', 'K', 'F', 'H', 1, 0, 0, 0};
    EXPECT_FALSE(scn.Deserialize(notAScenario.data(), notAScenario.size(), error));
    EXPECT_NE(error.find("bad magic"), std::string::npos) << error;

    EXPECT_FALSE(scn.Deserialize(nullptr, 0, error));

    const ScenarioFile good = MakeScenario(4, true);
    std::vector<uint8_t> bytes = good.Serialize();
    bytes.resize(bytes.size() / 2);
    EXPECT_FALSE(scn.Deserialize(bytes.data(), bytes.size(), error));
    EXPECT_TRUE(scn.input.empty());
}

TEST(ScenarioFileTest, RejectsAVersionItDoesNotKnow)
{
    std::vector<uint8_t> bytes = MakeScenario(2, false).Serialize();
    bytes[4] = 99;

    ScenarioFile scn;
    std::string error;
    EXPECT_FALSE(scn.Deserialize(bytes.data(), bytes.size(), error));
    EXPECT_NE(error.find("unsupported scenario version"), std::string::npos) << error;
}

// A golden table that does not cover every frame would silently pass the frames it does
// cover and say nothing about the rest.
TEST(ScenarioFileTest, RejectsAGoldenTableOfTheWrongLength)
{
    ScenarioFile scn = MakeScenario(6, true);
    scn.golden.frames.pop_back();
    const std::vector<uint8_t> bytes = scn.Serialize();

    ScenarioFile restored;
    std::string error;
    EXPECT_FALSE(restored.Deserialize(bytes.data(), bytes.size(), error));
    EXPECT_NE(error.find("golden table covers"), std::string::npos) << error;
}

// EMU-01: a wrongly typed manifest field is an error, like every binary-level problem
TEST(ScenarioFileTest, WrongTypedManifestFieldsAreAnErrorNotAThrow)
{
    for (const std::string manifest :
         {R"({"frameCount": "1"})", R"({"media": [{"drive": "x"}]})", R"({"created": 5})"}) {
        // The writer puts MANI first, straight after the magic and version: swap its body.
        const std::vector<uint8_t> good = MakeScenario(2, false).Serialize();
        uint64_t oldLength = 0;
        for (int i = 0; i < 8; ++i)
            oldLength |= uint64_t(good[12 + i]) << (8 * i);
        std::vector<uint8_t> bytes(good.begin(), good.begin() + 12);
        for (int i = 0; i < 8; ++i)
            bytes.push_back(static_cast<uint8_t>(uint64_t(manifest.size()) >> (8 * i)));
        bytes.insert(bytes.end(), manifest.begin(), manifest.end());
        bytes.insert(bytes.end(), good.begin() + 20 + static_cast<std::ptrdiff_t>(oldLength),
                     good.end());

        ScenarioFile scn;
        std::string error;
        bool ok = true;
        EXPECT_NO_THROW(ok = scn.Deserialize(bytes.data(), bytes.size(), error)) << manifest;
        EXPECT_FALSE(ok) << manifest;
        EXPECT_NE(error.find("manifest"), std::string::npos) << error;
    }
}

TEST(ScenarioFileTest, UnknownChunksAreSkipped)
{
    const ScenarioFile original = MakeScenario(3, false);
    std::vector<uint8_t> bytes = original.Serialize();

    // Splice a chunk from an imaginary newer writer onto the end.
    const char tag[4] = {'X', 'X', 'X', 'X'};
    bytes.insert(bytes.end(), tag, tag + 4);
    const uint64_t length = 5;
    for (int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<uint8_t>(length >> (8 * i)));
    bytes.insert(bytes.end(), {1, 2, 3, 4, 5});

    ScenarioFile restored;
    std::string error;
    ASSERT_TRUE(restored.Deserialize(bytes.data(), bytes.size(), error)) << error;
    EXPECT_EQ(restored.input.size(), 3u);
}

TEST(ScenarioFileTest, SavesAndLoadsThroughTheFilesystem)
{
    const ScenarioFile original = MakeScenario(8, true);
    const auto path = std::filesystem::temp_directory_path() / "adm_scenario_roundtrip.pkscn";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    std::string error;
    ASSERT_TRUE(original.Save(path, error)) << error;

    ScenarioFile restored;
    ASSERT_TRUE(restored.Load(path, error)) << error;
    EXPECT_EQ(restored.input.size(), 8u);
    EXPECT_EQ(restored.golden.frames.size(), 8u);
    std::filesystem::remove(path, ec);

    EXPECT_FALSE(restored.Load(path, error));
}

TEST(ScenarioFileTest, MachineMatchRefusesADifferentMachine)
{
    const ScenarioFile scn = MakeScenario(2, false);
    std::string why;

    ScenarioManifest same = scn.manifest;
    EXPECT_TRUE(scn.MachineMatches(same, why)) << why;
    EXPECT_TRUE(why.empty());

    ScenarioManifest otherModel = scn.manifest;
    otherModel.model = "a1200";
    EXPECT_FALSE(scn.MachineMatches(otherModel, why));
    EXPECT_NE(why.find("model"), std::string::npos) << why;

    ScenarioManifest otherStandard = scn.manifest;
    otherStandard.videoStandard = "NTSC";
    EXPECT_FALSE(scn.MachineMatches(otherStandard, why));
    EXPECT_NE(why.find("video standard"), std::string::npos) << why;

    ScenarioManifest otherMedia = scn.manifest;
    otherMedia.media[0].md5 = "ffffffffffffffff";
    otherMedia.media[0].name = "something_else.adf";
    EXPECT_FALSE(scn.MachineMatches(otherMedia, why));
    EXPECT_NE(why.find("something_else.adf"), std::string::npos) << why;
}

// The core version is recorded so a divergence can be explained, not so a replay can be
// refused. A rebuilt core must still run the scenario - that is how the regression is seen.
TEST(ScenarioFileTest, MachineMatchIgnoresTheCoreVersion)
{
    const ScenarioFile scn = MakeScenario(2, false);
    ScenarioManifest newer = scn.manifest;
    newer.coreVersion = "1.81.0+9ff01ac";

    std::string why;
    EXPECT_TRUE(scn.MachineMatches(newer, why)) << why;
}

TEST(ScenarioGoldenTest, NearestKeyframeWalksBackwards)
{
    ScenarioGolden golden;
    golden.keyframes.push_back({0, {1}});
    golden.keyframes.push_back({50, {2}});
    golden.keyframes.push_back({100, {3}});

    ASSERT_NE(golden.NearestKeyframe(0), nullptr);
    EXPECT_EQ(golden.NearestKeyframe(0)->frame, 0u);
    EXPECT_EQ(golden.NearestKeyframe(49)->frame, 0u);
    EXPECT_EQ(golden.NearestKeyframe(50)->frame, 50u);
    EXPECT_EQ(golden.NearestKeyframe(214)->frame, 100u);

    const ScenarioGolden empty;
    EXPECT_EQ(empty.NearestKeyframe(10), nullptr);
}
