#include <gtest/gtest.h>

#include <filesystem>

#include "core/DebugLines.h"

using adm::core::DebugLines;
using adm::core::SourceLine;

namespace {

std::string FindFixture()
{
    const char* candidates[] = {
        "tests/images/a.elf",
        "../tests/images/a.elf",
        "../../tests/images/a.elf",
    };
    for (const char* c : candidates) {
        if (std::filesystem::exists(c)) {
            return c;
        }
    }
    return {};
}

// Ground truth from `readelf --debug-dump=decodedline a.elf` at known function
// starts (addresses are .text offsets; the toolchain links .text at 0).
struct Known
{
    uint32_t addr;
    const char* fileSuffix;
    uint32_t line;
};
constexpr Known kKnown[] = {
    {0x2498, "system.c", 30},
    {0x24ec, "system.c", 40},
    {0x2540, "loading.h", 14},
    {0x257a, "goth_scroll.c", 681},
};

bool EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

TEST(DebugLinesTest, LoadsLineProgram)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(path));
    EXPECT_TRUE(lines.IsLoaded());
    EXPECT_GT(lines.RowCount(), 100u); // 20 CUs -> plenty of rows
}

TEST(DebugLinesTest, ResolvesKnownFunctionStarts)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(path));

    for (const auto& k : kKnown) {
        auto src = lines.ResolveLine(k.addr);
        ASSERT_TRUE(src.has_value()) << "no line for $" << std::hex << k.addr;
        EXPECT_EQ(src->line, k.line) << "at $" << std::hex << k.addr << " file=" << src->file;
        EXPECT_TRUE(EndsWith(src->file, k.fileSuffix))
            << "at $" << std::hex << k.addr << " got file=" << src->file;
    }
}

TEST(DebugLinesTest, ResolvesMidFunctionAddress)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(path));

    // A few bytes into system_disable_cache should still be inside system.c and at
    // a line >= the function's first line (30).
    auto src = lines.ResolveLine(0x2498 + 4);
    ASSERT_TRUE(src.has_value());
    EXPECT_TRUE(EndsWith(src->file, "system.c"));
    EXPECT_GE(src->line, 30u);
}

TEST(DebugLinesTest, UnmappedAddressReturnsNullopt)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(path));

    EXPECT_FALSE(lines.ResolveLine(0xF0000000).has_value());
}

