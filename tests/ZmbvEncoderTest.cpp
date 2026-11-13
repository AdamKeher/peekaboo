#include "core/ZmbvEncoder.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <zlib.h>

// ZMBV is only worth having if what it writes decodes to exactly what went in. These tests decode
// every packet with a reference decoder written from ffmpeg's (libavcodec/zmbv.c) - a zlib stream
// reset at keyframes and inflated with Z_SYNC_FLUSH, motion vectors as signed bytes shifted right
// one, out-of-frame source pixels read as zero, and a 32-bit XOR - and compare pixel for pixel.
// Compatibility with the real ffmpeg is checked end to end in AviRecorderTest.
namespace {

class ReferenceDecoder
{
public:
    ReferenceDecoder(int w, int h) : m_w(w), m_h(h), m_cur(w * h * 4, 0), m_prev(w * h * 4, 0) {}
    ~ReferenceDecoder()
    {
        if (m_init)
            inflateEnd(&m_zs);
    }

    // Returns false with a reason if the packet is malformed.
    bool Decode(const uint8_t* data, size_t size, std::string& why)
    {
        if (size < 1) {
            why = "empty packet";
            return false;
        }
        const uint8_t flags = data[0];
        size_t pos = 1;
        const bool key = (flags & 1) != 0;
        if (key) {
            if (size < 7) {
                why = "short keyframe header";
                return false;
            }
            if (data[1] != 0 || data[2] != 1) {
                why = "version is not 0.1";
                return false;
            }
            if (data[3] != 1) {
                why = "not zlib-compressed";
                return false;
            }
            if (data[4] != 8) {
                why = "not 32 bpp";
                return false;
            }
            m_bw = data[5];
            m_bh = data[6];
            pos = 7;
            if (m_init)
                inflateEnd(&m_zs);
            m_zs = z_stream{};
            inflateInit(&m_zs);
            m_init = true;
            m_gotKey = true;
        }
        if (!m_gotKey) {
            why = "delta before any keyframe";
            return false;
        }

        std::vector<uint8_t> out(static_cast<size_t>(m_w) * m_h * 4 + 1024 * 1024);
        m_zs.next_in = const_cast<Bytef*>(data + pos);
        m_zs.avail_in = static_cast<uInt>(size - pos);
        m_zs.next_out = out.data();
        m_zs.avail_out = static_cast<uInt>(out.size());
        const uLong before = m_zs.total_out;
        const int ret = inflate(&m_zs, Z_SYNC_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            why = "inflate failed";
            return false;
        }
        const size_t len = m_zs.total_out - before;

        const size_t frameBytes = static_cast<size_t>(m_w) * m_h * 4;
        m_prev = m_cur;
        if (key) {
            // ffmpeg rejects a keyframe whose inflated size is not exactly one frame.
            if (len != frameBytes) {
                why = "keyframe inflated to " + std::to_string(len) + " bytes, not " +
                      std::to_string(frameBytes);
                return false;
            }
            std::memcpy(m_cur.data(), out.data(), frameBytes);
            return true;
        }

        const int bx = (m_w + m_bw - 1) / m_bw;
        const int by = (m_h + m_bh - 1) / m_bh;
        const size_t vectorBytes = (static_cast<size_t>(bx) * by * 2 + 3) & ~size_t{3};
        if (len < vectorBytes) {
            why = "delta shorter than its motion vectors";
            return false;
        }
        const int8_t* mv = reinterpret_cast<const int8_t*>(out.data());
        size_t src = vectorBytes;
        int block = 0;
        for (int y = 0; y < m_h; y += m_bh) {
            const int bh2 = std::min(m_bh, m_h - y);
            for (int x = 0; x < m_w; x += m_bw) {
                const int bw2 = std::min(m_bw, m_w - x);
                const bool xored = (mv[block] & 1) != 0;
                const int dx = mv[block] >> 1;
                const int dy = mv[block + 1] >> 1;
                block += 2;
                for (int j = 0; j < bh2; ++j) {
                    for (int i = 0; i < bw2; ++i) {
                        const int sx = x + dx + i;
                        const int sy = y + dy + j;
                        uint32_t px = 0;
                        if (sx >= 0 && sx < m_w && sy >= 0 && sy < m_h)
                            std::memcpy(
                                &px, m_prev.data() + (static_cast<size_t>(sy) * m_w + sx) * 4, 4);
                        if (xored) {
                            if (src + 4 > len) {
                                why = "XOR data runs past the payload";
                                return false;
                            }
                            uint32_t x32 = 0;
                            std::memcpy(&x32, out.data() + src, 4);
                            px ^= x32;
                            src += 4;
                        }
                        std::memcpy(m_cur.data() + (static_cast<size_t>(y + j) * m_w + x + i) * 4,
                                    &px, 4);
                    }
                }
            }
        }
        if (src != len) {
            why = "payload has " + std::to_string(len - src) + " bytes left over";
            return false;
        }
        return true;
    }

