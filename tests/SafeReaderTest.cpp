#include <gtest/gtest.h>
#include <limits>
#include "core/BigEndian.h"
#include "core/SafeReader.h"

TEST(BigEndianTest, ReadsAllWidths)
{
    const uint8_t bytes[] = {0x12, 0x34, 0x56, 0x78};
    EXPECT_EQ(adm::core::be::U16(bytes), 0x1234u);
    EXPECT_EQ(adm::core::be::U24(bytes), 0x123456u);
    EXPECT_EQ(adm::core::be::U32(bytes), 0x12345678u);

    const uint8_t neg[] = {0xFF, 0xFF, 0xFF, 0xFE};
    EXPECT_EQ(adm::core::be::S32(neg), -2);
    EXPECT_EQ(adm::core::be::U16(neg), 0xFFFFu);
}

using adm::core::SafeReader;

TEST(SafeReaderTest, ReadU32BE)
{
    std::vector<uint8_t> data = {0x00, 0x00, 0x03, 0xF3}; // 0x3F3 = HUNK_HEADER
    SafeReader reader(data);
    EXPECT_EQ(reader.ReadU32BE(), 0x000003F3u);
    EXPECT_TRUE(reader.AtEnd());
}

TEST(SafeReaderTest, ReadU16BE)
{
    std::vector<uint8_t> data = {0x4A, 0xFC}; // 0x4AFC
    SafeReader reader(data);
    EXPECT_EQ(reader.ReadU16BE(), 0x4AFCu);
    EXPECT_TRUE(reader.AtEnd());
}

TEST(SafeReaderTest, ReadU8)
{
    std::vector<uint8_t> data = {0xFF};
    SafeReader reader(data);
    EXPECT_EQ(reader.ReadU8(), 0xFFu);
    EXPECT_TRUE(reader.AtEnd());
}

TEST(SafeReaderTest, HugeCountDoesNotWrapBoundsCheck)
{
    // A file-derived count near SIZE_MAX must fail the bounds check, not wrap m_pos + count
    // around to a small value and pass it.
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
    SafeReader reader(data);
    reader.ReadU16BE(); // m_pos = 2, so m_pos + SIZE_MAX-1 wraps to 1
    const size_t huge = std::numeric_limits<size_t>::max() - 1;
    EXPECT_FALSE(reader.HasBytes(huge));
    EXPECT_THROW(reader.Skip(huge), std::out_of_range);
    EXPECT_THROW(reader.ReadBytes(huge), std::out_of_range);
}

TEST(SafeReaderTest, ReadU32BEPastEnd)
{
    std::vector<uint8_t> data = {0x01, 0x02};
    SafeReader reader(data);
    EXPECT_THROW(reader.ReadU32BE(), std::out_of_range);
}

TEST(SafeReaderTest, ReadU16BEPastEnd)
{
    std::vector<uint8_t> data = {0x01};
    SafeReader reader(data);
    EXPECT_THROW(reader.ReadU16BE(), std::out_of_range);
}

TEST(SafeReaderTest, ReadU8PastEnd)
{
    std::vector<uint8_t> data = {};
    SafeReader reader(data);
    EXPECT_THROW(reader.ReadU8(), std::out_of_range);
}

TEST(SafeReaderTest, SkipValid)
{
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    SafeReader reader(data);
    reader.Skip(4);
    EXPECT_EQ(reader.GetPosition(), 4u);
    EXPECT_EQ(reader.ReadU32BE(), 0x05060708u);
}

TEST(SafeReaderTest, SkipPastEnd)
{
    std::vector<uint8_t> data = {0x01, 0x02};
    SafeReader reader(data);
    EXPECT_THROW(reader.Skip(3), std::out_of_range);
}

TEST(SafeReaderTest, ReadStringLW)
{
    // "TEST" followed by 4 null bytes = 2 longwords
    std::vector<uint8_t> data = {'T', 'E', 'S', 'T', 0, 0, 0, 0};
    SafeReader reader(data);
    EXPECT_EQ(reader.ReadStringLW(2), "TEST");
    EXPECT_TRUE(reader.AtEnd());
}

TEST(SafeReaderTest, ReadStringLWOverflow)
{
    std::vector<uint8_t> data = {0x01};
    SafeReader reader(data);
    // SIZE_MAX / 4 + 1 should overflow
    EXPECT_THROW(reader.ReadStringLW(SIZE_MAX / 4 + 1), std::out_of_range);
}

TEST(SafeReaderTest, SkipLW)
{
    std::vector<uint8_t> data(16, 0xAA);
    SafeReader reader(data);
    reader.SkipLW(3); // 12 bytes
    EXPECT_EQ(reader.GetPosition(), 12u);
    EXPECT_EQ(reader.GetRemaining(), 4u);
}

TEST(SafeReaderTest, PositionTracking)
{
    std::vector<uint8_t> data = {0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0xFF};
    SafeReader reader(data);

    EXPECT_EQ(reader.GetPosition(), 0u);
    EXPECT_EQ(reader.GetRemaining(), 7u);
    EXPECT_FALSE(reader.AtEnd());

    reader.ReadU32BE(); // 4 bytes
    EXPECT_EQ(reader.GetPosition(), 4u);
    EXPECT_EQ(reader.GetRemaining(), 3u);

    reader.ReadU16BE(); // 2 bytes
    EXPECT_EQ(reader.GetPosition(), 6u);
    EXPECT_EQ(reader.GetRemaining(), 1u);

    reader.ReadU8(); // 1 byte
    EXPECT_TRUE(reader.AtEnd());
    EXPECT_EQ(reader.GetRemaining(), 0u);
}

TEST(SafeReaderTest, EmptySpan)
{
    std::vector<uint8_t> data;
    SafeReader reader(data);

    EXPECT_TRUE(reader.AtEnd());
    EXPECT_EQ(reader.GetPosition(), 0u);
    EXPECT_EQ(reader.GetRemaining(), 0u);
    EXPECT_FALSE(reader.HasBytes(1));
    EXPECT_THROW(reader.ReadU8(), std::out_of_range);
}

TEST(SafeReaderTest, HasBytes)
{
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    SafeReader reader(data);

    EXPECT_TRUE(reader.HasBytes(3));
    EXPECT_FALSE(reader.HasBytes(4));
    reader.Skip(2);
    EXPECT_TRUE(reader.HasBytes(1));
    EXPECT_FALSE(reader.HasBytes(2));
}

TEST(SafeReaderTest, ReadBytesValid)
{
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
    SafeReader reader(data);
    auto bytes = reader.ReadBytes(3);
    EXPECT_EQ(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 0x01);
    EXPECT_EQ(bytes[1], 0x02);
    EXPECT_EQ(bytes[2], 0x03);
    EXPECT_EQ(reader.GetRemaining(), 1u);
}

TEST(SafeReaderTest, ReadBytesPastEnd)
{
    std::vector<uint8_t> data = {0x01};
    SafeReader reader(data);
    EXPECT_THROW(reader.ReadBytes(2), std::out_of_range);
}
