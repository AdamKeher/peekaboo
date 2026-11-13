#include "AviRecorder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "CaptureFrame.h"
#include "Logger.h"
#include "ZmbvEncoder.h"

namespace adm::core {
namespace {

constexpr uint32_t Fourcc(const char (&s)[5])
{
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
}

constexpr uint32_t kVideoChunk = Fourcc("00db");           // stream 0, uncompressed DIB
constexpr uint32_t kVideoChunkCompressed = Fourcc("00dc"); // stream 0, compressed frame
constexpr uint32_t kZmbv = Fourcc("ZMBV");
// A keyframe every six seconds at 50 fps, as DOSBox does: often enough to seek, rare enough that
// the full frames do not dominate the file.
constexpr int kZmbvKeyframeInterval = 300;
constexpr uint32_t kAudioChunk = Fourcc("01wb"); // stream 1, waveform bytes
constexpr uint32_t kKeyframe = 0x00000010;       // AVIIF_KEYFRAME
constexpr uint32_t kHasIndex = 0x00000010;       // AVIF_HASINDEX

// Every frame of BI_RGB is a keyframe, so the whole file is seekable.
void PutU32(std::ofstream& f, uint32_t v)
{
    unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                          static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
    f.write(reinterpret_cast<const char*>(b), 4);
}

void PutU16(std::ofstream& f, uint16_t v)
{
    unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    f.write(reinterpret_cast<const char*>(b), 2);
}

} // namespace

AviRecorder::AviRecorder() = default;

AviRecorder::~AviRecorder() { End(); }

void AviRecorder::ChooseScale(int width, int height, double displayAspect, int& scaleX, int& scaleY)
{
    scaleX = 1;
    scaleY = 1;
    if (width <= 0 || height <= 0 || !(displayAspect > 0.0))
        return;
    const auto clampScale = [](double v) {
        const long rounded = std::lround(v);
        return static_cast<int>(std::clamp(rounded, 1L, 4L));
    };
    // Rows first: the height a square-pixel picture of this width would have, in source rows.
    scaleY = clampScale((width / displayAspect) / height);
    // Then columns, against the height just chosen, for a buffer narrower than its shape.
    scaleX = clampScale((height * scaleY * displayAspect) / width);
}

bool AviRecorder::Begin(const Config& config, std::string& outError)
{
    End();
    outError.clear();

    if (config.width <= 0 || config.height <= 0) {
        outError = "the frame size is not known yet";
        return false;
    }
    if (config.fps <= 0) {
        outError = "the frame rate must be positive";
        return false;
    }
    if (config.scaleX < 1 || config.scaleY < 1 || config.scaleX > 8 || config.scaleY > 8) {
        outError = "the scale factors must be between 1 and 8";
        return false;
    }
    if (config.aspectNum < 0 || config.aspectDen < 0 || config.aspectNum > 0xFFFF ||
        config.aspectDen > 0xFFFF || ((config.aspectNum == 0) != (config.aspectDen == 0))) {
        outError = "the display aspect must be two 16-bit values, or none";
        return false;
    }

    m_config = config;
    m_outWidth = config.width * config.scaleX;
    m_outHeight = config.height * config.scaleY;
    if (config.codec == VideoCodec::Zmbv) {
        // ZMBV stores BGRX rows, four bytes a pixel, with no padding.
        m_frameBytes = static_cast<uint32_t>(m_outWidth) * static_cast<uint32_t>(m_outHeight) * 4u;
        m_zmbv = std::make_unique<ZmbvEncoder>();
        if (!m_zmbv->Begin(m_outWidth, m_outHeight, kZmbvKeyframeInterval, outError)) {
            m_zmbv.reset();
            return false;
        }
        m_maxVideoChunk = static_cast<uint32_t>(m_zmbv->MaxPacketSize());
    } else {
        // BI_RGB rows are padded to a 4-byte boundary. 24-bit means 3 bytes a pixel, so a width
        // that is not a multiple of 4 needs the pad; getting this wrong shears the picture.
        const uint32_t rowBytes = (static_cast<uint32_t>(m_outWidth) * 3u + 3u) & ~3u;
        m_frameBytes = rowBytes * static_cast<uint32_t>(m_outHeight);
        m_zmbv.reset();
        m_maxVideoChunk = m_frameBytes;
    }

    std::error_code ec;
    const auto parent = std::filesystem::path(config.path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);

    m_file.open(config.path, std::ios::binary | std::ios::trunc);
    if (!m_file) {
        outError = "could not open '" + config.path + "' for writing";
        return false;
    }

    m_videoFrames = 0;
    m_audioFrames = 0;
    m_bytesWritten = 0;
    m_full = false;
    m_failed = false;
    m_stopReason.clear();
    m_audioFramesDropped = 0;
    m_result.clear();
    m_index.clear();
    m_pendingAudio.clear();
    m_scratch.assign(m_frameBytes, 0);
    m_active = true;

    WriteHeaders();
    if (!StreamOk("the headers")) {
        outError = "could not write '" + config.path + "'";
        m_active = false;
        m_file.close();
        return false;
    }
    return true;
}

bool AviRecorder::StreamOk(const char* what)
{
    if (m_config.failAfterBytes && m_bytesWritten >= m_config.failAfterBytes)
        m_file.setstate(std::ios::badbit);
    if (!m_failed && (!m_file || m_file.tellp() == std::streampos(-1))) {
        m_failed = true;
        Logger::Error("AVI: could not write {} to '{}' after {} frames (is the disk full?)", what,
                      m_config.path, m_videoFrames);
    }
    return !m_failed;
}

void AviRecorder::WriteHeaders()
{
    const bool haveAudio = m_config.sampleRate > 0 && m_config.channels > 0;
    const uint32_t streams = haveAudio ? 2u : 1u;
    const uint16_t blockAlign = static_cast<uint16_t>(m_config.channels * 2u);

    const bool haveAspect = m_config.aspectNum > 0 && m_config.aspectDen > 0;
    const bool zmbv = m_config.codec == VideoCodec::Zmbv;
    const uint32_t outW = static_cast<uint32_t>(m_outWidth);
    const uint32_t outH = static_cast<uint32_t>(m_outHeight);

    // hdrl holds avih plus one strl per stream; its size is fixed once the stream count is.
    // 'strl' + strh + strf, and vprp (9 fields plus one 8-field field description) when the
    // display shape is known.
    const uint32_t kVprpSize = 9 * 4 + 8 * 4;
    const uint32_t videoStrl = 4 + (8 + 56) + (8 + 40) + (haveAspect ? (8 + kVprpSize) : 0);
    const uint32_t audioStrl = haveAudio ? (4 + (8 + 56) + (8 + 18)) : 0;
    const uint32_t hdrlSize = 4 + (8 + 56) + (8 + videoStrl) + (haveAudio ? (8 + audioStrl) : 0);

    m_file.seekp(0);
    // RIFF
    PutU32(m_file, Fourcc("RIFF"));
    PutU32(m_file, 0); // patched by End()
    PutU32(m_file, Fourcc("AVI "));

    // LIST hdrl
    PutU32(m_file, Fourcc("LIST"));
    PutU32(m_file, hdrlSize);
    PutU32(m_file, Fourcc("hdrl"));

    // avih
    PutU32(m_file, Fourcc("avih"));
    PutU32(m_file, 56);
    PutU32(m_file, static_cast<uint32_t>(1000000 / m_config.fps));      // dwMicroSecPerFrame
    PutU32(m_file, m_frameBytes * static_cast<uint32_t>(m_config.fps)); // dwMaxBytesPerSec
    PutU32(m_file, 0);                                                  // dwPaddingGranularity
    PutU32(m_file, kHasIndex);                                          // dwFlags
    PutU32(m_file, m_videoFrames);                                      // dwTotalFrames
    PutU32(m_file, 0);                                                  // dwInitialFrames
    PutU32(m_file, streams);                                            // dwStreams
    PutU32(m_file, m_maxVideoChunk);                                    // dwSuggestedBufferSize
    PutU32(m_file, outW);
    PutU32(m_file, outH);
    for (int i = 0; i < 4; ++i)
        PutU32(m_file, 0); // dwReserved

    // LIST strl (video)
    PutU32(m_file, Fourcc("LIST"));
    PutU32(m_file, videoStrl);
    PutU32(m_file, Fourcc("strl"));
    PutU32(m_file, Fourcc("strh"));
    PutU32(m_file, 56);
    PutU32(m_file, Fourcc("vids"));
    PutU32(m_file, zmbv ? kZmbv : Fourcc("DIB "));       // fccHandler
    PutU32(m_file, 0);                                   // dwFlags
    PutU16(m_file, 0);                                   // wPriority
    PutU16(m_file, 0);                                   // wLanguage
    PutU32(m_file, 0);                                   // dwInitialFrames
    PutU32(m_file, 1);                                   // dwScale
    PutU32(m_file, static_cast<uint32_t>(m_config.fps)); // dwRate
    PutU32(m_file, 0);                                   // dwStart
    PutU32(m_file, m_videoFrames);                       // dwLength
    PutU32(m_file, m_maxVideoChunk);                     // dwSuggestedBufferSize
    PutU32(m_file, 0xFFFFFFFFu);                         // dwQuality (-1: default)
    PutU32(m_file, 0);                                   // dwSampleSize (0: each chunk is a frame)
    PutU16(m_file, 0);
    PutU16(m_file, 0);
    PutU16(m_file, static_cast<uint16_t>(outW));
    PutU16(m_file, static_cast<uint16_t>(outH));

    // strf: BITMAPINFOHEADER. A positive biHeight means bottom-up rows, which is what
    // WriteVideoFrame produces.
    PutU32(m_file, Fourcc("strf"));
    PutU32(m_file, 40);
    PutU32(m_file, 40); // biSize
    PutU32(m_file, outW);
    PutU32(m_file, outH);
    PutU16(m_file, 1);                // biPlanes
    PutU16(m_file, zmbv ? 32 : 24);   // biBitCount
    PutU32(m_file, zmbv ? kZmbv : 0); // biCompression: ZMBV, or BI_RGB
    PutU32(m_file, m_frameBytes);     // biSizeImage
    PutU32(m_file, 0);                // biXPelsPerMeter
    PutU32(m_file, 0);                // biYPelsPerMeter
    PutU32(m_file, 0);                // biClrUsed
    PutU32(m_file, 0);                // biClrImportant

    if (haveAspect) {
        // vprp, OpenDML's video properties header - the only place an AVI can say what shape the
        // picture is. The frame aspect is one DWORD, numerator in the high word. ffmpeg (and so
        // mpv and friends) turns it into the sample aspect ratio; players that ignore it show the
        // line-doubled frame as it is, which is already within a few percent.
        PutU32(m_file, Fourcc("vprp"));
        PutU32(m_file, kVprpSize);
        PutU32(m_file, 0); // VideoFormatToken
        PutU32(m_file, m_config.fps == 50 ? 1u : (m_config.fps == 60 ? 2u : 0u)); // PAL / NTSC
        PutU32(m_file, static_cast<uint32_t>(m_config.fps)); // dwVerticalRefreshRate
        PutU32(m_file, outW);                                // dwHTotalInT
        PutU32(m_file, outH);                                // dwVTotalInLines
        PutU32(m_file, (static_cast<uint32_t>(m_config.aspectNum) << 16) |
                           static_cast<uint32_t>(m_config.aspectDen)); // dwFrameAspectRatio
        PutU32(m_file, outW);                                          // dwFrameWidthInPixels
        PutU32(m_file, outH);                                          // dwFrameHeightInLines
        PutU32(m_file, 1);                                             // nbFieldPerFrame
        PutU32(m_file, outH);                                          // CompressedBMHeight
        PutU32(m_file, outW);                                          // CompressedBMWidth
        PutU32(m_file, outH);                                          // ValidBMHeight
        PutU32(m_file, outW);                                          // ValidBMWidth
        PutU32(m_file, 0);                                             // ValidBMXOffset
        PutU32(m_file, 0);                                             // ValidBMYOffset
        PutU32(m_file, 0);                                             // VideoXOffsetInT
        PutU32(m_file, 0);                                             // VideoYValidStartLine
    }

    if (haveAudio) {
        PutU32(m_file, Fourcc("LIST"));
        PutU32(m_file, audioStrl);
        PutU32(m_file, Fourcc("strl"));
        PutU32(m_file, Fourcc("strh"));
        PutU32(m_file, 56);
        PutU32(m_file, Fourcc("auds"));
        PutU32(m_file, 0); // fccHandler
        PutU32(m_file, 0); // dwFlags
        PutU16(m_file, 0);
        PutU16(m_file, 0);
        PutU32(m_file, 0);                                // dwInitialFrames
        PutU32(m_file, blockAlign);                       // dwScale
        PutU32(m_file, m_config.sampleRate * blockAlign); // dwRate: bytes a second
        PutU32(m_file, 0);                                // dwStart
        PutU32(m_file, m_audioFrames);                    // dwLength, in sample frames
        PutU32(m_file, m_config.sampleRate * blockAlign); // dwSuggestedBufferSize
        PutU32(m_file, 0xFFFFFFFFu);                      // dwQuality
        PutU32(m_file, blockAlign);                       // dwSampleSize
        PutU16(m_file, 0);
        PutU16(m_file, 0);
        PutU16(m_file, 0);
        PutU16(m_file, 0);

        // strf: WAVEFORMATEX
        PutU32(m_file, Fourcc("strf"));
        PutU32(m_file, 18);
        PutU16(m_file, 1); // WAVE_FORMAT_PCM
        PutU16(m_file, static_cast<uint16_t>(m_config.channels));
        PutU32(m_file, m_config.sampleRate);
        PutU32(m_file, m_config.sampleRate * blockAlign);
        PutU16(m_file, blockAlign);
        PutU16(m_file, 16); // wBitsPerSample
        PutU16(m_file, 0);  // cbSize
    }

    // LIST movi
    PutU32(m_file, Fourcc("LIST"));
    m_moviListSizePos = m_file.tellp();
    PutU32(m_file, 0); // patched by End()
    m_moviFourccPos = m_file.tellp();
    PutU32(m_file, Fourcc("movi"));
}

bool AviRecorder::WriteVideoFrame(const uint8_t* rgba, int width, int height)
{
    if (!m_active || !rgba)
        return false;
    if (m_full || m_failed || !m_stopReason.empty())
        return false;
    // A resolution change mid-recording is a different film; refuse rather than shear the rest,
    // and say so - the film ends here, not at a frozen picture with the sound running on
    if (width != m_config.width || height != m_config.height) {
        m_stopReason = fmt::format("The picture changed size ({}x{} -> {}x{}).", m_config.width,
                                   m_config.height, width, height);
        Logger::Warn("AVI: {} Stopping after {} frames.", m_stopReason, m_videoFrames);
        return false;
    }

    // The frame, in the codec's own layout.
    const uint8_t* payload = nullptr;
    uint32_t payloadBytes = 0;
    uint32_t chunkId = kVideoChunk;
    uint32_t indexFlags = kKeyframe;
    if (m_config.codec == VideoCodec::Zmbv) {
        ConvertCaptureFrame(rgba, width, height, m_config.scaleX, m_config.scaleY,
                            CaptureLayout::BgrxTopDown, m_scratch.data());
        const auto packet = m_zmbv->EncodeFrame(m_scratch.data());
        if (packet.size == 0) {
            m_stopReason = fmt::format("The ZMBV encoder failed on frame {}.", m_videoFrames);
            Logger::Error("AVI: {}", m_stopReason);
            return false;
        }
        payload = packet.data;
        payloadBytes = static_cast<uint32_t>(packet.size);
        chunkId = kVideoChunkCompressed;
        // Only keyframes can be seeked to: a delta means nothing without the frames before it.
        indexFlags = packet.keyframe ? kKeyframe : 0;
    } else {
        ConvertCaptureFrame(rgba, width, height, m_config.scaleX, m_config.scaleY,
                            CaptureLayout::BgrBottomUpPadded, m_scratch.data());
        payload = m_scratch.data();
        payloadBytes = m_frameBytes;
    }
    const uint32_t paddedBytes = payloadBytes + (payloadBytes & 1u);

    // Would this frame, the audio waiting to go with it, their two index entries and the index
    // header still leave the file under the limit? If not, refuse it: End() then writes a file
    // whose every size and offset is valid, where one more frame would have wrapped them.
    //
    // The audio that will arrive for THIS frame is counted too: it is written after the frame is
    // accepted, and if the next frame is the one refused, End() flushes it with no further
    // check. Two frames' worth is reserved, since the core does not deliver exactly one frame's
    // audio per frame; End() trims anything past that rather than overshoot.
    const uint64_t pendingAudioBytes = m_pendingAudio.size() * sizeof(int16_t);
    const uint64_t blockAlign = m_config.channels * 2u;
    const uint64_t audioReserve =
        m_config.sampleRate > 0
            ? 2 * (m_config.sampleRate * blockAlign / static_cast<uint64_t>(m_config.fps)) + 8 + 1
            : 0;
    const uint64_t projected = static_cast<uint64_t>(static_cast<std::streamoff>(m_file.tellp())) +
                               (8 + pendingAudioBytes + 1) + (8 + paddedBytes) + audioReserve +
                               (m_index.size() + 3) * 16 + 8;
    if (projected > m_config.maxFileBytes) {
        m_full = true;
        Logger::Warn("AVI: '{}' reached the {} MB limit after {} frames; stopping", m_config.path,
                     m_config.maxFileBytes / (1024 * 1024), m_videoFrames);
        return false;
    }

    // Any audio gathered for the previous frame goes out before this one, so the two streams
    // stay in step chunk for chunk.
    FlushPendingAudio();
    if (!StreamOk("the sound"))
        return false;

    const auto chunkPos = m_file.tellp();
    PutU32(m_file, chunkId);
    PutU32(m_file, payloadBytes);
    m_file.write(reinterpret_cast<const char*>(payload), payloadBytes);
    if (payloadBytes & 1u) { // RIFF chunks are padded to an even length
        const char pad = 0;
        m_file.write(&pad, 1);
    }

    m_index.push_back({chunkId, indexFlags,
                       static_cast<uint32_t>(static_cast<std::streamoff>(chunkPos) -
                                             static_cast<std::streamoff>(m_moviFourccPos)),
                       payloadBytes});
    m_bytesWritten += 8 + paddedBytes;
    if (!StreamOk("a frame"))
        return false;
    ++m_videoFrames;
    return true;
}

void AviRecorder::WriteAudio(const int16_t* interleaved, size_t frames)
{
    if (!m_active || !interleaved || frames == 0 || m_config.sampleRate == 0)
        return;
    // Held only until the next picture: with no picture coming (a refused frame), at most 2 s,
    // the rest dropped and counted, rather than hoarded and written as one chunk at End()
    const size_t cap = 2ull * m_config.sampleRate * m_config.channels;
    const size_t room = m_pendingAudio.size() < cap ? cap - m_pendingAudio.size() : 0;
    const size_t samples =
        std::min(frames * m_config.channels, room / m_config.channels * m_config.channels);
    if (samples < frames * m_config.channels) {
        if (!m_audioFramesDropped)
            Logger::Warn("AVI: no picture for 2 s of sound; dropping sound until one comes");
        m_audioFramesDropped += frames - samples / m_config.channels;
    }
    m_pendingAudio.insert(m_pendingAudio.end(), interleaved, interleaved + samples);
}

void AviRecorder::FlushPendingAudio()
{
    if (m_pendingAudio.empty())
        return;

    const uint32_t bytes = static_cast<uint32_t>(m_pendingAudio.size() * sizeof(int16_t));
    const auto chunkPos = m_file.tellp();
    PutU32(m_file, kAudioChunk);
    PutU32(m_file, bytes);
    m_file.write(reinterpret_cast<const char*>(m_pendingAudio.data()), bytes);
    // A RIFF chunk is padded to an even length. 16-bit samples are always even, but the pad is
    // written from the length rather than assumed, so a future 8-bit tap cannot corrupt the file.
    if (bytes & 1u) {
        const char pad = 0;
        m_file.write(&pad, 1);
    }

    m_index.push_back({kAudioChunk, 0,
                       static_cast<uint32_t>(static_cast<std::streamoff>(chunkPos) -
                                             static_cast<std::streamoff>(m_moviFourccPos)),
                       bytes});
    m_audioFrames += static_cast<uint32_t>(m_pendingAudio.size() / m_config.channels);
    m_bytesWritten += 8 + bytes;
    m_pendingAudio.clear();
}

bool AviRecorder::End()
{
    if (!m_active)
        return false;
    m_active = false;
    if (m_failed) {
        // Sizes and offsets past the failed write are not to be trusted: no index, no headers
        m_file.close();
        if (m_zmbv) {
            m_zmbv->End();
            m_zmbv.reset();
        }
        m_result = fmt::format("Could not write {} after {} frames - is the disk full? The file is "
                               "incomplete.",
                               m_config.path, m_videoFrames);
        return false;
    }

    // The audio still waiting belongs to the last frame. It was reserved for when that frame was
    // accepted, but a block larger than the reserve must not push the file past its limit: cut
    // it to what fits, on a whole sample frame, rather than write sizes that wrap.
    if (!m_pendingAudio.empty()) {
        const uint64_t here = static_cast<uint64_t>(static_cast<std::streamoff>(m_file.tellp()));
        const uint64_t fixed = 8 + 1 + (m_index.size() + 1) * 16 + 8; // chunk head, pad, idx1
        const uint64_t room =
            m_config.maxFileBytes > here + fixed ? m_config.maxFileBytes - (here + fixed) : 0;
        const size_t channels = m_config.channels > 0 ? m_config.channels : 1;
        const size_t maxSamples = static_cast<size_t>(room / sizeof(int16_t)) / channels * channels;
        if (m_pendingAudio.size() > maxSamples)
            m_pendingAudio.resize(maxSamples);
    }
    FlushPendingAudio();

    // idx1 closes the movi list, so the list size is known here and nowhere earlier.
    const std::streamoff moviEnd = static_cast<std::streamoff>(m_file.tellp());
    PutU32(m_file, Fourcc("idx1"));
    PutU32(m_file, static_cast<uint32_t>(m_index.size() * 16));
    for (const auto& entry : m_index) {
        PutU32(m_file, entry.fourcc);
        PutU32(m_file, entry.flags);
        PutU32(m_file, entry.offset);
        PutU32(m_file, entry.length);
    }
    const std::streamoff fileEnd = static_cast<std::streamoff>(m_file.tellp());
    const std::streamoff moviFourcc = static_cast<std::streamoff>(m_moviFourccPos);

    // Rewrite the headers now the totals are known. The layout is byte-identical - only the
    // counts differ - so this lands exactly over the area Begin() reserved.
    //
    // It must come BEFORE the two size patches below, not after: WriteHeaders() ends by writing
    // the movi LIST header again with a zero size, so patching that first would be undone.
    WriteHeaders();

    // The movi list runs from its own FOURCC to the start of idx1.
    m_file.seekp(m_moviListSizePos);
    PutU32(m_file, static_cast<uint32_t>(moviEnd - moviFourcc));

    // RIFF size covers everything after its own size field.
    m_file.seekp(4);
    PutU32(m_file, static_cast<uint32_t>(fileEnd - 8));

    // the index and the headers, written; then the close, which flushes
    const bool written = StreamOk("the index") && (m_file.close(), !m_file.fail());
    if (m_file.is_open())
        m_file.close();
    if (m_zmbv) {
        m_zmbv->End();
        m_zmbv.reset();
    }
    if (!written || fileEnd < 0) {
        m_failed = true;
        m_result = fmt::format("Could not finish {} - is the disk full? The file is incomplete.",
                               m_config.path);
        return false;
    }
    m_result = fmt::format("{}Saved {} frames to {}",
                           m_stopReason.empty() ? std::string() : m_stopReason + " ", m_videoFrames,
                           m_config.path);
    Logger::Info("AVI: wrote '{}' - {} frames, {} audio frames, {} bytes", m_config.path,
                 m_videoFrames, m_audioFrames, static_cast<uint64_t>(fileEnd));
    return true;
}

} // namespace adm::core
