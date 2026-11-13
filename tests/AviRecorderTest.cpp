#include "core/AviRecorder.h"
#include "core/Subprocess.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// The AVI muxer is checked by reading the file back and walking its RIFF structure, not by
// trusting the writer's own counters: a container that does not parse is the whole failure mode.
namespace {

std::vector<uint8_t> ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

uint32_t U32(const std::vector<uint8_t>& b, size_t at)
{
    if (at + 4 > b.size())
        return 0;
    return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

uint16_t U16(const std::vector<uint8_t>& b, size_t at)
{
    if (at + 2 > b.size())
        return 0;
    return static_cast<uint16_t>(static_cast<uint16_t>(b[at]) |
                                 (static_cast<uint16_t>(b[at + 1]) << 8));
}

std::string Tag(const std::vector<uint8_t>& b, size_t at)
{
    if (at + 4 > b.size())
        return {};
    return std::string(reinterpret_cast<const char*>(b.data() + at), 4);
}

// Finds a top-level or nested chunk by tag, returning the offset of its FOURCC. Good enough for
// a file this shape: the tags used here appear once, except the movi payload chunks.
size_t Find(const std::vector<uint8_t>& b, const std::string& tag, size_t from = 0)
{
    for (size_t i = from; i + 4 <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, tag.data(), 4) == 0)
            return i;
    }
    return std::string::npos;
}

std::filesystem::path OutputPath(const char* name)
{
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "adm-avi-test";
    std::filesystem::create_directories(dir, ec);
    return dir / name;
}

struct Written
{
    std::filesystem::path path;
    int width = 0;
    int height = 0;
    int frames = 0;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t audioFramesPerVideoFrame = 0;
};

// Writes a small clip: a flat colour that changes per frame, and a square wave.
Written WriteClip(const char* name, int width, int height, int frames, uint32_t sampleRate,
                  uint32_t channels)
{
    Written w{OutputPath(name), width, height, frames, sampleRate, channels, 0};
    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = w.path.string();
    config.width = width;
    config.height = height;
    config.fps = 50;
    config.sampleRate = sampleRate;
    config.channels = channels;

    std::string error;
    EXPECT_TRUE(recorder.Begin(config, error)) << error;

    w.audioFramesPerVideoFrame = sampleRate / 50;
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    std::vector<int16_t> audio(static_cast<size_t>(w.audioFramesPerVideoFrame) * channels);

    for (int f = 0; f < frames; ++f) {
        for (size_t p = 0; p < rgba.size(); p += 4) {
            rgba[p + 0] = static_cast<uint8_t>(f * 10); // R
            rgba[p + 1] = static_cast<uint8_t>(f * 20); // G
            rgba[p + 2] = static_cast<uint8_t>(f * 30); // B
            rgba[p + 3] = 255;
        }
        EXPECT_TRUE(recorder.WriteVideoFrame(rgba.data(), width, height));
        if (sampleRate > 0) {
            for (size_t s = 0; s < audio.size(); ++s)
                audio[s] = static_cast<int16_t>((s % 32) < 16 ? 8000 : -8000);
            recorder.WriteAudio(audio.data(), w.audioFramesPerVideoFrame);
        }
    }
    EXPECT_TRUE(recorder.End());
    return w;
}

TEST(AviRecorder, WritesAParsableRiffAviContainer)
{
    const auto w = WriteClip("clip.avi", 32, 16, 4, 44100, 2);
    const auto bytes = ReadAll(w.path);
    ASSERT_GT(bytes.size(), 256u);

    EXPECT_EQ(Tag(bytes, 0), "RIFF");
    EXPECT_EQ(Tag(bytes, 8), "AVI ");
    // The RIFF size counts everything after its own size field, so it must land on the file end.
    EXPECT_EQ(U32(bytes, 4) + 8u, bytes.size())
        << "RIFF size " << U32(bytes, 4) << " vs file " << bytes.size();

    EXPECT_NE(Find(bytes, "hdrl"), std::string::npos);
    EXPECT_NE(Find(bytes, "avih"), std::string::npos);
    EXPECT_NE(Find(bytes, "movi"), std::string::npos);
    EXPECT_NE(Find(bytes, "idx1"), std::string::npos);
}

