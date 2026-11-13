#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <glad/glad.h>
#include <imgui.h>

#include "../../core/AmigaEmulator.h"
#include "../TextureManager.h"
#include "AddressActions.h"
#include "EmulatorView.h"

namespace adm::core {
struct BlitRecord;
}

namespace adm::ui {

class MemoryMapView
{
public:
    MemoryMapView(EmulatorView* emulatorView, TextureManager* textureManager);
    ~MemoryMapView();

    // Shared address-link context (set by Application) so the detected-graphics Address column
    // gets the same hyperlink + right-click menu as the debugger views.
    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }

    void Open()
    {
        m_isOpen = true;
        // Bring to front even if already open behind other windows. Request focus for two frames:
        // a left-click that triggered this also makes ImGui re-focus the *clicked* window at
        // end-of-frame, stealing our one-shot focus. Re-applying next frame (no competing click)
        // wins.
        m_focusFramesLeft = 2;
    }

    // Open + focus the window and navigate to addr. The row width is adopted from the matching
    // detected bitplane guess (resolved on the next Draw, once guesses are computed) when addr is a
    // live bitplane pointer; otherwise the current width is kept.
    void ShowAddress(uint32_t addr)
    {
        Open();
        m_startAddress = addr;
        std::snprintf(m_addressBuf, sizeof(m_addressBuf), "%06X", m_startAddress);
        m_pendingWidthFromGuess = true;
    }
    void Restore()
    {
        m_isOpen = true;
        m_noFocusOnAppearing = true;
    }
    void Close() { m_isOpen = false; }
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    // The blit to highlight in the main view this frame (nullptr = none). Both the destination
    // and any active source channels are outlined, since the map shows all of chip RAM. Owned by
    // the caller; valid only for the current Draw().
    void SetActiveBlit(const core::BlitRecord* rec) { m_activeBlit = rec; }

    void Draw();

    // Persisted layout (saved/restored via UserPreferences). 0 from prefs => keep default.
    float GetLeftColumnWidth() const { return m_leftColumnW; }
    void SetLeftColumnWidth(float w) { m_leftColumnW = w; }
    float GetPreviewHeight() const { return m_leftPreviewH; }
    void SetPreviewHeight(float h) { m_leftPreviewH = h; }

private:
    // A single bitplane guess entry derived from per-frame bplpt events
    struct GuessEntry
    {
        int plane;         // 1-based bitplane number
        uint32_t address;  // bitplane pointer (start of data)
        int width;         // estimated display width in pixels
        int height;        // estimated display height in scanlines
        int scanlineStart; // vpos where this pointer was first set
        int scanlineEnd;   // vpos where next different pointer was set (or frame end)
        bool enabled;      // true if BPLCON0.BPU >= plane at time of write
    };

    // A single vertical block in the preview stack: either a populated memory region or a void
    // gap. Populated segments get a (capped) proportional height + content heatmap; void gaps get
    // a small fixed height + notice. y0/y1 are preview-local pixels; texRow* index the texture.
    struct PreviewSegment
    {
        bool isVoid;        // true = unmapped gap, false = populated region
        int regionIdx;      // original index into m_regions (-1 for void)
        uint32_t addrStart; // inclusive
        uint32_t addrEnd;   // exclusive
        float y0 = 0.0f;    // top in preview-local pixels
        float y1 = 0.0f;    // bottom in preview-local pixels
        int texRow0 = 0;    // first texture row owned by this segment
        int texRowCount = 0;
    };

    // State
    const AddressActionContext* m_addrCtx = nullptr;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    int m_focusFramesLeft = 0;            // frames remaining to request SetNextWindowFocus (Open)
    bool m_pendingWidthFromGuess = false; // resolve row width from a matching guess on next Draw
    uint32_t m_startAddress = 0;
    int m_width = 320; // row width in pixels (bits); each 16-bit word = 16px
    char m_addressBuf[9] = "00000000";

    // Bitplane guesses (refreshed each frame from hardware registers)
    std::vector<GuessEntry> m_guesses;
    // Scratch for the bplpt event fetch (was a function-local static).
    std::vector<adm::AmigaEmulator::BplptEvent> m_bplptScratch;

    // Drag-to-pan state for the main view
    float m_dragAccumX = 0.0f; // sub-pixel horizontal accumulator (16px = 1 word)
    float m_dragAccumY = 0.0f; // sub-pixel vertical accumulator (1px = 1 row)

    // Pixel magnifier ("loupe") over the main view, toggled from the toolbar magnify button.
    bool m_showMagnifier = false;
    // Magnifier source when zoomed out: the main texture then holds averaged blocks, so the loupe
    // gets its own small texture of real bits rendered around the hovered memory pixel.
    static constexpr int kLoupeW = 32;
    static constexpr int kLoupeH = 16;
    GLuint m_loupeTexId = 0;
    std::vector<uint8_t> m_loupePixels;  // RGBA, kLoupeW*kLoupeH*4
    std::vector<uint8_t> m_loupeScratch; // RGBA, m_gridW*kLoupeH*4

    // Main view scale, set by the toolbar -/+ buttons (or Ctrl+wheel over the map). Negative steps
    // zoom out: each screen pixel averages a 2^-step square of memory pixels. Positive steps zoom
    // in: each memory pixel is drawn (step+1) screen pixels square.
    static constexpr int kMinScaleStep = -3; // 1/8x
    static constexpr int kMaxScaleStep = 3;  // 4x
    int m_scaleStep = 0;
    int DownscaleFactor() const { return m_scaleStep < 0 ? 1 << -m_scaleStep : 1; }
    int UpscaleFactor() const { return m_scaleStep > 0 ? m_scaleStep + 1 : 1; }
    // Screen pixels per memory pixel.
    float ScreenPerGridPx() const
    {
        return static_cast<float>(UpscaleFactor()) / static_cast<float>(DownscaleFactor());
    }

