#include <gtest/gtest.h>
#include <chrono>
#include <vector>

#include "core/ArCommandInterpreter.h"
#include "core/ArMemoryAccess.h"
#include "core/MemoryAnnotations.h"
#include "core/MemoryLocks.h"
#include "core/UaeAssembler.h"

#include <cstdio>
#include <cstring>
#include <fstream>

using adm::core::ArCommandInterpreter;
using adm::core::ArCpuState;
using adm::core::ArHost;
using adm::core::ArLine;
using adm::core::ArMemoryAccess;
using adm::core::Disassembler;

using LineKind = ArCommandInterpreter::LineKind;

namespace {

// Test fixture: 64 KB of RAM at 0x1000, 256 bytes of ROM at 0xF80000.
class ArInterpreterFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_ram.assign(0x10000, 0);
        m_rom.assign(0x100, 0xEE);

        std::vector<ArMemoryAccess::Region> regions;
        regions.push_back({m_ram.data(), 0x1000, static_cast<uint32_t>(m_ram.size()), "RAM", true});
        regions.push_back(
            {m_rom.data(), 0xF80000, static_cast<uint32_t>(m_rom.size()), "ROM", false});
        m_mem.SetRegions(std::move(regions));

        ArHost host;
        host.isRunning = []() { return true; };
        host.getCpuState = [this](ArCpuState& st) {
            st = m_cpu;
            return true;
        };
        host.setCpuRegister = [this](const std::string& reg, uint32_t value) {
            m_setRegs.push_back({reg, value});
            return true;
        };
        ConfigureHost(host);
        m_interp = std::make_unique<ArCommandInterpreter>(&m_mem, std::move(host));
        m_interp->emitLine = [this](const ArLine& line) {
            m_output.push_back(line.text);
            m_outputLines.push_back(line);
        };
    }

    // Derived fixtures extend the host (assembler, disassembler, ...) here.
    virtual void ConfigureHost(ArHost&) {}

    std::string AllOutput() const
    {
        std::string all;
        for (const auto& l : m_output)
            all += l + "\n";
        return all;
    }

    std::vector<uint8_t> m_ram;
    std::vector<uint8_t> m_rom;
    ArMemoryAccess m_mem;
    ArCpuState m_cpu;
    std::vector<std::pair<std::string, uint32_t>> m_setRegs;
    std::vector<std::string> m_output;
    std::vector<ArLine> m_outputLines;
    std::unique_ptr<ArCommandInterpreter> m_interp;
};

} // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

TEST(ArParseNumber, HexIsDefault)
{
    EXPECT_EQ(ArCommandInterpreter::ParseNumber("100"), 0x100u);
    EXPECT_EQ(ArCommandInterpreter::ParseNumber("DFF180"), 0xDFF180u);
    EXPECT_EQ(ArCommandInterpreter::ParseNumber("$c00000"), 0xC00000u);
    EXPECT_EQ(ArCommandInterpreter::ParseNumber("0x20"), 0x20u);
}

TEST(ArParseNumber, DecimalAndBinaryPrefixes)
{
    EXPECT_EQ(ArCommandInterpreter::ParseNumber("!100"), 100u);
    EXPECT_EQ(ArCommandInterpreter::ParseNumber("%1010"), 10u);
}

TEST(ArParseNumber, RejectsJunk)
{
    EXPECT_FALSE(ArCommandInterpreter::ParseNumber("").has_value());
    EXPECT_FALSE(ArCommandInterpreter::ParseNumber("XYZ").has_value());
    EXPECT_FALSE(ArCommandInterpreter::ParseNumber("!12AB").has_value());
    EXPECT_FALSE(ArCommandInterpreter::ParseNumber("123456789AB").has_value()); // > 32 bit
}

TEST(ArSplitComma, SplitsOutsideQuotes)
{
    std::string l, r;
    ASSERT_TRUE(ArCommandInterpreter::SplitAtLastComma("\"a,b\",1000", l, r));
    EXPECT_EQ(l, "\"a,b\"");
    EXPECT_EQ(r, "1000");
    EXPECT_FALSE(ArCommandInterpreter::SplitAtLastComma("\"a,b\"", l, r));
}

TEST(ArParsePattern, QuotedString)
{
    std::vector<uint8_t> out;
    ASSERT_TRUE(ArCommandInterpreter::ParsePattern("\"AB\"", out));
    EXPECT_EQ(out, (std::vector<uint8_t>{'A', 'B'}));
}

TEST(ArParsePattern, HexBytes)
{
    std::vector<uint8_t> out;
    ASSERT_TRUE(ArCommandInterpreter::ParsePattern("DE AD BEEF", out));
    EXPECT_EQ(out, (std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
    ASSERT_TRUE(ArCommandInterpreter::ParsePattern("5", out));
    EXPECT_EQ(out, (std::vector<uint8_t>{0x05}));
    EXPECT_FALSE(ArCommandInterpreter::ParsePattern("GG", out));
}

TEST(ArMemHexFormat, RoundTrip)
{
    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i)
        bytes[i] = static_cast<uint8_t>(0x41 + i);
    std::vector<bool> valid(16, true);

    std::string line = ArCommandInterpreter::FormatMemHexText(0x00001000, bytes, valid);
    EXPECT_EQ(line.substr(0, 10), "00001000: ");
    EXPECT_EQ(line.size(), 67u); // 10 + 40 + 1 + 16

    uint32_t addr;
    uint8_t parsed[16];
    bool provided[16];
    ASSERT_TRUE(ArCommandInterpreter::ParseMemHexLine(line, addr, parsed, provided));
    EXPECT_EQ(addr, 0x00001000u);
    for (int i = 0; i < 16; ++i) {
        EXPECT_TRUE(provided[i]);
        EXPECT_EQ(parsed[i], bytes[i]);
    }
}

TEST(ArMemHexFormat, InvalidBytesShowAsQuestionMarks)
{
    uint8_t bytes[16] = {};
    std::vector<bool> valid(16, true);
    valid[0] = valid[1] = false;

    std::string line = ArCommandInterpreter::FormatMemHexText(0, bytes, valid);
    EXPECT_EQ(line.substr(10, 4), "????");

    uint32_t addr;
    uint8_t parsed[16];
    bool provided[16];
    ASSERT_TRUE(ArCommandInterpreter::ParseMemHexLine(line, addr, parsed, provided));
    EXPECT_FALSE(provided[0]);
    EXPECT_FALSE(provided[1]);
    EXPECT_TRUE(provided[2]);
}

TEST(ArMemAsciiFormat, RoundTrip)
{
    uint8_t bytes[16];
    memcpy(bytes, "HELLO WORLD!!!16", 16);
    std::vector<bool> valid(16, true);
    std::string line = ArCommandInterpreter::FormatMemAsciiText(0x2000, bytes, valid, 0);
    EXPECT_EQ(line, "00002000: \"HELLO WORLD!!!16\"");

    uint32_t addr;
    char chars[16];
    ASSERT_TRUE(ArCommandInterpreter::ParseMemAsciiLine(line, addr, chars));
    EXPECT_EQ(addr, 0x2000u);
    EXPECT_EQ(std::string(chars, 16), "HELLO WORLD!!!16");
}

TEST(ArMemBinFormat, RoundTrip)
{
    uint8_t bytes[2] = {0xA5, 0x0F};
    std::vector<bool> valid(2, true);
    std::string line = ArCommandInterpreter::FormatMemBinText(0x3000, bytes, valid, 2);
    EXPECT_EQ(line, "00003000: 10100101 00001111");

    uint32_t addr = 0;
    uint8_t parsed[ArCommandInterpreter::kMemBinMaxBytes];
    int count = 0;
    ASSERT_TRUE(ArCommandInterpreter::ParseMemBinLine(line, addr, parsed, count));
    EXPECT_EQ(addr, 0x3000u);
    EXPECT_EQ(count, 2);
    EXPECT_EQ(parsed[0], 0xA5);
    EXPECT_EQ(parsed[1], 0x0F);

    // A hex dump line must not be mistaken for a binary line.
    uint8_t hb[16] = {};
    std::vector<bool> hv(16, true);
    EXPECT_FALSE(ArCommandInterpreter::ParseMemBinLine(
        ArCommandInterpreter::FormatMemHexText(0x1000, hb, hv), addr, parsed, count));
}

// ---------------------------------------------------------------------------
// Memory annotations
// ---------------------------------------------------------------------------

using adm::core::MemoryAnnotations;

TEST(MemoryAnnotations, ExtractsOperandAddressesSkippingImmediates)
{
    // Absolute long address operand.
    auto a = MemoryAnnotations::ExtractOperandAddresses("JMP $00FC00D2");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0], 0x00FC00D2u);

    // Immediate values (#$...) are not addresses and must be skipped.
    auto b = MemoryAnnotations::ExtractOperandAddresses("MOVE.W #$0005,D0");
    EXPECT_TRUE(b.empty());

    // Register-only operands have no $hex literals.
    auto c = MemoryAnnotations::ExtractOperandAddresses("MOVE.B (A1),D1");
    EXPECT_TRUE(c.empty());

    // Mixed: immediate skipped, displacement kept.
    auto d = MemoryAnnotations::ExtractOperandAddresses("MOVE.L #$10,$00020000");
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0], 0x00020000u);

    // Register-relative displacements are offsets off an address register, not
    // absolute addresses, in both spellings UAE emits — and must be skipped.
    auto e = MemoryAnnotations::ExtractOperandAddresses("JSR (a6,-$01c8)");
    EXPECT_TRUE(e.empty());
    auto f = MemoryAnnotations::ExtractOperandAddresses("MOVE.L -$01c8(a6),d0");
    EXPECT_TRUE(f.empty());
    // A real absolute that follows a register-relative source is still kept.
    auto g = MemoryAnnotations::ExtractOperandAddresses("MOVE.L (a6,-$01c8),$00020000");
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0], 0x00020000u);
}

TEST(MemoryAnnotations, BuildSuffixMatchesInstructionAndOperandAddresses)
{
    MemoryAnnotations ann;
    ann.Clear(); // ensure a known-empty store regardless of any on-disk file

    ann.SetComment(0x00020000, "Loop start here");
    ann.SetComment(0x000200A4, "Custom copper list");

    // Instruction-address match.
    EXPECT_EQ(ann.BuildAnnotationSuffix(0x00020000, "MOVE.W #$0005,D0"), "  ; Loop start here");

    // Operand-address match (instruction address itself is unannotated).
    EXPECT_EQ(ann.BuildAnnotationSuffix(0x00030000, "JMP $000200A4"), "  ; Custom copper list");

    // Both instruction and operand annotated -> two comments, instruction first.
    EXPECT_EQ(ann.BuildAnnotationSuffix(0x00020000, "JMP $000200A4"),
              "  ; Loop start here  ; Custom copper list");

    // No match -> empty suffix.
    EXPECT_TRUE(ann.BuildAnnotationSuffix(0x00040000, "NOP").empty());

    ann.Clear();
}

TEST(MemoryAnnotations, SetRemoveGetAndOrdering)
{
    MemoryAnnotations ann;
    ann.Clear();

    ann.SetComment(0x2000, "second");
    ann.SetComment(0x1000, "first");
    ann.SetComment(0x3000, "third");

    ASSERT_TRUE(ann.GetComment(0x1000).has_value());
    EXPECT_EQ(*ann.GetComment(0x1000), "first");

    // All() is ordered ascending by address.
    std::vector<uint32_t> addrs;
    for (const auto& [addr, entry] : ann.All())
        addrs.push_back(addr);
    ASSERT_EQ(addrs.size(), 3u);
    EXPECT_EQ(addrs[0], 0x1000u);
    EXPECT_EQ(addrs[1], 0x2000u);
    EXPECT_EQ(addrs[2], 0x3000u);

    // Overwrite existing.
    ann.SetComment(0x2000, "updated");
    EXPECT_EQ(*ann.GetComment(0x2000), "updated");

    EXPECT_TRUE(ann.Remove(0x2000));
    EXPECT_FALSE(ann.Remove(0x2000)); // already gone
    EXPECT_FALSE(ann.GetComment(0x2000).has_value());

    ann.Clear();
    EXPECT_TRUE(ann.Empty());
}

TEST(MemoryAnnotations, LabelsCoexistWithCommentsAndClearIndependently)
{
    MemoryAnnotations ann;
    ann.Clear();

    // Both fields on one address.
    ann.SetLabel(0x1000, "loop");
    ann.SetComment(0x1000, "wait for ready");
    ASSERT_TRUE(ann.GetLabel(0x1000).has_value());
    EXPECT_EQ(*ann.GetLabel(0x1000), "loop");
    EXPECT_EQ(*ann.GetComment(0x1000), "wait for ready");

    // Clearing the comment leaves the label (entry stays alive).
    EXPECT_TRUE(ann.RemoveComment(0x1000));
    EXPECT_FALSE(ann.GetComment(0x1000).has_value());
    ASSERT_TRUE(ann.GetLabel(0x1000).has_value());
    EXPECT_FALSE(ann.Empty());

    // Clearing the label too erases the entry.
    EXPECT_TRUE(ann.RemoveLabel(0x1000));
    EXPECT_TRUE(ann.Empty());

    ann.Clear();
}

TEST(MemoryAnnotations, BuildBranchLabelSuffixMatchesTargetLabels)
{
    MemoryAnnotations ann;
    ann.Clear();

    ann.SetLabel(0x00FC00D2, "loop");

    // Absolute jump to a labeled target -> "<loop>" (raw address kept by caller).
    EXPECT_EQ(ann.BuildBranchLabelSuffix(0x00FC0000, "JMP $00FC00D2"), "  <loop>");

    // Relative branches encode a displacement, not the target, so the address
    // must be resolved against the PC. BRA.S #$FE at $00001000 loops to $1000.
    ann.Clear();
    ann.SetLabel(0x00001000, "tight");
    EXPECT_EQ(ann.BuildBranchLabelSuffix(0x00001000 + 2, "BNE.S #$FC"), "  <tight>");
    // ($1002 + 2 - 4 = $1000)

    // The instruction's own label is excluded — it renders as the line above.
    EXPECT_TRUE(ann.BuildBranchLabelSuffix(0x00FC00D2, "NOP").empty());

    // Comments do not produce branch-label references.
    ann.Clear();
    ann.SetComment(0x00FC00D2, "just a comment");
    EXPECT_TRUE(ann.BuildBranchLabelSuffix(0x00FC0000, "BRA $00FC00D2").empty());

    ann.Clear();
}

TEST(MemoryAnnotations, LoadsLegacyRemarkSchemaAsComment)
{
    // Write an old-format file (single "remark" key) to the default path, then
    // construct a fresh store so its Load() runs against it.
    {
        std::ofstream f("memory_annotations.json");
        f << R"([{"address": 4096, "remark": "legacy note"}])";
    }
    {
        MemoryAnnotations loaded;
        ASSERT_TRUE(loaded.GetComment(0x1000).has_value());
        EXPECT_EQ(*loaded.GetComment(0x1000), "legacy note");
        EXPECT_FALSE(loaded.GetLabel(0x1000).has_value());
        loaded.Clear();
    }
}

TEST(MemoryAnnotations, BuildCommentBlockLinesWrapsAndPrefixes)
{
    auto lines = MemoryAnnotations::BuildCommentBlockLines("first\nsecond", 4);
    ASSERT_EQ(lines.size(), 4u); // top divider, 2 content lines, bottom divider
    EXPECT_EQ(lines.front(), ";====");
    EXPECT_EQ(lines.back(), ";====");
    EXPECT_EQ(lines[1], "; first");
    EXPECT_EQ(lines[2], "; second");

    // A single line still gets both dividers.
    auto one = MemoryAnnotations::BuildCommentBlockLines("solo", 2);
    ASSERT_EQ(one.size(), 3u);
    EXPECT_EQ(one[1], "; solo");

    // Empty input -> no lines. Divider width is clamped to >= 1.
    EXPECT_TRUE(MemoryAnnotations::BuildCommentBlockLines("", 8).empty());
    EXPECT_EQ(MemoryAnnotations::BuildCommentBlockLines("x", 0).front(), ";=");
}

TEST(MemoryAnnotations, BlockCoexistsAndRoundTripsThroughDisk)
{
    {
        MemoryAnnotations ann;
        ann.Clear();
        ann.SetLabel(0x2000, "routine");
        ann.SetBlock(0x2000, "Init copper\nThen wait");
        ASSERT_TRUE(ann.GetBlock(0x2000).has_value());
        EXPECT_EQ(*ann.GetBlock(0x2000), "Init copper\nThen wait");

        // Clearing the block leaves the label (entry stays alive).
        EXPECT_TRUE(ann.RemoveBlock(0x2000));
        EXPECT_FALSE(ann.GetBlock(0x2000).has_value());
        EXPECT_FALSE(ann.Empty());

        // Re-set so the next store loads it from disk.
        ann.SetBlock(0x2000, "Init copper\nThen wait");
    }
    {
        MemoryAnnotations loaded;
        ASSERT_TRUE(loaded.GetBlock(0x2000).has_value());
        EXPECT_EQ(*loaded.GetBlock(0x2000), "Init copper\nThen wait"); // newlines preserved
        EXPECT_EQ(*loaded.GetLabel(0x2000), "routine");
        loaded.Clear();
    }
}

