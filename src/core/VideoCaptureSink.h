#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace adm::core {

// What the capture codecs offer. Uncompressed and Zmbv are written by AviRecorder, the rest by
// FfmpegRecorder.
enum class VideoCaptureCodec {
    Uncompressed,
    Zmbv,
    H264,
    H265,
    Ffv1,
};

struct VideoCaptureCodecInfo
{
    VideoCaptureCodec codec;
    const char* id;        // stable, for the saved preference: "zmbv", "h264", ...
    const char* label;     // as the capture window lists it
    const char* extension; // ".avi", ".mp4", ".mkv"
    bool needsFfmpeg;
    bool lossless;
    const char* ffmpegEncoder; // "libx264" etc., or nullptr
    int defaultQuality;        // CRF for the lossy ones, 0 otherwise
    const char* description;   // the tooltip
};

const VideoCaptureCodecInfo& GetVideoCaptureCodecInfo(VideoCaptureCodec codec);
// nullptr for an id no codec has.
const VideoCaptureCodecInfo* FindVideoCaptureCodec(const std::string& id);
// Every codec, in the order the capture window lists them.
const VideoCaptureCodecInfo* GetVideoCaptureCodecs(size_t& outCount);

// One recording in progress, whichever codec writes it. AmigaEmulator drives it from the frame
// loop: audio as the core delivers it, pictures at frame boundaries, both on the same thread.
class VideoCaptureSink
{
public:
    virtual ~VideoCaptureSink() = default;

    // RGBA, top row first, at the size Begin was given. False when the frame could not be
    // written; IsFull or HasFailed then say whether the recording has to end.
    virtual bool WriteVideoFrame(const uint8_t* rgba, int width, int height) = 0;
    virtual void WriteAudio(const int16_t* interleaved, size_t frames) = 0;
    // Ends the recording. A sink that has work left after this (the FFmpeg mux) keeps
    // IsFinishing true until it is done; the object must outlive that.
    virtual bool End() = 0;

    virtual bool IsActive() const = 0;
    virtual bool IsFinishing() const { return false; }
    // The file cannot take another frame (AVI's 4 GB limit).
    virtual bool IsFull() const { return false; }
    // Something went wrong that ends the recording - FfmpegRecorder when the encoder dies.
    virtual bool HasFailed() const { return false; }
    // What happened, once the recording is over: where it was saved, or why it was not.
    virtual std::string ResultMessage() const { return {}; }

    virtual uint32_t FramesWritten() const = 0;
    virtual uint64_t BytesWritten() const = 0;
    virtual const std::string& Path() const = 0;
    virtual int OutputWidth() const = 0;
    virtual int OutputHeight() const = 0;
};

} // namespace adm::core