TEST(AviRecorder, MainHeaderCarriesTheRealTotals)
{
    const auto w = WriteClip("totals.avi", 32, 16, 5, 44100, 2);
    const auto bytes = ReadAll(w.path);

    const size_t avih = Find(bytes, "avih");
    ASSERT_NE(avih, std::string::npos);
    // avih fields, from the FOURCC: +0 tag, +4 size, +8 dwMicroSecPerFrame...
    EXPECT_EQ(U32(bytes, avih + 8), 20000u) << "50 fps is 20000 microseconds a frame";
    EXPECT_EQ(U32(bytes, avih + 20), 0x10u) << "AVIF_HASINDEX should be set";
    // The totals are the point: Begin() reserves the header with zeros and End() rewrites it.
    EXPECT_EQ(U32(bytes, avih + 24), static_cast<uint32_t>(w.frames)) << "dwTotalFrames";
    EXPECT_EQ(U32(bytes, avih + 32), 2u) << "dwStreams: video and audio";
    EXPECT_EQ(U32(bytes, avih + 40), static_cast<uint32_t>(w.width));
    EXPECT_EQ(U32(bytes, avih + 44), static_cast<uint32_t>(w.height));
}

TEST(AviRecorder, VideoIsBottomUpBgrWithPaddedRows)
{
    // A width of 30 is deliberately not a multiple of 4: 30*3 = 90 bytes, padded to 92. Getting
    // that wrong shears every row of the picture by two bytes.
    const auto w = WriteClip("stride.avi", 30, 4, 1, 0, 0);
    const auto bytes = ReadAll(w.path);

    const size_t strf = Find(bytes, "strf");
    ASSERT_NE(strf, std::string::npos);
    EXPECT_EQ(U32(bytes, strf + 8), 40u) << "biSize";
    EXPECT_EQ(U32(bytes, strf + 12), 30u) << "biWidth";
    EXPECT_EQ(U32(bytes, strf + 16), 4u) << "biHeight, positive for bottom-up rows";
    EXPECT_EQ(U16(bytes, strf + 22), 24u) << "biBitCount";
    EXPECT_EQ(U32(bytes, strf + 24), 0u) << "biCompression must be BI_RGB";
    EXPECT_EQ(U32(bytes, strf + 28), 92u * 4u) << "biSizeImage = padded stride * height";

    // The single video chunk should carry exactly that many bytes.
    const size_t chunk = Find(bytes, "00db");
    ASSERT_NE(chunk, std::string::npos);
    EXPECT_EQ(U32(bytes, chunk + 4), 92u * 4u);

    // Frame 0 was written as R=0,G=0,B=0 with alpha 255, so the pixels are black either way;
    // check a non-black frame instead for the channel order.
    const auto w2 = WriteClip("bgr.avi", 4, 2, 2, 0, 0);
    const auto b2 = ReadAll(w2.path);
    const size_t first = Find(b2, "00db");
    ASSERT_NE(first, std::string::npos);
    const size_t second = Find(b2, "00db", first + 4);
    ASSERT_NE(second, std::string::npos) << "the second frame's chunk";
    // Frame 1 was R=10, G=20, B=30. BI_RGB stores B,G,R.
    const size_t pixel = second + 8;
    EXPECT_EQ(b2[pixel + 0], 30) << "blue first";
    EXPECT_EQ(b2[pixel + 1], 20) << "then green";
    EXPECT_EQ(b2[pixel + 2], 10) << "then red";
}