TEST(DebugLinesTest, RejectsNonElf)
{
    DebugLines lines;
    const uint8_t garbage[] = {'n', 'o', 't', 'e', 'l', 'f', 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_FALSE(lines.LoadFromElf(std::span<const uint8_t>(garbage, sizeof(garbage))));
    EXPECT_FALSE(lines.IsLoaded());
}

// --- reverse lookup: line -> addresses -------------------------------------------------------
//
// This is the direction that breaks on -Ofast -flto builds, and the reason it exists is to
// audit and explain GDB's placement rather than to place breakpoints itself.

TEST(DebugLinesTest, PathScoreComparesTrailingComponents)
{
    // DWARF carries whatever the compiler saw; the editor sends an absolute path. Comparing
    // from the right is what makes the two meet.
    EXPECT_EQ(DebugLines::PathScore("../engine/foo.c", "C:/proj/engine/foo.c"), 2);
    EXPECT_EQ(DebugLines::PathScore("foo.c", "C:/proj/engine/foo.c"), 1);
    // Separator style and case must not matter on Windows.
    EXPECT_EQ(DebugLines::PathScore("engine/foo.c", "C:\\PROJ\\Engine\\FOO.C"), 2);
    // A different file is not a match at any score.
    EXPECT_EQ(DebugLines::PathScore("engine/bar.c", "C:/proj/engine/foo.c"), -1);
    EXPECT_EQ(DebugLines::PathScore("", "foo.c"), -1);
}

TEST(DebugLinesTest, ReverseLookupAgreesWithTheForwardDirection)
{
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    // Every address the forward test pins down must come back from the reverse lookup. If the
    // two directions ever disagree, one of them is lying about where code lives.
    for (const Known& k : kKnown) {
        const auto lookup = lines.ResolveLineToAddresses(k.fileSuffix, k.line);
        ASSERT_EQ(lookup.status, adm::core::LineLookupStatus::Exact)
            << k.fileSuffix << ":" << k.line;
        ASSERT_FALSE(lookup.sites.empty());
        bool found = false;
        for (const auto& site : lookup.sites) {
            if (site.lo == k.addr)
                found = true;
            EXPECT_LT(site.lo, site.hi) << "a site must cover at least one byte";
        }
        EXPECT_TRUE(found) << k.fileSuffix << ":" << k.line << " did not report " << std::hex
                           << k.addr;
    }
}

TEST(DebugLinesTest, ALineThatGeneratedNoCodeSaysSo)
{
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    // Line 1 of a C file is an #include or a comment: real source, no code. Reporting this
    // honestly is the whole point -- a breakpoint there can never fire.
    const auto lookup = lines.ResolveLineToAddresses("system.c", 1, /*stmtOnly=*/true,
                                                     /*forwardSearch=*/0);
    EXPECT_EQ(lookup.status, adm::core::LineLookupStatus::NoCode);
    EXPECT_TRUE(lookup.sites.empty());
}

TEST(DebugLinesTest, ForwardSearchReportsTheMoveRatherThanHidingIt)
{
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    const auto withCode = lines.LinesWithCode("system.c");
    ASSERT_FALSE(withCode.empty());
    const uint32_t first = withCode.front();
    ASSERT_GT(first, 1u);

    // Asking a line or two before the first executable one should find it, and must say that
    // it moved -- silently adjusting is exactly the dishonesty this replaces.
    const auto lookup = lines.ResolveLineToAddresses("system.c", first - 1, true, 25);
    EXPECT_EQ(lookup.status, adm::core::LineLookupStatus::MovedForward);
    EXPECT_EQ(lookup.requestedLine, first - 1);
    EXPECT_EQ(lookup.resolvedLine, first);
    EXPECT_FALSE(lookup.sites.empty());
}

TEST(DebugLinesTest, AnUnknownFileIsNotTheSameAsALineWithNoCode)
{
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    // Saying "this line generated no code" about a file we never parsed would be a lie.
    const auto lookup = lines.ResolveLineToAddresses("definitely_not_in_this_build.c", 10);
    EXPECT_EQ(lookup.status, adm::core::LineLookupStatus::FileUnknown);
    EXPECT_TRUE(lookup.sites.empty());
}

TEST(DebugLinesTest, LinesWithCodeIsSortedAndDeduplicated)
{
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    const auto withCode = lines.LinesWithCode("system.c");
    ASSERT_FALSE(withCode.empty());
    for (size_t i = 1; i < withCode.size(); ++i)
        EXPECT_LT(withCode[i - 1], withCode[i]) << "must be ascending and unique";

    // A line reported as having code must actually resolve to some.
    const auto lookup = lines.ResolveLineToAddresses("system.c", withCode.front(), false, 0);
    EXPECT_EQ(lookup.status, adm::core::LineLookupStatus::Exact);

    EXPECT_TRUE(lines.LinesWithCode("nope.c").empty());
}

TEST(DebugLinesTest, AnInlinedOrUnrolledLineReportsEverySite)
{
    // The case that makes single-address breakpoints wrong: one source line, several disjoint
    // stretches of code. Whichever line it is, the sites must not overlap and must be ordered.
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    size_t multiSiteLines = 0;
    for (uint32_t line : lines.LinesWithCode("system.c")) {
        const auto lookup = lines.ResolveLineToAddresses("system.c", line, false, 0);
        if (lookup.sites.size() > 1) {
            ++multiSiteLines;
            for (size_t i = 1; i < lookup.sites.size(); ++i) {
                EXPECT_LE(lookup.sites[i - 1].lo, lookup.sites[i].lo) << "sites must be ordered";
                EXPECT_LE(lookup.sites[i - 1].hi, lookup.sites[i].lo) << "sites must not overlap";
            }
        }
    }
    // An -Ofast build of a real file is expected to have at least one such line; if this ever
    // becomes zero the build stopped optimising and the interesting cases went with it.
    EXPECT_GT(multiSiteLines, 0u);
}

TEST(DebugLinesTest, StatementSitesArePreferredButNotRequired)
{
    const std::string fixture = FindFixture();
    ASSERT_FALSE(fixture.empty());
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(fixture));

    // Relaxing is_stmt can only ever find more code, never less.
    for (uint32_t line : lines.LinesWithCode("system.c")) {
        const auto strict = lines.ResolveLineToAddresses("system.c", line, true, 0);
        const auto relaxed = lines.ResolveLineToAddresses("system.c", line, false, 0);
        EXPECT_LE(strict.sites.size(), relaxed.sites.size()) << "line " << line;
    }
}

