#include "core/UaeSavestate.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <vector>

using adm::core::ExtractCustomRegisters;
using adm::core::SavestateRegisterWord;

namespace {

// Builds savestate blobs in exactly the shape savestate.c:save_chunk() writes them, so the
// parser is tested against the real container rather than a convenient approximation.
class StateBuilder
{
public:
    StateBuilder() { AddChunk("ASF ", std::vector<uint8_t>(16, 0)); }

    void AddChunk(const char* name, const std::vector<uint8_t>& payload)
    {
        for (int i = 0; i < 4; ++i)
            m_data.push_back(static_cast<uint8_t>(name[i]));
        PushU32(static_cast<uint32_t>(payload.size()) + 12); // total = payload + name/len/flags
        PushU32(0);                                          // flags: uncompressed
        m_data.insert(m_data.end(), payload.begin(), payload.end());
        // UAE always emits 1-4 alignment bytes, never zero.
        for (size_t i = 0, n = 4 - (payload.size() & 3); i < n; ++i)
            m_data.push_back(0);
    }

    // A compressed chunk: flags bit 0 set, payload prefixed by its uncompressed length. The
    // parser must step over these without trying to inflate them.
    void AddCompressedChunk(const char* name, const std::vector<uint8_t>& blob)
    {
        for (int i = 0; i < 4; ++i)
            m_data.push_back(static_cast<uint8_t>(name[i]));
        PushU32(static_cast<uint32_t>(blob.size()) + 4 + 12);
        PushU32(1);
        PushU32(0xDEADBEEF); // uncompressed length
        m_data.insert(m_data.end(), blob.begin(), blob.end());
        for (size_t i = 0, n = 4 - (blob.size() & 3); i < n; ++i)
            m_data.push_back(0);
    }

    void AddEnd()
    {
        const uint8_t end[] = {'E', 'N', 'D', ' ', 0, 0, 0, 8};
        m_data.insert(m_data.end(), std::begin(end), std::end(end));
    }

    const std::vector<uint8_t>& Data() const { return m_data; }

private:
    void PushU32(uint32_t v)
    {
        m_data.push_back(static_cast<uint8_t>(v >> 24));
        m_data.push_back(static_cast<uint8_t>(v >> 16));
        m_data.push_back(static_cast<uint8_t>(v >> 8));
        m_data.push_back(static_cast<uint8_t>(v));
    }

    std::vector<uint8_t> m_data;
};

// Writes big-endian words/longs into a chunk payload the way save_custom() does.
class Payload
{
public:
    void W(uint16_t v)
    {
        m_bytes.push_back(static_cast<uint8_t>(v >> 8));
        m_bytes.push_back(static_cast<uint8_t>(v));
    }
    void L(uint32_t v)
    {
        W(static_cast<uint16_t>(v >> 16));
        W(static_cast<uint16_t>(v));
    }
    void B(uint8_t v) { m_bytes.push_back(v); }
    const std::vector<uint8_t>& Bytes() const { return m_bytes; }

private:
    std::vector<uint8_t> m_bytes;
};

// A CHIP payload in save_custom(..., full = 0) order. Every register is filled with a
// value derived from its position so a mis-stepped run shows up as a shifted value rather
// than an accidental match. Selected registers can be overridden.
std::vector<uint8_t> MakeChipPayload(const std::map<uint16_t, uint32_t>& overrides = {})
{
    struct Run
    {
        uint16_t first, last;
        uint8_t width;
    };
    static const Run kRuns[] = {
        {0x000, 0x01E, 2}, {0x020, 0x020, 4}, {0x024, 0x046, 2}, {0x048, 0x054, 4},
        {0x058, 0x07E, 2}, {0x080, 0x084, 4}, {0x088, 0x09E, 2}, {0x0E0, 0x0FC, 4},
        {0x100, 0x11E, 2}, {0x180, 0x1FE, 2},
    };

    Payload p;
    p.L(0x00000002); // currprefs.chipset_mask
    for (const auto& r : kRuns) {
        for (uint16_t off = r.first; off <= r.last; off = static_cast<uint16_t>(off + r.width)) {
            auto it = overrides.find(off);
            if (r.width == 4)
                p.L(it != overrides.end() ? it->second : 0xAA000000u | off);
            else
                p.W(it != overrides.end() ? static_cast<uint16_t>(it->second)
                                          : static_cast<uint16_t>(0x1000u | off));
        }
    }
    p.L(0); // trailing refptr
    return p.Bytes();
}

std::optional<uint16_t> Find(const std::vector<SavestateRegisterWord>& regs, uint16_t offset)
{
    for (const auto& r : regs)
        if (r.offset == offset)
            return r.value;
    return std::nullopt;
}

} // namespace