// Orientation, pinned with an asymmetric frame: a uniform colour cannot catch a flip or a
// mirror, and BI_RGB's bottom-up convention makes both easy to get wrong.
TEST(AviRecorder, CornersLandWhereTheyWereWritten)
{
    constexpr int kW = 8;
    constexpr int kH = 4;
    const auto path = OutputPath("corners.avi");

    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = path.string();
    config.width = kW;
    config.height = kH;
    config.fps = 50;
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;

    // Every corner a different colour, so a horizontal mirror and a vertical flip are each
    // distinguishable from a correct frame and from one another.
    std::vector<uint8_t> rgba(static_cast<size_t>(kW) * kH * 4, 0);
    const auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = rgba.data() + (static_cast<size_t>(y) * kW + x) * 4;
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
    };
    put(0, 0, 255, 0, 0);             // top-left     red
    put(kW - 1, 0, 0, 255, 0);        // top-right    green
    put(0, kH - 1, 0, 0, 255);        // bottom-left  blue
    put(kW - 1, kH - 1, 255, 255, 0); // bottom-right yellow
    ASSERT_TRUE(recorder.WriteVideoFrame(rgba.data(), kW, kH));
    ASSERT_TRUE(recorder.End());

    const auto bytes = ReadAll(path);
    const size_t chunk = Find(bytes, "00db");
    ASSERT_NE(chunk, std::string::npos);
    const size_t pixels = chunk + 8;
    const uint32_t stride = (kW * 3u + 3u) & ~3u;

    // The stored frame is bottom-up: DIB row 0 is the picture's LAST row. Reading it back that
    // way must return the corners exactly as written, in BGR.
    const auto at = [&](int x, int y) -> std::array<uint8_t, 3> {
        const size_t dibRow = static_cast<size_t>(kH - 1 - y);
        const size_t off = pixels + dibRow * stride + static_cast<size_t>(x) * 3;
        return {bytes[off + 2], bytes[off + 1], bytes[off + 0]}; // BGR stored -> RGB returned
    };
    EXPECT_EQ(at(0, 0), (std::array<uint8_t, 3>{255, 0, 0})) << "top-left should be red";
    EXPECT_EQ(at(kW - 1, 0), (std::array<uint8_t, 3>{0, 255, 0})) << "top-right should be green";
    EXPECT_EQ(at(0, kH - 1), (std::array<uint8_t, 3>{0, 0, 255})) << "bottom-left should be blue";
    EXPECT_EQ(at(kW - 1, kH - 1), (std::array<uint8_t, 3>{255, 255, 0}))
        << "bottom-right should be yellow";
}

// The enlargement for each screen shape the core produces, at the PAL aspect it reports.
TEST(AviRecorder, ChoosesWholeNumberScalesTowardsTheDisplayShape)
{
    // What the core reports to the host for PAL (720/574), and what its own maths gives with the
    // 26:25 PAL pixel folded in. The choice must be the same either way.
    constexpr double kPal = 720.0 / 574.0;
    constexpr double kPalPixel = 1.3059;
    int sx = 0, sy = 0;

    adm::core::AviRecorder::ChooseScale(720, 287, kPalPixel, sx, sy);
    EXPECT_EQ(sx, 1);
    EXPECT_EQ(sy, 2);

    adm::core::AviRecorder::ChooseScale(720, 287, kPal, sx, sy); // hires width, one field
    EXPECT_EQ(sx, 1);
    EXPECT_EQ(sy, 2) << "each field line must be doubled, or the picture is squashed";

    adm::core::AviRecorder::ChooseScale(720, 574, kPal, sx, sy); // interlaced
    EXPECT_EQ(sx, 1);
    EXPECT_EQ(sy, 1) << "an interlaced frame already has both fields";

    adm::core::AviRecorder::ChooseScale(360, 574, kPal, sx, sy); // lores width, both fields
    EXPECT_EQ(sx, 2);
    EXPECT_EQ(sy, 1);

    adm::core::AviRecorder::ChooseScale(360, 287, kPal, sx, sy); // lores width, one field
    EXPECT_EQ(sx, 1);
    EXPECT_EQ(sy, 1) << "already close to square pixels";

    adm::core::AviRecorder::ChooseScale(720, 287, 0.0, sx, sy); // shape unknown
    EXPECT_EQ(sx, 1);
    EXPECT_EQ(sy, 1);
}