TEST(ArClassify, RecognisesKinds)
{
    uint8_t bytes[16] = {};
    std::vector<bool> valid(16, true);
    std::string memLine = ArCommandInterpreter::FormatMemHexText(0x1000, bytes, valid);
    EXPECT_EQ(ArCommandInterpreter::ClassifyLine(memLine), LineKind::MemHexEdit);

    std::string asciiLine = ArCommandInterpreter::FormatMemAsciiText(0x1000, bytes, valid, 0);
    EXPECT_EQ(ArCommandInterpreter::ClassifyLine(asciiLine), LineKind::MemAsciiEdit);

    std::string binLine = ArCommandInterpreter::FormatMemBinText(0x1000, bytes, valid, 2);
    EXPECT_EQ(ArCommandInterpreter::ClassifyLine(binLine), LineKind::MemBinEdit);

    EXPECT_EQ(ArCommandInterpreter::ClassifyLine(""), LineKind::Blank);
    EXPECT_EQ(ArCommandInterpreter::ClassifyLine("   "), LineKind::Blank);
    EXPECT_EQ(ArCommandInterpreter::ClassifyLine("M 1000"), LineKind::Command);
    EXPECT_EQ(ArCommandInterpreter::ClassifyLine("HELP"), LineKind::Command);

    // Disassembly output is addressed but not a memory dump
    EXPECT_EQ(
        ArCommandInterpreter::ClassifyLine("00FC0000: 4EF9 00FC 00D2           JMP $00FC00D2"),
        LineKind::AddressedListing);

    EXPECT_EQ(ArCommandInterpreter::ClassifyLine("D0=00000000 D1=00000001"),
              LineKind::RegisterEdit);
    EXPECT_EQ(ArCommandInterpreter::ClassifyLine("DFF180 COLOR00      = 0ABC"),
              LineKind::CustomRegEdit);
}

// ---------------------------------------------------------------------------
// Interpreter behaviour over fake memory
// ---------------------------------------------------------------------------

TEST_F(ArInterpreterFixture, MemDumpEmitsLineAndEntersMode)
{
    m_ram[0] = 0xCA;
    m_ram[1] = 0xFE;
    m_interp->ExecuteLine("M 1000");
    ASSERT_EQ(m_output.size(), 1u);
    EXPECT_EQ(m_output[0].substr(0, 14), "00001000: CAFE");
    EXPECT_EQ(m_interp->GetMode(), ArCommandInterpreter::Mode::MemHex);
    EXPECT_EQ(m_interp->PreferredCursorColumn(), 10);
}

TEST_F(ArInterpreterFixture, MemHexEditWritesChangedBytes)
{
    m_interp->ExecuteLine("M 1000");
    std::string line = m_output[0];
    // Type over the first word: AB CD
    line[10] = 'A';
    line[11] = 'B';
    line[12] = 'C';
    line[13] = 'D';
    m_interp->ExecuteLine(line);
    EXPECT_EQ(m_ram[0], 0xAB);
    EXPECT_EQ(m_ram[1], 0xCD);
    // The executed line is refreshed (falls back to emitLine without a
    // rewrite sink), then interactive mode emitted the next line
    ASSERT_EQ(m_output.size(), 3u);
    EXPECT_EQ(m_output[1].substr(0, 14), "00001000: ABCD");
    EXPECT_EQ(m_output[2].substr(0, 10), "00001010: ");
}

TEST_F(ArInterpreterFixture, MemAsciiTailEditWritesBytes)
{
    m_ram[0] = 'A';
    m_ram[1] = 'B';
    m_interp->ExecuteLine("M 1000");
    std::string line = m_output[0];
    // Type over the ASCII tail: first two chars become "XY"
    line[51] = 'X';
    line[52] = 'Y';
    m_interp->ExecuteLine(line);
    EXPECT_EQ(m_ram[0], 'X');
    EXPECT_EQ(m_ram[1], 'Y');
}

TEST_F(ArInterpreterFixture, MemAsciiTrailingSpaceEditWritesByte)
{
    m_ram[15] = 'Z';
    m_interp->ExecuteLine("M 1000");
    std::string line = m_output[0];
    // Type a space over the last ASCII character of the line
    line[66] = ' ';
    m_interp->ExecuteLine(line);
    EXPECT_EQ(m_ram[15], 0x20);
}

TEST_F(ArInterpreterFixture, MemHexEditTakesPrecedenceOverAscii)
{
    m_ram[0] = 'A';
    m_interp->ExecuteLine("M 1000");
    std::string line = m_output[0];
    // Edit both columns for byte 0: hex says 5A, ascii says 'Q'
    line[10] = '5';
    line[11] = 'A';
    line[51] = 'Q';
    m_interp->ExecuteLine(line);
    EXPECT_EQ(m_ram[0], 0x5A);
}

TEST_F(ArInterpreterFixture, RewriteSinkReceivesRefreshedLine)
{
    std::vector<std::string> rewrites;
    m_interp->rewriteLine = [&](const ArLine& l) { rewrites.push_back(l.text); };

    m_interp->ExecuteLine("M 1000");
    std::string line = m_output[0];
    line[10] = 'F';
    line[11] = 'F';
    m_interp->ExecuteLine(line);

    ASSERT_EQ(rewrites.size(), 1u);
    EXPECT_EQ(rewrites[0].substr(0, 14), "00001000: FF00");
    // Sequential output only contains the dump line + next interactive line
    ASSERT_EQ(m_output.size(), 2u);
}

TEST_F(ArInterpreterFixture, RomWriteFails)
{
    m_interp->ExecuteLine("WS 42, F80000");
    EXPECT_EQ(m_rom[0], 0xEE); // unchanged
    EXPECT_NE(AllOutput().find("WRITE PROTECTED"), std::string::npos);
}

TEST_F(ArInterpreterFixture, FillAndFind)
{
    m_interp->ExecuteLine("O \"AB\", 1100 1107");
    EXPECT_EQ(m_ram[0x100], 'A');
    EXPECT_EQ(m_ram[0x101], 'B');
    EXPECT_EQ(m_ram[0x107], 'B');

    m_output.clear();
    m_interp->ExecuteLine("F \"ABAB\"");
    EXPECT_NE(AllOutput().find("00001100"), std::string::npos);
}

TEST_F(ArInterpreterFixture, LongFindDumpsMatchesWithHighlight)
{
    memcpy(&m_ram[0x234], "HELLO", 5);
    m_interp->ExecuteLine("F -l \"HELLO\"");

    // Two dump lines + count line
    ASSERT_EQ(m_output.size(), 3u);
    EXPECT_EQ(m_output[0].substr(0, 10), "00001234: ");
    EXPECT_NE(m_output[0].find("4845 4C4C 4F"), std::string::npos); // HELLO hex
    EXPECT_NE(m_output[0].find("HELLO"), std::string::npos);        // ascii tail
    EXPECT_EQ(m_output[1].substr(0, 10), "00001244: ");
    EXPECT_NE(m_output[2].find("1 match"), std::string::npos);

    // The matched bytes are highlighted: first hex group at col 10 and the
    // first ascii char at col 51
    bool hexHighlighted = false, asciiHighlighted = false;
    for (const auto& s : m_outputLines[0].spans) {
        if (s.color != adm::core::ArColor::Highlight)
            continue;
        if (s.col == 10)
            hexHighlighted = true;
        if (s.col == 51)
            asciiHighlighted = true;
    }
    EXPECT_TRUE(hexHighlighted);
    EXPECT_TRUE(asciiHighlighted);

    // Dump lines remain valid editable memory lines
    EXPECT_EQ(ArCommandInterpreter::ClassifyLine(m_output[0]), LineKind::MemHexEdit);
}

TEST_F(ArInterpreterFixture, LongFindCapsAtTwentyResults)
{
    for (int i = 0; i < 30; ++i)
        m_ram[static_cast<size_t>(i) * 0x40] = 0x7E;
    m_interp->ExecuteLine("F -l 7E");
    // 20 results * 2 lines + summary line
    ASSERT_EQ(m_output.size(), 41u);
    EXPECT_NE(m_output.back().find("Showing first 20 of 30"), std::string::npos);
}

TEST_F(ArInterpreterFixture, CaseInsensitiveFind)
{
    const char* s = "Hello";
    memcpy(&m_ram[0x200], s, 5);
    m_interp->ExecuteLine("FS \"hello\"");
    EXPECT_NE(AllOutput().find("00001200"), std::string::npos);
    m_output.clear();
    m_interp->ExecuteLine("F \"hello\"");
    EXPECT_NE(AllOutput().find("Not found."), std::string::npos);
}

TEST_F(ArInterpreterFixture, RelativeFind)
{
    // "IFMMP" = "HELLO" + 1
    memcpy(&m_ram[0x300], "IFMMP", 5);
    m_interp->ExecuteLine("FR \"HELLO\"");
    std::string out = AllOutput();
    EXPECT_NE(out.find("00001300"), std::string::npos);
    EXPECT_NE(out.find("offset +01"), std::string::npos);
}

TEST_F(ArInterpreterFixture, TransferHandlesOverlap)
{
    for (int i = 0; i < 8; ++i)
        m_ram[i] = static_cast<uint8_t>(i + 1);
    // Copy 1000-1007 -> 1004 (overlapping forward)
    m_interp->ExecuteLine("TRANS 1000 1007 1004");
    EXPECT_EQ(m_ram[4], 1);
    EXPECT_EQ(m_ram[5], 2);
    EXPECT_EQ(m_ram[11], 8);
}

TEST_F(ArInterpreterFixture, CompareReportsDifferences)
{
    m_ram[0] = 0x11;
    m_ram[0x10] = 0x22;
    m_interp->ExecuteLine("COMP 1000 1003 1010");
    std::string out = AllOutput();
    EXPECT_NE(out.find("1 difference"), std::string::npos);
}

TEST_F(ArInterpreterFixture, MemcodeAndAdd)
{
    m_ram[0] = 0x0F;
    m_interp->ExecuteLine("MEMCODE 1000 1000 FF");
    EXPECT_EQ(m_ram[0], 0xF0);
    m_interp->ExecuteLine("ADD 1000 1000 10");
    EXPECT_EQ(m_ram[0], 0x00);
}

TEST_F(ArInterpreterFixture, ExchangeSwapsBlocks)
{
    m_ram[0] = 0xAA;
    m_ram[0x10] = 0xBB;
    m_interp->ExecuteLine("EX 1000 1000 1010");
    EXPECT_EQ(m_ram[0], 0xBB);
    EXPECT_EQ(m_ram[0x10], 0xAA);
}

TEST_F(ArInterpreterFixture, RegisterShowAndEdit)
{
    m_cpu.d[0] = 0x11111111;
    m_cpu.pc = 0x1000;
    m_interp->ExecuteLine("R");
    std::string out = AllOutput();
    EXPECT_NE(out.find("D0=11111111"), std::string::npos);
    EXPECT_NE(out.find("PC=00001000"), std::string::npos);

    // Edit the D0 line
    std::string regLine = m_output[0]; // "D0=... D1=... D2=... D3=..."
    regLine.replace(regLine.find("D0=") + 3, 8, "DEADBEEF");
    m_interp->ExecuteLine(regLine);
    ASSERT_EQ(m_setRegs.size(), 1u);
    EXPECT_EQ(m_setRegs[0].first, "d0");
    EXPECT_EQ(m_setRegs[0].second, 0xDEADBEEFu);
}

TEST_F(ArInterpreterFixture, UnknownCommandReports)
{
    m_interp->ExecuteLine("FROBNICATE");
    EXPECT_NE(AllOutput().find("unknown command"), std::string::npos);
}

TEST_F(ArInterpreterFixture, EscapeCancelsMode)
{
    m_interp->ExecuteLine("M 1000");
    EXPECT_EQ(m_interp->GetMode(), ArCommandInterpreter::Mode::MemHex);
    m_interp->CancelInteractive();
    EXPECT_EQ(m_interp->GetMode(), ArCommandInterpreter::Mode::None);
}

TEST_F(ArInterpreterFixture, ScrollingDumpPagesViaTick)
{
    m_interp->ExecuteLine("MQ 1000 11FF");
    EXPECT_TRUE(m_interp->IsScrolling());
    m_output.clear();
    m_interp->Tick();
    EXPECT_EQ(m_output.size(), 32u); // 32 lines * 16 bytes = 512 bytes per tick
    m_interp->Tick();
    EXPECT_FALSE(m_interp->IsScrolling()); // 0x200 bytes done
}

TEST_F(ArInterpreterFixture, AssemblerNotInstalled)
{
    m_interp->ExecuteLine("A 1000");
    EXPECT_NE(AllOutput().find("Error: assembler not installed"), std::string::npos);
}

// ---------------------------------------------------------------------------
// A (assemble, type-over)
// ---------------------------------------------------------------------------

namespace {

// Fixture with a toy instruction set: NOP/RTS (2 bytes) and MOVE.W #$1234,D0
// (4 bytes), disassembled straight from fixture memory.
class ArAssemblerFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.disassembleOne = [this](uint32_t addr, Disassembler::Line& out) {
            bool ok = false;
            const uint16_t w = m_mem.ReadWord(addr, &ok);
            if (!ok)
                return false;
            char buf[32];
            out.address = addr;
            if (w == 0x4E71) {
                out.mnemonic = "NOP";
                out.hex = "4E71";
                out.nextAddress = addr + 2;
            } else if (w == 0x4E75) {
                out.mnemonic = "RTS";
                out.hex = "4E75";
                out.nextAddress = addr + 2;
            } else if (w == 0x303C) {
                const uint16_t imm = m_mem.ReadWord(addr + 2, &ok);
                snprintf(buf, sizeof(buf), "MOVE.W #$%04X,D0", imm);
                out.mnemonic = buf;
                snprintf(buf, sizeof(buf), "303C %04X", imm);
                out.hex = buf;
                out.nextAddress = addr + 4;
            } else {
                snprintf(buf, sizeof(buf), "DC.W $%04X", w);
                out.mnemonic = buf;
                snprintf(buf, sizeof(buf), "%04X", w);
                out.hex = buf;
                out.nextAddress = addr + 2;
            }
            return true;
        };
        host.assembler = std::make_shared<adm::core::UaeAssembler>(
            [this](const std::string& src, uint32_t, uint16_t* words, int) -> int {
                ++m_asmCalls;
                if (src == "NOP") {
                    words[0] = 0x4E71;
                    return 1;
                }
                if (src == "RTS") {
                    words[0] = 0x4E75;
                    return 1;
                }
                if (src == "MOVE.W #$1234,D0") {
                    words[0] = 0x303C;
                    words[1] = 0x1234;
                    return 2;
                }
                return 0; // syntax error
            });
    }

    // "AAAAAAAA: <hex>" padded to the mnemonic column + <mnemonic>
    static std::string Listing(uint32_t addr, const std::string& hex, const std::string& mnemonic)
    {
        char head[16];
        snprintf(head, sizeof(head), "%08X: ", addr);
        std::string s = std::string(head) + hex;
        if (s.size() < static_cast<size_t>(Disassembler::kMnemonicColumn))
            s.append(static_cast<size_t>(Disassembler::kMnemonicColumn) - s.size(), ' ');
        return s + mnemonic;
    }

    int m_asmCalls = 0;
};

} // namespace

TEST_F(ArAssemblerFixture, AssembleEntersTypeOverMode)
{
    m_ram[0x1000] = 0x4E; // NOP at amiga addr 0x2000
    m_ram[0x1001] = 0x71;
    m_interp->ExecuteLine("A 2000");
    EXPECT_EQ(m_interp->GetMode(), ArCommandInterpreter::Mode::Assemble);
    EXPECT_EQ(m_interp->PreferredCursorColumn(), Disassembler::kMnemonicColumn);
    // Emits the existing instruction for type-over
    EXPECT_NE(AllOutput().find("00002000: 4E71"), std::string::npos);
    EXPECT_NE(AllOutput().find("NOP"), std::string::npos);
}

