#include <gtest/gtest.h>

#include "core/SourceFileCache.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using adm::core::SourceFileCache;
using adm::core::SourceFileStatus;

namespace {

class SourceFileCacheTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_root = fs::temp_directory_path() /
                 ("adm_srccache_" +
                  std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
        std::error_code ec;
        fs::remove_all(m_root, ec);
        fs::create_directories(m_root, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_root, ec);
    }

    // Writes `content` to <root>/rel, creating parent directories. Returns the full path
    // in the forward-slash form the cache normalizes to.
    std::string Write(const std::string& rel, const std::string& content)
    {
        fs::path p = m_root / rel;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        return Fwd(p);
    }

    static std::string Fwd(const fs::path& p)
    {
        std::string s = p.string();
        for (char& c : s)
            if (c == '\\')
                c = '/';
        return s;
    }

    std::string Root() const { return Fwd(m_root); }

    fs::path m_root;
};

} // namespace

TEST_F(SourceFileCacheTest, ProbeOneResolvesAnExistingAbsolutePath)
{
    const std::string path = Write("src/main.c", "int main(void) { return 0; }\n");
    SourceFileCache cache;
    const auto& file = cache.Get(path);
    EXPECT_EQ(file.status, SourceFileStatus::Ok);
    EXPECT_EQ(file.resolvedPath, path);
    ASSERT_EQ(file.LineCount(), 1u);
    EXPECT_EQ(file.Line(1), "int main(void) { return 0; }");
}

TEST_F(SourceFileCacheTest, ProbeTwoResolvesRelativeToTheElfDirectory)
{
    Write("src/main.c", "one\ntwo\n");
    Write("build/a.elf", "not really an elf");

    SourceFileCache cache;
    cache.SetElfPath(Root() + "/build/a.elf");
    const auto& file = cache.Get("../src/main.c");
    EXPECT_EQ(file.status, SourceFileStatus::Ok);
    EXPECT_EQ(file.resolvedPath, Root() + "/src/main.c");
    EXPECT_EQ(file.LineCount(), 2u);
}

TEST_F(SourceFileCacheTest, ProbeThreeWalksUpAndPrefersTheLongerSuffix)
{
    // A shallow decoy that a basename-only match would wrongly pick.
    Write("main.c", "decoy\n");
    Write("src/main.c", "real\n");
    Write("build/out/a.elf", "elf");

    SourceFileCache cache;
    cache.SetElfPath(Root() + "/build/out/a.elf");
    // A path from someone else's machine: only the trailing components can match.
    const auto& file = cache.Get("/home/bob/proj/src/main.c");
    ASSERT_EQ(file.status, SourceFileStatus::Ok);
    EXPECT_EQ(file.resolvedPath, Root() + "/src/main.c") << "longer suffix must win";
    EXPECT_EQ(file.Line(1), "real");
}

TEST_F(SourceFileCacheTest, ProbeFourUsesSearchRootsAndRetryFailedPicksThemUp)
{
    Write("elsewhere/engine/foo.c", "found via root\n");
    Write("build/a.elf", "elf");

    SourceFileCache cache;
    cache.SetElfPath(Root() + "/build/a.elf");

    const std::string dwarf = "/opt/ci/engine/foo.c";
    EXPECT_EQ(cache.Get(dwarf).status, SourceFileStatus::NotFound);

    cache.SetSearchRoots({Root() + "/elsewhere"});
    const auto& file = cache.Get(dwarf);
    ASSERT_EQ(file.status, SourceFileStatus::Ok);
    EXPECT_EQ(file.resolvedPath, Root() + "/elsewhere/engine/foo.c");
}

TEST_F(SourceFileCacheTest, RetryFailedKeepsSuccessfulEntriesSticky)
{
    const std::string path = Write("src/a.c", "x\n");
    SourceFileCache cache;
    const uint32_t v1 = cache.Get(path).version;
    ASSERT_TRUE(cache.Get(path).Ok());

    cache.SetSearchRoots({Root()}); // triggers RetryFailed
    EXPECT_EQ(cache.Get(path).version, v1) << "a success must not be re-resolved";
}

TEST_F(SourceFileCacheTest, MissingFileIsNegativelyCachedAndProbesAreBounded)
{
    SourceFileCache cache;
    cache.SetElfPath(Root() + "/build/a.elf");

    const std::string missing = "/nowhere/at/all/ghost.c";
    const auto& first = cache.Get(missing);
    EXPECT_EQ(first.status, SourceFileStatus::NotFound);
    const uint32_t v = first.version;
    EXPECT_EQ(cache.Get(missing).version, v) << "second Get must not re-probe";

    const auto candidates = cache.ProbeCandidates(missing);
    EXPECT_FALSE(candidates.empty());
    EXPECT_LT(candidates.size(), 200u);
    EXPECT_EQ(candidates.front(), missing) << "the recorded path is always tried first";
}