// Scaling replicates pixels; it never blends them. Every output pixel must equal a source pixel.
TEST(AviRecorder, ScaledFramesReplicatePixelsExactly)
{
    constexpr int kW = 3;
    constexpr int kH = 2;
    const auto path = OutputPath("scaled.avi");

    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = path.string();
    config.width = kW;
    config.height = kH;
    config.scaleX = 2;
    config.scaleY = 2;
    config.fps = 50;
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;
    EXPECT_EQ(recorder.OutputWidth(), 6);
    EXPECT_EQ(recorder.OutputHeight(), 4);

    // Six distinct colours, one per source pixel.
    std::vector<uint8_t> rgba(kW * kH * 4, 255);
    for (int i = 0; i < kW * kH; ++i) {
        rgba[i * 4 + 0] = static_cast<uint8_t>(10 + i);
        rgba[i * 4 + 1] = static_cast<uint8_t>(100 + i);
        rgba[i * 4 + 2] = static_cast<uint8_t>(200 + i);
    }
    ASSERT_TRUE(recorder.WriteVideoFrame(rgba.data(), kW, kH));
    ASSERT_TRUE(recorder.End());

    const auto bytes = ReadAll(path);
    const size_t avih = Find(bytes, "avih");
    ASSERT_NE(avih, std::string::npos);
    EXPECT_EQ(U32(bytes, avih + 40), 6u) << "the header describes the enlarged frame";
    EXPECT_EQ(U32(bytes, avih + 44), 4u);

    const size_t chunk = Find(bytes, "00db");
    ASSERT_NE(chunk, std::string::npos);
    const size_t pixels = chunk + 8;
    const uint32_t stride = (6u * 3u + 3u) & ~3u;
    for (int oy = 0; oy < 4; ++oy) {
        for (int ox = 0; ox < 6; ++ox) {
            const int source = (oy / 2) * kW + (ox / 2);
            const size_t off = pixels + static_cast<size_t>(4 - 1 - oy) * stride + ox * 3;
            EXPECT_EQ(bytes[off + 2], 10 + source) << "R at " << ox << "," << oy;
            EXPECT_EQ(bytes[off + 1], 100 + source) << "G at " << ox << "," << oy;
            EXPECT_EQ(bytes[off + 0], 200 + source) << "B at " << ox << "," << oy;
        }
    }
}

// vprp is the only place an AVI can carry the display shape; ffmpeg reads the frame aspect from
// it as numerator:denominator in the high and low words.
TEST(AviRecorder, WritesTheDisplayAspectIntoVprp)
{
    const auto path = OutputPath("aspect.avi");
    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = path.string();
    config.width = 8;
    config.height = 4;
    config.scaleY = 2;
    config.aspectNum = 13059;
    config.aspectDen = 10000;
    config.fps = 50;
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;
    std::vector<uint8_t> rgba(8 * 4 * 4, 128);
    ASSERT_TRUE(recorder.WriteVideoFrame(rgba.data(), 8, 4));
    ASSERT_TRUE(recorder.End());

    const auto bytes = ReadAll(path);
    const size_t vprp = Find(bytes, "vprp");
    ASSERT_NE(vprp, std::string::npos) << "no vprp chunk was written";
    EXPECT_EQ(U32(bytes, vprp + 4), 68u) << "chunk size";
    EXPECT_EQ(U32(bytes, vprp + 8 + 4), 1u) << "VideoStandard PAL";
    EXPECT_EQ(U32(bytes, vprp + 8 + 8), 50u) << "refresh";
    EXPECT_EQ(U32(bytes, vprp + 8 + 20), (13059u << 16) | 10000u) << "frame aspect";
    EXPECT_EQ(U32(bytes, vprp + 8 + 24), 8u) << "width in pixels";
    EXPECT_EQ(U32(bytes, vprp + 8 + 28), 8u) << "height in lines, after scaling";

    // The hdrl list must have grown to hold it, or every later chunk is misread.
    const size_t hdrl = Find(bytes, "hdrl");
    ASSERT_NE(hdrl, std::string::npos);
    const size_t movi = Find(bytes, "movi");
    ASSERT_NE(movi, std::string::npos);
    // LIST size field sits 4 bytes before 'hdrl'; the list ends where the movi LIST begins.
    EXPECT_EQ(hdrl + U32(bytes, hdrl - 4), movi - 8) << "hdrl size does not cover its contents";
}