TEST_F(ArAssemblerFixture, PlainSourceAssemblesAtCurrentAddress)
{
    m_interp->ExecuteLine("A 2000");
    m_output.clear();
    m_interp->ExecuteLine("MOVE.W #$1234,D0");
    EXPECT_EQ(m_ram[0x1000], 0x30);
    EXPECT_EQ(m_ram[0x1001], 0x3C);
    EXPECT_EQ(m_ram[0x1002], 0x12);
    EXPECT_EQ(m_ram[0x1003], 0x34);
    // Verified disassembly rewrites the line, next instruction follows
    EXPECT_NE(AllOutput().find("00002000: 303C 1234"), std::string::npos);
    EXPECT_NE(AllOutput().find("00002004:"), std::string::npos);
    EXPECT_EQ(m_interp->GetMode(), ArCommandInterpreter::Mode::Assemble);
}

TEST_F(ArAssemblerFixture, TypedOverListingUsesAddressPrefixAndMnemonicColumn)
{
    m_ram[0x1000] = 0x4E; // NOP at 0x2000
    m_ram[0x1001] = 0x71;
    m_interp->ExecuteLine("A 2000");
    m_interp->ExecuteLine(Listing(0x2000, "4E71", "RTS"));
    EXPECT_EQ(m_ram[0x1000], 0x4E);
    EXPECT_EQ(m_ram[0x1001], 0x75); // now RTS
    EXPECT_EQ(m_asmCalls, 1);
}

TEST_F(ArAssemblerFixture, UnchangedLineAdvancesWithoutWriting)
{
    m_ram[0x1000] = 0x4E; // NOP at 0x2000
    m_ram[0x1001] = 0x71;
    m_ram[0x1002] = 0x4E; // RTS at 0x2002
    m_ram[0x1003] = 0x75;
    m_interp->ExecuteLine("A 2000");
    m_output.clear();
    m_interp->ExecuteLine(Listing(0x2000, "4E71", "NOP"));
    EXPECT_EQ(m_asmCalls, 0); // no re-assembly of unedited text
    EXPECT_EQ(m_ram[0x1000], 0x4E);
    EXPECT_EQ(m_ram[0x1001], 0x71);
    // Advanced to the next instruction
    EXPECT_NE(AllOutput().find("00002002: 4E75"), std::string::npos);
}

TEST_F(ArAssemblerFixture, SyntaxErrorStaysInMode)
{
    m_interp->ExecuteLine("A 2000");
    m_interp->ExecuteLine("FNORD");
    EXPECT_NE(AllOutput().find("SYNTAX ERROR"), std::string::npos);
    EXPECT_EQ(m_interp->GetMode(), ArCommandInterpreter::Mode::Assemble);
}

TEST_F(ArAssemblerFixture, WriteProtectedRomReportsError)
{
    m_interp->ExecuteLine("A F80000");
    m_interp->ExecuteLine("NOP");
    EXPECT_NE(AllOutput().find("WRITE PROTECTED"), std::string::npos);
    EXPECT_EQ(m_rom[0], 0xEE);
    EXPECT_EQ(m_interp->GetMode(), ArCommandInterpreter::Mode::Assemble);
}

TEST_F(ArAssemblerFixture, EscapeExitsAssembleMode)
{
    m_interp->ExecuteLine("A 2000");
    m_interp->CancelInteractive();
    EXPECT_EQ(m_interp->GetMode(), ArCommandInterpreter::Mode::None);
}

TEST_F(ArAssemblerFixture, SectionSeparatorDoesNotHaltStepping)
{
    m_ram[2] = 0x4E;
    m_ram[3] = 0x75; // RTS at 0x1002
    m_ram[4] = 0x4E;
    m_ram[5] = 0x71; // NOP at 0x1004

    // Disassembling an RTS emits the listing line plus a ';====' section separator.
    m_interp->ExecuteLine("D 1002");
    ASSERT_GE(m_output.size(), 2u);
    EXPECT_NE(m_output[0].find("RTS"), std::string::npos);
    EXPECT_EQ(m_output[1].front(), ';');

    // Return lands the cursor on the separator; pressing it (a blank/comment line)
    // must continue disassembly rather than stall.
    m_output.clear();
    m_interp->ExecuteLine("");
    ASSERT_FALSE(m_output.empty());
    EXPECT_NE(m_output[0].find("NOP"), std::string::npos);
}

// ---------------------------------------------------------------------------
// TS/T/TX/TF/TFD + deep trainer (AR5 manual chapter 8)
// ---------------------------------------------------------------------------

TEST_F(ArInterpreterFixture, TrainerSearchNarrowsCandidates)
{
    m_ram[0x1000] = 0x06; // amiga 0x2000
    m_ram[0x2000] = 0x06; // amiga 0x3000
    m_interp->ExecuteLine("TS 6");
    EXPECT_NE(AllOutput().find("Trainer started"), std::string::npos);
    EXPECT_NE(AllOutput().find("Change the value in-game"), std::string::npos);

    // Lose a life at 0x2000 only
    m_ram[0x1000] = 0x05;
    m_output.clear();
    m_interp->ExecuteLine("T 5");
    EXPECT_NE(AllOutput().find("still match"), std::string::npos);
    EXPECT_NE(AllOutput().find("00002000"), std::string::npos);
    EXPECT_EQ(AllOutput().find("00003000"), std::string::npos);
    EXPECT_NE(AllOutput().find("still match"), std::string::npos);
}

TEST_F(ArInterpreterFixture, TrainerGluedFormsParse)
{
    m_ram[0x1000] = 0x06;
    m_interp->ExecuteLine("TS6"); // manual: "TS6"
    EXPECT_NE(AllOutput().find("Trainer started"), std::string::npos);
    m_ram[0x1000] = 0x05;
    m_output.clear();
    m_interp->ExecuteLine("T5"); // manual: "T5"
    EXPECT_NE(AllOutput().find("00002000"), std::string::npos);
}

TEST_F(ArInterpreterFixture, TrainerExactCommandsShadowGluedForms)
{
    // "TF" must be the sub finder, not trainer-continue with value F
    m_interp->ExecuteLine("TF");
    EXPECT_NE(AllOutput().find("Usage: TF"), std::string::npos);
    // "TD" must be the deep trainer list, not trainer-continue with value D
    m_output.clear();
    m_interp->ExecuteLine("TD");
    EXPECT_NE(AllOutput().find("deep trainer not active"), std::string::npos);
}

TEST_F(ArInterpreterFixture, TrainerContinueRequiresTrainmode)
{
    m_interp->ExecuteLine("T 5");
    EXPECT_NE(AllOutput().find("not in trainer mode"), std::string::npos);
}

TEST_F(ArInterpreterFixture, TrainerExitClearsState)
{
    m_interp->ExecuteLine("TS 6");
    m_interp->ExecuteLine("TX");
    EXPECT_NE(AllOutput().find("Trainer cleared."), std::string::npos);
    m_output.clear();
    m_interp->ExecuteLine("T 5");
    EXPECT_NE(AllOutput().find("not in trainer mode"), std::string::npos);
}

TEST_F(ArInterpreterFixture, TrainerFindMatchesSubInstructions)
{
    // SUBQ.B #1,$00044972.L at amiga 0x2100
    const uint8_t subq[] = {0x53, 0x39, 0x00, 0x04, 0x49, 0x72};
    memcpy(&m_ram[0x1100], subq, sizeof(subq));
    // SUBI.W #1,$00044972.L at amiga 0x2110
    const uint8_t subi[] = {0x04, 0x79, 0x00, 0x01, 0x00, 0x04, 0x49, 0x72};
    memcpy(&m_ram[0x1110], subi, sizeof(subi));
    // SUBQ.B #1 with a different operand at amiga 0x2120 — must not match
    const uint8_t other[] = {0x53, 0x39, 0x00, 0x04, 0x49, 0x74};
    memcpy(&m_ram[0x1120], other, sizeof(other));

    m_interp->ExecuteLine("TF 44972");
    EXPECT_NE(AllOutput().find("Decrement found at $00002100"), std::string::npos);
    EXPECT_NE(AllOutput().find("Decrement found at $00002110"), std::string::npos);
    EXPECT_EQ(AllOutput().find("00002120"), std::string::npos);
}

TEST_F(ArInterpreterFixture, TrainerFindMatchesShortAbsolute)
{
    // SUBQ.B #1,$2000.W at amiga 0x2100
    const uint8_t subq[] = {0x53, 0x38, 0x20, 0x00};
    memcpy(&m_ram[0x1100], subq, sizeof(subq));
    m_interp->ExecuteLine("TF 2000");
    EXPECT_NE(AllOutput().find("Decrement found at $00002100"), std::string::npos);
}

TEST_F(ArInterpreterFixture, TrainerFindDeleteNopsInstruction)
{
    // SUBQ.L #1,$00044972.L at amiga 0x2100
    const uint8_t subq[] = {0x53, 0xB9, 0x00, 0x04, 0x49, 0x72};
    memcpy(&m_ram[0x1100], subq, sizeof(subq));
    m_interp->ExecuteLine("TFD 44972");
    EXPECT_NE(AllOutput().find("Decrement found at $00002100"), std::string::npos);
    EXPECT_NE(AllOutput().find("NOP'd"), std::string::npos);
    const uint8_t nops[] = {0x4E, 0x71, 0x4E, 0x71, 0x4E, 0x71};
    EXPECT_EQ(memcmp(&m_ram[0x1100], nops, sizeof(nops)), 0);
}

TEST_F(ArInterpreterFixture, TrainerFindReportsNoSubs)
{
    m_interp->ExecuteLine("TF 44972");
    EXPECT_NE(AllOutput().find("No decrement instructions found."), std::string::npos);
}

// ---------------------------------------------------------------------------
// FB (break on bootblock entry)
// ---------------------------------------------------------------------------

namespace {

class ArBootblockFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.armBootblockBreak = [this](bool on) { m_fbArmed = on; };
        host.isBootblockArmed = [this]() { return m_fbArmed; };
        host.consumeBootblockHit = [this]() {
            const bool h = m_fbHit;
            m_fbHit = false;
            return h;
        };
        host.bootblockEntry = [this]() { return m_fbEntry; };
        host.resume = [this]() { ++m_resumeCount; };
        host.getCpuState = [this](ArCpuState& st) {
            st = m_cpu;
            return true;
        };
        host.disassembleOne = [](uint32_t addr, Disassembler::Line& out) {
            out.address = addr;
            out.nextAddress = addr + 2;
            out.mnemonic = "NOP";
            out.hex = "4E71";
            return true;
        };
    }

    bool m_fbArmed = false;
    bool m_fbHit = false;
    uint32_t m_fbEntry = 0;
    int m_resumeCount = 0;
};

} // namespace

TEST_F(ArBootblockFixture, FbTogglesAndResumes)
{
    m_interp->ExecuteLine("FB");
    EXPECT_TRUE(m_fbArmed);
    EXPECT_EQ(m_resumeCount, 1);
    EXPECT_NE(AllOutput().find("Break on bootblock entry armed"), std::string::npos);

    m_output.clear();
    m_interp->ExecuteLine("FB"); // toggle off
    EXPECT_FALSE(m_fbArmed);
    EXPECT_EQ(m_resumeCount, 1); // disarm does not resume
    EXPECT_NE(AllOutput().find("Bootblock break disarmed"), std::string::npos);
}

TEST_F(ArBootblockFixture, FbBreakBannerAnnouncesEntry)
{
    m_fbHit = true;
    m_fbEntry = 0x070000;
    m_cpu.pc = 0x070000;
    m_interp->OnDebugBreak();
    EXPECT_NE(AllOutput().find("** BOOTBLOCK ENTRY @ 070000 **"), std::string::npos);
    EXPECT_NE(AllOutput().find("BREAK AT 00070000"), std::string::npos);
}

TEST_F(ArBootblockFixture, NonFbBreakHasNoBootblockBanner)
{
    m_fbHit = false;
    m_cpu.pc = 0x001234;
    m_interp->OnDebugBreak();
    EXPECT_EQ(AllOutput().find("BOOTBLOCK ENTRY"), std::string::npos);
    EXPECT_NE(AllOutput().find("BREAK AT 00001234"), std::string::npos);
}

TEST_F(ArInterpreterFixture, FbUnavailableWithoutCoreSupport)
{
    // Base fixture wires no armBootblockBreak hook (simulates an old core)
    m_interp->ExecuteLine("FB");
    EXPECT_NE(AllOutput().find("FB unavailable"), std::string::npos);
}

namespace {

class ArRunUntilFrameFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.isPaused = [this]() { return m_paused; };
        host.getFrameCount = [this]() { return m_frame; };
        host.runUntilFrame = [this](uint64_t target, bool turbo) {
            m_runTarget = target;
            m_runTurbo = turbo;
            m_paused = false;
            return true;
        };
        host.getCpuState = [this](ArCpuState& st) {
            st = m_cpu;
            return true;
        };
        host.disassembleOne = [](uint32_t addr, Disassembler::Line& out) {
            out.address = addr;
            out.nextAddress = addr + 2;
            out.mnemonic = "NOP";
            out.hex = "4E71";
            return true;
        };
    }

    bool m_paused = true;
    uint64_t m_frame = 10;
    uint64_t m_runTarget = 0;
    bool m_runTurbo = false;
};

} // namespace

TEST_F(ArRunUntilFrameFixture, RunsToTargetFrame)
{
    m_interp->ExecuteLine("RUNTO 100"); // frame numbers are decimal
    EXPECT_EQ(m_runTarget, 100u);
    EXPECT_FALSE(m_runTurbo);
    EXPECT_NE(AllOutput().find("Running to frame 100..."), std::string::npos);
}

TEST_F(ArRunUntilFrameFixture, TurboFlagEnablesWarp)
{
    m_interp->ExecuteLine("RUNTO 100 T");
    EXPECT_EQ(m_runTarget, 100u);
    EXPECT_TRUE(m_runTurbo);
    EXPECT_NE(AllOutput().find("(turbo)"), std::string::npos);
}

TEST_F(ArRunUntilFrameFixture, FrameNumberIsDecimalNotHex)
{
    m_interp->ExecuteLine("RUNTO 20"); // decimal 20, not hex 0x20
    EXPECT_EQ(m_runTarget, 20u);
}

TEST_F(ArRunUntilFrameFixture, RejectsTargetAtOrBeforeCurrent)
{
    m_frame = 100;
    m_interp->ExecuteLine("RUNTO 100"); // == current
    EXPECT_EQ(m_runTarget, 0u);         // hook not called
    EXPECT_NE(AllOutput().find("already at/past frame"), std::string::npos);
}

TEST_F(ArRunUntilFrameFixture, RejectsNonDecimalFrame)
{
    m_interp->ExecuteLine("RUNTO $64");
    EXPECT_EQ(m_runTarget, 0u);
    EXPECT_NE(AllOutput().find("bad frame number"), std::string::npos);
}

TEST_F(ArRunUntilFrameFixture, RequiresFrameArgument)
{
    m_interp->ExecuteLine("RUNTO");
    EXPECT_EQ(m_runTarget, 0u);
    EXPECT_NE(AllOutput().find("Usage: RUNTO"), std::string::npos);
}

TEST_F(ArInterpreterFixture, RunUntilFrameUnavailableWithoutCoreSupport)
{
    m_interp->ExecuteLine("RUNTO 100");
    EXPECT_NE(AllOutput().find("RUNTO unavailable"), std::string::npos);
}

TEST_F(ArInterpreterFixture, DeepTrainerNarrowsChangedBytes)
{
    m_interp->ExecuteLine("TDS");
    EXPECT_NE(AllOutput().find("Deep trainer armed"), std::string::npos);

    m_ram[0x10] = 0x01; // amiga 0x1010
    m_ram[0x20] = 0x02; // amiga 0x1020
    m_output.clear();
    m_interp->ExecuteLine("TDC");
    EXPECT_NE(AllOutput().find("2 addresses changed"), std::string::npos);
    EXPECT_NE(AllOutput().find("00001010"), std::string::npos);
    EXPECT_NE(AllOutput().find("00001020"), std::string::npos);

    m_ram[0x10] = 0x07; // only 0x1010 changes again
    m_output.clear();
    m_interp->ExecuteLine("TDC");
    EXPECT_NE(AllOutput().find("1 address changed"), std::string::npos);
    EXPECT_NE(AllOutput().find("00001010"), std::string::npos);
    EXPECT_EQ(AllOutput().find("00001020"), std::string::npos);

    // TD shows current values, TDI lists matches
    m_output.clear();
    m_interp->ExecuteLine("TD");
    EXPECT_NE(AllOutput().find("00001010 = 07"), std::string::npos);
    m_output.clear();
    m_interp->ExecuteLine("TDI");
    EXPECT_NE(AllOutput().find("1 candidate"), std::string::npos);

    // TDD removes the range, TDX exits
    m_output.clear();
    m_interp->ExecuteLine("TDD 1010 1010");
    EXPECT_NE(AllOutput().find("0 candidates remain."), std::string::npos);
    m_interp->ExecuteLine("TDX");
    EXPECT_NE(AllOutput().find("Deep trainer cleared."), std::string::npos);
    m_output.clear();
    m_interp->ExecuteLine("TDC");
    EXPECT_NE(AllOutput().find("deep trainer not active"), std::string::npos);
}

