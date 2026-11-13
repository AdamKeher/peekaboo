#include "VideoCaptureSink.h"

#include <iterator>

namespace adm::core {

namespace {

// clang-format off
constexpr VideoCaptureCodecInfo kCodecs[] = {
    {VideoCaptureCodec::Zmbv, "zmbv", "ZMBV (lossless, small)", ".avi", false, true, nullptr, 0,
     "DOSBox's capture codec, built in. Every pixel exact, and an Amiga screen costs well under a "
     "megabyte a second. Plays in VLC, mpv and anything built on ffmpeg; not in Windows' own "
     "player."},
    {VideoCaptureCodec::H264, "h264", "H.264 (MP4)", ".mp4", true, false, "libx264", 18,
     "Plays everywhere and uploads anywhere. Lossy: fine detail in hires modes softens a little. "
     "Needs FFmpeg."},
    {VideoCaptureCodec::H265, "h265", "H.265 (MP4)", ".mp4", true, false, "libx265", 22,
     "About half the size of H.264 at the same quality; not every player has it. Needs FFmpeg."},
    {VideoCaptureCodec::Ffv1, "ffv1", "FFV1 (lossless, MKV)", ".mkv", true, true, "ffv1", 0,
     "FFmpeg's lossless codec - every pixel exact, for archiving or editing. Plays in VLC and "
     "mpv. Needs FFmpeg."},
    {VideoCaptureCodec::Uncompressed, "uncompressed", "Uncompressed AVI", ".avi", false, true,
     nullptr, 0,
     "Raw RGB, built in. Plays in anything, but about 60 MB a second and stops at 4 GB."},
};
// clang-format on

} // namespace

const VideoCaptureCodecInfo& GetVideoCaptureCodecInfo(VideoCaptureCodec codec)
{
    for (const auto& info : kCodecs) {
        if (info.codec == codec)
            return info;
    }
    return kCodecs[0];
}

const VideoCaptureCodecInfo* FindVideoCaptureCodec(const std::string& id)
{
    for (const auto& info : kCodecs) {
        if (id == info.id)
            return &info;
    }
    return nullptr;
}

const VideoCaptureCodecInfo* GetVideoCaptureCodecs(size_t& outCount)
{
    outCount = std::size(kCodecs);
    return kCodecs;
}

} // namespace adm::core