// AVI 1.0 is limited to 4 GB. Past it the sizes wrap and nothing can open the file, so the
// recorder refuses the frame that would cross the limit and End() still writes a valid file.
TEST(AviRecorder, StopsBeforeTheFileLimitAndLeavesAValidFile)
{
    const auto path = OutputPath("full.avi");
    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = path.string();
    config.width = 64;
    config.height = 64; // 12 KB a frame
    config.fps = 50;
    config.sampleRate = 44100;
    config.maxFileBytes = 200 * 1024; // room for a dozen or so
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;

    std::vector<uint8_t> rgba(64 * 64 * 4, 90);
    std::vector<int16_t> audio(882 * 2, 1000);
    int accepted = 0;
    for (int i = 0; i < 100; ++i) {
        if (!recorder.WriteVideoFrame(rgba.data(), 64, 64))
            break;
        recorder.WriteAudio(audio.data(), 882);
        ++accepted;
    }
    EXPECT_TRUE(recorder.IsFull());
    EXPECT_GT(accepted, 0);
    EXPECT_LT(accepted, 100) << "the limit never stopped it";
    EXPECT_FALSE(recorder.WriteVideoFrame(rgba.data(), 64, 64)) << "full stays full";
    ASSERT_TRUE(recorder.End());

    const auto bytes = ReadAll(path);
    EXPECT_LE(bytes.size(), config.maxFileBytes) << "the file went past the limit it was given";
    EXPECT_EQ(U32(bytes, 4) + 8u, bytes.size()) << "the RIFF size must still be exact";
    const size_t avih = Find(bytes, "avih");
    ASSERT_NE(avih, std::string::npos);
    EXPECT_EQ(U32(bytes, avih + 24), static_cast<uint32_t>(accepted));
    EXPECT_NE(Find(bytes, "idx1"), std::string::npos);
}

// RGBA frames for the ZMBV tests: a scrolling pattern, a fixed bar and a moving block.
std::vector<uint8_t> PatternFrame(int w, int h, int scroll, int sprite)
{
    std::vector<uint8_t> f(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = f.data() + (static_cast<size_t>(y) * w + x) * 4;
            if (y < 6) {
                p[0] = 200;
                p[1] = static_cast<uint8_t>(x * 3);
                p[2] = 40;
                continue;
            }
            const int u = x + scroll;
            p[0] = static_cast<uint8_t>((u * 7) ^ (y * 5));
            p[1] = static_cast<uint8_t>((u / 3) * 13 + y);
            p[2] = static_cast<uint8_t>((u / 7 + y / 5) * 31);
        }
    }
    for (int j = 0; j < 6; ++j)
        for (int i = 0; i < 6; ++i) {
            const int x = (sprite * 5) % (w - 6) + i, y = 20 + j;
            uint8_t* p = f.data() + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = 255;
            p[1] = 0;
            p[2] = 255;
        }
    return f;
}

TEST(AviRecorder, ZmbvFilesDeclareTheCodecAndIndexOnlyKeyframesAsSeekable)
{
    const auto path = OutputPath("zmbv.avi");
    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = path.string();
    config.width = 64;
    config.height = 48;
    config.codec = adm::core::AviRecorder::VideoCodec::Zmbv;
    config.fps = 50;
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;
    for (int f = 0; f < 5; ++f) {
        const auto frame = PatternFrame(64, 48, f * 2, f);
        ASSERT_TRUE(recorder.WriteVideoFrame(frame.data(), 64, 48)) << "frame " << f;
    }
    ASSERT_TRUE(recorder.End());

    const auto bytes = ReadAll(path);
    EXPECT_EQ(U32(bytes, 4) + 8u, bytes.size());
    const size_t strh = Find(bytes, "strh");
    ASSERT_NE(strh, std::string::npos);
    EXPECT_EQ(Tag(bytes, strh + 12), "ZMBV") << "strh handler";
    const size_t strf = Find(bytes, "strf");
    ASSERT_NE(strf, std::string::npos);
    EXPECT_EQ(U16(bytes, strf + 22), 32u) << "biBitCount";
    EXPECT_EQ(Tag(bytes, strf + 24), "ZMBV") << "biCompression";
    EXPECT_EQ(Find(bytes, "00db"), std::string::npos) << "compressed frames are 00dc chunks";

    const size_t idx1 = Find(bytes, "idx1");
    ASSERT_NE(idx1, std::string::npos);
    ASSERT_EQ(U32(bytes, idx1 + 4), 5u * 16u);
    for (int i = 0; i < 5; ++i) {
        const size_t e = idx1 + 8 + i * 16;
        EXPECT_EQ(Tag(bytes, e), "00dc");
        EXPECT_EQ(U32(bytes, e + 4), i == 0 ? 0x10u : 0u) << "only the first frame is a keyframe";
    }
    // A delta of a mostly unchanged frame is tiny compared with the raw 64x48x4.
    EXPECT_LT(U32(bytes, idx1 + 8 + 16 + 12), 64u * 48u * 4u);
}