TEST_F(ArInterpreterFixture, SaveAndLoadMemory)
{
    for (int i = 0; i < 16; ++i)
        m_ram[i] = static_cast<uint8_t>(i);
    std::string path = testing::TempDir() + "/ar_sm_test.bin";
    m_interp->ExecuteLine("SM " + path + ",1000 100F");
    EXPECT_NE(AllOutput().find("16 bytes saved"), std::string::npos);

    m_interp->ExecuteLine("LM " + path + ",1100");
    EXPECT_EQ(memcmp(&m_ram[0x100], &m_ram[0], 16), 0);
}

// ---------------------------------------------------------------------------
// Exec / system structure commands (EXECBASE, LIBRARIES, KICKVER, ...)
// ---------------------------------------------------------------------------

namespace {

// RAM mapped at 0 (so address $4 / the CPU vector table are addressable) with a
// hand-built ExecBase, one library node and a Kickstart ROM region.
class ArSystemFixture : public ::testing::Test
{
protected:
    static constexpr uint32_t kExecBase = 0x1000;
    static constexpr uint32_t kLibNode = 0x2100;

    void SetUp() override
    {
        m_ram.assign(0x4000, 0);
        m_rom.assign(0x80000, 0xEE);

        std::vector<ArMemoryAccess::Region> regions;
        regions.push_back({m_ram.data(), 0, static_cast<uint32_t>(m_ram.size()), "Chip RAM", true});
        regions.push_back(
            {m_rom.data(), 0xF80000, static_cast<uint32_t>(m_rom.size()), "Kickstart ROM", false});
        m_mem.SetRegions(std::move(regions));

        BuildExecBase();

        ArHost host;
        host.isRunning = []() { return true; };
        m_interp = std::make_unique<ArCommandInterpreter>(&m_mem, std::move(host));
        m_interp->emitLine = [this](const ArLine& line) { m_output.push_back(line.text); };
    }

    void W16(uint32_t a, uint16_t v)
    {
        m_ram[a] = static_cast<uint8_t>(v >> 8);
        m_ram[a + 1] = static_cast<uint8_t>(v & 0xFF);
    }
    void W32(uint32_t a, uint32_t v)
    {
        W16(a, static_cast<uint16_t>(v >> 16));
        W16(a + 2, static_cast<uint16_t>(v & 0xFFFF));
    }
    void WStr(uint32_t a, const char* s) { std::strcpy(reinterpret_cast<char*>(&m_ram[a]), s); }

    void BuildExecBase()
    {
        const uint32_t eb = kExecBase;
        W32(4, eb);       // *($4) -> ExecBase
        W16(eb + 20, 40); // lib_Version
        W16(eb + 22, 68); // lib_Revision
        WStr(0x2000, "exec 40.68 (15.10.93)");
        W32(eb + 24, 0x2000);     // lib_IdString
        W16(eb + 296, 0x0006);    // AttnFlags: 68020 + 68030
        W16(eb + 292, 0x0000);    // SysFlags
        W32(eb + 62, 0x00200000); // MaxLocMem

        // One library node on LibList (head at eb+378).
        const uint32_t list = eb + 378;
        WStr(0x2200, "exec.library");
        W32(kLibNode + 10, 0x2200);  // ln_Name
        W16(kLibNode + 20, 40);      // lib_Version
        W16(kLibNode + 22, 10);      // lib_Revision
        W16(kLibNode + 32, 3);       // lib_OpenCnt
        W32(kLibNode + 0, list + 4); // ln_Succ -> lh_Tail (sentinel)
        W32(list + 0, kLibNode);     // lh_Head
        W32(list + 4, 0);            // lh_Tail = 0

        // Exception vectors: $2 in ROM (not hooked), $9 (trace) in RAM (hooked).
        W32(2 * 4, 0xF80100);
        W32(9 * 4, 0x00003000);
    }

    std::string AllOutput() const
    {
        std::string all;
        for (const auto& l : m_output)
            all += l + "\n";
        return all;
    }

    std::vector<uint8_t> m_ram;
    std::vector<uint8_t> m_rom;
    ArMemoryAccess m_mem;
    std::vector<std::string> m_output;
    std::unique_ptr<ArCommandInterpreter> m_interp;
};

} // namespace

TEST_F(ArSystemFixture, ExecBaseDumpsFields)
{
    m_interp->ExecuteLine("EXECBASE");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("ExecBase at $00001000"), std::string::npos);
    EXPECT_NE(out.find("40.068"), std::string::npos);
    EXPECT_NE(out.find("68020"), std::string::npos);
    EXPECT_NE(out.find("68030"), std::string::npos);
    EXPECT_NE(out.find("$00200000"), std::string::npos);
    EXPECT_NE(out.find("exec 40.68 (15.10.93)"), std::string::npos);
}

TEST_F(ArSystemFixture, LibrariesWalksList)
{
    m_interp->ExecuteLine("LIBRARIES");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("$00002100"), std::string::npos);
    EXPECT_NE(out.find("40.10"), std::string::npos);
    EXPECT_NE(out.find("exec.library"), std::string::npos);
}

TEST_F(ArSystemFixture, KickVerReadsVersionAndRom)
{
    m_interp->ExecuteLine("KICKVER");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("40.68"), std::string::npos);
    EXPECT_NE(out.find("AmigaOS 3.1"), std::string::npos);
    EXPECT_NE(out.find("15.10.93"), std::string::npos);
    EXPECT_NE(out.find("$00F80000 - $00FFFFFF"), std::string::npos);
}

TEST_F(ArSystemFixture, ExceptionsFlagsHookedVectors)
{
    m_interp->ExecuteLine("EXCEPTIONS");
    const std::string out = AllOutput();
    // Trace ($24) points into RAM -> hooked; Bus Error ($08) points into ROM.
    EXPECT_NE(out.find("Trace"), std::string::npos);
    EXPECT_NE(out.find("$00003000 [hooked]"), std::string::npos);
    EXPECT_NE(out.find("$00F80100"), std::string::npos);
    EXPECT_EQ(out.find("$00F80100 [hooked]"), std::string::npos);
}

TEST_F(ArSystemFixture, ExecCommandsNeedExecBase)
{
    W32(4, 0); // wipe the ExecBase pointer
    m_interp->ExecuteLine("EXECBASE");
    EXPECT_NE(AllOutput().find("ExecBase unavailable"), std::string::npos);
}

TEST_F(ArSystemFixture, DChipDecodesBits)
{
    // Provide minimal custom-register metadata via the host hooks.
    ArHost host;
    host.isRunning = []() { return true; };
    host.customRegName = [](uint32_t off) -> std::string {
        return off == 0x002 ? "DMACONR" : std::string();
    };
    host.customRegBits = [](uint32_t off) {
        std::vector<ArHost::RegBitField> bits;
        if (off == 0x002) {
            bits.push_back({15, 15, "SETCLR"});
            bits.push_back({9, 9, "DMAEN"});
        }
        return bits;
    };
    auto interp = std::make_unique<ArCommandInterpreter>(&m_mem, std::move(host));
    std::vector<std::string> out;
    interp->emitLine = [&out](const ArLine& l) { out.push_back(l.text); };
    m_mem.readCustomReg = [](uint32_t off, uint16_t& v) {
        v = (off == 0x002) ? 0x8200 : 0; // SETCLR=1, DMAEN=1
        return true;
    };

    interp->ExecuteLine("DCHIP DMACONR");
    std::string all;
    for (const auto& l : out)
        all += l + "\n";
    EXPECT_NE(all.find("DMACONR ($DFF002) = $8200"), std::string::npos);
    EXPECT_NE(all.find("Bit 15"), std::string::npos);
    EXPECT_NE(all.find("SETCLR"), std::string::npos);
    EXPECT_NE(all.find("DMAEN"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Y/YY/YYY (binary view) + YS, ? calculator, CRC, BAMCHK, STACK, CALL
// ---------------------------------------------------------------------------

TEST_F(ArInterpreterFixture, BinaryDumpEmitsLineAndEntersMode)
{
    m_ram[0] = 0xA5;
    m_ram[1] = 0x0F;
    m_interp->ExecuteLine("Y 1000");
    ASSERT_EQ(m_output.size(), 1u);
    EXPECT_EQ(m_output[0], "00001000: 10100101 00001111");
    EXPECT_EQ(m_interp->GetMode(), ArCommandInterpreter::Mode::MemBin);
    EXPECT_EQ(m_interp->PreferredCursorColumn(), 10);
}

TEST_F(ArInterpreterFixture, BinaryEditWritesChangedBytes)
{
    m_interp->ExecuteLine("Y 1000");
    std::string line = m_output[0];
    // Flip the first byte to 1111 0000 = 0xF0
    for (int b = 0; b < 8; ++b)
        line[10 + b] = (b < 4) ? '1' : '0';
    m_interp->ExecuteLine(line);
    EXPECT_EQ(m_ram[0], 0xF0u);
    // refreshed line + next line emitted
    ASSERT_EQ(m_output.size(), 3u);
    EXPECT_EQ(m_output[1].substr(0, 18), "00001000: 11110000");
    EXPECT_EQ(m_output[2].substr(0, 10), "00001002: ");
}

TEST_F(ArInterpreterFixture, BinarySizeChangesWidth)
{
    m_interp->ExecuteLine("YS 1");
    m_interp->ExecuteLine("Y 1000");
    // One byte per line: one 8-bit group only
    EXPECT_EQ(m_output.back().size(), 18u); // "AAAAAAAA: bbbbbbbb"
}

TEST_F(ArInterpreterFixture, CalculatorEvaluatesExpression)
{
    m_interp->ExecuteLine("? 10+!10"); // 0x10 + 10 = 26 = 0x1A
    EXPECT_NE(AllOutput().find("= $1A"), std::string::npos);
    EXPECT_NE(AllOutput().find("!26"), std::string::npos);

    m_output.clear();
    m_interp->ExecuteLine("? (2+3)*4"); // 20 = 0x14
    EXPECT_NE(AllOutput().find("= $14"), std::string::npos);

    m_output.clear();
    m_interp->ExecuteLine("? 1+"); // malformed
    EXPECT_NE(AllOutput().find("bad expression"), std::string::npos);
}

TEST_F(ArInterpreterFixture, CrcChecksumsRange)
{
    m_ram[0] = 0x12;
    m_ram[1] = 0x34;
    m_interp->ExecuteLine("CRC32 1000 1001");
    EXPECT_NE(AllOutput().find("CRC32:"), std::string::npos);
    EXPECT_NE(AllOutput().find("(2 bytes)"), std::string::npos);
    // Identical data hashes identically; different data differs.
    m_output.clear();
    m_interp->ExecuteLine("CRC16 1000 1001");
    std::string a = AllOutput();
    m_ram[0] = 0x99;
    m_output.clear();
    m_interp->ExecuteLine("CRC16 1000 1001");
    EXPECT_NE(a, AllOutput());
}

TEST_F(ArInterpreterFixture, BamCheckMakesSumZero)
{
    // Put some non-zero longwords in the bitmap block; checksum should zero the sum.
    m_ram[4] = 0x00;
    m_ram[5] = 0x00;
    m_ram[6] = 0x00;
    m_ram[7] = 0x05;
    m_ram[8] = 0x00;
    m_ram[9] = 0x00;
    m_ram[10] = 0x00;
    m_ram[11] = 0x07;
    m_interp->ExecuteLine("BAMCHK 1000");
    EXPECT_NE(AllOutput().find("Bitmap checksum fixed"), std::string::npos);
    uint32_t sum = 0;
    for (int i = 0; i < 128; ++i)
        sum += (uint32_t(m_ram[i * 4]) << 24) | (uint32_t(m_ram[i * 4 + 1]) << 16) |
               (uint32_t(m_ram[i * 4 + 2]) << 8) | m_ram[i * 4 + 3];
    EXPECT_EQ(sum, 0u);
}

TEST_F(ArInterpreterFixture, StackDumpsLongwords)
{
    m_cpu.a[7] = 0x1000;
    m_ram[0] = 0x00;
    m_ram[1] = 0x00;
    m_ram[2] = 0x10;
    m_ram[3] = 0x20; // -> RAM region
    m_interp->ExecuteLine("STACK 2");
    EXPECT_NE(AllOutput().find("Stack at A7=$00001000"), std::string::npos);
    EXPECT_NE(AllOutput().find("Offset  Address    Value      Points to"), std::string::npos);
    EXPECT_NE(AllOutput().find("RAM"), std::string::npos);

    // First entry: "A7+000  $00001000  $00001020  RAM" — the address hyperlink
    // covers the full $-prefixed address under the "Address" column header.
    const ArLine& row = m_outputLines[3]; // [0]=title, [1]=col header, [2]=rule, [3]=first entry
    EXPECT_NE(row.text.find("A7+000  $00001000  $00001020"), std::string::npos);
    bool addrSpan = false;
    for (const auto& s : row.spans)
        if (s.color == adm::core::ArColor::Address) {
            addrSpan = true;
            EXPECT_EQ(s.col, 8);
            EXPECT_EQ(s.len, 9);
            EXPECT_EQ(row.text.substr(s.col, s.len), "$00001000");
        }
    EXPECT_TRUE(addrSpan);
}

namespace {

// Fixture whose disassembler reports a BSR.W (4 bytes) at one address so the
// CALL heuristic can confirm a return-address frame.
class ArCallFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.disassembleOne = [this](uint32_t addr, Disassembler::Line& out) {
            bool ok = false;
            uint16_t w = m_mem.ReadWord(addr, &ok);
            if (!ok)
                return false;
            out.address = addr;
            if (w == 0x6100) { // BSR.W <disp16>
                out.mnemonic = "BSR.W $2000";
                out.hex = "6100";
                out.nextAddress = addr + 4;
            } else {
                out.mnemonic = "NOP";
                out.hex = "4E71";
                out.nextAddress = addr + 2;
            }
            return true;
        };
    }
};

} // namespace

TEST_F(ArCallFixture, CallFindsReturnFrame)
{
    // Place a BSR.W at 0x1100; its return address is 0x1104. Put 0x1104 on the stack.
    m_ram[0x100] = 0x61;
    m_ram[0x101] = 0x00; // BSR.W opcode at amiga 0x1100
    m_cpu.a[7] = 0x1000;
    m_ram[0] = 0x00;
    m_ram[1] = 0x00;
    m_ram[2] = 0x11;
    m_ram[3] = 0x04; // SP -> 0x1104
    m_interp->ExecuteLine("CALL 4");
    EXPECT_NE(AllOutput().find("Frame  Return     Call site  Instruction"), std::string::npos);
    EXPECT_NE(AllOutput().find("$00001104"), std::string::npos); // return address
    EXPECT_NE(AllOutput().find("$00001100"), std::string::npos); // call site
    EXPECT_NE(AllOutput().find("BSR.W"), std::string::npos);
}

namespace {

// Fixture providing a canned UAE `r`/`c` console capture for W/RC/RF/RM.
class ArUaeCaptureFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.execUaeCommand = [](const std::string& cmd, std::string& out) {
            if (cmd == "c") {
                out = "A: CRA 01 CRB 00 ICR 00 IM 00 TA FFFF (FFFF) TB FFFF (FFFF)\n"
                      "   PRA FF PRB 00 DDRA 03 DDRB FF\n"
                      "   TOD 000000 (000000) ALARM 000000 -S CYC=0\n"
                      "B: CRA 00 CRB 00 ICR 00 IM 00 TA FFFF (FFFF) TB FFFF (FFFF)\n"
                      "   PRA FF PRB 7F DDRA 03 DDRB FF\n"
                      "   TOD 000000 (000000) ALARM 000000 -S\n"
                      "DSKLEN ...\n";
                return true;
            }
            return false;
        };
    }
};

} // namespace

TEST_F(ArUaeCaptureFixture, CiaDumpShowsBothChips)
{
    m_interp->ExecuteLine("W");
    std::string all = AllOutput();
    EXPECT_NE(all.find("CIA REGISTERS"), std::string::npos);
    EXPECT_NE(all.find("A: CRA 01"), std::string::npos);
    EXPECT_NE(all.find("B: CRA 00"), std::string::npos);
    EXPECT_EQ(all.find("DSKLEN"), std::string::npos); // stops before disk dump
}

TEST_F(ArInterpreterFixture, PaletteEmitsSwatchSpan)
{
    // COLOR00 = $0F0 (pure green) -> expanded 24-bit #00FF00
    m_mem.readCustomReg = [](uint32_t off, uint16_t& v) {
        v = (off == 0x180) ? 0x0F0 : 0;
        return true;
    };
    m_interp->ExecuteLine("PAL");
    // Header (2 lines) + 32 colour rows
    ASSERT_EQ(m_outputLines.size(), 34u);
    const ArLine& row0 = m_outputLines[2];
    EXPECT_NE(row0.text.find("#00FF00"), std::string::npos);
    bool foundSwatch = false;
    for (const auto& s : row0.spans) {
        if (s.color == adm::core::ArColor::Swatch) {
            foundSwatch = true;
            EXPECT_EQ(s.payload, 0x00FF00u);
        }
    }
    EXPECT_TRUE(foundSwatch);
}