    // Resizable splitter: width of left column in pixels
    float m_leftColumnW = 280.0f;
    // Horizontal splitter inside the left column: height of the top preview sub-area in pixels.
    float m_leftPreviewH = 300.0f;

    // Preview texture: 128 wide, height tracks the allocated preview area (m_previewTexH).
    static constexpr int kPreviewW = 128;
    static constexpr int kPreviewH = 400; // initial allocation hint only
    GLuint m_previewTexId = 0;
    std::vector<uint8_t> m_previewPixels; // RGBA, kPreviewW*m_previewTexH*4
    int m_previewTexH = 0;                // current texture height in pixels

    // Preview segment stack (rebuilt when m_segLayoutDirty). Voids collapse to a fixed height.
    std::vector<PreviewSegment> m_segments;
    bool m_segLayoutDirty = true;
    int m_lastPreviewDispH = 0; // last allocated preview height the layout was built for

    // Preview layout constants
    static constexpr float kVoidSegPx = 18.0f;    // fixed height of a void block
    static constexpr float kRegionMinPx = 24.0f;  // min height of a populated region block
    static constexpr float kRegionMaxPx = 220.0f; // cap so one huge region doesn't dominate
    static constexpr float kLeftSplitterH = 6.0f; // horizontal splitter thickness
    static constexpr float kPreviewMinH = 40.0f;  // min height of the preview sub-area

    // Main view texture (resized when window changes)
    int m_mainTexW = 0;
    int m_mainTexH = 0;
    GLuint m_mainTexId = 0;
    std::vector<uint8_t> m_mainPixels; // RGBA, m_mainTexW*m_mainTexH*4
    // Memory-pixel grid the main view shows (1 bit = 1 pixel). Equals the texture size at 1x;
    // m_mainTex* * factor when zoomed out, m_mainTex* when zoomed in (the texture is stretched).
    int m_gridW = 0;
    int m_gridH = 0;

    // Persistent worker + double buffer for off-main-thread pixel rendering.
    struct RenderRequest
    {
        int texW = 0;
        int texH = 0;
        int gridW = 0;
        int gridH = 0;
        int downscale = 1; // texel = downscale x downscale memory pixels, averaged
        uint32_t startAddress = 0;
        int colW = 0;
        std::vector<adm::AmigaEmulator::MemoryRegion> regions;
        std::shared_ptr<std::vector<std::vector<uint8_t>>> regionBytes;
    };
    std::thread m_renderThread;
    std::mutex m_renderMutex;
    std::condition_variable m_renderCv;
    bool m_stopThread = false;
    bool m_renderPending = false;
    bool m_pixelReady = false;
    RenderRequest m_renderRequest;
    std::vector<uint8_t> m_backPixels; // written by worker
    int m_readyTexW = 0;
    int m_readyTexH = 0;

    EmulatorView* m_emulatorView = nullptr;
    TextureManager* m_textureManager = nullptr;
    const core::BlitRecord* m_activeBlit = nullptr;
    std::vector<adm::AmigaEmulator::MemoryRegion> m_regions;

    // Frame-gating: the overview path (region fetch, guess recompute, 128x400 preview scan +
    // upload) only re-runs when the emulator advances a frame. Paused => memory is frozen => skip.
    uint64_t m_lastPreviewFrame = UINT64_MAX;

    // The main-view worker is only re-kicked when one of its inputs actually changes, instead of
    // spinning a render thread every UI frame. Tracks the inputs of the last kick.
    bool m_mainKickValid = false;
    uint64_t m_lastMainKickFrame = 0;
    uint32_t m_lastMainKickAddr = 0;
    int m_lastMainKickWidth = 0;
    int m_lastMainKickTexW = 0;
    int m_lastMainKickTexH = 0;
    int m_lastMainKickScale = 0;

    uint32_t GetTotalSpan() const;
    // Pixel renderer — runs on the worker thread; no ImGui or GL calls. Renders grid rows
    // [rowBegin, rowBegin+rowCount) of a w x h memory-pixel grid into outPixels (w x rowCount).
    static void DoRenderPixels(uint8_t* outPixels, int w, int h, int rowBegin, int rowCount,
                               uint32_t startAddress, int colW,
                               const std::vector<adm::AmigaEmulator::MemoryRegion>& regions);
    // Full render of a request, box-averaging down to the texture size when zoomed out.
    static void RenderRequestPixels(const RenderRequest& request, std::vector<uint8_t>& out);
    // Draw the magnifier tooltip body from real memory pixels around grid pixel (gx, gy).
    void DrawZoomedOutLoupe(int gx, int gy);
    void RenderWorkerMain();
    RenderRequest BuildRenderRequest(int texW, int texH, int gridW, int gridH, int downscale) const;
    void RenderPreview();
    // Build m_segments + assign y0/y1/texRow* for a preview area of the given pixel height.
    void BuildSegmentLayout(float availPx);
    // Piecewise address<->preview-local-Y mapping over the segment stack.
    bool PreviewAddrToY(uint32_t addr, float& outY) const;
    uint32_t PreviewYToAddr(float y) const;
    void UploadTexture(GLuint id, const uint8_t* px, int w, int h);
    void EnsureMainTexture(int w, int h);
    void ComputeGuesses(); // populate m_guesses from custom registers
    void DrawGuessTable(); // draw clickable guess table below preview
    void JumpToGuess(const GuessEntry& g);
    // Bring the outer PanelView tab(s) hosting this window forward when it is focused via
    // ShowAddress() from a different panel (nested-dockspace focus fix).
    void FocusContainingPanels();
};

} // namespace adm::ui
