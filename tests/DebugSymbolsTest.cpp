#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "core/DebugSymbols.h"

using adm::core::DebugSymbols;
using adm::core::FunctionSymbol;

namespace {

// The fixture (a.elf) is an ELF32 big-endian MC68000 binary produced by the
// bartman m68k-amiga-elf toolchain. Tests run from the build directory, so try
// a few relative locations the way AdfWrapperTest does.
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

// Known symbols read from `readelf -sW a.elf` (address, size).
struct KnownSym
{
    uint32_t addr;
    uint32_t size;
    const char* name;
};
constexpr KnownSym kSystemDisableCache{0x2498, 84, "system_disable_cache"};
constexpr KnownSym kSystemEnableCache{0x24ec, 84, "system_enable_cache"};
constexpr KnownSym kGetCacheStatus{0x2a30, 6, "get_cache_status"};

} // namespace

TEST(DebugSymbolsTest, LoadsRealElfFixture)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugSymbols syms;
    ASSERT_TRUE(syms.LoadFromElf(path));
    EXPECT_TRUE(syms.IsLoaded());
    EXPECT_GT(syms.FunctionCount(), 0u);
    EXPECT_EQ(syms.SourcePath(), path);
}

TEST(DebugSymbolsTest, ResolvesKnownFunctionStart)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugSymbols syms;
    ASSERT_TRUE(syms.LoadFromElf(path));

    const FunctionSymbol* fn = syms.ResolvePc(kSystemDisableCache.addr);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, kSystemDisableCache.name);
    EXPECT_EQ(fn->address, kSystemDisableCache.addr);
    EXPECT_EQ(fn->size, kSystemDisableCache.size);
}

TEST(DebugSymbolsTest, ResolvesAddressInsideFunctionBody)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugSymbols syms;
    ASSERT_TRUE(syms.LoadFromElf(path));

    // Midway through the function should still resolve to it.
    const FunctionSymbol* fn = syms.ResolvePc(kSystemDisableCache.addr + 40);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, kSystemDisableCache.name);
}

TEST(DebugSymbolsTest, FunctionEndBoundaryBelongsToNextFunction)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugSymbols syms;
    ASSERT_TRUE(syms.LoadFromElf(path));

    // system_disable_cache is [0x2498, 0x2498+84) = [0x2498, 0x24ec); 0x24ec is
    // exactly system_enable_cache's start.
    ASSERT_EQ(kSystemDisableCache.addr + kSystemDisableCache.size, kSystemEnableCache.addr);
    const FunctionSymbol* fn = syms.ResolvePc(kSystemEnableCache.addr);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, kSystemEnableCache.name);
}

TEST(DebugSymbolsTest, ResolvesSmallFunction)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugSymbols syms;
    ASSERT_TRUE(syms.LoadFromElf(path));

    const FunctionSymbol* fn = syms.ResolvePc(kGetCacheStatus.addr);
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->name, kGetCacheStatus.name);
}

TEST(DebugSymbolsTest, ReturnsNullForAddressPastEnd)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugSymbols syms;
    ASSERT_TRUE(syms.LoadFromElf(path));

    EXPECT_EQ(syms.ResolvePc(0xFFFFFFFF), nullptr);
}

TEST(DebugSymbolsTest, RejectsNonElfData)
{
    DebugSymbols syms;
    const uint8_t garbage[] = {'N', 'O', 'T', 'E', 'L', 'F', 0, 0, 1, 2, 3, 4};
    EXPECT_FALSE(syms.LoadFromElf(std::span<const uint8_t>(garbage, sizeof(garbage))));
    EXPECT_FALSE(syms.IsLoaded());
    EXPECT_EQ(syms.FunctionCount(), 0u);
}

TEST(DebugSymbolsTest, LoadMissingFileReturnsFalse)
{
    DebugSymbols syms;
    EXPECT_FALSE(syms.LoadFromElf("definitely_not_a_real_file.elf"));
    EXPECT_FALSE(syms.IsLoaded());
}

// --- Sibling .elf auto-discovery (the "when an exe is loaded, find the .elf"
//     requirement). These are hermetic: they build a temp directory. ---

TEST(DebugSymbolsTest, FindSiblingElfPrefersSameBaseName)
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "adm_debugsyms_same";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "a.exe") << "hunk";
    }
    {
        std::ofstream(dir / "a.elf") << "elf";
    }
    {
        std::ofstream(dir / "other.elf") << "elf";
    }

    auto found = DebugSymbols::FindSiblingElf((dir / "a.exe").string());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(fs::path(*found).filename().string(), "a.elf");

    fs::remove_all(dir);
}

TEST(DebugSymbolsTest, FindSiblingElfFallsBackToLoneElf)
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "adm_debugsyms_lone";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "prog.exe") << "hunk";
    }
    {
        std::ofstream(dir / "build.elf") << "elf";
    }

    auto found = DebugSymbols::FindSiblingElf((dir / "prog.exe").string());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(fs::path(*found).filename().string(), "build.elf");

    fs::remove_all(dir);
}

TEST(DebugSymbolsTest, FindSiblingElfAmbiguousReturnsNone)
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "adm_debugsyms_ambig";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "prog.exe") << "hunk";
    }
    {
        std::ofstream(dir / "one.elf") << "elf";
    }
    {
        std::ofstream(dir / "two.elf") << "elf";
    }

    auto found = DebugSymbols::FindSiblingElf((dir / "prog.exe").string());
    EXPECT_FALSE(found.has_value());

    fs::remove_all(dir);
}

TEST(DebugSymbolsTest, FindSiblingElfNoneReturnsNone)
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "adm_debugsyms_none";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "prog.exe") << "hunk";
    }

    auto found = DebugSymbols::FindSiblingElf((dir / "prog.exe").string());
    EXPECT_FALSE(found.has_value());

    fs::remove_all(dir);
}