// ---------------------------------------------------------------------------
// PROFILE / CALLSTACK / TRACE
// ---------------------------------------------------------------------------

namespace {

// Fixture that wires a stub profiler + CPU-trace toggle onto the host.
class ArProfileFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.getProfile = [this](const adm::core::ProfileQuery& q) {
            m_lastQuery = q;
            return m_snapshot;
        };
        host.setCpuTrace = [this](bool on) { m_traceOn = on; };
        host.isCpuTrace = [this]() { return m_traceOn; };
    }

    adm::core::ProfileSnapshot m_snapshot;
    adm::core::ProfileQuery m_lastQuery;
    bool m_traceOn = false;
};

} // namespace

TEST_F(ArProfileFixture, ProfileReportsNoDataWhenTraceEmpty)
{
    m_snapshot.available = false;
    m_interp->ExecuteLine("PROFILE");
    EXPECT_NE(AllOutput().find("No profile data"), std::string::npos);
}

TEST_F(ArProfileFixture, ProfileFunctionsTable)
{
    m_snapshot.available = true;
    m_snapshot.haveSymbols = true;
    m_snapshot.resolvedCycles = 800;
    m_snapshot.unresolvedCycles = 200;
    adm::core::FunctionStats fn;
    fn.name = "GameLoop";
    fn.totalCycles = 400;
    fn.count = 100;
    fn.pctFrame = 0.4f;
    m_snapshot.functions.push_back(fn);

    m_interp->ExecuteLine("PROFILE FUNC 5");
    EXPECT_EQ(m_lastQuery.view, adm::core::ProfileView::Functions);
    EXPECT_EQ(m_lastQuery.limit, 5);
    EXPECT_NE(AllOutput().find("GameLoop"), std::string::npos);
    EXPECT_NE(AllOutput().find("Function profile"), std::string::npos);
}

TEST_F(ArProfileFixture, ProfileFlameRoutesToBands)
{
    m_snapshot.available = true;
    m_interp->ExecuteLine("PROFILE FLAME");
    EXPECT_EQ(m_lastQuery.view, adm::core::ProfileView::Flame);
    EXPECT_NE(AllOutput().find("Flame graph"), std::string::npos);
}

TEST_F(ArProfileFixture, CallStackCommandQueriesPlayhead)
{
    m_snapshot.available = true;
    m_snapshot.haveSymbols = true;
    m_snapshot.playheadLRef = 0x1234;
    adm::core::CallFrame f;
    f.depth = 0;
    f.funcAddress = 0x2000;
    f.name = "main";
    m_snapshot.callStack.push_back(f);

    m_interp->ExecuteLine("CALLSTACK");
    EXPECT_EQ(m_lastQuery.view, adm::core::ProfileView::CallStack);
    EXPECT_NE(AllOutput().find("main"), std::string::npos);
    EXPECT_NE(AllOutput().find("001234"), std::string::npos);
}

TEST_F(ArProfileFixture, TraceTogglesCpuTrace)
{
    m_interp->ExecuteLine("TRACE ON");
    EXPECT_TRUE(m_traceOn);
    m_interp->ExecuteLine("TRACE OFF");
    EXPECT_FALSE(m_traceOn);
    // No arg -> reports status.
    m_interp->ExecuteLine("TRACE");
    EXPECT_NE(AllOutput().find("CPU trace is OFF"), std::string::npos);
}

// ---------------------------------------------------------------------------
// BPL / SPR / PF / DISPLAY (debug display masking)
// ---------------------------------------------------------------------------

namespace {

// Backs the display-mask hooks with a plain struct so the commands can be exercised
// without an emulator.
class ArDisplayMaskFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.getDisplayMasks = [this]() { return m_masks; };
        host.setDisplayMasks = [this](const adm::core::DisplayMasks& m) { m_masks = m; };
    }

    adm::core::DisplayMasks m_masks;
};

} // namespace

TEST_F(ArInterpreterFixture, DisplayMaskCommandsReportUnavailableWithoutHooks)
{
    m_interp->ExecuteLine("BPL");
    EXPECT_NE(AllOutput().find("display masking unavailable"), std::string::npos);
}

TEST_F(ArDisplayMaskFixture, NoArgShowsStatus)
{
    m_interp->ExecuteLine("BPL");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("Bitplanes"), std::string::npos);
    EXPECT_NE(out.find("$FF"), std::string::npos);
    EXPECT_NE(out.find("BPL1:on"), std::string::npos);
    EXPECT_NE(out.find("BPL8:on"), std::string::npos);
    // Reading state must not change it.
    EXPECT_EQ(m_masks.bitplanes, 0xFF);
}

TEST_F(ArDisplayMaskFixture, AllAndNoneSetWholeGroup)
{
    m_interp->ExecuteLine("SPR NONE");
    EXPECT_EQ(m_masks.sprites, 0x00);
    EXPECT_EQ(m_masks.bitplanes, 0xFF); // other groups untouched
    m_interp->ExecuteLine("SPR ALL");
    EXPECT_EQ(m_masks.sprites, 0xFF);
}

TEST_F(ArDisplayMaskFixture, IndexTogglesAndExplicitState)
{
    m_interp->ExecuteLine("BPL 3 OFF");
    EXPECT_EQ(m_masks.bitplanes, 0xFF & ~0x04); // BPL3 -> bit 2
    m_interp->ExecuteLine("BPL 3");             // no state -> toggle back on
    EXPECT_EQ(m_masks.bitplanes, 0xFF);
    m_interp->ExecuteLine("SPR 0 OFF");
    EXPECT_EQ(m_masks.sprites, 0xFE); // SPR0 -> bit 0
}

TEST_F(ArDisplayMaskFixture, MaskKeywordSetsRawValue)
{
    m_interp->ExecuteLine("BPL MASK 0F");
    EXPECT_EQ(m_masks.bitplanes, 0x0F);
    m_interp->ExecuteLine("SPR MASK %10000001");
    EXPECT_EQ(m_masks.sprites, 0x81);
    // Playfield masks are 2 bits wide; extra bits are dropped.
    m_interp->ExecuteLine("PF MASK FF");
    EXPECT_EQ(m_masks.playfields, 0x03);
}

TEST_F(ArDisplayMaskFixture, RejectsOutOfRangeIndex)
{
    m_interp->ExecuteLine("PF 3 OFF");
    EXPECT_NE(AllOutput().find("Usage: PF"), std::string::npos);
    EXPECT_EQ(m_masks.playfields, 0x03); // unchanged
}

TEST_F(ArDisplayMaskFixture, HidingPlayfieldReportsEffectiveBitplaneMask)
{
    m_interp->ExecuteLine("PF 2 OFF");
    EXPECT_EQ(m_masks.playfields, 0x01);
    // PF2 is drawn from BPL2/4/6, so those planes drop out of the composed mask.
    EXPECT_NE(AllOutput().find("Effective bitplane mask $D5"), std::string::npos);
}

TEST_F(ArDisplayMaskFixture, DisplayShowsAllGroupsAndResets)
{
    m_masks = {0x0F, 0x00, 0x01};
    m_interp->ExecuteLine("DISPLAY");
    std::string out = AllOutput();
    EXPECT_NE(out.find("Bitplanes"), std::string::npos);
    EXPECT_NE(out.find("Sprites"), std::string::npos);
    EXPECT_NE(out.find("Playfields"), std::string::npos);
    EXPECT_NE(out.find("SPR0:OFF"), std::string::npos);

    m_interp->ExecuteLine("DISPLAY ALL");
    EXPECT_EQ(m_masks.bitplanes, 0xFF);
    EXPECT_EQ(m_masks.sprites, 0xFF);
    EXPECT_EQ(m_masks.playfields, 0x03);

    m_interp->ExecuteLine("DISPLAY NONE");
    EXPECT_EQ(m_masks.bitplanes, 0x00);
    EXPECT_EQ(m_masks.sprites, 0x00);
    EXPECT_EQ(m_masks.playfields, 0x00);
}

// ---------------------------------------------------------------------------
// BLIT (blitter operations log)
// ---------------------------------------------------------------------------

namespace {

// Wires a stub blitter-log source onto the host and records the requested limit.
class ArBlitLogFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.getBlitLog = [this](int limit) {
            m_lastLimit = limit;
            return m_snapshot;
        };
    }

    adm::core::BlitLogSnapshot m_snapshot;
    int m_lastLimit = -999;
};

} // namespace

TEST_F(ArBlitLogFixture, ReportsUnavailableWhenNotRunning)
{
    m_snapshot.available = false;
    m_interp->ExecuteLine("BLIT");
    EXPECT_NE(AllOutput().find("No blitter data"), std::string::npos);
}

TEST_F(ArBlitLogFixture, ReportsNoBlitsForEmptyFrame)
{
    m_snapshot.available = true;
    m_snapshot.totalBlits = 0;
    m_interp->ExecuteLine("BLIT");
    EXPECT_NE(AllOutput().find("No blits this frame"), std::string::npos);
}

TEST_F(ArBlitLogFixture, DumpsBlitRowWithChannels)
{
    m_snapshot.available = true;
    m_snapshot.totalBlits = 1;
    m_snapshot.cpuBlits = 1;
    m_snapshot.totalDmaCycles = 320;
    m_snapshot.busPercent = 0.45f;
    adm::core::BlitLogEntry e;
    e.startCycle = 0x1A2B;
    e.startLine = 100;
    e.startClock = 40;
    e.fromCopper = false;
    e.op = "copy";
    e.widthPx = 160;
    e.rows = 32;
    e.minterm = 0xCA;
    e.dmaCycles = 320;
    e.useA = true;
    e.aptr = 0x012345;
    e.amod = 0;
    e.useD = true;
    e.dptr = 0x0ABCDE;
    e.dmod = -2;
    m_snapshot.blits.push_back(e);

    m_interp->ExecuteLine("BLIT");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("Blitter log - 1 blits"), std::string::npos);
    EXPECT_NE(out.find("L0001A2B"), std::string::npos);
    EXPECT_NE(out.find("copy"), std::string::npos);
    EXPECT_NE(out.find("A:$012345"), std::string::npos);
    EXPECT_NE(out.find("D:$0ABCDE(-2)"), std::string::npos);
    // B/C unused -> not listed.
    EXPECT_EQ(out.find("B:$"), std::string::npos);
}

TEST_F(ArBlitLogFixture, NumericArgSetsLimitAllClearsIt)
{
    m_snapshot.available = true;
    m_snapshot.totalBlits = 0; // content irrelevant; we assert the plumbed limit
    m_interp->ExecuteLine("BLIT 5");
    EXPECT_EQ(m_lastLimit, 5);
    m_interp->ExecuteLine("BLIT ALL");
    EXPECT_EQ(m_lastLimit, 0);
}

TEST_F(ArBlitLogFixture, TruncationHintShown)
{
    m_snapshot.available = true;
    m_snapshot.totalBlits = 40;
    m_snapshot.truncated = true;
    adm::core::BlitLogEntry e;
    e.op = "-";
    m_snapshot.blits.push_back(e);
    m_interp->ExecuteLine("BLIT 1");
    EXPECT_NE(AllOutput().find("BLIT ALL"), std::string::npos);
}

// ---------------------------------------------------------------------------
// DSKLOG / BDSK (disk DMA log + breakpoint)
// ---------------------------------------------------------------------------

namespace {

// Wires stub disk-log and disk-breakpoint sources and records what was requested.
class ArDiskLogFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.getDiskLog = [this](const adm::core::DiskLogQuery& q) {
            m_lastQuery = q;
            return m_snapshot;
        };
        host.armDiskBreak = [this](int access, int cyl, int side, int sector, uint32_t lo,
                                   uint32_t hi) {
            m_access = access;
            m_cyl = cyl;
            m_side = side;
            m_sector = sector;
            m_addrLo = lo;
            m_addrHi = hi;
            return true;
        };
    }

    // A single decoded-track write transfer, enough to exercise the row formatting.
    void PushWriteTransfer()
    {
        adm::core::DiskLogEntry e;
        e.seq = 7;
        e.frame = 12;
        e.direction = "write";
        e.trigger = "cpu";
        e.pc = 0x012345;
        e.drive = 0;
        e.cyl = 40;
        e.side = 1;
        e.dskpt = 0x0ABCDE;
        e.dsklen = 0x9C00;
        e.bytes = 5632;
        e.sha1 = "0123456789abcdef";
        e.standardTrack = true;
        e.sectorCount = 11;
        e.sectorsChecksumOk = 11;
        m_snapshot.transfers.push_back(e);
    }

    adm::core::DiskLogSnapshot m_snapshot;
    adm::core::DiskLogQuery m_lastQuery;
    int m_access = -99, m_cyl = -99, m_side = -99, m_sector = -99;
    uint32_t m_addrLo = 0xFFFFFFFF, m_addrHi = 0xFFFFFFFF;
};

} // namespace

TEST_F(ArDiskLogFixture, ReportsUnavailableWhenNotRunning)
{
    m_snapshot.available = false;
    m_interp->ExecuteLine("DSKLOG");
    EXPECT_NE(AllOutput().find("No disk data"), std::string::npos);
}

TEST_F(ArDiskLogFixture, ReportsNoTransfersForEmptySession)
{
    m_snapshot.available = true;
    m_snapshot.totalTransfers = 0;
    m_interp->ExecuteLine("DSKLOG");
    EXPECT_NE(AllOutput().find("No disk transfers"), std::string::npos);
}

TEST_F(ArDiskLogFixture, DumpsTransferRow)
{
    m_snapshot.available = true;
    m_snapshot.totalTransfers = 1;
    m_snapshot.writes = 1;
    m_snapshot.bytesWritten = 5632;
    m_snapshot.anyWrites = true;
    PushWriteTransfer();

    m_interp->ExecuteLine("DSKLOG");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("Disk log - 1 transfers"), std::string::npos);
    // The headline finding gets its own line.
    EXPECT_NE(out.find("wrote back to the disk"), std::string::npos);
    EXPECT_NE(out.find("write"), std::string::npos);
    EXPECT_NE(out.find("012345"), std::string::npos); // arming PC
    EXPECT_NE(out.find("$0ABCDE"), std::string::npos);
    // A write that never reached the ADF is called out.
    EXPECT_NE(out.find("not persisted"), std::string::npos);
}

TEST_F(ArDiskLogFixture, NoWriteBannerWhenSessionIsReadOnly)
{
    m_snapshot.available = true;
    m_snapshot.totalTransfers = 1;
    m_snapshot.reads = 1;
    m_snapshot.anyWrites = false;
    adm::core::DiskLogEntry e;
    e.direction = "read";
    e.trigger = "cpu";
    m_snapshot.transfers.push_back(e);

    m_interp->ExecuteLine("DSKLOG");
    EXPECT_EQ(AllOutput().find("wrote back to the disk"), std::string::npos);
}

TEST_F(ArDiskLogFixture, NumericArgSetsLimitAllClearsItAndDirectionFilters)
{
    m_snapshot.available = true;
    m_interp->ExecuteLine("DSKLOG 5");
    EXPECT_EQ(m_lastQuery.limit, 5);
    EXPECT_EQ(m_lastQuery.direction, 0);

    m_interp->ExecuteLine("DSKLOG ALL");
    EXPECT_EQ(m_lastQuery.limit, 0);

    m_interp->ExecuteLine("DSKLOG ALL WRITE");
    EXPECT_EQ(m_lastQuery.limit, 0);
    EXPECT_EQ(m_lastQuery.direction, 3);

    m_interp->ExecuteLine("DSKLOG 3 READ");
    EXPECT_EQ(m_lastQuery.limit, 3);
    EXPECT_EQ(m_lastQuery.direction, 2);
}

TEST_F(ArDiskLogFixture, TruncationHintShown)
{
    m_snapshot.available = true;
    m_snapshot.totalTransfers = 40;
    m_snapshot.truncated = true;
    PushWriteTransfer();
    m_interp->ExecuteLine("DSKLOG 1");
    EXPECT_NE(AllOutput().find("DSKLOG ALL"), std::string::npos);
}

TEST_F(ArDiskLogFixture, DiskBreakArmsWriteWithTrackAndAddressRange)
{
    m_interp->ExecuteLine("BDSK WRITE track=81 addr=$40000-$50000");
    EXPECT_EQ(m_access, 2);
    EXPECT_EQ(m_cyl, 40);
    EXPECT_EQ(m_side, 1);
    EXPECT_EQ(m_sector, -1);
    EXPECT_EQ(m_addrLo, 0x40000u);
    EXPECT_EQ(m_addrHi, 0x50000u);
    EXPECT_NE(AllOutput().find("Disk break armed on write"), std::string::npos);
}