// --- Pre-DWARF-5 line programs -------------------------------------------------------------
//
// A project with hand-written .s files gets v2/v3 units from the assembler alongside v5 units
// from the C compiler, in one .debug_line -- amiga_kapock's ELF has 14 v5 units plus one v2 and
// two v3. Those older headers are laid out differently enough that skipping them (what this
// reader used to do) left every assembly frame in a backtrace with no source at all.
//
// The unit is synthesized rather than checked in as a second multi-megabyte ELF: it pins the
// exact header layout under test, which is the part at risk.
namespace {

void PutU16(std::vector<uint8_t>& v, size_t at, uint16_t x)
{
    v[at] = static_cast<uint8_t>(x >> 8);
    v[at + 1] = static_cast<uint8_t>(x);
}
void PutU32(std::vector<uint8_t>& v, size_t at, uint32_t x)
{
    v[at] = static_cast<uint8_t>(x >> 24);
    v[at + 1] = static_cast<uint8_t>(x >> 16);
    v[at + 2] = static_cast<uint8_t>(x >> 8);
    v[at + 3] = static_cast<uint8_t>(x);
}
void AppendU32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
void AppendSleb(std::vector<uint8_t>& v, int64_t x)
{
    // Proper SLEB128: a single byte only works up to 63, because bit 6 is the sign bit and a
    // value like 98 would otherwise be read back as -30.
    bool more = true;
    while (more) {
        uint8_t byte = static_cast<uint8_t>(x & 0x7f);
        x >>= 7;
        if ((x == 0 && (byte & 0x40) == 0) || (x == -1 && (byte & 0x40) != 0))
            more = false;
        else
            byte |= 0x80;
        v.push_back(byte);
    }
}

void AppendStr(std::vector<uint8_t>& v, const char* s)
{
    while (*s)
        v.push_back(static_cast<uint8_t>(*s++));
    v.push_back(0);
}

/// One .debug_line unit of the given version, mapping `addr` to support/gcc8_a_support.s:`line`.
std::vector<uint8_t> MakeLineProgram(uint16_t version, uint32_t addr, int line)
{
    std::vector<uint8_t> u;
    AppendU32(u, 0); // unit_length, patched below
    u.push_back(static_cast<uint8_t>(version >> 8));
    u.push_back(static_cast<uint8_t>(version));
    const size_t headerLenAt = u.size();
    AppendU32(u, 0); // header_length, patched below
    const size_t afterHeaderLen = u.size();

    u.push_back(1); // minimum_instruction_length
    if (version >= 4)
        u.push_back(1);                    // maximum_operations_per_instruction (v4+ only)
    u.push_back(1);                        // default_is_stmt
    u.push_back(static_cast<uint8_t>(-5)); // line_base
    u.push_back(14);                       // line_range
    u.push_back(13);                       // opcode_base
    const uint8_t stdLens[12] = {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
    for (uint8_t n : stdLens)
        u.push_back(n);

    // v2-4 tables are NUL-terminated string lists ended by an empty string, and are indexed
    // from 1 -- index 0 means the CU's own dir/file, which lives in .debug_info.
    AppendStr(u, "support");          // include_directories[1]
    u.push_back(0);                   // end of directories
    AppendStr(u, "gcc8_a_support.s"); // file_names[1]
    u.push_back(1);                   // directory index
    u.push_back(0);                   // mtime
    u.push_back(0);                   // length
    u.push_back(0);                   // end of file names

    PutU32(u, headerLenAt, static_cast<uint32_t>(u.size() - afterHeaderLen));

    // DW_LNE_set_address <addr>
    u.push_back(0);
    u.push_back(5);
    u.push_back(2);
    AppendU32(u, addr);
    // DW_LNS_advance_line (line - 1)
    u.push_back(3);
    AppendSleb(u, line - 1);
    u.push_back(1); // DW_LNS_copy -> emit the row
    // DW_LNS_advance_pc 4. A row's extent runs to the next row in its sequence, so without
    // this the end-of-sequence row lands on the same address and the row covers nothing.
    u.push_back(2);
    u.push_back(4);
    // DW_LNE_end_sequence
    u.push_back(0);
    u.push_back(1);
    u.push_back(1);

    PutU32(u, 0, static_cast<uint32_t>(u.size() - 4));
    return u;
}

/// Wrap a .debug_line payload in the smallest big-endian ELF32 the reader will accept.
std::vector<uint8_t> MakeElfWithDebugLine(const std::vector<uint8_t>& lineSec)
{
    const char kStrTab[] = "\0.debug_line\0.shstrtab";
    const size_t strTabSize = sizeof(kStrTab); // includes the trailing NUL
    constexpr size_t kEhSize = 52, kShSize = 40, kShNum = 3;

    std::vector<uint8_t> f(kEhSize, 0);
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 1 /*ELFCLASS32*/, 2 /*ELFDATA2MSB*/, 1};
    std::copy(std::begin(ident), std::end(ident), f.begin());
    PutU16(f, 16, 1); // e_type = ET_REL
    PutU16(f, 18, 4); // e_machine = EM_68K
    PutU16(f, 40, kEhSize);
    PutU16(f, 46, kShSize);
    PutU16(f, 48, kShNum);
    PutU16(f, 50, 2); // e_shstrndx

    const size_t lineOff = f.size();
    f.insert(f.end(), lineSec.begin(), lineSec.end());
    const size_t strOff = f.size();
    f.insert(f.end(), kStrTab, kStrTab + strTabSize);
    const size_t shOff = f.size();
    PutU32(f, 32, static_cast<uint32_t>(shOff)); // e_shoff

    f.resize(shOff + kShNum * kShSize, 0);
    auto section = [&](size_t idx, uint32_t nameOff, size_t off, size_t size) {
        const size_t sh = shOff + idx * kShSize;
        PutU32(f, sh + 0, nameOff);
        PutU32(f, sh + 16, static_cast<uint32_t>(off));
        PutU32(f, sh + 20, static_cast<uint32_t>(size));
    };
    section(0, 0, 0, 0);                    // SHT_NULL
    section(1, 1, lineOff, lineSec.size()); // ".debug_line"
    section(2, 13, strOff, strTabSize);     // ".shstrtab"
    return f;
}

} // namespace

