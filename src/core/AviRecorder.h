#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "VideoCaptureSink.h"

namespace adm::core {

// One file with the picture and the sound in it, muxed from the same two streams the GIF and WAV
// recorders already take: RGBA frames from CaptureFrameRGBA and 16-bit interleaved PCM from the
// core audio tap.
//
// The container is uncompressed AVI (BI_RGB video, PCM audio) and nothing else: it needs no
// external encoder, no extra dependency and no second process, and every player reads it. The
// price is size - a line-doubled 720x574 PAL frame is 1.2 MB, so a 50 fps capture writes about
// 62 MB a second - so this is for short clips, which is what the GIF recorder was being used for.
//
// AVI 1.0 sizes and index offsets are 32-bit, so a file cannot pass 4 GB without corrupting.
// The recorder stops itself short of that (IsFull) rather than write a file nothing can open;
// OpenDML (AVI 2.0) would lift the limit and is not implemented.
//
// Amiga pixels are not square: a PAL hires-width frame is 720x287, each lores pixel two columns
// wide and one row tall. Written as-is, players show it squashed to 2.5:1 and upscale 287 lines
// with a blurring filter. So frames are enlarged by whole-number factors (scaleX, scaleY) -
// replicated pixels, never interpolated - and the true display shape goes in a 'vprp' header for
// the players that read it.
//
// Chunk order is one video chunk then that frame's audio, which is the layout players expect and
// keeps the two streams in step without an interleaving pass at the end.
class ZmbvEncoder;

class AviRecorder final : public VideoCaptureSink
{
public:
    enum class VideoCodec {
        Uncompressed, // BI_RGB, plays everywhere, ~62 MB/s
        Zmbv,         // DOSBox's lossless capture codec, ~0.5-1 MB/s on Amiga footage
    };

    AviRecorder();
    ~AviRecorder() override;

    AviRecorder(const AviRecorder&) = delete;
    AviRecorder& operator=(const AviRecorder&) = delete;

    // Past this the 32-bit RIFF size and index offsets would wrap. A little short of 4 GiB, so
    // the final index still fits after the last frame is accepted.
    static constexpr uint64_t kMaxFileBytes = 0xF0000000ull;

    struct Config
    {
        std::string path;
        int width = 0;  // of the frames WriteVideoFrame is given
        int height = 0; //
        int scaleX = 1; // whole-number enlargement applied when writing; the file is
        int scaleY = 1; // width*scaleX by height*scaleY
        // The shape the picture should be shown at, as num:den (e.g. 12544:10000 for a PAL
        // 720x574). 0 writes no 'vprp' header and leaves the player to assume square pixels. Both
        // must fit in 16 bits - that is how vprp stores them.
        int aspectNum = 0;
        int aspectDen = 0;
        VideoCodec codec = VideoCodec::Uncompressed;
        int fps = 50;            // PAL; 60 for NTSC
        uint32_t sampleRate = 0; // 0 writes a video-only file
        uint32_t channels = 2;
        uint64_t maxFileBytes = kMaxFileBytes; // tests lower it to reach the limit quickly
        // Tests: the stream goes bad once this many bytes of chunks are written, as a full disk
        // makes it. 0: never.
        uint64_t failAfterBytes = 0;
    };

    // Whole-number enlargement that brings a width x height frame closest to `displayAspect`
    // (width / height as it should be shown) without interpolating: 720x287 PAL hires at 1.25
    // gives 1x2, a 720x574 interlaced frame 1x1, a 360x574 one 2x1. Each factor is at least 1
    // and at most 4.
    static void ChooseScale(int width, int height, double displayAspect, int& scaleX, int& scaleY);

    // Reserves the header area and opens the file. Fails on a bad size, an fps of zero, or a
    // path that cannot be written.
    bool Begin(const Config& config, std::string& outError);

    // One frame, top row first, width*height*4 bytes. The dimensions must match Begin()'s
    // width/height (before scaling); a frame of another size is refused rather than stretched,
    // because a mid-recording resolution change is a different film. Also refused once the file
    // is full - check IsFull() and End() the recording.
    bool WriteVideoFrame(const uint8_t* rgba, int width, int height) override;

    // Interleaved 16-bit PCM for the frame just written. Held until the next video frame so each
    // audio chunk sits beside the picture it belongs to.
    void WriteAudio(const int16_t* interleaved, size_t frames) override;

    // Patches the headers with the totals and writes the index. Safe to call when not active.
    bool End() override;

    bool IsActive() const override { return m_active; }
    // The next frame would take the file past Config::maxFileBytes.
    bool IsFull() const override { return m_full; }
    // The recording has to end: a write to the file failed (a full disk, say; End() then says
    // the file is incomplete rather than "Saved"), or it cannot go on - the picture changed
    // size, or the ZMBV encoder failed - and End() closes a valid file of what came before.
    bool HasFailed() const override { return m_failed || !m_stopReason.empty(); }
    // Sound frames thrown away because no picture came to carry them (capped at 2 s)
    uint64_t AudioFramesDropped() const { return m_audioFramesDropped; }
    int OutputWidth() const override { return m_outWidth; }
    int OutputHeight() const override { return m_outHeight; }
    uint32_t FramesWritten() const override { return m_videoFrames; }
    uint64_t BytesWritten() const override { return m_bytesWritten; }
    const std::string& Path() const override { return m_config.path; }
    std::string ResultMessage() const override { return m_result; }

private:
    struct IndexEntry
    {
        uint32_t fourcc;
        uint32_t flags;
        uint32_t offset; // from the 'movi' FOURCC, the convention every reader assumes
        uint32_t length;
    };

    void WriteHeaders(); // called at Begin with zeroed totals, again at End with the real ones
    void FlushPendingAudio();
    // After a write: false (and the recording failed) once the stream has gone bad. After
    // badbit every write is a no-op and tellp() is -1, which would wrap the size checks.
    bool StreamOk(const char* what);

    std::ofstream m_file;
    Config m_config;
    bool m_active = false;
    bool m_full = false;
    bool m_failed = false;
    std::string m_stopReason; // why the recording cannot go on, the file still valid
    uint64_t m_audioFramesDropped = 0;
    std::string m_result;
    int m_outWidth = 0;
    int m_outHeight = 0;
    uint32_t m_videoFrames = 0;
    uint32_t m_audioFrames = 0; // sample frames, not video frames
    uint64_t m_bytesWritten = 0;
    uint32_t m_frameBytes = 0; // padded BI_RGB bytes per frame
    std::streampos m_moviFourccPos = 0;
    std::streampos m_moviListSizePos = 0;
    std::vector<IndexEntry> m_index;
    std::vector<uint8_t> m_scratch;      // one frame, converted
    std::vector<int16_t> m_pendingAudio; // audio for the frame being assembled
    std::unique_ptr<ZmbvEncoder> m_zmbv; // only while recording ZMBV
    uint32_t m_maxVideoChunk = 0;        // the header's buffer-size hint
};

} // namespace adm::core