TEST_F(ArDiskLogFixture, DiskBreakOffDisarms)
{
    m_interp->ExecuteLine("BDSK OFF");
    EXPECT_EQ(m_access, -1);
    EXPECT_NE(AllOutput().find("disarmed"), std::string::npos);
}

TEST_F(ArDiskLogFixture, DiskBreakSectorFilterWarnsAboutCompletionTiming)
{
    m_interp->ExecuteLine("BDSK ANY sector=3");
    EXPECT_EQ(m_access, 0);
    EXPECT_EQ(m_sector, 3);
    EXPECT_NE(AllOutput().find("transfer completion"), std::string::npos);
}

TEST_F(ArDiskLogFixture, DiskBreakRejectsBadArguments)
{
    m_interp->ExecuteLine("BDSK sideways");
    EXPECT_NE(AllOutput().find("Usage: BDSK"), std::string::npos);
    EXPECT_EQ(m_access, -99); // hook never called

    m_interp->ExecuteLine("BDSK WRITE track=400");
    EXPECT_NE(AllOutput().find("track must be 0-159"), std::string::npos);

    m_interp->ExecuteLine("BDSK WRITE addr=$40000");
    EXPECT_NE(AllOutput().find("addr must be a range"), std::string::npos);
}

class ArHardDriveFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        m_configured[0].device = adm::core::HostMountDevice::DH0;
        m_configured[0].volumeLabel = "DH0";
        m_configured[1].device = adm::core::HostMountDevice::DH1;
        m_configured[1].volumeLabel = "DH1";
        m_effective = m_configured;
        host.getConfiguredHostMounts = [this]() { return m_configured; };
        host.getEffectiveHostMounts = [this]() { return m_effective; };
        host.setHostMount = [this](const adm::core::HostDirectoryMount& mount, std::string&) {
            m_configured[static_cast<size_t>(mount.device)] = mount;
            return true;
        };
        host.applyHostMounts = [this](std::string&) {
            m_effective = m_configured;
            m_applied = true;
            return true;
        };
        host.openHostMount = [this](adm::core::HostMountDevice device) {
            m_opened = device;
            return true;
        };
        host.refreshHostMount = [this](adm::core::HostMountDevice device) {
            m_refreshed = device;
            return true;
        };
        host.getOpenDocuments = []() { return std::vector<std::string>{"disk.adf", "C:/Work"}; };
        host.getOpenDocumentInfo = []() {
            return std::vector<ArHost::DocumentInfo>{
                {"disk.adf", "ADF", std::nullopt},
                {"C:/Work", "HostDirectory", adm::core::HostMountDevice::DH1}};
        };
        host.isRunning = []() { return false; };
        host.startEmulator = [this]() { m_started = true; };
        host.getHardDriveControllerRoms = [this]() { return m_roms; };
        host.setHardDriveControllerRoms = [this](const adm::core::HardDriveControllerRoms& roms,
                                                 std::string&) {
            m_roms = roms;
            return true;
        };
        host.getModelId = []() { return std::string("A500"); };
        host.launchWhdload = [this](const std::string& path, std::string&) {
            m_whdloadLaunched = path;
            return true;
        };
        host.getWhdloadStatus = [this]() -> std::optional<ArHost::WhdloadStatus> {
            if (m_whdloadLaunched.empty())
                return std::nullopt;
            return ArHost::WhdloadStatus{"Super Frog", m_whdloadLaunched, true};
        };
        host.getWhdloadSettings = [this]() { return m_whdloadSettings; };
        host.setWhdloadSettings = [this](const adm::core::WhdloadSettings& settings) {
            m_whdloadSettings = settings;
        };
        host.endWhdload = [this](std::string&) {
            m_applied = true;
            return true;
        };
        host.getCdStatus = [this]() { return m_cd; };
        host.insertCd = [this](const std::string& path, std::string&) {
            m_cd.source = path;
            m_cd.extracting = true;
            return true;
        };
        host.ejectCd = [this]() { m_cd = {}; };
        host.listModels = []() {
            return std::vector<ArHost::ModelInfo>{
                {"A500", "Amiga 500", true, ""},
                {"A1200", "Amiga 1200", true, ""},
                {"CD32", "Amiga CD32", false, "needs kick40060.CD32"}};
        };
        host.selectModel = [this](const std::string& id, std::string&) {
            m_selectedModel = id;
            return true;
        };
        host.getMachineStatus = [this]() { return m_machine; };
        host.setMachineSetting = [this](const std::string& key, const std::string& value) {
            m_machine.settings[key] = value;
        };
        host.restartMachine = [this](std::string&) {
            m_machineRestarted = true;
            return true;
        };
        host.getHostPorts = [this]() { return m_ports; };
        host.setSerialPort = [this](const adm::core::SerialPortTarget& target) {
            m_ports.serial = target;
        };
        host.setParallelOutput = [this](const std::string& path) { m_ports.parallelPath = path; };
        host.getMidi = [this]() { return m_midi; };
        host.getNetwork = [this]() { return m_network; };
        host.getSoundCard = [this]() { return m_soundCard; };
        host.setSoundCard = [this](const std::string& board) { m_soundCard.board = board; };
        host.setNetworkSetting = [this](const std::string& key, const std::string& value) {
            if (key == "puae_bsdsocket")
                m_network.bsdsocket = value == "enabled";
            else if (key == "puae_a2065")
                m_network.a2065 = value;
            else if (key == "puae_sana2")
                m_network.uaenet = value == "enabled";
        };
        host.setMidiDevices = [this](int out, int in) {
            m_midi.outDevice = out;
            m_midi.inDevice = in;
        };
    }

    std::string m_selectedModel;
    ArHost::MachineStatus m_machine;
    bool m_machineRestarted = false;
    ArHost::HostPortsInfo m_ports;
    ArHost::MidiInfo m_midi;
    ArHost::NetworkInfo m_network;
    ArHost::SoundCardInfo m_soundCard;
    std::string m_whdloadLaunched;
    ArHost::CdStatus m_cd;
    adm::core::WhdloadSettings m_whdloadSettings;

    adm::core::HardDriveControllerRoms m_roms;
    std::array<adm::core::HostDirectoryMount, 2> m_configured;
    std::array<adm::core::HostDirectoryMount, 2> m_effective;
    bool m_applied = false;
    bool m_started = false;
    std::optional<adm::core::HostMountDevice> m_opened;
    std::optional<adm::core::HostMountDevice> m_refreshed;
};

TEST_F(ArHardDriveFixture, ParsesQuotedMountOptionsAndAppliesExplicitly)
{
    m_interp->ExecuteLine("HD MOUNT DH0 \"C:/Work Dir\" RW LABEL \"System Disk\" PRI -5");
    const auto& mount = m_configured[0];
    EXPECT_TRUE(mount.enabled);
    EXPECT_EQ(mount.hostDirectory, std::filesystem::path("C:/Work Dir"));
    EXPECT_FALSE(mount.readOnly);
    EXPECT_EQ(mount.volumeLabel, "System Disk");
    EXPECT_EQ(mount.bootPriority, -5);
    EXPECT_FALSE(m_applied);
    EXPECT_NE(AllOutput().find("HD APPLY"), std::string::npos);

    m_interp->ExecuteLine("HD APPLY");
    EXPECT_TRUE(m_applied);
    EXPECT_EQ(m_effective[0], m_configured[0]);
}

TEST_F(ArHardDriveFixture, SetUnmountOpenRefreshAndListingUseSharedCallbacks)
{
    m_interp->ExecuteLine("HD MOUNT DH1 C:/Work RO");
    m_interp->ExecuteLine("HD SET DH1 PRI 10");
    EXPECT_EQ(m_configured[1].bootPriority, 10);
    m_interp->ExecuteLine("HD OPEN DH1");
    m_interp->ExecuteLine("HD REFRESH DH1");
    EXPECT_EQ(m_opened, adm::core::HostMountDevice::DH1);
    EXPECT_EQ(m_refreshed, adm::core::HostMountDevice::DH1);

    m_interp->ExecuteLine("HD");
    EXPECT_NE(AllOutput().find("restart pending"), std::string::npos);
    m_interp->ExecuteLine("DOCS");
    EXPECT_NE(AllOutput().find("HostDirectory"), std::string::npos);
    EXPECT_NE(AllOutput().find("DH1"), std::string::npos);

    m_interp->ExecuteLine("HD UNMOUNT DH1");
    EXPECT_FALSE(m_configured[1].enabled);
}

TEST_F(ArHardDriveFixture, RejectsInvalidSlotPriorityAndOptions)
{
    m_interp->ExecuteLine("HD MOUNT DH2 path");
    EXPECT_NE(AllOutput().find("device must be DH0 or DH1"), std::string::npos);
    m_interp->ExecuteLine("HD SET DH0 PRI -129");
    EXPECT_NE(AllOutput().find("-128 through 127"), std::string::npos);
    m_interp->ExecuteLine("HD SET DH0 MAGIC");
    EXPECT_NE(AllOutput().find("expected RO"), std::string::npos);
}

TEST_F(ArHardDriveFixture, MountsHardfilesWithControllerUnitAndGeometry)
{
    m_interp->ExecuteLine(
        "HD MOUNT DH0 \"C:/images/work bench.hdf\" RW CTRL IDE UNIT 1 GEOM 63/16/2/512 PRI 3");
    const auto& mount = m_configured[0];
    EXPECT_TRUE(mount.enabled);
    EXPECT_TRUE(mount.IsHardfile());
    EXPECT_EQ(mount.hardfilePath, std::filesystem::u8path("C:/images/work bench.hdf"));
    EXPECT_EQ(mount.controller, adm::core::HardDriveController::IdeMainboard);
    EXPECT_EQ(mount.controllerUnit, 1);
    EXPECT_EQ(mount.geometry.mode, adm::core::HardfileGeometryMode::Manual);
    EXPECT_EQ(mount.geometry.sectors, 63);
    EXPECT_EQ(mount.geometry.surfaces, 16);
    EXPECT_EQ(mount.geometry.reserved, 2);
    EXPECT_EQ(mount.geometry.blockSize, 512);
    EXPECT_EQ(mount.bootPriority, 3);
    EXPECT_FALSE(mount.readOnly);

    // A500 has no built-in IDE: the change is accepted but the console says why it will not show.
    EXPECT_NE(AllOutput().find("will not appear"), std::string::npos);

    m_interp->ExecuteLine("HD SET DH0 CTRL A590 GEOM RDB");
    EXPECT_EQ(m_configured[0].controller, adm::core::HardDriveController::ScsiA590A2091);
    EXPECT_EQ(m_configured[0].controllerUnit, 1);
    EXPECT_EQ(m_configured[0].geometry.mode, adm::core::HardfileGeometryMode::Rdb);

    m_interp->ExecuteLine("HD");
    EXPECT_NE(AllOutput().find("HDF RW a2091 unit 1 GEOM rdb"), std::string::npos) << AllOutput();

    // A directory mount on the other slot is still a directory.
    m_interp->ExecuteLine("HD MOUNT DH1 C:/Work");
    EXPECT_FALSE(m_configured[1].IsHardfile());
}

TEST_F(ArHardDriveFixture, RejectsBadControllerUnitAndGeometry)
{
    m_interp->ExecuteLine("HD MOUNT DH0 disk.hdf CTRL SATA");
    EXPECT_NE(AllOutput().find("controller must be"), std::string::npos);
    m_interp->ExecuteLine("HD MOUNT DH0 disk.hdf UNIT 9");
    EXPECT_NE(AllOutput().find("unit must be 0 through 6"), std::string::npos);
    m_interp->ExecuteLine("HD MOUNT DH0 disk.hdf GEOM 32/1/2");
    EXPECT_NE(AllOutput().find("GEOM is AUTO, RDB"), std::string::npos);
    EXPECT_FALSE(m_configured[0].enabled);
}

TEST_F(ArHardDriveFixture, SetsAndClearsSCSICardRoms)
{
    m_interp->ExecuteLine("HD ROM A2091 \"C:/roms/amiga-boot-a590.rom\"");
    EXPECT_EQ(m_roms.a590A2091, std::filesystem::u8path("C:/roms/amiga-boot-a590.rom"));
    m_interp->ExecuteLine("HD ROM A4091 C:/roms/a4091.rom");
    EXPECT_EQ(m_roms.a4091, std::filesystem::u8path("C:/roms/a4091.rom"));
    m_interp->ExecuteLine("HD ROM");
    EXPECT_NE(AllOutput().find("amiga-boot-a590.rom"), std::string::npos);
    m_interp->ExecuteLine("HD ROM A590 CLEAR");
    EXPECT_TRUE(m_roms.a590A2091.empty());
    m_interp->ExecuteLine("HD ROM IDE C:/x.rom");
    EXPECT_NE(AllOutput().find("Usage: HD ROM"), std::string::npos);
}

TEST_F(ArHardDriveFixture, InfoPrintsTheRdbPartitionTable)
{
    const auto path =
        std::filesystem::temp_directory_path() /
        ("peekaboo_hd_info_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".hdf");
    {
        // RDSK at block 0 -> PART "DH0" at block 1, cylinders 2-9, one surface, 32 sectors.
        std::vector<uint8_t> image(16 * 32 * 512, 0);
        const auto put = [&](size_t offset, uint32_t value) {
            image[offset] = static_cast<uint8_t>(value >> 24);
            image[offset + 1] = static_cast<uint8_t>(value >> 16);
            image[offset + 2] = static_cast<uint8_t>(value >> 8);
            image[offset + 3] = static_cast<uint8_t>(value);
        };
        put(0, 0x5244534B);
        put(4, 64);
        put(0x10, 512);
        put(0x1C, 1);
        put(0x20, 0xFFFFFFFF);
        put(0x40, 16);
        put(0x44, 32);
        put(0x48, 1);
        put(512, 0x50415254);
        put(512 + 4, 64);
        put(512 + 0x10, 0xFFFFFFFF);
        put(512 + 0x14, 1);
        image[512 + 0x24] = 3;
        image[512 + 0x25] = 'D';
        image[512 + 0x26] = 'H';
        image[512 + 0x27] = '0';
        put(512 + 0x80 + 3 * 4, 1);
        put(512 + 0x80 + 5 * 4, 32);
        put(512 + 0x80 + 9 * 4, 2);
        put(512 + 0x80 + 10 * 4, 9);
        put(512 + 0x80 + 16 * 4, 0x444F5301);
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(image.data()),
                   static_cast<std::streamsize>(image.size()));
    }
    m_interp->ExecuteLine("HD INFO \"" + path.generic_string() + "\"");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("16 cylinders x 1 heads x 32 sectors"), std::string::npos) << out;
    EXPECT_NE(out.find("DH0"), std::string::npos);
    EXPECT_NE(out.find("DOS1"), std::string::npos);
    EXPECT_NE(out.find("BAD CHECKSUM"), std::string::npos); // no checksum was written

    m_interp->ExecuteLine("HD INFO DH1");
    EXPECT_NE(AllOutput().find("DH1 is not set to a hardfile"), std::string::npos);
    std::filesystem::remove(path);
}

TEST_F(ArHardDriveFixture, ModelListsAndSelectsPresets)
{
    std::string selected;
    m_interp->ExecuteLine("MODEL");
    EXPECT_NE(AllOutput().find("* A500"), std::string::npos) << AllOutput();
    EXPECT_NE(AllOutput().find("unavailable: needs kick40060.CD32"), std::string::npos);

    m_interp->ExecuteLine("MODEL a1200");
    EXPECT_EQ(m_selectedModel, "A1200");
    m_interp->ExecuteLine("MODEL CD32");
    EXPECT_NE(AllOutput().find("CD32 is unavailable"), std::string::npos);
    EXPECT_EQ(m_selectedModel, "A1200");
    m_interp->ExecuteLine("MODEL A9000");
    EXPECT_NE(AllOutput().find("unknown machine"), std::string::npos);
}

