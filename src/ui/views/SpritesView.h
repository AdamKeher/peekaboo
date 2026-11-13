#pragma once

#include <imgui.h>
#include <functional>
#include <string>
#include <vector>

#include "../../core/AmigaCustom.h"
#include "../../core/AmigaEmulator.h"

typedef unsigned int GLuint;

namespace adm::ui {

class EmulatorView;

struct SpriteInstance
{
    int spriteIdx;
    uint16_t vpos_armed; // scanline when SPRxPT was written (from SprptEvent)
    uint32_t ptr;
    int vstart, vstop, hstart;
    // Per-instance texture for trace mode (0 = not built yet)
    GLuint texId = 0;
    int texW = 0;
    int texH = 0;
    // Palette snapshot captured at arm time (OCS COLOR16-31); index 0 = transparent.
    // Valid only when paletteValid = true (built from a SprptEvent with color data).
    bool paletteValid = false;
    ImU32 palette[16] = {};
};

struct DecodedSprite
{
    int idx = 0;
    uint32_t ptr = 0;
    uint16_t pos_reg = 0, ctl_reg = 0;
    int vstart = 0, vstop = 0, hstart = 0, height = 0;

    // Attachment state
    bool attached = false;     // odd sprite: ATT bit (ctl & 0x80) set
    bool isEvenOfPair = false; // even sprite: its odd partner has ATT bit set
    bool active = false;       // has valid height

    // Colour mode
    int colorBase = 16;     // first colour register for this sprite pair
    int numColors = 3;      // 3 (normal) or 15 (attached)
    ImU32 palette[16] = {}; // [0]=transparent, [1..15]=live colours

    std::vector<SpriteInstance> instances; // per-frame instances from SprptEvents or fallback

    GLuint texId = 0;
    int texW = 0;
    int texH = 0;
    bool texDirty = true;
};

class SpritesView
{
public:
    explicit SpritesView(EmulatorView* emulatorView);
    ~SpritesView();

    // Draw the sprites window contents (called when the window is open)
    void Draw();
    void Clear();

    // Draw bounding-box overlays onto the emulator screen (always called when callback fires)
    void DrawOverlay(ImDrawList* dl, ImVec2 screenMin, ImVec2 screenMax);

    void SetAllBBoxes(bool show);
    void SetShowCrosshairs(bool show) { m_showCrosshairs = show; }

    void SetOnJumpToMemory(std::function<void(uint32_t)> cb) { m_onJumpToMemory = std::move(cb); }

    // Persisted layout — the resizable Scanlines panel width and bottom detail-panel height.
    // Saved to user prefs on exit and restored on launch (see DebuggerView / main.cpp).
    float GetScanlinesPanelWidth() const { return m_histPanelW; }
    void SetScanlinesPanelWidth(float w)
    {
        if (w > 0.0f)
            m_histPanelW = w;
    }
    float GetDetailPanelHeight() const { return m_detailPanelH; }
    void SetDetailPanelHeight(float h)
    {
        if (h > 0.0f)
            m_detailPanelH = h;
    }

    static const ImU32 kSpriteColors[8];

private:
    EmulatorView* m_emulatorView;

    DecodedSprite m_decoded[8];

    // UI state
    uint8_t m_bboxMask = 0; // bit n = draw bbox overlay for sprite n
    bool m_showCrosshairs = true;
    int m_selectedRow = -1;   // which sprite row is expanded (-1 = none)
    bool m_traceMode = false; // trace mode: one row per instance instead of per channel

    // Detail-panel state — the unified 3-pane (instance list | breakdown | preview+palette)
    // panel shown when a row is selected. m_detailInstance picks which of the selected
    // channel's instances the middle/right panes describe.
    int m_detailInstance = 0;      // selected instance within the channel's instances[]
    float m_previewZoom = 4.0f;    // right-pane preview zoom (lores px → screen px)
    float m_detailLeftW = 150.0f;  // resizable left instance-list width
    float m_detailRightW = 220.0f; // resizable right preview/palette width