TEST_F(SourceFileCacheTest, SplitsAllThreeLineEndings)
{
    SourceFileCache cache;
    EXPECT_EQ(cache.Get(Write("lf.c", "a\nb\nc\n")).lines,
              (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(cache.Get(Write("crlf.c", "a\r\nb\r\nc\r\n")).lines,
              (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(cache.Get(Write("cr.c", "a\rb\rc\r")).lines,
              (std::vector<std::string>{"a", "b", "c"}));
}

TEST_F(SourceFileCacheTest, TrailingNewlineDoesNotCreateAPhantomLine)
{
    SourceFileCache cache;
    EXPECT_EQ(cache.Get(Write("with.c", "a\nb\n")).LineCount(), 2u);
    EXPECT_EQ(cache.Get(Write("without.c", "a\nb")).LineCount(), 2u);
    EXPECT_EQ(cache.Get(Write("blankend.c", "a\n\n")).LineCount(), 2u);
    EXPECT_EQ(cache.Get(Write("empty.c", "")).LineCount(), 0u);
}

TEST_F(SourceFileCacheTest, ExpandsTabsToTheConfiguredWidth)
{
    const std::string path = Write("tabs.c", "\tif (x)\n\t\ty = 1;\n");

    SourceFileCache four;
    four.SetTabWidth(4);
    const auto& f = four.Get(path);
    EXPECT_EQ(f.Line(1), "    if (x)");
    EXPECT_EQ(f.Line(2), "        y = 1;");

    SourceFileCache eight;
    eight.SetTabWidth(8);
    const auto& e = eight.Get(path);
    EXPECT_EQ(e.Line(1), "        if (x)");

    // Mid-line tabs advance to the next stop, not by a fixed count.
    SourceFileCache mid;
    mid.SetTabWidth(4);
    EXPECT_EQ(mid.Get(Write("mid.c", "ab\tc\n")).Line(1), "ab  c");
}

TEST_F(SourceFileCacheTest, StripsBom)
{
    SourceFileCache cache;
    const auto& file = cache.Get(Write("bom.c", "\xEF\xBB\xBF"
                                                "int x;\n"));
    ASSERT_EQ(file.LineCount(), 1u);
    EXPECT_EQ(file.Line(1), "int x;");
}

TEST_F(SourceFileCacheTest, KeepsValidUtf8AndReplacesInvalidBytes)
{
    SourceFileCache cache;
    // U+00E9 as valid UTF-8, then a lone 0xE9 (Latin-1) that cannot be decoded.
    const auto& file = cache.Get(Write("utf.c", "// caf\xC3\xA9\n// caf\xE9 bad\n"));
    ASSERT_EQ(file.LineCount(), 2u);
    EXPECT_EQ(file.Line(1), "// caf\xC3\xA9") << "well-formed UTF-8 must survive";
    EXPECT_EQ(file.Line(2), "// caf? bad") << "invalid bytes become '?'";
}

TEST_F(SourceFileCacheTest, ControlCharactersBecomeSpaces)
{
    SourceFileCache cache;
    const auto& file = cache.Get(Write("ctrl.c", "a\x01\x02"
                                                 "b\n"));
    ASSERT_EQ(file.LineCount(), 1u);
    EXPECT_EQ(file.Line(1), "a  b");
}

TEST_F(SourceFileCacheTest, RejectsBinaryAndOversizeFiles)
{
    SourceFileCache cache;
    const auto& bin = cache.Get(Write("bin.c", std::string("MZ\0\0padding", 11)));
    EXPECT_EQ(bin.status, SourceFileStatus::Binary);
    EXPECT_TRUE(bin.lines.empty());

    SourceFileCache small;
    small.SetMaxFileBytes(8);
    const auto& big = small.Get(Write("big.c", "way more than eight bytes\n"));
    EXPECT_EQ(big.status, SourceFileStatus::TooLarge);
    EXPECT_TRUE(big.lines.empty());
}

TEST_F(SourceFileCacheTest, TruncatesVeryLongLines)
{
    SourceFileCache cache;
    const std::string huge(SourceFileCache::kMaxLineLength + 200, 'x');
    const auto& file = cache.Get(Write("long.c", huge + "\n"));
    ASSERT_EQ(file.LineCount(), 1u);
    EXPECT_EQ(file.Line(1).size(), static_cast<size_t>(SourceFileCache::kMaxLineLength) + 3);
    EXPECT_EQ(file.Line(1).substr(file.Line(1).size() - 3), "...");
}

TEST_F(SourceFileCacheTest, LineAccessIsBoundsSafe)
{
    SourceFileCache cache;
    const auto& file = cache.Get(Write("two.c", "a\nb\n"));
    EXPECT_TRUE(file.Line(0).empty());
    EXPECT_EQ(file.Line(1), "a");
    EXPECT_EQ(file.Line(2), "b");
    EXPECT_TRUE(file.Line(3).empty());
    EXPECT_TRUE(file.Line(0xFFFFFFFFu).empty());

    EXPECT_FALSE(cache.GetLine(Write("two.c", "a\nb\n"), 3).has_value());
    EXPECT_FALSE(cache.GetLine("/nope/nope.c", 1).has_value());
}

TEST_F(SourceFileCacheTest, ChangingTheElfPathClearsTheCacheButAResetDoesNot)
{
    const std::string path = Write("src/a.c", "x\n");
    SourceFileCache cache;
    cache.SetElfPath(Root() + "/build/a.elf");
    const uint32_t v1 = cache.Get(path).version;

    cache.SetElfPath(Root() + "/build/a.elf"); // unchanged
    EXPECT_EQ(cache.Get(path).version, v1);

    cache.SetElfPath(Root() + "/other/b.elf"); // changed
    EXPECT_NE(cache.Get(path).version, v1);
}

TEST_F(SourceFileCacheTest, ForgetDropsOneEntryAndStatsCount)
{
    SourceFileCache cache;
    const std::string a = Write("a.c", "one\n");
    cache.Get(a);
    cache.Get("/missing/b.c");

    auto stats = cache.GetStats();
    EXPECT_EQ(stats.entries, 2u);
    EXPECT_EQ(stats.resolved, 1u);
    EXPECT_EQ(stats.bytes, 3u);

    cache.Forget(a);
    EXPECT_EQ(cache.GetStats().entries, 1u);

    cache.Clear();
    EXPECT_EQ(cache.GetStats().entries, 0u);
}

TEST_F(SourceFileCacheTest, BackslashPathsResolve)
{
    Write("src/main.c", "x\n");
    Write("build/a.elf", "elf");
    SourceFileCache cache;
    cache.SetElfPath(Root() + "\\build\\a.elf");
    EXPECT_TRUE(cache.Get("..\\src\\main.c").Ok());
}

TEST_F(SourceFileCacheTest, DwarfPseudoFilenamesAreRecognisedNotProbed)
{
    // GCC names compiler-generated code <artificial> / <built-in> in the DWARF file table,
    // joined onto the compilation directory. Nothing is on disk and nothing ever will be.
    EXPECT_TRUE(SourceFileCache::IsPseudoPath("<artificial>"));
    EXPECT_TRUE(SourceFileCache::IsPseudoPath("C:/proj/<artificial>"));
    EXPECT_TRUE(SourceFileCache::IsPseudoPath("C:\\proj\\<built-in>"));
    EXPECT_TRUE(SourceFileCache::IsPseudoPath("/home/bob/<built-in>"));
    EXPECT_TRUE(SourceFileCache::IsPseudoPath(""));

    EXPECT_FALSE(SourceFileCache::IsPseudoPath("main.c"));
    EXPECT_FALSE(SourceFileCache::IsPseudoPath("C:/proj/main.c"));
    EXPECT_FALSE(SourceFileCache::IsPseudoPath("C:/<odd>/main.c")) << "only the leaf counts";
}

TEST_F(SourceFileCacheTest, PseudoPathsShortCircuitAndDoNotSkewTheResolvedRatio)
{
    SourceFileCache cache;
    cache.SetElfPath(Root() + "/build/a.elf");

    const auto& pseudo = cache.Get(Root() + "/<artificial>");
    EXPECT_EQ(pseudo.status, SourceFileStatus::Pseudo);
    EXPECT_TRUE(pseudo.resolvedPath.empty());
    EXPECT_FALSE(pseudo.Ok());

    cache.Get(Write("real.c", "int x;\n"));

    // The pseudo entry must not appear as an unresolved file: the ratio is a health signal
    // for the search roots, and a permanent phantom miss would make it meaningless.
    const auto stats = cache.GetStats();
    EXPECT_EQ(stats.entries, 1u);
    EXPECT_EQ(stats.resolved, 1u);
}