TEST(UaeSavestateTest, RejectsBlobThatIsNotAnAmigaStateFile)
{
    std::vector<uint8_t> junk(256, 0x5A);
    EXPECT_TRUE(ExtractCustomRegisters(junk.data(), junk.size()).empty());
    EXPECT_TRUE(ExtractCustomRegisters(nullptr, 0).empty());
}

TEST(UaeSavestateTest, ReturnsNothingWhenTheChipChunkIsMissing)
{
    StateBuilder b;
    b.AddChunk("CPU ", std::vector<uint8_t>(64, 0));
    b.AddEnd();
    EXPECT_TRUE(ExtractCustomRegisters(b.Data().data(), b.Data().size()).empty());
}

TEST(UaeSavestateTest, ReadsWriteOnlyRegistersThatDoNotSurviveARestore)
{
    // Exactly the registers that read as zero after load_state today.
    StateBuilder b;
    b.AddChunk("CHIP", MakeChipPayload({
                           {0x08E, 0x2C81}, // DIWSTRT
                           {0x090, 0x2CC1}, // DIWSTOP
                           {0x096, 0x83C0}, // DMACON
                           {0x09A, 0xC020}, // INTENA
                           {0x09E, 0x1100}, // ADKCON
                       }));
    b.AddEnd();

    const auto regs = ExtractCustomRegisters(b.Data().data(), b.Data().size());
    ASSERT_FALSE(regs.empty());
    EXPECT_EQ(Find(regs, 0x08E), 0x2C81);
    EXPECT_EQ(Find(regs, 0x090), 0x2CC1);
    EXPECT_EQ(Find(regs, 0x096), 0x83C0);
    EXPECT_EQ(Find(regs, 0x09A), 0xC020);
    EXPECT_EQ(Find(regs, 0x09E), 0x1100);
}

TEST(UaeSavestateTest, SplitsPointerRegistersHighWordFirst)
{
    // custom_storage holds COP1LCH at $080 and COP1LCL at $082; GetCustomRegisters()
    // reassembles them with SwapPtrWords(), so getting the halves the wrong way round
    // would corrupt every pointer the debugger shows.
    StateBuilder b;
    b.AddChunk("CHIP", MakeChipPayload({{0x080, 0x00021A64}, {0x0E0, 0x00050000}}));
    b.AddEnd();

    const auto regs = ExtractCustomRegisters(b.Data().data(), b.Data().size());
    ASSERT_FALSE(regs.empty());
    EXPECT_EQ(Find(regs, 0x080), 0x0002); // COP1LCH
    EXPECT_EQ(Find(regs, 0x082), 0x1A64); // COP1LCL
    EXPECT_EQ(Find(regs, 0x0E0), 0x0005); // BPL1PTH
    EXPECT_EQ(Find(regs, 0x0E2), 0x0000); // BPL1PTL
}

TEST(UaeSavestateTest, RunsStayAlignedAcrossTheSkippedAudioAndSpriteBlocks)
{
    // save_custom(..., full = 0) omits $0A0-$0DE and $120-$17E. If the parser walked those
    // gaps as if they were present, every later register would be read from the wrong
    // offset — this pins the two registers immediately after each gap.
    StateBuilder b;
    b.AddChunk("CHIP", MakeChipPayload({{0x0E0, 0x00012345}, {0x100, 0x3200}, {0x180, 0x0123}}));
    b.AddEnd();

    const auto regs = ExtractCustomRegisters(b.Data().data(), b.Data().size());
    ASSERT_FALSE(regs.empty());
    EXPECT_EQ(Find(regs, 0x0E0), 0x0001); // first register after the audio gap
    EXPECT_EQ(Find(regs, 0x100), 0x3200); // BPLCON0
    EXPECT_EQ(Find(regs, 0x180), 0x0123); // first colour, after the sprite gap
    EXPECT_EQ(Find(regs, 0x1FE), 0x11FE); // the very last word of the chunk
}