    const std::vector<uint8_t>& Frame() const { return m_cur; }

private:
    int m_w, m_h;
    int m_bw = 16, m_bh = 16;
    bool m_gotKey = false;
    bool m_init = false;
    z_stream m_zs{};
    std::vector<uint8_t> m_cur, m_prev;
};

// A frame that looks like a game: a patterned playfield that can be scrolled, a static status bar,
// and a few moving sprites.
std::vector<uint8_t> MakeFrame(int w, int h, int scrollX, int scrollY, int sprite, int barHeight)
{
    std::vector<uint8_t> f(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = f.data() + (static_cast<size_t>(y) * w + x) * 4;
            if (y < barHeight) { // status bar: never moves
                p[0] = 30;
                p[1] = static_cast<uint8_t>((x / 8) * 11);
                p[2] = 200;
                continue;
            }
            const int u = x + scrollX;
            const int v = y + scrollY;
            p[0] = static_cast<uint8_t>((u * 7) ^ (v * 3));
            p[1] = static_cast<uint8_t>((u / 5) * 17 + v);
            p[2] = static_cast<uint8_t>(((u / 13) + (v / 9)) * 29);
        }
    }
    // A sprite that moves independently.
    for (int j = 0; j < 12; ++j) {
        for (int i = 0; i < 12; ++i) {
            const int x = 40 + sprite * 3 + i;
            const int y = barHeight + 20 + j;
            if (x < w && y < h) {
                uint8_t* p = f.data() + (static_cast<size_t>(y) * w + x) * 4;
                p[0] = 255;
                p[1] = 255;
                p[2] = 0;
            }
        }
    }
    return f;
}

void ExpectSameFrame(const std::vector<uint8_t>& decoded, const std::vector<uint8_t>& original,
                     int w, const std::string& what)
{
    ASSERT_EQ(decoded.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        if (decoded[i] != original[i]) {
            const size_t px = i / 4;
            FAIL() << what << ": first difference at pixel (" << (px % w) << "," << (px / w)
                   << ") byte " << (i % 4) << ": decoded " << int(decoded[i]) << " expected "
                   << int(original[i]);
        }
    }
}

TEST(ZmbvEncoder, KeyframeRoundTripsExactly)
{
    constexpr int w = 64, h = 48;
    adm::core::ZmbvEncoder enc;
    std::string error;
    ASSERT_TRUE(enc.Begin(w, h, 300, error)) << error;
    ReferenceDecoder dec(w, h);

    const auto frame = MakeFrame(w, h, 0, 0, 0, 8);
    const auto packet = enc.EncodeFrame(frame.data());
    ASSERT_GT(packet.size, 0u);
    EXPECT_TRUE(packet.keyframe) << "the first frame must be a keyframe";
    std::string why;
    ASSERT_TRUE(dec.Decode(packet.data, packet.size, why)) << why;
    ExpectSameFrame(dec.Frame(), frame, w, "keyframe");
}

TEST(ZmbvEncoder, AnUnchangedFrameCostsAlmostNothing)
{
    constexpr int w = 720, h = 574;
    adm::core::ZmbvEncoder enc;
    std::string error;
    ASSERT_TRUE(enc.Begin(w, h, 300, error)) << error;
    ReferenceDecoder dec(w, h);

    const auto frame = MakeFrame(w, h, 0, 0, 0, 20);
    std::string why;
    const auto key = enc.EncodeFrame(frame.data());
    ASSERT_TRUE(dec.Decode(key.data, key.size, why)) << why;
    const auto same = enc.EncodeFrame(frame.data());
    ASSERT_GT(same.size, 0u);
    EXPECT_FALSE(same.keyframe);
    EXPECT_LT(same.size, 200u) << "a repeated frame should be a few bytes of zero vectors";
    EXPECT_EQ(enc.LastCleanBlocks(), 45 * 36) << "every block should need no XOR";
    ASSERT_TRUE(dec.Decode(same.data, same.size, why)) << why;
    ExpectSameFrame(dec.Frame(), frame, w, "repeat");
}

// Scrolling is what the global motion search is for, and it is where a sign or shift mistake in
// the vectors would corrupt the picture. Both directions on both axes, and diagonally.
TEST(ZmbvEncoder, ScrollingIsEncodedAsMotionAndDecodesExactly)
{
    constexpr int w = 320, h = 200;
    struct Step
    {
        int sx, sy;
    };
    const Step path[] = {{0, 0}, {2, 0}, {4, 0},  {3, 0},   {3, 2},   {3, 5},
                         {3, 3}, {0, 1}, {-4, 1}, {-9, -2}, {-9, -2}, {5, 7}};

    adm::core::ZmbvEncoder enc;
    std::string error;
    ASSERT_TRUE(enc.Begin(w, h, 300, error)) << error;
    ReferenceDecoder dec(w, h);

    int movedFrames = 0;
    for (size_t i = 0; i < std::size(path); ++i) {
        const auto frame = MakeFrame(w, h, path[i].sx, path[i].sy, static_cast<int>(i), 16);
        const auto packet = enc.EncodeFrame(frame.data());
        ASSERT_GT(packet.size, 0u) << "frame " << i;
        if (i > 0 && (path[i].sx != path[i - 1].sx || path[i].sy != path[i - 1].sy) &&
            enc.LastMovedBlocks() > 0)
            ++movedFrames;
        std::string why;
        ASSERT_TRUE(dec.Decode(packet.data, packet.size, why)) << "frame " << i << ": " << why;
        ExpectSameFrame(dec.Frame(), frame, w, "frame " + std::to_string(i));
    }
    EXPECT_GE(movedFrames, 8) << "scrolled frames should be coded with motion vectors";
}