// The real decoder, if this machine has it: every pixel ffmpeg decodes must be the one written,
// scrolling and scaling included. Skipped where ffmpeg is not on PATH.
TEST(AviRecorder, FfmpegDecodesZmbvToExactlyTheFramesWritten)
{
    constexpr int kW = 64, kH = 48, kFrames = 14;
    const auto path = OutputPath("zmbv-ffmpeg.avi");
    const auto decoded = OutputPath("zmbv-ffmpeg.raw");
    std::error_code ec;
    std::filesystem::remove(decoded, ec);

    std::vector<std::vector<uint8_t>> frames;
    {
        adm::core::AviRecorder recorder;
        adm::core::AviRecorder::Config config;
        config.path = path.string();
        config.width = kW;
        config.height = kH;
        config.scaleY = 2; // the line doubling a PAL capture gets
        config.codec = adm::core::AviRecorder::VideoCodec::Zmbv;
        config.fps = 50;
        config.sampleRate = 44100;
        std::string error;
        ASSERT_TRUE(recorder.Begin(config, error)) << error;
        std::vector<int16_t> audio(882 * 2, 0);
        for (int f = 0; f < kFrames; ++f) {
            const int scroll = f < 7 ? f * 3 : 21 - (f - 7) * 2; // right, then back left
            frames.push_back(PatternFrame(kW, kH, scroll, f));
            ASSERT_TRUE(recorder.WriteVideoFrame(frames.back().data(), kW, kH));
            recorder.WriteAudio(audio.data(), 882);
        }
        ASSERT_TRUE(recorder.End());
    }

    adm::core::Subprocess ffmpeg;
    std::string log;
    const int rc = ffmpeg.Run({"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
                               "-i", path.string(), "-map", "0:v", "-f", "rawvideo", "-pix_fmt",
                               "bgra", decoded.string()},
                              [&](const std::string& line) { log += line + "\n"; });
    if (rc == -1)
        GTEST_SKIP() << "ffmpeg is not on PATH";
    ASSERT_EQ(rc, 0) << "ffmpeg could not decode the file:\n" << log;

    const auto raw = ReadAll(decoded);
    const size_t outFrame = static_cast<size_t>(kW) * (kH * 2) * 4;
    ASSERT_EQ(raw.size(), outFrame * kFrames) << "ffmpeg decoded a different number of frames";
    for (int f = 0; f < kFrames; ++f) {
        for (int y = 0; y < kH * 2; ++y) {
            for (int x = 0; x < kW; ++x) {
                const uint8_t* want = frames[f].data() + (static_cast<size_t>(y / 2) * kW + x) * 4;
                const uint8_t* got =
                    raw.data() + f * outFrame + (static_cast<size_t>(y) * kW + x) * 4;
                // decoded is BGRA; written was RGBA
                if (got[0] != want[2] || got[1] != want[1] || got[2] != want[0]) {
                    FAIL() << "frame " << f << " pixel (" << x << "," << y << "): ffmpeg gives BGR "
                           << int(got[0]) << "," << int(got[1]) << "," << int(got[2])
                           << ", written RGB " << int(want[0]) << "," << int(want[1]) << ","
                           << int(want[2]);
                }
            }
        }
    }
}