    // Row-hover bounding box. While a listview row is hovered we draw its bbox(es) on the
    // emulator screen even if the row's BBox checkbox is off. Set during DrawTable/DrawTraceTable,
    // consumed (and reset) by DrawOverlay each frame — so it auto-clears when the window is closed.
    // m_hoverInstance == -1 → draw every instance of the channel (default mode); >= 0 → only that
    // instance index (trace mode).
    int m_hoverChannel = -1;
    int m_hoverInstance = -1;

    // Trace mode flat row list — rebuilt each decode
    struct TraceRow
    {
        int spriteIdx;
        int instIdx;
    };
    std::vector<TraceRow> m_traceRows;

    // Histogram
    GLuint m_histTexId = 0;
    static constexpr int kHistW = 80;  // 8 sprites × 10px each
    static constexpr int kHistH = 320; // scanlines 0-319 (covers PAL 312 + margin)
    bool m_histDirty = true;

    // Resizable panel sizes (driven by splitter bars)
    float m_histPanelW = static_cast<float>(kHistW) + 34.0f; // Scanlines panel width
    float m_detailPanelH = 280.0f;                           // bottom detail panel height

    // Decode gate. Decode output depends only on emulator state (registers, chip RAM,
    // sprpt/copper events), which advances with the emulator frame; per-instance textures are
    // now built in both modes, so the trace flag no longer affects decode output. While paused
    // the cached sprites + textures stay valid. Also coalesces Draw()/DrawOverlay() per frame.
    uint64_t m_lastDecodeEmuFrame = UINT64_MAX;
    int m_lastSprptEventCount = 0; // raw event count from last frame (0 = DLL patch not active)

    // Reusable heap buffer for copper events (avoids 320 KB stack allocation per frame)
    std::vector<adm::AmigaEmulator::CopperExecEvent> m_copperEventBuf;

    // Cached state from last decode (for overlay)
    core::Custom m_cachedRegs{};
    bool m_cachedRegsValid = false;

    std::function<void(uint32_t)> m_onJumpToMemory;

    // ---- internal helpers ----

    // Ensure decoded data is fresh for the current Dear ImGui frame
    void EnsureDecoded();
    void DecodeSprites();

    // Deletes every per-instance GL texture for one sprite. Must run before any
    // instances.clear() — BuildInstanceTexture generates a fresh id per instance.
    void ReleaseInstanceTextures(int spriteIdx);
    void BuildInstances(const adm::AmigaEmulator::SprptEvent* events, int count,
                        const std::vector<adm::AmigaEmulator::MemoryRegion>& regions);

    void BuildSpriteTexture(int idx, const std::vector<adm::AmigaEmulator::MemoryRegion>& regions,
                            const core::Custom& regs);

    bool ReadChipWord(uint32_t addr, uint16_t& out,
                      const std::vector<adm::AmigaEmulator::MemoryRegion>& regions) const;

    void DrawTable(const core::Custom& regs);
    void DrawTraceTable(const core::Custom& regs);
    // Unified detail panel: 3 panes (instance list | full breakdown | enlarged preview +
    // palette) for one channel's selected instance. Shared by both standard and trace mode.
    void DrawInstanceDetail(int channel, const core::Custom& regs);
    void DrawHistogram();
    void RebuildHistogram();

    // Render a clickable memory-address hyperlink (jumps to the Memory tab on click)
    void DrawAddressLink(uint32_t addr);

    // Render the per-sprite enable checkbox. Reads/writes the emulator's shared sprite mask
    // so it stays in sync with the Sprites section of the Display window.
    void DrawSpriteEnableCheckbox(int spriteIdx);

    void BuildInstanceTexture(int sprIdx, int instIdx,
                              const std::vector<adm::AmigaEmulator::MemoryRegion>& regions);

    // Derive per-instance COLOR16-31 from the copper event log by replaying MOVE instructions
    // forward in time and snapshotting colors at each instance's vstart scanline.
    void AssignInstanceColorsFromCopper(const core::Custom& regs,
                                        const adm::AmigaEmulator::CopperExecEvent* copperEvents,
                                        int copperCount);

    static void DrawDashedRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, ImU32 col, float thickness,
                               float dashLen);
};

} // namespace adm::ui
