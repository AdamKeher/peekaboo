#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adm::core {

// DOSBox's lossless capture codec (fourcc ZMBV), for AviRecorder.
//
// A zlib stream over the frames: a keyframe holds the whole picture, and every other frame holds
// one motion vector per 16x16 block plus the XOR of each block that changed. Emulator footage is
// mostly unchanged or scrolled from one frame to the next, which is exactly what that encodes
// cheaply.
//
// Always 32 bpp: ffmpeg's decoder only accepts 24-bit ZMBV when built with ZMBV_ENABLE_24BPP.
class ZmbvEncoder
{
public:
    ZmbvEncoder();
    ~ZmbvEncoder();

    ZmbvEncoder(const ZmbvEncoder&) = delete;
    ZmbvEncoder& operator=(const ZmbvEncoder&) = delete;

    // `keyframeInterval` frames between keyframes; 300 is six seconds at 50 fps, as DOSBox does.
    bool Begin(int width, int height, int keyframeInterval, std::string& outError);
    void End();

    // A packet, pointing into the encoder's own buffer: valid until the next EncodeFrame.
    struct Packet
    {
        const uint8_t* data = nullptr;
        size_t size = 0;       // 0 means the frame could not be encoded; the encoder has stopped
        bool keyframe = false; // a decoder can start here
    };

    // One frame of BGRX pixels (blue, green, red, unused), top row first, width*height*4 bytes -
    // the layout ZMBV stores.
    Packet EncodeFrame(const uint8_t* bgrx);

    // Largest packet EncodeFrame can return, for the AVI header's buffer-size hint.
    size_t MaxPacketSize() const;

    // How many blocks of the last delta frame were matched by a motion vector other than (0,0),
    // and how many needed no XOR at all. Diagnostics for the tests and the benchmark.
    int LastMovedBlocks() const { return m_lastMovedBlocks; }
    int LastCleanBlocks() const { return m_lastCleanBlocks; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    int m_lastMovedBlocks = 0;
    int m_lastCleanBlocks = 0;
};

} // namespace adm::core