TEST(AviRecorder, AudioStreamDescribesThePcmItWrote)
{
    const auto w = WriteClip("audio.avi", 16, 16, 3, 22050, 2);
    const auto bytes = ReadAll(w.path);

    // The second 'strf' is the audio one (WAVEFORMATEX).
    const size_t videoStrf = Find(bytes, "strf");
    ASSERT_NE(videoStrf, std::string::npos);
    const size_t audioStrf = Find(bytes, "strf", videoStrf + 4);
    ASSERT_NE(audioStrf, std::string::npos) << "no audio stream format was written";

    EXPECT_EQ(U16(bytes, audioStrf + 8), 1u) << "WAVE_FORMAT_PCM";
    EXPECT_EQ(U16(bytes, audioStrf + 10), 2u) << "channels";
    EXPECT_EQ(U32(bytes, audioStrf + 12), 22050u) << "sample rate";
    EXPECT_EQ(U32(bytes, audioStrf + 16), 22050u * 4u) << "bytes a second";
    EXPECT_EQ(U16(bytes, audioStrf + 20), 4u) << "block align";
    EXPECT_EQ(U16(bytes, audioStrf + 22), 16u) << "bits a sample";

    // One audio chunk per video frame, each holding that frame's worth of samples. The search
    // stops at idx1: the index repeats every chunk's FOURCC, and there the following field is
    // the flags word, not a length.
    const size_t idx1 = Find(bytes, "idx1");
    ASSERT_NE(idx1, std::string::npos);
    size_t at = Find(bytes, "01wb");
    ASSERT_NE(at, std::string::npos);
    int audioChunks = 0;
    while (at != std::string::npos && at < idx1) {
        EXPECT_EQ(U32(bytes, at + 4), w.audioFramesPerVideoFrame * 2u * sizeof(int16_t))
            << "audio chunk " << audioChunks;
        ++audioChunks;
        at = Find(bytes, "01wb", at + 4);
    }
    EXPECT_EQ(audioChunks, w.frames) << "one audio chunk per video frame";
}

TEST(AviRecorder, IndexOffsetsPointAtTheChunksTheyDescribe)
{
    const auto w = WriteClip("index.avi", 16, 8, 3, 44100, 2);
    const auto bytes = ReadAll(w.path);

    const size_t movi = Find(bytes, "movi");
    const size_t idx1 = Find(bytes, "idx1");
    ASSERT_NE(movi, std::string::npos);
    ASSERT_NE(idx1, std::string::npos);

    const uint32_t indexBytes = U32(bytes, idx1 + 4);
    ASSERT_EQ(indexBytes % 16u, 0u);
    const uint32_t entries = indexBytes / 16u;
    EXPECT_EQ(entries, static_cast<uint32_t>(w.frames * 2)) << "a video and an audio chunk each";

    for (uint32_t i = 0; i < entries; ++i) {
        const size_t e = idx1 + 8 + i * 16u;
        const std::string tag = Tag(bytes, e);
        const uint32_t offset = U32(bytes, e + 8);
        const uint32_t length = U32(bytes, e + 12);
        // Offsets are relative to the 'movi' FOURCC - the convention every reader assumes.
        const size_t chunkAt = movi + offset;
        ASSERT_LT(chunkAt + 8, bytes.size()) << "entry " << i << " points past the file";
        EXPECT_EQ(Tag(bytes, chunkAt), tag) << "entry " << i << " does not point at its chunk";
        EXPECT_EQ(U32(bytes, chunkAt + 4), length) << "entry " << i << " length disagrees";
    }
}

TEST(AviRecorder, RefusesAFrameOfADifferentSize)
{
    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = OutputPath("mismatch.avi").string();
    config.width = 16;
    config.height = 8;
    config.fps = 50;
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;

    std::vector<uint8_t> wrong(32 * 8 * 4, 0);
    EXPECT_FALSE(recorder.WriteVideoFrame(wrong.data(), 32, 8))
        << "a resolution change mid-recording is a different film, not a frame to stretch";
    EXPECT_EQ(recorder.FramesWritten(), 0u);
    recorder.End();
}

