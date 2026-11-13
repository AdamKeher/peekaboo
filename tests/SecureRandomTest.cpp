#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "core/SecureRandom.h"

TEST(SecureRandomTest, HexHasRequestedLengthAndCharset)
{
    const std::string token = adm::core::SecureRandomHex(16);
    ASSERT_EQ(token.size(), 32u);
    for (char c : token)
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "bad char: " << c;
}

TEST(SecureRandomTest, ConsecutiveTokensDiffer)
{
    // 128-bit values colliding means the OS source is broken (or we truncated it).
    EXPECT_NE(adm::core::SecureRandomHex(16), adm::core::SecureRandomHex(16));
}

TEST(SecureRandomTest, BytesFillsBuffer)
{
    uint8_t buf[64];
    std::memset(buf, 0, sizeof(buf));
    ASSERT_TRUE(adm::core::SecureRandomBytes(buf, sizeof(buf)));
    // All-zero output after a successful call is astronomically unlikely.
    bool anyNonZero = false;
    for (uint8_t b : buf)
        anyNonZero |= (b != 0);
    EXPECT_TRUE(anyNonZero);

    EXPECT_TRUE(adm::core::SecureRandomBytes(buf, 0));
}
