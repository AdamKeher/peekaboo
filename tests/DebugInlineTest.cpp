#include <gtest/gtest.h>

#include <filesystem>

#include "core/DebugInline.h"

using adm::core::DebugInline;

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

} // namespace

TEST(DebugInlineTest, LoadsScopes)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugInline di;
    ASSERT_TRUE(di.LoadFromElf(path));
    EXPECT_TRUE(di.IsLoaded());
    EXPECT_GT(di.ScopeCount(), 50u);
}

TEST(DebugInlineTest, NonInlinedFunctionHasSingleFrame)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugInline di;
    ASSERT_TRUE(di.LoadFromElf(path));

    // 0x2498 = system_disable_cache, no inlining (addr2line ground truth).
    auto stack = di.ResolveInlineStack(0x2498);
    ASSERT_EQ(stack.size(), 1u);
    EXPECT_EQ(stack[0], "system_disable_cache");
}

TEST(DebugInlineTest, ResolvesNestedInlineStack)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugInline di;
    ASSERT_TRUE(di.LoadFromElf(path));

    // 0x268e, from `addr2line -i -f`:
    //   system_wait_blt   (innermost)
    //   voxel_c2p_pass2
    //   voxel_c2p_kick    (enclosing function)
    // Our stack is outermost-first.
    auto stack = di.ResolveInlineStack(0x268e);
    ASSERT_EQ(stack.size(), 3u) << "expected voxel_c2p_kick > voxel_c2p_pass2 > system_wait_blt";
    EXPECT_EQ(stack[0], "voxel_c2p_kick");
    EXPECT_EQ(stack[1], "voxel_c2p_pass2");
    EXPECT_EQ(stack[2], "system_wait_blt");
}

TEST(DebugInlineTest, ResolvesEnclosingFunctionForNonInlinePc)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugInline di;
    ASSERT_TRUE(di.LoadFromElf(path));

    // 0x264c = voxel_c2p_kick, no inline at this PC (addr2line ground truth).
    auto stack = di.ResolveInlineStack(0x264c);
    ASSERT_GE(stack.size(), 1u);
    EXPECT_EQ(stack[0], "voxel_c2p_kick");
}

TEST(DebugInlineTest, UnmappedAddressReturnsEmpty)
{
    std::string path = FindFixture();
    if (path.empty()) {
        GTEST_SKIP() << "a.elf fixture not available";
    }
    DebugInline di;
    ASSERT_TRUE(di.LoadFromElf(path));

    EXPECT_TRUE(di.ResolveInlineStack(0xF0000000).empty());
}

TEST(DebugInlineTest, RejectsNonElf)
{
    DebugInline di;
    const uint8_t garbage[] = {'n', 'o', 'p', 'e', 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    EXPECT_FALSE(di.LoadFromElf(std::span<const uint8_t>(garbage, sizeof(garbage))));
    EXPECT_FALSE(di.IsLoaded());
}