TEST(AviRecorder, RejectsAnImpossibleConfiguration)
{
    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = OutputPath("bad.avi").string();
    config.width = 0;
    config.height = 8;
    config.fps = 50;
    std::string error;
    EXPECT_FALSE(recorder.Begin(config, error));
    EXPECT_FALSE(error.empty());

    config.width = 16;
    config.fps = 0;
    EXPECT_FALSE(recorder.Begin(config, error));
    EXPECT_FALSE(error.empty());
}

TEST(AviRecorder, AVideoOnlyFileHasOneStream)
{
    const auto w = WriteClip("silent.avi", 16, 8, 2, 0, 0);
    const auto bytes = ReadAll(w.path);
    const size_t avih = Find(bytes, "avih");
    ASSERT_NE(avih, std::string::npos);
    EXPECT_EQ(U32(bytes, avih + 32), 1u) << "dwStreams";
    EXPECT_EQ(Find(bytes, "01wb"), std::string::npos) << "no audio chunks should be written";
}

} // namespace

// EMU-04: a stream that goes bad (a full disk) fails the recording; it is never "Saved"
TEST(AviRecorder, AWriteThatFailsEndsTheRecordingAndIsNotSaved)
{
    const auto path = OutputPath("failing.avi");
    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = path.string();
    config.width = 64;
    config.height = 64;
    config.fps = 50;
    config.sampleRate = 44100;
    config.failAfterBytes = 100 * 1024;
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;

    std::vector<uint8_t> rgba(64 * 64 * 4, 90);
    std::vector<int16_t> audio(882 * 2, 1000);
    int accepted = 0;
    for (int i = 0; i < 100; ++i) {
        if (!recorder.WriteVideoFrame(rgba.data(), 64, 64))
            break;
        recorder.WriteAudio(audio.data(), 882);
        ++accepted;
    }
    EXPECT_TRUE(recorder.HasFailed());
    EXPECT_FALSE(recorder.IsFull());
    EXPECT_LT(accepted, 100);
    EXPECT_FALSE(recorder.WriteVideoFrame(rgba.data(), 64, 64)) << "failed stays failed";
    EXPECT_FALSE(recorder.End());
    EXPECT_NE(recorder.ResultMessage().rfind("Saved", 0), 0u) << recorder.ResultMessage();
    EXPECT_NE(recorder.ResultMessage().find("incomplete"), std::string::npos);
}

// EMU-03: a frame of another size ends the film (the file kept, the reason given), and the
// sound with no picture to carry it stays bounded
TEST(AviRecorder, ASizeChangeStopsTheRecordingAndTheSoundStaysBounded)
{
    const auto path = OutputPath("resized.avi");
    adm::core::AviRecorder recorder;
    adm::core::AviRecorder::Config config;
    config.path = path.string();
    config.width = 4;
    config.height = 4;
    config.fps = 50;
    config.sampleRate = 44100;
    std::string error;
    ASSERT_TRUE(recorder.Begin(config, error)) << error;

    std::vector<uint8_t> small(4 * 4 * 4, 90), big(8 * 8 * 4, 90);
    std::vector<int16_t> audio(882 * 2, 1000);
    ASSERT_TRUE(recorder.WriteVideoFrame(small.data(), 4, 4));
    for (int i = 0; i < 150; ++i) { // 3 s of sound with no picture
        recorder.WriteAudio(audio.data(), 882);
        EXPECT_FALSE(recorder.WriteVideoFrame(big.data(), 8, 8));
    }
    EXPECT_TRUE(recorder.HasFailed());
    EXPECT_EQ(150u * 882u - recorder.AudioFramesDropped(), 2u * 44100u); // capped at 2 s
    ASSERT_TRUE(recorder.End()); // a valid file of what came before
    EXPECT_NE(recorder.ResultMessage().find("changed size (4x4 -> 8x8)"), std::string::npos)
        << recorder.ResultMessage();
    const auto bytes = ReadAll(path);
    EXPECT_EQ(U32(bytes, 4) + 8u, bytes.size());
    EXPECT_NE(Find(bytes, "idx1"), std::string::npos);
}