TEST(ZmbvEncoder, ArbitraryChangesAndOddSizesDecodeExactly)
{
    // 37x23: neither dimension is a multiple of the 16-pixel block.
    constexpr int w = 37, h = 23;
    adm::core::ZmbvEncoder enc;
    std::string error;
    ASSERT_TRUE(enc.Begin(w, h, 5, error)) << error; // keyframes every 5, to cross a few
    ReferenceDecoder dec(w, h);

    std::mt19937 rng(1234);
    std::vector<uint8_t> frame(static_cast<size_t>(w) * h * 4, 0);
    int keyframes = 0;
    for (int f = 0; f < 23; ++f) {
        // Change a random handful of pixels; the fourth byte stays zero as BGRX requires.
        for (int n = 0; n < 15; ++n) {
            const size_t px = rng() % (static_cast<size_t>(w) * h);
            frame[px * 4 + 0] = static_cast<uint8_t>(rng());
            frame[px * 4 + 1] = static_cast<uint8_t>(rng());
            frame[px * 4 + 2] = static_cast<uint8_t>(rng());
        }
        const auto packet = enc.EncodeFrame(frame.data());
        ASSERT_GT(packet.size, 0u);
        if (packet.keyframe)
            ++keyframes;
        std::string why;
        ASSERT_TRUE(dec.Decode(packet.data, packet.size, why)) << "frame " << f << ": " << why;
        ExpectSameFrame(dec.Frame(), frame, w, "frame " + std::to_string(f));
    }
    EXPECT_EQ(keyframes, 5) << "frames 0, 5, 10, 15 and 20";
}

TEST(ZmbvEncoder, RejectsAnImpossibleSize)
{
    adm::core::ZmbvEncoder enc;
    std::string error;
    EXPECT_FALSE(enc.Begin(0, 10, 300, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(enc.EncodeFrame(nullptr).size, 0u);
}

// Not part of the suite: measures size and speed on a real capture. Point ADM_ZMBV_BENCH at an
// uncompressed RGB24 raw file (ffmpeg -i capture.avi -f rawvideo -pix_fmt bgra out.raw) with
// ADM_ZMBV_BENCH_SIZE=WxH, then run with --gtest_also_run_disabled_tests.
TEST(ZmbvEncoder, DISABLED_BenchmarkOnARealCapture)
{
    const char* path = std::getenv("ADM_ZMBV_BENCH");
    const char* size = std::getenv("ADM_ZMBV_BENCH_SIZE");
    if (!path || !size)
        GTEST_SKIP() << "set ADM_ZMBV_BENCH and ADM_ZMBV_BENCH_SIZE";
    int w = 0, h = 0;
    ASSERT_EQ(std::sscanf(size, "%dx%d", &w, &h), 2);
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in) << path;

    adm::core::ZmbvEncoder enc;
    std::string error;
    ASSERT_TRUE(enc.Begin(w, h, 300, error)) << error;
    ReferenceDecoder dec(w, h);

    std::vector<uint8_t> frame(static_cast<size_t>(w) * h * 4);
    uint64_t rawBytes = 0, packedBytes = 0;
    int frames = 0, moved = 0;
    double encodeSeconds = 0;
    while (in.read(reinterpret_cast<char*>(frame.data()),
                   static_cast<std::streamsize>(frame.size()))) {
        for (size_t i = 3; i < frame.size(); i += 4)
            frame[i] = 0; // BGRA from ffmpeg: the fourth byte must be zero for BGRX
        const auto t0 = std::chrono::steady_clock::now();
        const auto packet = enc.EncodeFrame(frame.data());
        encodeSeconds +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ASSERT_GT(packet.size, 0u);
        if (enc.LastMovedBlocks() > 0)
            ++moved;
        std::string why;
        ASSERT_TRUE(dec.Decode(packet.data, packet.size, why)) << "frame " << frames << ": " << why;
        if (frames % 97 == 0)
            ExpectSameFrame(dec.Frame(), frame, w, "frame " + std::to_string(frames));
        rawBytes += frame.size();
        packedBytes += packet.size;
        ++frames;
    }
    std::printf("ZMBV bench: %d frames %dx%d, %.1f MB -> %.2f MB (%.1fx), %.2f MB/s at 50 fps, "
                "%.2f ms/frame, %d frames with motion\n",
                frames, w, h, rawBytes / 1048576.0, packedBytes / 1048576.0,
                packedBytes ? double(rawBytes) / packedBytes : 0.0,
                frames ? packedBytes / 1048576.0 / (frames / 50.0) : 0.0,
                frames ? encodeSeconds * 1000.0 / frames : 0.0, moved);
}

} // namespace
