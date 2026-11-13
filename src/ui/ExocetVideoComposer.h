#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <imgui.h>

#include "views/ExocetMemoryView.h"

namespace adm {
class AmigaEmulator;
}

namespace adm::core {
class ExocetSession;
struct ExocetBlockState;
} // namespace adm::core

namespace adm::ui {

class EmulatorView;

// The Exocet video: each emulated frame drawn into a 4K
// picture with what Exocet knows of it - the Amiga's picture top left, the grid top right (the
// song's order row over the row, frame, time, the four channels and every track of the groups the
// demo has enabled), and along the bottom the timeline, the memory map's bars over the drives
// (head, lamp, the tracks being read, the transfers), and Paula's four voices over the frame's DMA
// slots. The recorder hands it the frame at the frame boundary
// (AmigaEmulator::VideoCaptureOptions::compose).
//
// It draws with a second ImGui context of its own, sharing the application's fonts, into an
// offscreen framebuffer it reads back; so it runs on the main thread with the GL context
// current, as the frame loop is. Made for one recording and destroyed with it, which frees the
// context and the GL objects.
class ExocetVideoComposer
{
public:
    static constexpr int kWidth = 3840;
    static constexpr int kHeight = 2160;

    ExocetVideoComposer(core::ExocetSession* session, EmulatorView* emulatorView);
    ~ExocetVideoComposer();
    ExocetVideoComposer(const ExocetVideoComposer&) = delete;
    ExocetVideoComposer& operator=(const ExocetVideoComposer&) = delete;

    // The frame - RGBA, top row first, `scaleX` x `scaleY` its whole-number enlargement towards
    // the shape it is shown at - composed into `out`, kWidth x kHeight RGBA top row first.
    bool Compose(const uint8_t* rgba, int width, int height, int scaleX, int scaleY,
                 std::vector<uint8_t>& out);
    // Why the last Compose failed.
    const std::string& Error() const { return m_error; }

private:
    bool EnsureResources();
    void UploadFrame(const uint8_t* rgba, int width, int height);
    // One ImGui frame of the four panels, in m_context
    void DrawPanels(int width, int height, int scaleX, int scaleY);
    void DrawGrid(const core::ExocetBlockState& block);
    void DrawDrives();
    void DrawPaula();
    void DrawDma();
    // What the frame just run left in the emulator's taps, taken once a frame before the
    // panels draw - the first frame draws them twice
    void TakeFrame();
    void TakeDiskTransfers(AmigaEmulator& emu);
    void TakeAudio(AmigaEmulator& emu);
    void TakeDmaRecord(AmigaEmulator& emu);

    core::ExocetSession* m_session;
    EmulatorView* m_emulatorView;
    ExocetMemoryView m_memory;
    std::string m_error;

    ImGuiContext* m_context = nullptr;
    bool m_backend = false; // the OpenGL3 backend is up in m_context
    bool m_warm = false;    // a first frame drawn and thrown away, so every panel has settled
    unsigned m_framebuffer = 0;
    unsigned m_target = 0; // the picture, kWidth x kHeight
    unsigned m_frame = 0;  // the Amiga's frame
    int m_frameWidth = 0, m_frameHeight = 0;
    std::vector<uint8_t> m_readback;

    // The drives: the floppy transfers since the film started, newest last, and how lately each
    // drive's tracks were read or written (1 just now, fading to 0), by side and cylinder
    struct Transfer
    {
        uint64_t frame = 0;
        int drive = 0, cyl = 0, side = 0;
        bool write = false, fromCopper = false, aborted = false;
        uint32_t words = 0, sync = 0, address = 0;
    };
    static constexpr int kCylinders = 80; // one a copy-protected disk steps past counts as 79
    std::deque<Transfer> m_transfers;
    uint32_t m_nextSeq = 0; // the transfer log's next sequence number not yet taken
    bool m_seqSeeded = false;
    struct TrackHeat
    {
        float heat = 0.0f;
        bool write = false;
    };
    std::array<std::array<std::array<TrackHeat, kCylinders>, 2>, 4> m_heat{};
    std::array<int, 4> m_lastSide{}; // the side each drive last moved data on

    // Paula: each voice's last samples, -1..1 after its volume, and what it plays at
    static constexpr int kScopeSamples = 512;
    struct Voice
    {
        std::array<float, kScopeSamples> samples{};
        int writePos = 0;
        uint16_t per = 0, vol = 0;
        bool dmaOn = false;
        float peak = 0.0f;
    };
    std::array<Voice, 4> m_voices{};

    // The frame's DMA slots as a picture, a colour a slot, and their count by kind
    unsigned m_dmaTexture = 0;
    int m_dmaColumns = 0, m_dmaLines = 0;
    std::vector<uint32_t> m_dmaPixels;
    std::array<uint32_t, 10> m_dmaCounts{}; // by DMARECORD_* type, 0 idle
    bool m_dmaValid = false;
};

} // namespace adm::ui
