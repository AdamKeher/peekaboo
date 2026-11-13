#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/core/ExocetEval.h"

using namespace adm::core;

namespace {

// The vectors are tools/exocet.py's (the crass repo): its Python runtime run over a sequence of
// updates, enables and fallback-clock frames, with what every track read after each step. The
// engine's own test (productions/exotest) holds the 68000 to the same files.
//   exotest_vary: the exotest production's keys on rows of every length (`pack --vary`),
//                 director track 0 - every interpolation, fix and event tracks
//   cra-sneezeman: the Sneezeman disk's exocet.bin, director track 3
std::vector<uint8_t> Fixture(const std::string& name)
{
    for (const char* dir : {"tests/data/exocet/", "../tests/data/exocet/",
                            "../../tests/data/exocet/", "../../../tests/data/exocet/"}) {
        std::ifstream in(std::string(dir) + name, std::ios::binary);
        if (in)
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    return {};
}

uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t Be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Replays an EXOV file ('EXOV', version, tracks, steps, director; then per step op, a, b,
// reached and a LONG a track) against the port. Returns the steps checked.
int Replay(const std::string& binName, const std::string& vecName)
{
    const auto bin = Fixture(binName);
    const auto vec = Fixture(vecName);
    EXPECT_FALSE(bin.empty()) << binName;
    EXPECT_FALSE(vec.empty()) << vecName;
    if (bin.empty() || vec.size() < 16)
        return 0;
    ExocetBin b;
    std::string error;
    EXPECT_TRUE(b.Parse(bin.data(), bin.size(), &error)) << error;
    EXPECT_EQ(Be32(vec.data()), 0x45584F56u); // "EXOV"
    const uint16_t tracks = Be16(vec.data() + 6);
    const uint32_t steps = Be32(vec.data() + 8);
    const uint32_t director = Be32(vec.data() + 12);
    EXPECT_EQ(tracks, b.tracks.size());

    ExocetRuntime rt(b);
    if (director != 0xFFFFFFFFu)
        rt.SetDirector(static_cast<int>(director));
    const size_t stepBytes = 8 + 4u * tracks;
    EXPECT_EQ(vec.size(), 16 + stepBytes * steps);
    int checked = 0;
    for (uint32_t s = 0; s < steps; ++s) {
        const uint8_t* p = vec.data() + 16 + stepBytes * s;
        const uint16_t op = Be16(p), a = Be16(p + 2), bb = Be16(p + 4), reached = Be16(p + 6);
        switch (op) {
            case 0:
                rt.Update(a, bb);
                break;
            case 1:
                rt.Enable(a, true);
                break;
            case 2:
                rt.Enable(a, false);
                break;
            case 3:
                rt.UpdateFrame(a);
                break;
            default:
                ADD_FAILURE() << "unknown op " << op;
        }
        EXPECT_EQ(rt.Reached() & 0xFFFF, reached) << "step " << s;
        for (uint16_t t = 0; t < tracks; ++t) {
            const int32_t want = static_cast<int32_t>(Be32(p + 8 + 4 * t));
            if (rt.Values()[t] != want) {
                ADD_FAILURE() << vecName << " step " << s << " (op " << op << " " << a << " " << bb
                              << ") track " << t << ": " << rt.Values()[t] << ", Python " << want;
                return checked;
            }
        }
        ++checked;
    }
    return checked;
}

} // namespace

TEST(ExocetEvalTest, AgreesWithTheVectorsOnRowsOfEveryLength)
{
    EXPECT_EQ(Replay("exotest_vary.bin", "exotest_vary_vectors.bin"), 1600);
}

TEST(ExocetEvalTest, AgreesWithTheVectorsOnTheSneezemanDisk)
{
    EXPECT_EQ(Replay("cra-sneezeman.bin", "cra-sneezeman_vectors.bin"), 1600);
}

TEST(ExocetEvalTest, ParsesTheSneezemanBin)
{
    const auto bin = Fixture("cra-sneezeman.bin");
    ExocetBin b;
    ASSERT_TRUE(b.Parse(bin.data(), bin.size()));
    EXPECT_EQ(b.manifest, 0x09515221u);
    EXPECT_EQ(b.song, 0x111F7093u);
    EXPECT_EQ(b.rows, 3617);
    EXPECT_EQ(b.frames, 14468);
    ASSERT_EQ(b.tracks.size(), 13u);
    EXPECT_EQ(b.tracks[12].type, static_cast<uint8_t>(ExocetType::Event));
    EXPECT_EQ(b.tracks[12].keys.size(), 4u);
    // lst.flash_color: white, whole, as a LONG
    EXPECT_EQ(b.tracks[9].type, static_cast<uint8_t>(ExocetType::Color));
    EXPECT_EQ(b.tracks[9].def, -1);
    ASSERT_EQ(b.spans.size(), 1u);
    EXPECT_EQ(b.spans[0].length, 4);
}

TEST(ExocetEvalTest, RefusesWhatIsNotABin)
{
    std::vector<uint8_t> junk(64, 0);
    ExocetBin b;
    std::string error;
    EXPECT_FALSE(b.Parse(junk.data(), junk.size(), &error));
    EXPECT_FALSE(error.empty());
    auto bin = Fixture("cra-sneezeman.bin");
    ASSERT_FALSE(bin.empty());
    EXPECT_FALSE(b.Parse(bin.data(), 40, &error)); // tables past the end
}

TEST(ExocetEvalTest, ValueAtMatchesTheRuntimeOnValueTracks)
{
    // The stateless reading the grid uses must be what the runtime settles on at the same place.
    const auto bin = Fixture("exotest_vary.bin");
    ExocetBin b;
    ASSERT_TRUE(b.Parse(bin.data(), bin.size()));
    for (size_t t = 0; t < b.tracks.size(); ++t) {
        if (b.tracks[t].type == static_cast<uint8_t>(ExocetType::Event) || b.tracks[t].keys.empty())
            continue;
        for (uint32_t row = 0; row < b.rows; row += 3) {
            for (uint16_t frac : {uint16_t(0), uint16_t(0x4000), uint16_t(0xC123)}) {
                ExocetRuntime rt(b);
                rt.Enable(b.tracks[t].group, true);
                rt.Update(row, frac);
                ASSERT_EQ(ExocetValueAt(b.tracks[t].keys, b.tracks[t].def, row, frac,
                                        static_cast<ExocetType>(b.tracks[t].type)),
                          rt.Values()[t])
                    << "track " << t << " row " << row << " frac " << frac;
            }
        }
    }
}

TEST(ExocetEvalTest, TheVectorsCoverAColourTrack)
{
    // exotest's b.tint is keyed with every interpolation and each channel both ways, so the
    // replay above holds the per-channel blend to exocet.py's
    const auto bin = Fixture("exotest_vary.bin");
    ExocetBin b;
    ASSERT_TRUE(b.Parse(bin.data(), bin.size()));
    int colours = 0;
    for (const auto& t : b.tracks)
        if (t.type == static_cast<uint8_t>(ExocetType::Color)) {
            ++colours;
            EXPECT_EQ(t.def, 0x12345678);
            EXPECT_EQ(t.keys.size(), 8u);
        }
    EXPECT_EQ(colours, 1);
}

TEST(ExocetEvalTest, ColoursBlendChannelByChannel)
{
    // half way: each byte its own value track, rounded as exo_lerp rounds
    EXPECT_EQ(
        static_cast<uint32_t>(ExocetBlend(0x00FF80FF, static_cast<int32_t>(0xFF0001FFu), 0x8000)),
        0x808041FFu);
    EXPECT_EQ(ExocetBlend(0x01020304, 0x7F7F7F7F, 0), 0x01020304);
    // no channel carries into the next, whatever the signs
    EXPECT_EQ(
        static_cast<uint32_t>(ExocetBlend(static_cast<int32_t>(0xFF00FF00u), 0x00FF00FF, 0xFFFF)),
        0x00FF00FFu);
    const std::vector<ExocetKey> keys = {{0, 1, 0x000000FF},
                                         {4, 0, static_cast<int32_t>(0xFFFFFFFFu)}};
    EXPECT_EQ(static_cast<uint32_t>(ExocetValueAt(keys, 0, 2, 0, ExocetType::Color)), 0x808080FFu);
    // as a plain LONG the same keys go somewhere else entirely
    EXPECT_NE(static_cast<uint32_t>(ExocetValueAt(keys, 0, 2, 0, ExocetType::Int)), 0x808080FFu);
    EXPECT_EQ(ExocetToBin(ExocetType::Color, 4294967295.0), -1);
    EXPECT_EQ(ExocetFromBin(ExocetType::Color, -1), 4294967295.0);
}

TEST(ExocetEvalTest, FixValuesRoundAsPythonDoes)
{
    EXPECT_EQ(ExocetToBin(ExocetType::Fix, 1.5), 98304);
    EXPECT_EQ(ExocetToBin(ExocetType::Fix, -0.25), -16384);
    // half a 65536th: to even, as round() does
    EXPECT_EQ(ExocetToBin(ExocetType::Fix, 0.5 / 65536), 0);
    EXPECT_EQ(ExocetToBin(ExocetType::Fix, 1.5 / 65536), 2);
    EXPECT_EQ(ExocetToBin(ExocetType::Int, 16), 16);
    EXPECT_DOUBLE_EQ(ExocetFromBin(ExocetType::Fix, 98304), 1.5);
}

TEST(ExocetEvalTest, ParsesAKeysSubFromAVersionTwoBin)
{
    const auto bin = Fixture("exotest_vary.bin");
    ExocetBin b;
    std::string error;
    ASSERT_TRUE(b.Parse(bin.data(), bin.size(), &error)) << error;
    int subs = 0;
    for (const auto& t : b.tracks)
        for (const auto& k : t.keys)
            subs += k.sub != 0;
    EXPECT_GT(subs, 0); // exotest.exocet keys some ticks
}

TEST(ExocetEvalTest, AKeyOnATickTakesEffectOnThatTick)
{
    // rows of 4 ticks: 16 on row 2 tick 1 (sub 64), linear to 0 on row 3 tick 3 (sub 192)
    const std::vector<ExocetKey> keys = {{2, 1, 16, 64}, {3, 0, 0, 192}};
    EXPECT_EQ(ExocetValueAt(keys, 5, 2, 0), 5);      // tick 0: before it
    EXPECT_EQ(ExocetValueAt(keys, 5, 2, 16384), 16); // tick 1: on it
    // row 3 tick 0 is halfway through the span of six ticks
    EXPECT_EQ(ExocetValueAt(keys, 5, 3, 0), 16 + ((-16 * 32768 + 0x8000) >> 16));
    EXPECT_EQ(ExocetValueAt(keys, 5, 3, 49152), 0); // tick 3: the last key
    EXPECT_EQ(ExocetSub(3, 4), 192);
    EXPECT_EQ(ExocetSub(1, 6), 42);
    EXPECT_EQ(ExocetTickFrac(1, 6), 10922);
    // whole rows take one division, as before keys had ticks
    EXPECT_EQ(ExocetSpanFraction((3u << 16) | 5, 7u << 16), ((3u << 16) | 5) / 7);
    // a span of 256 rows or more with a sub is halved until it fits (exocet.py span_fraction)
    EXPECT_EQ(ExocetSpanFraction(100u << 16, (300u << 16) | (64u << 8)), 21827u);
}