TEST(DebugLinesPreV5Test, ParsesVersion3LineProgram)
{
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(MakeElfWithDebugLine(MakeLineProgram(3, 0x1000, 17))));

    const auto src = lines.ResolveLine(0x1000);
    ASSERT_TRUE(src.has_value());
    EXPECT_EQ(src->line, 17u);
    EXPECT_EQ(src->file, "support/gcc8_a_support.s");
}

TEST(DebugLinesPreV5Test, ParsesVersion2LineProgram)
{
    // v2 and v3 share a header layout, so this guards against a version check that is too
    // narrow rather than a different encoding.
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(MakeElfWithDebugLine(MakeLineProgram(2, 0x2000, 42))));

    const auto src = lines.ResolveLine(0x2000);
    ASSERT_TRUE(src.has_value());
    EXPECT_EQ(src->line, 42u);
}

TEST(DebugLinesPreV5Test, ParsesVersion4LineProgramWithMaxOpsByte)
{
    // v4 inserts maximum_operations_per_instruction after minimum_instruction_length. Missing
    // it shifts every following header field by one byte, which is the kind of error that
    // yields plausible-looking garbage rather than a clean failure.
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(MakeElfWithDebugLine(MakeLineProgram(4, 0x3000, 99))));

    const auto src = lines.ResolveLine(0x3000);
    ASSERT_TRUE(src.has_value());
    EXPECT_EQ(src->line, 99u);
    EXPECT_EQ(src->file, "support/gcc8_a_support.s");
}