TEST(UaeSavestateTest, DoesNotReportRegistersInTheSkippedBlocksWithoutTheirChunks)
{
    StateBuilder b;
    b.AddChunk("CHIP", MakeChipPayload());
    b.AddEnd();

    const auto regs = ExtractCustomRegisters(b.Data().data(), b.Data().size());
    ASSERT_FALSE(regs.empty());
    EXPECT_FALSE(Find(regs, 0x0A0).has_value()); // AUD0LCH
    EXPECT_FALSE(Find(regs, 0x120).has_value()); // SPR0PTH
    EXPECT_FALSE(Find(regs, 0x140).has_value()); // SPR0POS
}

TEST(UaeSavestateTest, RecoversSpritePointersAndPositionsFromTheSprChunks)
{
    StateBuilder b;
    b.AddChunk("CHIP", MakeChipPayload());
    for (int i = 0; i < 8; ++i) {
        Payload p;
        p.L(0x00030000u + static_cast<uint32_t>(i) * 0x100); // pt
        p.W(static_cast<uint16_t>(0x2A40 + i));              // pos
        p.W(static_cast<uint16_t>(0x2B00 + i));              // ctl
        p.W(static_cast<uint16_t>(0xF000 + i));              // data[0]
        p.W(static_cast<uint16_t>(0x0F00 + i));              // datb[0]
        for (int k = 0; k < 6; ++k)
            p.W(0); // data[1..3] / datb[1..3]
        p.B(1);     // armed
        const char name[5] = {'S', 'P', 'R', static_cast<char>('0' + i), 0};
        b.AddChunk(name, p.Bytes());
    }
    b.AddEnd();

    const auto regs = ExtractCustomRegisters(b.Data().data(), b.Data().size());
    ASSERT_FALSE(regs.empty());
    EXPECT_EQ(Find(regs, 0x120), 0x0003); // SPR0PTH
    EXPECT_EQ(Find(regs, 0x122), 0x0000); // SPR0PTL
    EXPECT_EQ(Find(regs, 0x13C), 0x0003); // SPR7PTH
    EXPECT_EQ(Find(regs, 0x13E), 0x0700); // SPR7PTL
    EXPECT_EQ(Find(regs, 0x140), 0x2A40); // SPR0POS
    EXPECT_EQ(Find(regs, 0x142), 0x2B00); // SPR0CTL
    EXPECT_EQ(Find(regs, 0x144), 0xF000); // SPR0DATA
    EXPECT_EQ(Find(regs, 0x146), 0x0F00); // SPR0DATB
    EXPECT_EQ(Find(regs, 0x178), 0x2A47); // SPR7POS
}