TEST_F(ArHardDriveFixture, WhdLaunchesShowsStatusAndChangesSettings)
{
    m_interp->ExecuteLine("WHD");
    EXPECT_NE(AllOutput().find("Running    - (WHD <archive> to launch)"), std::string::npos);
    EXPECT_NE(AllOutput().find("NOWRITECACHE ON"), std::string::npos);

    m_interp->ExecuteLine("WHD \"C:/games/Super Frog.lha\"");
    EXPECT_EQ(m_whdloadLaunched, "C:/games/Super Frog.lha");
    m_interp->ExecuteLine("WHD");
    EXPECT_NE(AllOutput().find("Running    'Super Frog' on DH0 (archive)"), std::string::npos)
        << AllOutput();

    m_interp->ExecuteLine("WHD SET BUTTONWAIT OFF");
    m_interp->ExecuteLine("WHD SET PREFS SPLASH");
    m_interp->ExecuteLine("WHD SET THEME NATIVE");
    EXPECT_FALSE(m_whdloadSettings.buttonWait);
    EXPECT_EQ(m_whdloadSettings.prefsMode, 2);
    EXPECT_EQ(m_whdloadSettings.theme, 1);
    m_interp->ExecuteLine("WHD SET PREFS LOUD");
    EXPECT_NE(AllOutput().find("expected BUTTONWAIT"), std::string::npos);

    m_interp->ExecuteLine("WHD OFF");
    EXPECT_TRUE(m_applied);
}

TEST_F(ArHardDriveFixture, CdShowsInsertsAndEjects)
{
    m_interp->ExecuteLine("CD");
    EXPECT_NE(AllOutput().find("Disc      -"), std::string::npos) << AllOutput();
    EXPECT_NE(AllOutput().find("powered off"), std::string::npos);

    m_interp->ExecuteLine("CD INSERT \"C:/cd32/Chuck Rock II.7z\"");
    EXPECT_EQ(m_cd.source, "C:/cd32/Chuck Rock II.7z");
    EXPECT_NE(AllOutput().find("Extracting the archive"), std::string::npos);
    m_interp->ExecuteLine("CD");
    EXPECT_NE(AllOutput().find("extracting the archive"), std::string::npos);

    m_interp->ExecuteLine("CD EJECT");
    EXPECT_TRUE(m_cd.source.empty());
    m_interp->ExecuteLine("CD SPIN");
    EXPECT_NE(AllOutput().find("Usage: CD"), std::string::npos);
}

TEST_F(ArHardDriveFixture, MachineShowsChangesAndAppliesChipsetSettings)
{
    m_interp->ExecuteLine("MACHINE");
    EXPECT_NE(AllOutput().find("no chipset options"), std::string::npos) << AllOutput();

    m_machine.supported = true;
    adm::core::MachineConfig running;
    running.compatible = 5; // A500
    running.cpuModel = 68000;
    m_machine.running = running;
    m_interp->ExecuteLine("MACHINE");
    EXPECT_NE(AllOutput().find("Chipset extra  EXTRA      auto           A500"), std::string::npos)
        << AllOutput();
    EXPECT_NE(AllOutput().find("MMU None, 24-bit"), std::string::npos) << AllOutput();

    m_interp->ExecuteLine("MACHINE denise a1000_NOEHB");
    EXPECT_EQ(m_machine.settings["puae_denise_model"], "a1000_noehb");
    EXPECT_NE(AllOutput().find("next power-on"), std::string::npos);
    m_interp->ExecuteLine("MACHINE EXTRA A500+");
    EXPECT_EQ(m_machine.settings["puae_chipset_compatible"], "A500+");
    m_interp->ExecuteLine("MACHINE MMU ec");
    EXPECT_EQ(m_machine.settings["puae_mmu_model"], "ec");
    EXPECT_NE(AllOutput().find("MMU set to EC (No Translation)"), std::string::npos) << AllOutput();

    m_interp->ExecuteLine("MACHINE CHIPSET SUPERAGA");
    EXPECT_NE(AllOutput().find("CHIPSET takes AUTO|OCS|ECS_AGNUS"), std::string::npos)
        << AllOutput();
    m_interp->ExecuteLine("MACHINE PAULA 8364");
    EXPECT_NE(AllOutput().find("Usage: MACHINE"), std::string::npos);

    m_interp->ExecuteLine("MACHINE DEFAULTS");
    EXPECT_EQ(m_machine.settings["puae_denise_model"], "auto");
    EXPECT_EQ(m_machine.settings["puae_chipset_compatible"], "auto");
    m_interp->ExecuteLine("MACHINE APPLY");
    EXPECT_TRUE(m_machineRestarted);
}

TEST_F(ArHardDriveFixture, SerialAndParallelPortsConnectAndReport)
{
    m_interp->ExecuteLine("SERIAL");
    EXPECT_NE(AllOutput().find("no host serial port"), std::string::npos) << AllOutput();

    m_ports.supported = true;
    m_ports.devices = {"COM3"};
    m_interp->ExecuteLine("SERIAL LISTEN 4000"); // APP-08: this computer only
    EXPECT_EQ(m_ports.serial.ToCoreName(), "TCP://127.0.0.1:4000");
    EXPECT_NE(AllOutput().find("opens when the machine runs"), std::string::npos) << AllOutput();
    EXPECT_EQ(AllOutput().find("other machines on the network"), std::string::npos);
    m_interp->ExecuteLine("SERIAL LISTEN 0.0.0.0:4000"); // asked for by its address, and said
    EXPECT_EQ(m_ports.serial.ToCoreName(), "TCP://0.0.0.0:4000");
    EXPECT_NE(AllOutput().find("other machines on the network"), std::string::npos) << AllOutput();

    m_interp->ExecuteLine("SERIAL CONNECT 10.0.0.2:23");
    EXPECT_EQ(m_ports.serial.ToCoreName(), "TCP://10.0.0.2:23/connect");

    adm::core::HostPortsStatus status;
    status.serialMode = adm::core::SerialPortTarget::Mode::TcpConnect;
    status.serialOpen = true;
    status.serialConnected = true;
    status.serialPeer = "10.0.0.2:23";
    status.serialTx = 5;
    m_ports.status = status;
    m_interp->ExecuteLine("SERIAL");
    EXPECT_NE(AllOutput().find("connected to 10.0.0.2:23 - 5 out, 0 in"), std::string::npos)
        << AllOutput();
    EXPECT_NE(AllOutput().find("Devices  COM3"), std::string::npos);

    m_interp->ExecuteLine("SERIAL COM3");
    EXPECT_EQ(m_ports.serial.mode, adm::core::SerialPortTarget::Mode::Device);
    m_interp->ExecuteLine("SERIAL TCP://host:99999");
    EXPECT_NE(AllOutput().find("Usage: SERIAL"), std::string::npos);
    m_interp->ExecuteLine("SERIAL OFF");
    EXPECT_EQ(m_ports.serial.mode, adm::core::SerialPortTarget::Mode::Off);

    m_interp->ExecuteLine("PARALLEL C:/capture/printer.prn");
    EXPECT_EQ(m_ports.parallelPath, "C:/capture/printer.prn");
    m_interp->ExecuteLine("PARALLEL OFF");
    EXPECT_TRUE(m_ports.parallelPath.empty());
}

TEST_F(ArHardDriveFixture, MidiChoosesAndReportsHostDevices)
{
    m_interp->ExecuteLine("MIDI");
    EXPECT_NE(AllOutput().find("no host MIDI"), std::string::npos) << AllOutput();

    m_midi.supported = true;
    m_midi.outputs = {{0, "GS Synth"}, {1, "Loopback"}};
    m_midi.inputs = {{0, "Loopback"}};
    m_interp->ExecuteLine("MIDI OUT 1 IN 0");
    EXPECT_EQ(m_midi.outDevice, 1);
    EXPECT_EQ(m_midi.inDevice, 0);
    EXPECT_NE(AllOutput().find("MIDI out 1 (Loopback), in 0 (Loopback)."), std::string::npos)
        << AllOutput();

    adm::core::HostPortsStatus status;
    status.hasMidi = true;
    status.midiActive = true;
    status.midiOutMessages = 7;
    m_midi.status = status;
    m_interp->ExecuteLine("MIDI");
    EXPECT_NE(AllOutput().find("active - 7 messages out, 0 bytes in"), std::string::npos)
        << AllOutput();

    m_interp->ExecuteLine("MIDI OUT MAPPER");
    EXPECT_EQ(m_midi.outDevice, -1);
    EXPECT_EQ(m_midi.inDevice, 0);
    m_interp->ExecuteLine("MIDI OUT NONE IN NONE");
    EXPECT_EQ(m_midi.outDevice, -2);
    EXPECT_EQ(m_midi.inDevice, -1);
    const auto usages = [&] {
        size_t count = 0;
        const std::string output = AllOutput();
        for (auto pos = output.find("Usage: MIDI"); pos != std::string::npos;
             pos = output.find("Usage: MIDI", pos + 1))
            ++count;
        return count;
    };
    m_interp->ExecuteLine("MIDI OUT");
    EXPECT_EQ(usages(), 1u);
    m_interp->ExecuteLine("MIDI THRU 3");
    EXPECT_EQ(usages(), 2u);
    // APP-03: all digits, but past int: usage, not std::stoi's out_of_range
    EXPECT_NO_THROW(m_interp->ExecuteLine("MIDI OUT 99999999999"));
    EXPECT_EQ(usages(), 3u);
    m_interp->ExecuteLine("MIDI OUT 256");
    EXPECT_EQ(usages(), 4u);
    EXPECT_EQ(m_midi.outDevice, -2);
}

TEST_F(ArHardDriveFixture, NetworkTogglesBsdsocketForTheNextPowerOn)
{
    m_interp->ExecuteLine("NETWORK");
    EXPECT_NE(AllOutput().find("no networking"), std::string::npos) << AllOutput();

    m_network.supported = true;
    m_interp->ExecuteLine("NETWORK bsdsocket on");
    EXPECT_TRUE(m_network.bsdsocket);
    EXPECT_NE(AllOutput().find("next power-on"), std::string::npos);
    m_interp->ExecuteLine("NETWORK");
    EXPECT_NE(AllOutput().find("bsdsocket.library  on"), std::string::npos) << AllOutput();
    m_interp->ExecuteLine("NETWORK APPLY");
    EXPECT_TRUE(m_machineRestarted);
    m_interp->ExecuteLine("NETWORK BSDSOCKET MAYBE");
    EXPECT_NE(AllOutput().find("Usage: NETWORK"), std::string::npos);

    // A core without the card says so rather than accepting a setting it would ignore.
    m_interp->ExecuteLine("NETWORK A2065 NAT");
    EXPECT_EQ(m_network.a2065, "disabled");
    EXPECT_NE(AllOutput().find("no A2065"), std::string::npos) << AllOutput();
}

TEST_F(ArHardDriveFixture, SoundCardPicksTheBoardForTheNextPowerOn)
{
    m_interp->ExecuteLine("SOUNDCARD TOCCATA");
    EXPECT_NE(AllOutput().find("no sound cards"), std::string::npos) << AllOutput();
    EXPECT_EQ(m_soundCard.board, "disabled");

    m_soundCard.supported = true;
    m_interp->ExecuteLine("SOUNDCARD uaesnd");
    EXPECT_EQ(m_soundCard.board, "uaesnd_z2");
    EXPECT_NE(AllOutput().find("next power-on"), std::string::npos);
    m_interp->ExecuteLine("SOUNDCARD");
    EXPECT_NE(AllOutput().find("  UAESND"), std::string::npos) << AllOutput();
    m_interp->ExecuteLine("SOUNDCARD PRELUDE1200");
    EXPECT_EQ(m_soundCard.board, "prelude1200");
    m_interp->ExecuteLine("SOUNDCARD APPLY");
    EXPECT_TRUE(m_machineRestarted);
    m_interp->ExecuteLine("SOUNDCARD OFF");
    EXPECT_EQ(m_soundCard.board, "disabled");
    m_interp->ExecuteLine("SOUNDCARD GUS");
    EXPECT_NE(AllOutput().find("Usage: SOUNDCARD"), std::string::npos);
}

TEST_F(ArHardDriveFixture, NetworkSetsTheA2065AndUaenetDevice)
{
    m_network.supported = true;
    m_network.cardSupported = true;
    m_interp->ExecuteLine("NETWORK A2065 open");
    EXPECT_EQ(m_network.a2065, "slirp_inbound");
    m_interp->ExecuteLine("NETWORK A2065 NAT");
    EXPECT_EQ(m_network.a2065, "slirp");
    m_interp->ExecuteLine("NETWORK UAENET ON");
    EXPECT_TRUE(m_network.uaenet);
    m_interp->ExecuteLine("NETWORK");
    EXPECT_NE(AllOutput().find("A2065              NAT"), std::string::npos) << AllOutput();
    EXPECT_NE(AllOutput().find("uaenet.device      on"), std::string::npos) << AllOutput();
    m_interp->ExecuteLine("NETWORK A2065 OFF");
    EXPECT_EQ(m_network.a2065, "disabled");
}

TEST_F(ArHardDriveFixture, StartUsesTheEffectiveDirectoryProfileWithoutAnAdf)
{
    m_configured[0].enabled = true;
    m_effective = m_configured;
    m_interp->ExecuteLine("START");
    EXPECT_TRUE(m_started);
    EXPECT_NE(AllOutput().find("Starting emulator"), std::string::npos);
}

// ---------------------------------------------------------------------------
// STATE — machine snapshots
// ---------------------------------------------------------------------------

namespace {

class ArStateFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.getFrameCount = [this]() { return m_frame; };
        host.saveState = [this](const std::string& name, bool persist,
                                std::string& outWarning) -> std::string {
            m_savedName = name;
            m_savedPersist = persist;
            outWarning = m_saveWarning;
            return m_saveError;
        };
        host.loadState = [this](const std::string& name) -> std::string {
            m_loadedName = name;
            return m_loadError;
        };
        host.deleteState = [this](const std::string& name) {
            m_deletedName = name;
            return m_deleteOk;
        };
        host.listStates = [this]() { return m_states; };
    }

    uint64_t m_frame = 4242;
    std::vector<ArHost::StateEntry> m_states;
    std::string m_savedName, m_loadedName, m_deletedName;
    bool m_savedPersist = false;
    bool m_deleteOk = true;
    std::string m_saveError, m_loadError, m_saveWarning;
};

} // namespace

TEST_F(ArStateFixture, ListsSnapshotsWithTheirFrameAndStorage)
{
    m_states.push_back({"boot", 1536 * 1024, 900, true, "2026-08-14 20:58:03"});
    m_states.push_back({"menu", 512, 12, false, "2026-08-15 09:00:00"});
    m_interp->ExecuteLine("STATE");

    const std::string out = AllOutput();
    EXPECT_NE(out.find("boot"), std::string::npos);
    EXPECT_NE(out.find("900"), std::string::npos);
    EXPECT_NE(out.find("1.5M"), std::string::npos);
    EXPECT_NE(out.find("disk"), std::string::npos);
    EXPECT_NE(out.find("menu"), std::string::npos);
    EXPECT_NE(out.find("memory"), std::string::npos);
}

TEST_F(ArStateFixture, ListsTheSameWayWithTheExplicitVerbAsWithNoArguments)
{
    m_states.push_back({"boot", 1536 * 1024, 900, true, "2026-08-14 20:58:03"});

    m_interp->ExecuteLine("STATE");
    const auto bare = m_output;
    m_output.clear();
    m_outputLines.clear();

    m_interp->ExecuteLine("STATE LIST");
    EXPECT_EQ(m_output, bare);
}

TEST_F(ArStateFixture, ListRejectsTrailingArguments)
{
    // STATE LIST takes no name; silently ignoring one would hide a mistyped LOAD.
    m_interp->ExecuteLine("STATE LIST boot");
    EXPECT_NE(AllOutput().find("Usage: STATE LIST"), std::string::npos);
}

TEST_F(ArStateFixture, SaysSoWhenThereAreNoSnapshots)
{
    m_interp->ExecuteLine("STATE");
    EXPECT_NE(AllOutput().find("No snapshots"), std::string::npos);

    m_output.clear();
    m_interp->ExecuteLine("STATE LIST");
    EXPECT_NE(AllOutput().find("No snapshots"), std::string::npos);
}

TEST_F(ArStateFixture, SavesInMemoryByDefaultAndToDiskWithPersist)
{
    m_interp->ExecuteLine("STATE SAVE checkpoint");
    EXPECT_EQ(m_savedName, "checkpoint");
    EXPECT_FALSE(m_savedPersist);
    EXPECT_NE(AllOutput().find("Saved snapshot 'checkpoint'."), std::string::npos);

    m_interp->ExecuteLine("STATE SAVE keeper PERSIST");
    EXPECT_EQ(m_savedName, "keeper");
    EXPECT_TRUE(m_savedPersist);
    EXPECT_NE(AllOutput().find("to disk"), std::string::npos);
}

TEST_F(ArStateFixture, SnapshotNamesAreNotUppercasedLikeCommandTokens)
{
    // The command word is case-folded but the name must survive verbatim, or it would not
    // match the snapshot the MCP tools stored.
    m_interp->ExecuteLine("state save Tek36-Menu.v2");
    EXPECT_EQ(m_savedName, "Tek36-Menu.v2");
}

