#include <gtest/gtest.h>

#include "core/EditorLauncher.h"

#include <set>
#include <string>

using adm::core::EditorCommand;
using adm::core::FindVsCodeLauncher;
using adm::core::MakeVsCodeGoto;

namespace {

#ifdef _WIN32
constexpr bool kWindows = true;
constexpr char kSep = ';';
#else
constexpr bool kWindows = false;
constexpr char kSep = ':';
#endif

std::string P(std::initializer_list<const char*> parts)
{
    std::string out;
    for (const char* p : parts) {
        if (!out.empty())
            out.push_back(kSep);
        out += p;
    }
    return out;
}

// An existence predicate backed by a fixed set, normalised the way the resolver's
// lexically_normal() output looks so "dir/../Code.exe" collapses before lookup.
adm::core::PathExistsFn Fake(std::set<std::string> present)
{
    return [present = std::move(present)](const std::string& path) {
        std::string norm = path;
        for (char& c : norm)
            if (c == '\\')
                c = '/';
        return present.count(norm) > 0;
    };
}

} // namespace

TEST(EditorLauncherTest, GotoArgsUseTheDashGForm)
{
    const EditorCommand cmd = MakeVsCodeGoto("/usr/bin/code", "/src/main.c", 42);
    ASSERT_TRUE(cmd.Valid());
    EXPECT_EQ(cmd.argv, (std::vector<std::string>{"/usr/bin/code", "-g", "/src/main.c:42"}));
}

TEST(EditorLauncherTest, BatchShimIsRoutedThroughCmd)
{
    // CreateProcess resolves .exe only and ignores PATHEXT, so a .cmd must go via cmd.exe.
    const EditorCommand cmd = MakeVsCodeGoto("C:/vscode/bin/code.cmd", "C:/src/main.c", 7);
    ASSERT_TRUE(cmd.Valid());
    EXPECT_EQ(cmd.argv, (std::vector<std::string>{"cmd.exe", "/c", "C:/vscode/bin/code.cmd", "-g",
                                                  "C:/src/main.c:7"}));

    const EditorCommand bat = MakeVsCodeGoto("C:/vscode/CODE.BAT", "C:/src/main.c", 7);
    EXPECT_EQ(bat.argv.front(), "cmd.exe") << "extension match must be case-insensitive";
}

TEST(EditorLauncherTest, RealExecutableIsNotWrapped)
{
    const EditorCommand cmd = MakeVsCodeGoto("C:/vscode/Code.exe", "C:/src/main.c", 7);
    EXPECT_EQ(cmd.argv.front(), "C:/vscode/Code.exe");
    EXPECT_EQ(cmd.argv.size(), 3u);
}

TEST(EditorLauncherTest, LineZeroBecomesLineOne)
{
    // DWARF can report line 0 for compiler-generated code; VS Code rejects ":0".
    const EditorCommand cmd = MakeVsCodeGoto("code", "/src/main.c", 0);
    EXPECT_EQ(cmd.argv.back(), "/src/main.c:1");
}

TEST(EditorLauncherTest, EmptyInputsYieldNoCommand)
{
    EXPECT_FALSE(MakeVsCodeGoto("", "/src/main.c", 1).Valid());
    EXPECT_FALSE(MakeVsCodeGoto("code", "", 1).Valid());
}

TEST(EditorLauncherTest, DescribeJoinsArguments)
{
    EXPECT_EQ(MakeVsCodeGoto("code", "/a.c", 3).Describe(), "code -g /a.c:3");
}

TEST(EditorLauncherTest, PathSearchFindsTheLauncher)
{
    if (kWindows) {
        auto found = FindVsCodeLauncher(P({"C:/other", "C:/vscode/bin"}),
                                        Fake({"C:/vscode/bin/code.cmd", "C:/vscode/Code.exe"}), {});
        ASSERT_TRUE(found.has_value());
        // The sibling executable is preferred over the batch shim: it takes the same
        // arguments and skips cmd.exe's second round of command-line parsing.
        EXPECT_EQ(*found, "C:/vscode/Code.exe");
    } else {
        auto found = FindVsCodeLauncher(P({"/other", "/usr/bin"}), Fake({"/usr/bin/code"}), {});
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, "/usr/bin/code");
    }
}

TEST(EditorLauncherTest, FallsBackToTheShimWhenNoExecutableSitsBesideIt)
{
    if (!kWindows)
        GTEST_SKIP() << "batch shim fallback is Windows-only";
    auto found = FindVsCodeLauncher(P({"C:/vscode/bin"}), Fake({"C:/vscode/bin/code.cmd"}), {});
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, "C:/vscode/bin/code.cmd");
}

TEST(EditorLauncherTest, WellKnownRootsAreProbedWhenPathHasNothing)
{
    const std::string leaf =
        kWindows ? "C:/Program Files/Microsoft VS Code/Code.exe" : "/usr/share/code/bin/code";
    const std::string root =
        kWindows ? "C:/Program Files/Microsoft VS Code" : "/usr/share/code/bin";
    auto found = FindVsCodeLauncher("", Fake({leaf}), {root});
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, leaf);
}

TEST(EditorLauncherTest, PathEntriesAreTrimmedOfQuotesAndSpaces)
{
    const std::string leaf = kWindows ? "C:/vscode/bin/code.cmd" : "/usr/bin/code";
    const std::string entry = kWindows ? "\"C:/vscode/bin\" " : " \"/usr/bin\"";
    auto found = FindVsCodeLauncher(entry, Fake({leaf}), {});
    ASSERT_TRUE(found.has_value()) << "a quoted PATH entry is legal and must still resolve";
}

TEST(EditorLauncherTest, MissingLauncherReportsNothingRatherThanGuessing)
{
    EXPECT_FALSE(
        FindVsCodeLauncher(P({"/nowhere", "/also/nowhere"}), Fake({}), {"/nope"}).has_value());
    // A null predicate must not be invoked.
    EXPECT_FALSE(FindVsCodeLauncher("/usr/bin", nullptr, {}).has_value());
}
