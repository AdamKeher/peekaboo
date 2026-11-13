#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../core/AmigaEmulator.h"
#include "AddressActions.h"

namespace adm::ui {

class EmulatorView;

// "Bartman Bitmaps" window — surfaces the graphics resources an Amiga program registers via
// Bartman's vscode-amiga-debug debug_register_bitmap()/debug_register_palette() helpers. Mirrors
// the resource browser in Bartman's IDE integration: a dropdown of registered bitmaps and a
// dropdown of palettes (plus the live copper palette), the memory locations of each, the bitmap
// details (name/dimensions/depth/flags), and a live-decoded thumbnail preview.
class DebuggerBartmanView
{
public:
    explicit DebuggerBartmanView(EmulatorView* emulatorView);
    ~DebuggerBartmanView();

    void Draw();
    void Clear();
    bool* GetOpenPtr() { return &m_open; }
    void Restore(bool open);
    void SetTurboSuppressed(bool v) { m_turboSuppressed = v; }

    void SetOnJumpToMemory(std::function<void(uint32_t)> cb) { m_onJumpToMemory = std::move(cb); }
    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }
    // Fired when the "Enable Bartman debug support" checkbox is toggled, so the host can persist
    // the opt-in to user preferences.
    void SetOnSupportChanged(std::function<void(bool)> cb) { m_onSupportChanged = std::move(cb); }

private:
    using DebugResource = adm::AmigaEmulator::DebugResource;

    // Reads `len` bytes from Amiga address space into dst; false if the range isn't RAM-backed.
    bool ReadAmigaBytes(uint32_t addr, uint8_t* dst, uint32_t len,
                        const std::vector<adm::AmigaEmulator::MemoryRegion>& regions) const;

    // Per-preview memo so BuildPreview can skip the decode when nothing changed. `frame` is the
    // emulator frame counter; `key` hashes the bitmap geometry, address, and palette.
    struct PreviewCache
    {
        uint64_t frame = ~0ull;
        uint64_t key = 0;
        bool ok = false;
    };

    // Decodes the selected planar bitmap through `palette` (ARGB, palCount entries) into
    // m_previewRGBA and (re)uploads the texture. Skips the decode entirely (returning the
    // cached verdict) when the emulator frame and content key match `cache` — this used to
    // memcpy + per-pixel-decode + reallocate a texture for EVERY bitmap EVERY UI frame.
    bool BuildPreview(const DebugResource& bmp, const uint32_t* palette, int palCount,
                      const std::vector<adm::AmigaEmulator::MemoryRegion>& regions,
                      unsigned int& texture, int& width, int& height, PreviewCache& cache,
                      uint64_t emuFrame);

    void DrawAddress(const char* label, uint32_t addr, uint32_t size);

    EmulatorView* m_emulatorView;
    bool m_open = false;
    bool m_noFocus = false;
    bool m_turboSuppressed = false;
    const AddressActionContext* m_addrCtx = nullptr;
    std::function<void(uint32_t)> m_onJumpToMemory;
    std::function<void(bool)> m_onSupportChanged;

    // Selection persists across frames by resource address (survives registry reshuffles).
    uint32_t m_selBitmapAddr = 0;
    // Palette selection: 0 = live copper palette; otherwise a registered palette address.
    uint32_t m_selPaletteAddr = 0;
    int m_zoom = 2;

    // Preview texture + scratch decode buffer.
    unsigned int m_previewTex = 0; // GLuint
    int m_previewW = 0;
    int m_previewH = 0;
    PreviewCache m_previewCache;
    std::vector<uint8_t> m_previewRGBA;
    std::vector<uint8_t> m_planeScratch; // raw bitmap bytes read from Amiga RAM

    struct Thumbnail
    {
        unsigned int texture = 0;
        int width = 0;
        int height = 0;
        PreviewCache cache;
    };
    std::unordered_map<uint32_t, Thumbnail> m_thumbnails;

    // Region list refreshed only when the bank-layout signature changes.
    std::vector<adm::AmigaEmulator::MemoryRegion> m_regions;
    uint64_t m_regionsSig = ~0ull;
    // Scratch for the end-of-frame palette replay (was a function-local static).
    std::vector<adm::AmigaEmulator::CopperExecEvent> m_copperEventScratch;
    // Scratch for the resource registry fetch (was a function-local static).
    std::vector<DebugResource> m_resourceScratch;
};

} // namespace adm::ui