TEST_F(ArStateFixture, ReportsASaveThatCouldOnlyBeKeptInMemory)
{
    m_saveWarning = "kept in memory only: disk full";
    m_interp->ExecuteLine("STATE SAVE keeper PERSIST");
    EXPECT_NE(AllOutput().find("disk full"), std::string::npos);
}

TEST_F(ArStateFixture, LoadRestoresAndReportsTheNewFrame)
{
    m_interp->ExecuteLine("STATE LOAD checkpoint");
    EXPECT_EQ(m_loadedName, "checkpoint");
    EXPECT_NE(AllOutput().find("Restored snapshot 'checkpoint'."), std::string::npos);
    EXPECT_NE(AllOutput().find("Frame: 4242"), std::string::npos);
}

TEST_F(ArStateFixture, SurfacesTheCompatibilityRefusalFromTheStateOps)
{
    m_loadError = "snapshot was taken on core 1.18.0, this is 1.19.0";
    m_interp->ExecuteLine("STATE LOAD checkpoint");
    EXPECT_NE(AllOutput().find("core 1.18.0"), std::string::npos);
    // A refused restore must not print a frame line or claim success.
    EXPECT_EQ(AllOutput().find("Frame:"), std::string::npos);
    EXPECT_EQ(AllOutput().find("Restored"), std::string::npos);
}

TEST_F(ArStateFixture, DeleteAcceptsBothSpellingsAndReportsAMissingSnapshot)
{
    m_interp->ExecuteLine("STATE DELETE gone");
    EXPECT_EQ(m_deletedName, "gone");
    EXPECT_NE(AllOutput().find("Deleted snapshot 'gone'."), std::string::npos);

    m_deleteOk = false;
    m_interp->ExecuteLine("STATE DEL missing");
    EXPECT_NE(AllOutput().find("no snapshot named 'missing'"), std::string::npos);
}

TEST_F(ArStateFixture, RejectsUnknownActionsAndMissingNames)
{
    m_interp->ExecuteLine("STATE FROBNICATE x");
    EXPECT_NE(AllOutput().find("Usage: STATE (LIST)"), std::string::npos);

    m_interp->ExecuteLine("STATE SAVE");
    EXPECT_NE(AllOutput().find("Usage: STATE SAVE <name>"), std::string::npos);
    EXPECT_TRUE(m_savedName.empty());

    m_interp->ExecuteLine("STATE SAVE name BOGUS");
    EXPECT_NE(AllOutput().find("unknown option 'BOGUS'"), std::string::npos);
}

TEST_F(ArInterpreterFixture, StateReportsUnavailableWithoutASnapshotStore)
{
    m_interp->ExecuteLine("STATE");
    EXPECT_NE(AllOutput().find("state snapshots unavailable"), std::string::npos);
}

// ---------------------------------------------------------------------------
// PR / PROTECT — memory locks
//
// The grammar is the surface the user (and the memory_protect MCP tool) actually types, so
// what is pinned here is the parsing and the refusals: a bad size, a value that does not fit
// its width, and — the one that matters — an entry whose anchor cannot be verified never
// being left enabled. The enforcement itself lives in the core (patches/adm_protect.inc,
// covered by AdmProtectTest); MemoryLocks' own behaviour is covered by MemoryLocksTest.

namespace {

class ArProtectFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        std::remove(kLockPath);
        m_locks.SetPath(kLockPath);
        host.locks = &m_locks;
        host.supportsProtections = [this]() { return m_supported; };
    }

    void TearDown() override { std::remove(kLockPath); }

    // The id the interpreter assigned to the most recently added lock.
    std::string LastId() const { return m_locks.Empty() ? std::string() : m_locks.All().back().id; }

    static constexpr const char* kLockPath = "ar_protect_test_locks.json";
    adm::core::MemoryLocks m_locks;
    bool m_supported = true;
};

} // namespace

TEST_F(ArProtectFixture, ListsNothingWhenNoLocksAreSet)
{
    m_interp->ExecuteLine("PR");
    EXPECT_NE(AllOutput().find("No memory locks set."), std::string::npos);
}

TEST_F(ArProtectFixture, BlockAddsAnEnabledEntryOverTheRequestedLength)
{
    m_interp->ExecuteLine("PR 2000 BLOCK LEN=4 infinite lives");

    ASSERT_EQ(m_locks.Size(), 1u);
    const auto& e = m_locks.All().front();
    EXPECT_EQ(e.mode, adm::core::LockMode::Block);
    EXPECT_EQ(e.anchor.kind, adm::core::AnchorKind::Absolute);
    EXPECT_EQ(e.anchor.addr, 0x2000u);
    EXPECT_EQ(e.len, 4u);
    EXPECT_TRUE(e.enabled);
    // Everything left over after the keywords is the description, so the command reads the
    // way it looks.
    EXPECT_EQ(e.desc, "infinite lives");
    EXPECT_NE(AllOutput().find("blocking 4 byte(s) at $00002000"), std::string::npos);
}

TEST_F(ArProtectFixture, SetParsesValueAndSizeInBitsAndProtectsOnlyItsOwnWidth)
{
    m_interp->ExecuteLine("PR 2000 SET=0x1234 SIZE=16");

    ASSERT_EQ(m_locks.Size(), 1u);
    const auto& e = m_locks.All().front();
    EXPECT_EQ(e.mode, adm::core::LockMode::Set);
    EXPECT_EQ(e.value, 0x1234u);
    // SIZE is in bits on the command line and bytes in the entry.
    EXPECT_EQ(e.size, 2);
    // A forced value protects exactly its own width, so LEN would be meaningless.
    EXPECT_EQ(e.len, 2u);
}

TEST_F(ArProtectFixture, RejectsAValueTooWideForItsSize)
{
    m_interp->ExecuteLine("PR 2000 SET=0x1234 SIZE=8");
    EXPECT_NE(AllOutput().find("does not fit in 8 bits"), std::string::npos);
    EXPECT_TRUE(m_locks.Empty()) << "a value that cannot fit must not become a lock";
}

TEST_F(ArProtectFixture, RejectsASizeThatIsNotEightSixteenOrThirtyTwo)
{
    m_interp->ExecuteLine("PR 2000 SET=1 SIZE=12");
    EXPECT_NE(AllOutput().find("SIZE must be 8, 16 or 32"), std::string::npos);
    EXPECT_TRUE(m_locks.Empty());
}

TEST_F(ArProtectFixture, RequiresAModeAndReportsTheGrammarOtherwise)
{
    m_interp->ExecuteLine("PR 2000");
    EXPECT_NE(AllOutput().find("Usage: PR <addr> BLOCK"), std::string::npos);
    EXPECT_TRUE(m_locks.Empty());

    m_output.clear();
    m_interp->ExecuteLine("PR notanaddress BLOCK");
    EXPECT_NE(AllOutput().find("Usage: PR"), std::string::npos);
    EXPECT_TRUE(m_locks.Empty());
}

TEST_F(ArProtectFixture, LocksAnAddressOutsideAnyHostMemoryRegion)
{
    // An absolute anchor resolves to itself and is deliberately not checked against the memory
    // map. That is load-bearing rather than lax: chipset ($DFFxxx) and CIA ($BFExxx) registers
    // are reached through ArMemoryAccess hooks, not host-backed regions, so requiring a mapped
    // region would make the register windows' Lock checkbox impossible. The invariant that
    // stops a wrong write is the verify bytes, not the memory map.
    m_interp->ExecuteLine("PR DFF096 SET=0x8200 SIZE=16");

    ASSERT_EQ(m_locks.Size(), 1u);
    const auto& e = m_locks.All().front();
    EXPECT_TRUE(e.enabled);
    ASSERT_TRUE(e.resolved.has_value());
    EXPECT_EQ(*e.resolved, 0xDFF096u);
}

TEST_F(ArProtectFixture, OnAndOffToggleByIdAndReportUnknownOnes)
{
    m_interp->ExecuteLine("PR 2000 BLOCK");
    const std::string id = LastId();
    ASSERT_FALSE(id.empty());

    m_output.clear();
    m_interp->ExecuteLine("PR OFF " + id);
    EXPECT_FALSE(m_locks.Find(id)->enabled);

    m_output.clear();
    m_interp->ExecuteLine("PR ON " + id);
    EXPECT_TRUE(m_locks.Find(id)->enabled);

    m_output.clear();
    m_interp->ExecuteLine("PR ON nosuchid");
    EXPECT_NE(AllOutput().find("Error:"), std::string::npos);
}

TEST_F(ArProtectFixture, DeleteAcceptsEitherAnIdOrAnAddress)
{
    m_interp->ExecuteLine("PR 2000 BLOCK");
    const std::string id = LastId();
    m_interp->ExecuteLine("PR DEL " + id);
    EXPECT_TRUE(m_locks.Empty());

    m_interp->ExecuteLine("PR 2000 BLOCK LEN=4");
    m_output.clear();
    // Any address inside the span, not just its base.
    m_interp->ExecuteLine("PR DEL 2002");
    EXPECT_TRUE(m_locks.Empty());
    EXPECT_NE(AllOutput().find("Removed 1 lock(s)"), std::string::npos);

    m_output.clear();
    m_interp->ExecuteLine("PR DEL 2000");
    EXPECT_NE(AllOutput().find("no lock at $00002000"), std::string::npos);
}

TEST_F(ArProtectFixture, ClearRemovesEverything)
{
    m_interp->ExecuteLine("PR 2000 BLOCK");
    m_interp->ExecuteLine("PR 2100 SET=5 SIZE=8");
    ASSERT_EQ(m_locks.Size(), 2u);

    m_interp->ExecuteLine("PR CLEAR");
    EXPECT_TRUE(m_locks.Empty());
}

TEST_F(ArProtectFixture, RescanReResolvesEveryEntry)
{
    m_interp->ExecuteLine("PR 2000 BLOCK");
    m_output.clear();
    m_interp->ExecuteLine("PR RESCAN");
    EXPECT_NE(AllOutput().find("Re-resolved 1 lock(s): 1 resolved"), std::string::npos);
}

TEST_F(ArProtectFixture, ListingShowsIdModeAddressAndState)
{
    m_interp->ExecuteLine("PR 2000 SET=0x63 SIZE=8 lives");
    m_output.clear();
    m_interp->ExecuteLine("PR");

    const std::string out = AllOutput();
    EXPECT_NE(out.find("$00002000"), std::string::npos);
    EXPECT_NE(out.find("set"), std::string::npos);
    EXPECT_NE(out.find("lives"), std::string::npos);
    EXPECT_NE(out.find("resolved"), std::string::npos);
}

TEST_F(ArProtectFixture, WarnsWhenTheLoadedCoreCannotEnforceALock)
{
    m_supported = false;
    m_interp->ExecuteLine("PR 2000 BLOCK");
    EXPECT_NE(AllOutput().find("no protection hooks"), std::string::npos);
}

TEST_F(ArProtectFixture, SaveThenLoadRoundTripsThroughATrainerFile)
{
    const char* path = "ar_protect_test_trainer.json";
    std::remove(path);

    m_interp->ExecuteLine("PR 2000 SET=0x63 SIZE=8 lives");
    m_interp->ExecuteLine(std::string("PR SAVE ") + path);

    m_interp->ExecuteLine("PR CLEAR");
    ASSERT_TRUE(m_locks.Empty());

    m_output.clear();
    m_interp->ExecuteLine(std::string("PR LOAD ") + path);

    ASSERT_EQ(m_locks.Size(), 1u);
    const auto& e = m_locks.All().front();
    EXPECT_EQ(e.value, 0x63u);
    EXPECT_EQ(e.size, 1);
    EXPECT_EQ(e.desc, "lives");
    // Everything arrives disabled, however well it resolves: a file authored elsewhere has to
    // be reviewed before it is allowed to write to this machine's memory.
    EXPECT_FALSE(e.enabled);
    EXPECT_NE(AllOutput().find("all disabled"), std::string::npos);

    std::remove(path);
}

TEST_F(ArProtectFixture, ProtectIsAnAliasForPr)
{
    m_interp->ExecuteLine("PROTECT 2000 BLOCK");
    EXPECT_EQ(m_locks.Size(), 1u);
}

TEST_F(ArInterpreterFixture, ProtectReportsUnavailableWithoutALockStore)
{
    m_interp->ExecuteLine("PR");
    EXPECT_NE(AllOutput().find("memory locks unavailable"), std::string::npos);
}

namespace {

// The extended registers as the core reports them, for one configured CPU.
class ArCpuExtFixture : public ArInterpreterFixture
{
protected:
    void ConfigureHost(ArHost& host) override
    {
        host.getCpuExtState = [this](adm::core::CpuExtendedState& out) {
            out = m_ext;
            return true;
        };
    }

    void Configure(int cpuModel, int fpuModel = 0)
    {
        m_ext = {};
        m_ext.cpuModel = cpuModel;
        m_ext.fpuModel = fpuModel;
        m_ext.supervisor = true;
        m_ext.usp = 0x00001000;
        m_ext.isp = 0x00C7F000;
        m_ext.msp = 0x00C7E000;
        if (cpuModel == 68030) {
            // SFC DFC CACR, USP VBR CAAR MSP ISP (movec_illg for a 68030)
            m_ext.movecValid = 0x7 | (0x1F << 9);
            m_ext.mmuModel = 68030;
            m_ext.mmu030Present = true;
            m_ext.tc030 = 0x80C07760;
            m_ext.crp030 = 0x7FFF000200001000ull;
        }
        m_ext.movec[10] = 0x00000800; // VBR
        m_ext.fpuPresent = fpuModel != 0;
    }

    adm::core::CpuExtendedState m_ext;
};

TEST_F(ArCpuExtFixture, RShowsTheStacksA7IsNot)
{
    Configure(68000);
    m_interp->ExecuteLine("R");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("USP=00001000 SSP=00C7F000  A7=SSP"), std::string::npos) << out;
    EXPECT_EQ(out.find("VBR="), std::string::npos);
}

TEST_F(ArCpuExtFixture, RcListsWhatThe68000Lacks)
{
    Configure(68000);
    m_interp->ExecuteLine("RC");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("USP=00001000 SSP=00C7F000"), std::string::npos) << out;
    EXPECT_NE(out.find("Not on this 68000: MSP"), std::string::npos) << out;
    EXPECT_NE(out.find("Not on this 68000: VBR SFC DFC CACR CAAR BUSCR PCR"), std::string::npos)
        << out;
}

TEST_F(ArCpuExtFixture, RfWithoutAnFpuSaysSo)
{
    Configure(68000);
    m_interp->ExecuteLine("RF");
    EXPECT_NE(AllOutput().find("FPCR: not on a 68000 (68020+ with an FPU)"), std::string::npos);
}

TEST_F(ArCpuExtFixture, RfDecodesTheControlRegisters)
{
    Configure(68030, 68882);
    m_interp->ExecuteLine("RF");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("FPU 68882 REGISTERS"), std::string::npos) << out;
    EXPECT_NE(out.find("FPCR  00000000  prec X round RN enable -"), std::string::npos) << out;
}

TEST_F(ArCpuExtFixture, RmShowsThe030PmoveRegisters)
{
    Configure(68030);
    m_interp->ExecuteLine("RM");
    const std::string out = AllOutput();
    EXPECT_NE(out.find("TC=80C07760 CRP=7FFF0002:00001000"), std::string::npos) << out;
    EXPECT_NE(out.find("Not on this 68030: URP"), std::string::npos) << out;
}

TEST_F(ArCpuExtFixture, SettingARegisterTheCpuLacksIsRefused)
{
    Configure(68000);
    m_interp->ExecuteLine("R VBR 1000");
    EXPECT_NE(AllOutput().find("VBR: not on a 68000 (68010+)"), std::string::npos);
    EXPECT_TRUE(m_setRegs.empty());
}

TEST_F(ArCpuExtFixture, SettingAControlRegisterUsesUaesName)
{
    Configure(68030);
    m_interp->ExecuteLine("R vbr 1000");
    ASSERT_EQ(m_setRegs.size(), 1u);
    EXPECT_EQ(m_setRegs[0].first, "VBR");
    EXPECT_EQ(m_setRegs[0].second, 0x1000u);

    m_interp->ExecuteLine("R TC 0");
    EXPECT_NE(AllOutput().find("TC is read-only here"), std::string::npos);
}

TEST_F(ArCpuExtFixture, EditingTheStackLineWritesA7ForTheActiveStack)
{
    Configure(68000);
    m_interp->ExecuteLine("USP=00002000 SSP=00C7F000");
    ASSERT_EQ(m_setRegs.size(), 1u); // SSP unchanged
    EXPECT_EQ(m_setRegs[0].first, "USP");
    m_setRegs.clear();
    m_interp->ExecuteLine("USP=00001000 SSP=00C70000");
    ASSERT_EQ(m_setRegs.size(), 1u);
    EXPECT_EQ(m_setRegs[0].first, "A7"); // supervisor mode: SSP is A7
}

} // namespace