TEST(DebugLinesPreV5Test, ResolvesCallFileAgainstAPreV5UnitTable)
{
    // DW_AT_call_file indexes the unit's own file table. For v2-4 that table is 1-based, so
    // index 1 is the first real entry -- the offset by one that the v5 path does not have.
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(MakeElfWithDebugLine(MakeLineProgram(3, 0x1000, 17))));

    const auto file = lines.ResolveUnitFile(/*stmtListOffset=*/0, /*fileIndex=*/1);
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(*file, "support/gcc8_a_support.s");

    EXPECT_FALSE(lines.ResolveUnitFile(0, 99).has_value());
}

TEST(DebugLinesHostileInputTest, LineRangeZeroDoesNotDivideByZero)
{
    // line_range feeds `adj / lineRange` in the special-opcode decode; a crafted unit with
    // line_range == 0 must be rejected, not divided by.
    auto unit = MakeLineProgram(3, 0x1000, 17);
    // v2/v3 header layout: unit_length(4) version(2) header_length(4) min_inst(1)
    // default_is_stmt(1) line_base(1) → line_range at offset 13.
    ASSERT_EQ(unit[13], 14u); // the builder's line_range — guard against layout drift
    unit[13] = 0;
    DebugLines lines;
    EXPECT_FALSE(lines.LoadFromElf(MakeElfWithDebugLine(unit)));
    EXPECT_FALSE(lines.ResolveLine(0x1000).has_value());
}

TEST(DebugLinesHostileInputTest, HugeExtendedOpcodeLengthTerminates)
{
    // The extended-opcode length is an unbounded ULEB; a huge value used to wrap the "skip to
    // end of record" position backwards, and the parse loop never terminated.
    auto unit = MakeLineProgram(3, 0x1000, 17);
    // Find the DW_LNE_set_address record: opcode 0, length 5, sub-opcode 2.
    size_t at = std::string::npos;
    for (size_t i = 0; i + 2 < unit.size(); ++i) {
        if (unit[i] == 0 && unit[i + 1] == 5 && unit[i + 2] == 2) {
            at = i + 1;
            break;
        }
    }
    ASSERT_NE(at, std::string::npos);
    // Replace the 1-byte length (5) with a 5-byte ULEB for ~4G, and grow unit_length to match.
    const uint8_t hugeLen[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
    unit.erase(unit.begin() + static_cast<ptrdiff_t>(at));
    unit.insert(unit.begin() + static_cast<ptrdiff_t>(at), std::begin(hugeLen), std::end(hugeLen));
    PutU32(unit, 0, static_cast<uint32_t>(unit.size() - 4));

    DebugLines lines;
    EXPECT_FALSE(lines.LoadFromElf(MakeElfWithDebugLine(unit))); // and, crucially, it returns
    EXPECT_FALSE(lines.ResolveLine(0x1000).has_value());
}

TEST(DebugLinesPreV5Test, SkipsAnUnknownLineProgramVersion)
{
    // An unrecognised version must be stepped over via unit_length, never guessed at.
    DebugLines lines;
    EXPECT_FALSE(lines.LoadFromElf(MakeElfWithDebugLine(MakeLineProgram(9, 0x1000, 17))));
}

TEST(DebugLinesPreV5Test, ANegativeLineRegisterIsNotAHugeLineNumber)
{
    // The line register is signed and DW_LNS_advance_line can decrement it past zero. Casting
    // that straight to uint32_t used to yield ~4.29e9, which downstream read as a real line:
    // DebuggerDisassemblyView loops firstLine..lastLine, so a lastLine of 0xFFFFFFFF spun for
    // billions of iterations per frame and never terminated at all -- the counter is uint32_t,
    // so `line <= 0xFFFFFFFF` is always true and ++line wraps to 0. It wedged the whole UI
    // (observed on an LTO build of the crass engine). 0 is the established "unknown" value.
    DebugLines lines;
    ASSERT_TRUE(lines.LoadFromElf(MakeElfWithDebugLine(MakeLineProgram(3, 0x1000, -5))));
    const auto at = lines.ResolveLine(0x1000);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(at->line, 0u) << "negative line register surfaced as " << at->line;
}