TEST(UaeSavestateTest, RecoversProgrammedAudioRegistersFromTheAudChunks)
{
    StateBuilder b;
    b.AddChunk("CHIP", MakeChipPayload());
    for (int i = 0; i < 4; ++i) {
        Payload p;
        p.B(1);                                      // state
        p.B(static_cast<uint8_t>(0x40 + i));         // audvol
        p.B(0);                                      // intreq2
        p.B(0x80);                                   // dr/dsr flags
        p.W(0x0010);                                 // len (live counter, not a register)
        p.W(static_cast<uint16_t>(0x0100 + i));      // wlen -> AUDxLEN
        p.W(static_cast<uint16_t>(0x0200 + i));      // per  -> AUDxPER
        p.W(static_cast<uint16_t>(0x0300 + i));      // dat  -> AUDxDAT
        p.L(0x00040000u + static_cast<uint32_t>(i)); // lc   -> AUDxLC
        p.L(0);                                      // pt
        p.L(0);                                      // evtime
        p.B(0);                                      // drhpos
        const char name[5] = {'A', 'U', 'D', static_cast<char>('0' + i), 0};
        b.AddChunk(name, p.Bytes());
    }
    b.AddEnd();

    const auto regs = ExtractCustomRegisters(b.Data().data(), b.Data().size());
    ASSERT_FALSE(regs.empty());
    EXPECT_EQ(Find(regs, 0x0A0), 0x0004); // AUD0LCH
    EXPECT_EQ(Find(regs, 0x0A2), 0x0000); // AUD0LCL
    EXPECT_EQ(Find(regs, 0x0A4), 0x0100); // AUD0LEN — the reload value, not the countdown
    EXPECT_EQ(Find(regs, 0x0A6), 0x0200); // AUD0PER
    EXPECT_EQ(Find(regs, 0x0A8), 0x0040); // AUD0VOL
    EXPECT_EQ(Find(regs, 0x0AA), 0x0300); // AUD0DAT
    EXPECT_EQ(Find(regs, 0x0D0), 0x0004); // AUD3LCH
    EXPECT_EQ(Find(regs, 0x0D8), 0x0043); // AUD3VOL
}

TEST(UaeSavestateTest, StepsOverCompressedChunksWithoutLosingAlignment)
{
    // PUAE compresses the RAM images. They sit after the chipset chunks in practice, but a
    // mis-stepped compressed chunk would desynchronise the whole walk, so prove it both
    // before and after the one chunk we care about.
    StateBuilder b;
    b.AddCompressedChunk("CRAM", std::vector<uint8_t>(37, 0xC3));
    b.AddChunk("CHIP", MakeChipPayload({{0x096, 0x8390}}));
    b.AddCompressedChunk("FRAM", std::vector<uint8_t>(18, 0xF1));
    b.AddEnd();

    const auto regs = ExtractCustomRegisters(b.Data().data(), b.Data().size());
    ASSERT_FALSE(regs.empty());
    EXPECT_EQ(Find(regs, 0x096), 0x8390);
}

TEST(UaeSavestateTest, ReportsNothingWhenTheChipChunkIsTruncated)
{
    // A short chunk must not yield a half-built register file: the caller clears the
    // shadow before applying it, so a partial answer would replace stale values with
    // zeroes and look even more like a dead chipset.
    auto payload = MakeChipPayload();
    payload.resize(payload.size() / 2);

    StateBuilder b;
    b.AddChunk("CHIP", payload);
    b.AddEnd();

    EXPECT_TRUE(ExtractCustomRegisters(b.Data().data(), b.Data().size()).empty());
}

TEST(UaeSavestateTest, CoversEveryRegisterOffsetOnceGivenAFullSetOfChunks)
{
    StateBuilder b;
    b.AddChunk("CHIP", MakeChipPayload());
    for (int i = 0; i < 4; ++i) {
        Payload p;
        for (int k = 0; k < 25; ++k)
            p.B(0);
        const char name[5] = {'A', 'U', 'D', static_cast<char>('0' + i), 0};
        b.AddChunk(name, p.Bytes());
    }
    for (int i = 0; i < 8; ++i) {
        Payload p;
        for (int k = 0; k < 25; ++k) // save_custom_sprite() writes exactly 25 bytes
            p.B(0);
        const char name[5] = {'S', 'P', 'R', static_cast<char>('0' + i), 0};
        b.AddChunk(name, p.Bytes());
    }
    b.AddEnd();

    const auto regs = ExtractCustomRegisters(b.Data().data(), b.Data().size());
    std::map<uint16_t, int> seen;
    for (const auto& r : regs) {
        EXPECT_EQ(r.offset & 1, 0) << "register offsets are always even";
        EXPECT_LT(r.offset, 0x200);
        seen[r.offset]++;
    }
    for (const auto& [offset, count] : seen)
        EXPECT_EQ(count, 1) << "offset $" << std::hex << offset << " reported more than once";

    // The only offsets left uncovered are the four unused audio pad words at $xAC/$xAE.
    const size_t kUnusedAudioPads = 8;
    EXPECT_EQ(seen.size(), 256 - kUnusedAudioPads);
}
