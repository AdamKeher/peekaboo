#include "DebuggerBartmanView.h"

#include <glad/glad.h>
#include <imgui.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../core/AmigaCustom.h"
#include "../IconsFontAwesome6.h"
#include "../OcsColor.h"
#include "EmulatorView.h"
#include "TurboSuppressedPlaceholder.h"

namespace adm::ui {

namespace {

constexpr int kMaxDim = 4096; // sanity clamp for width/height
constexpr int kMaxPlanes = 8;
constexpr uint32_t kMaxBitmapBytes = 4u * 1024 * 1024;

// Palette-source sentinels stored in m_selPaletteAddr. Registered palettes live in chip RAM at
// low addresses, so these high values never collide with a real resource address.
constexpr uint32_t kPalLiveCopper = 0;           // COLOR00-31 as they read right now
constexpr uint32_t kPalEndOfFrame = 0xFFFFFFFFu; // replay the frame's copper COLOR writes
constexpr uint32_t kPalDebugRamp = 0xFFFFFFFEu;  // synthetic grayscale ramp (always visible)

GLuint UploadRGBA(GLuint existing, int w, int h, int prevW, int prevH, const uint8_t* rgba)
{
    GLuint tex = existing;
    const bool sameSize = tex && w == prevW && h == prevH;
    if (!tex)
        glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (sameSize) {
        // Same geometry: update in place instead of reallocating the texture every frame.
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void DetailRow(const char* k, const char* fmt, ...)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(k);
    ImGui::TableNextColumn();
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
}

const char* TypeName(uint16_t t)
{
    switch (t) {
        case 0:
            return "Bitmap";
        case 1:
            return "Palette";
        case 2:
            return "Copperlist";
        default:
            return "Unknown";
    }
}

} // namespace

DebuggerBartmanView::DebuggerBartmanView(EmulatorView* emulatorView) : m_emulatorView(emulatorView)
{
}

DebuggerBartmanView::~DebuggerBartmanView()
{
    if (m_previewTex) {
        glDeleteTextures(1, &m_previewTex);
        m_previewTex = 0;
    }
    for (auto& [address, thumbnail] : m_thumbnails) {
        (void)address;
        if (thumbnail.texture)
            glDeleteTextures(1, &thumbnail.texture);
    }
}

void DebuggerBartmanView::Clear()
{
    m_selBitmapAddr = 0;
    m_selPaletteAddr = 0;
    m_previewW = m_previewH = 0;
    // Texture object is retained (cheap) and reused on the next Draw.
}

void DebuggerBartmanView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
}

bool DebuggerBartmanView::ReadAmigaBytes(
    uint32_t addr, uint8_t* dst, uint32_t len,
    const std::vector<adm::AmigaEmulator::MemoryRegion>& regions) const
{
    std::memset(dst, 0, len);
    for (const auto& r : regions) {
        if (!r.hostPtr)
            continue;
        if (addr >= r.amigaAddr && addr < r.amigaAddr + r.size) {
            uint32_t avail = r.amigaAddr + r.size - addr;
            uint32_t n = std::min(len, avail);
            std::memcpy(dst, r.hostPtr + (addr - r.amigaAddr), n);
            return true; // best-effort: remainder (if any) stays zero-filled
        }
    }
    return false;
}

bool DebuggerBartmanView::BuildPreview(const DebugResource& bmp, const uint32_t* palette,
                                       int palCount,
                                       const std::vector<adm::AmigaEmulator::MemoryRegion>& regions,
                                       unsigned int& texture, int& previewWidth, int& previewHeight,
                                       PreviewCache& cache, uint64_t emuFrame)
{
    // Key on everything that changes the pixels: source bitmap identity/geometry + palette.
    // With an unchanged key on the same emulated frame the previous decode is still exact.
    uint64_t key = 1469598103934665603ull; // FNV-1a
    auto mix = [&key](uint64_t v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    mix(bmp.address);
    mix((static_cast<uint64_t>(static_cast<uint32_t>(bmp.width)) << 32) |
        static_cast<uint32_t>(bmp.height));
    mix((static_cast<uint64_t>(static_cast<uint32_t>(bmp.numPlanes)) << 32) | bmp.flags);
    for (int i = 0; i < palCount; ++i)
        mix(palette[i]);
    if (cache.frame == emuFrame && cache.key == key)
        return cache.ok;
    cache.frame = emuFrame;
    cache.key = key;
    cache.ok = false;

    int width = bmp.width;
    int height = bmp.height;
    int numPlanes = bmp.numPlanes;
    if (width <= 0 || height <= 0 || numPlanes <= 0 || width > kMaxDim || height > kMaxDim ||
        numPlanes > kMaxPlanes)
        return false;

    const bool interleaved = (bmp.flags & 0x1) != 0; // debug_resource_bitmap_interleaved
    const int rowBytes = ((width + 15) / 16) * 2;    // word-aligned bitplane row
    const uint32_t total = static_cast<uint32_t>(rowBytes) * height * numPlanes;
    if (total == 0 || total > kMaxBitmapBytes)
        return false;

    m_planeScratch.resize(total);
    if (!ReadAmigaBytes(bmp.address, m_planeScratch.data(), total, regions))
        return false;
    const uint8_t* base = m_planeScratch.data();

    m_previewRGBA.assign(static_cast<size_t>(width) * height * 4, 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int index = 0;
            const int byteInRow = x >> 3;
            const int bit = 7 - (x & 7);
            for (int p = 0; p < numPlanes; ++p) {
                uint32_t off;
                if (interleaved)
                    off =
                        static_cast<uint32_t>(y) * numPlanes * rowBytes + p * rowBytes + byteInRow;
                else
                    off = (static_cast<uint32_t>(p) * height + y) * rowBytes + byteInRow;
                if (off < total && ((base[off] >> bit) & 1))
                    index |= (1 << p);
            }
            uint32_t c = (index < palCount) ? palette[index] : IM_COL32(0, 0, 0, 255);
            uint8_t* d = &m_previewRGBA[(static_cast<size_t>(y) * width + x) * 4];
            d[0] = c & 0xFF;
            d[1] = (c >> 8) & 0xFF;
            d[2] = (c >> 16) & 0xFF;
            d[3] = (c >> 24) & 0xFF;
        }
    }

    texture = UploadRGBA(texture, width, height, previewWidth, previewHeight, m_previewRGBA.data());
    previewWidth = width;
    previewHeight = height;
    cache.ok = true;
    return true;
}

void DebuggerBartmanView::DrawAddress(const char* label, uint32_t addr, uint32_t size)
{
    ImGui::Text("%s:", label);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "$%06X", addr);
    if (size > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%u bytes)", size);
    }
    ImGui::PushID((int)(addr ^ (size << 3)));
    if (m_onJumpToMemory) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_MEMORY " View"))
            m_onJumpToMemory(addr);
    }
    if (m_addrCtx && m_addrCtx->showInMemoryMap) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_MAP " Map"))
            m_addrCtx->showInMemoryMap(addr);
    }
    ImGui::PopID();
}

void DebuggerBartmanView::Draw()
{
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (!ImGui::Begin(ICON_FA_FILL_DRIP " Bartman Bitmaps###BartmanBitmaps##debugger", &m_open,
                      flags)) {
        ImGui::End();
        return;
    }

    auto* emu = m_emulatorView ? m_emulatorView->GetEmulator() : nullptr;
    // When the emulator is stopped, UnloadCore() frees the core DLL and nulls the adm_* pointers.
    // Bail before touching any of them (calling a freed export would crash). A core loaded but
    // never run (Preferences and the setup wizard load one to identify ROMs) has no machine
    // memory for them to read either.
    if (!emu || !emu->IsCoreLoaded() || !emu->IsGameLoaded()) {
        ImGui::TextUnformatted("Emulator is not running — start a program to view registered "
                               "bitmaps and palettes.");
        ImGui::End();
        return;
    }

    // --- Availability / opt-in gate --------------------------------------------------------
    if (!emu->HasBartmanDebug()) {
        ImGui::TextWrapped(
            "This PuAE core does not export the Bartman debug-resource registry. Rebuild the "
            "core (scripts/build_puae) to enable this window.");
        ImGui::End();
        return;
    }

    if (!emu->IsBartmanDebugSupportEnabled()) {
        ImGui::TextWrapped(
            "Bartman debug support is disabled. Enable it in Preferences \xE2\x86\x92 General "
            "\xE2\x86\x92 \"Enable Bartman debug support\", then re-run your instrumented program "
            "to capture its registered bitmaps and palettes.");
        ImGui::End();
        return;
    }

    // Missing-export check — the Bartman feature relies on several adm_* symbols that only
    // exist in a freshly patched core. If the loaded DLL is stale/partial, some resolve to
    // null and the feature degrades silently; surface exactly which ones are absent so a
    // rebuild is an obvious next step rather than a guess.
    auto missingSyms = emu->GetMissingBartmanSymbols();
    if (!missingSyms.empty()) {
        std::string joined;
        for (size_t i = 0; i < missingSyms.size(); ++i)
            joined += (i ? ", " : "") + missingSyms[i];
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("[!] Loaded core is missing %zu Bartman debug export(s): %s. Rebuild "
                           "the core (scripts/build_puae) — the feature will not work correctly "
                           "until then.",
                           missingSyms.size(), joined.c_str());
        ImGui::PopStyleColor();
    }

    // Gate diagnostic — confirms the $F0FF60 uaelib gate is installed with an opcode that
    // Bartman's debug_cmd() guard accepts (0x4EB9 JSR, or 0xA00E). If this is wrong, no
    // debug_register_* call ever reaches us.
    {
        adm::AmigaEmulator::DebugGateInfo gi;
        if (emu->GetDebugGateInfo(gi)) {
            bool ok = (gi.opcode == 0x4EB9 || gi.opcode == 0xA00E);
            ImGui::TextDisabled("Gate $%06X = $%04X", gi.gateAddr, gi.opcode);
            ImGui::SameLine();
            if (ok)
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "installed");
            else if (gi.bootRomType == 0)
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f),
                                   "boot rom not active — reset / re-run the program");
            else
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "unexpected opcode");
        } else if (!emu->HasDebugGateInfo()) {
            // The gate diagnostic export is absent from this DLL — report it rather than
            // rendering nothing (which reads as "no problem").
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f),
                               "[!] Gate status unavailable: adm_get_debug_gate_info not exported "
                               "by the loaded core (rebuild required).");
        }
    }

    ImGui::Separator();

    if (m_turboSuppressed) {
        DrawTurboSuppressedPlaceholder();
        ImGui::End();
        return;
    }

    // --- Fetch the current registry --------------------------------------------------------
    if (m_resourceScratch.empty())
        m_resourceScratch.resize(adm::AmigaEmulator::kMaxDebugResources);
    DebugResource* resBuf = m_resourceScratch.data();
    int count = emu->GetDebugResources(resBuf, adm::AmigaEmulator::kMaxDebugResources);

    std::vector<const DebugResource*> bitmaps;
    std::vector<const DebugResource*> palettes;
    // The registry is shared with resource types this window has no business drawing: Bartman's
    // own copper lists (2) and the Crass engine's profiler block (3), which is a struct of
    // counters and would be nonsense as pixels or colours. Count them so a program that
    // registers one is not reported as having registered nothing, but never select them.
    int otherCount = 0;
    int profileCount = 0;
    for (int i = 0; i < count; ++i) {
        if (resBuf[i].type == static_cast<uint16_t>(adm::AmigaEmulator::DebugResourceType::Bitmap))
            bitmaps.push_back(&resBuf[i]);
        else if (resBuf[i].type ==
                 static_cast<uint16_t>(adm::AmigaEmulator::DebugResourceType::Palette))
            palettes.push_back(&resBuf[i]);
        else if (resBuf[i].type ==
                 static_cast<uint16_t>(adm::AmigaEmulator::DebugResourceType::Profile))
            ++profileCount;
        else
            ++otherCount;
    }

    ImGui::Text("Registered: %zu bitmap(s), %zu palette(s)", bitmaps.size(), palettes.size());
    if (profileCount > 0 || otherCount > 0) {
        ImGui::SameLine();
        if (profileCount > 0 && otherCount > 0)
            ImGui::TextDisabled("+ %d profile, %d other", profileCount, otherCount);
        else if (profileCount > 0)
            ImGui::TextDisabled("+ %d profile block(s) — shown in the emulator perf overlay",
                                profileCount);
        else
            ImGui::TextDisabled("+ %d other", otherCount);
    }
    if (count == 0) {
        ImGui::TextDisabled(
            "Nothing registered yet — run a program that calls debug_register_*().");
        ImGui::End();
        return;
    }

    // Keep the selected resource stable as the registry changes.
    const DebugResource* selBmp = nullptr;
    for (auto* b : bitmaps)
        if (b->address == m_selBitmapAddr)
            selBmp = b;
    if (!selBmp && !bitmaps.empty()) {
        selBmp = bitmaps.front();
        m_selBitmapAddr = selBmp->address;
    }

    char label[96];
    // Resolve palette selection before building the previews.
    const DebugResource* selPal = nullptr;
    for (auto* p : palettes)
        if (p->address == m_selPaletteAddr)
            selPal = p;
    // Only a *registered-address* selection can "fall away"; the synthetic sentinels always stay.
    const bool isSynthetic =
        (m_selPaletteAddr == kPalLiveCopper || m_selPaletteAddr == kPalEndOfFrame ||
         m_selPaletteAddr == kPalDebugRamp);
    if (!isSynthetic && !selPal)
        m_selPaletteAddr = kPalLiveCopper; // registered palette gone — fall back to live copper

    const char* kLiveLabel = "Live copper palette (COLOR00-31)";
    const char* kEofLabel = "End-of-frame palette (replay copper)";
    const char* kRampLabel = "Default ramp (grayscale)";
    auto palLabel = [&](const DebugResource* p) {
        std::snprintf(label, sizeof(label), "%s  (%d entries @ $%06X)",
                      p->name[0] ? p->name : "(unnamed)", p->numEntries, p->address);
        return label;
    };
    const char* palPreview = kLiveLabel;
    if (m_selPaletteAddr == kPalEndOfFrame)
        palPreview = kEofLabel;
    else if (m_selPaletteAddr == kPalDebugRamp)
        palPreview = kRampLabel;
    else if (selPal)
        palPreview = palLabel(selPal);
    // Refresh the region vector only when the bank layout actually changes (same pattern as
    // DebuggerMemoryView/BitplanesView) instead of copying it every frame.
    const uint64_t regionsSig = m_emulatorView->GetMemoryRegionsSignature();
    if (regionsSig != m_regionsSig) {
        m_regions = m_emulatorView->GetFreshMemoryRegions();
        m_regionsSig = regionsSig;
    }
    const auto& regions = m_regions;
    const uint64_t emuFrame = emu->GetTotalFrameCount();

    // --- Build the active palette ----------------------------------------------------------
    uint32_t pal[256];
    int palCount = 0;
    if (m_selPaletteAddr == kPalLiveCopper) {
        core::Custom custom{};
        if (emu->GetCustomRegisters(custom)) {
            for (int i = 0; i < 32; ++i)
                pal[i] = OcsColorToImU32(custom.color[i]);
            palCount = 32;
        }
    } else if (m_selPaletteAddr == kPalEndOfFrame) {
        // Start from the current registers, then replay every copper COLOR00-31 write from the
        // last frame in execution order (last write wins) to reach the end-of-frame palette.
        // This is what makes a copper-driven palette (black at the top, rebuilt per scanline)
        // actually previewable.
        uint16_t colorRegs[32] = {0};
        core::Custom custom{};
        if (emu->GetCustomRegisters(custom))
            for (int i = 0; i < 32; ++i)
                colorRegs[i] = custom.color[i];
        // Member, not function-local static: the static was ~330 KB of process-global scratch
        // shared across every instance of this view.
        auto& events = m_copperEventScratch;
        if (events.empty())
            events.resize(adm::AmigaEmulator::kMaxCopperExecEvents);
        int n = emu->GetCopperExecEvents(events.data(), static_cast<int>(events.size()));
        for (int i = 0; i < n; ++i) {
            uint16_t reg = events[i].word1 & 0x1FE; // register offset, MOVE clears bit 0
            if (reg >= 0x180 && reg <= 0x1BE)       // COLOR00..COLOR31
                colorRegs[(reg - 0x180) / 2] = events[i].word2;
        }
        for (int i = 0; i < 32; ++i)
            pal[i] = OcsColorToImU32(colorRegs[i]);
        palCount = 32;
    } else if (m_selPaletteAddr == kPalDebugRamp) {
        // Synthetic grayscale ramp sized to the selected bitmap's depth, so the plane structure
        // is always visible regardless of the real (possibly all-black) palette.
        int planes = selBmp ? selBmp->numPlanes : 5;
        int colors = 1 << std::clamp(planes, 1, 8);
        colors = std::clamp(colors, 2, 256);
        for (int i = 0; i < colors; ++i) {
            int v = (i * 255) / (colors - 1);
            pal[i] = IM_COL32(v, v, v, 255);
        }
        palCount = colors;
    } else if (selPal) {
        int n = std::min<int>(selPal->numEntries, 256);
        for (int i = 0; i < n; ++i) {
            uint8_t w[2] = {0, 0};
            ReadAmigaBytes(selPal->address + i * 2, w, 2, regions);
            uint16_t c = (uint16_t)((w[0] << 8) | w[1]); // big-endian colour word
            pal[i] = OcsColorToImU32(c);
        }
        palCount = n;
    }
    if (palCount == 0) {
        pal[0] = IM_COL32(0, 0, 0, 255);
        palCount = 1;
    }

    // Drop textures for resources that are no longer registered.
    for (auto it = m_thumbnails.begin(); it != m_thumbnails.end();) {
        bool present = std::any_of(bitmaps.begin(), bitmaps.end(),
                                   [&](const DebugResource* b) { return b->address == it->first; });
        if (!present) {
            if (it->second.texture)
                glDeleteTextures(1, &it->second.texture);
            it = m_thumbnails.erase(it);
        } else {
            ++it;
        }
    }

    const float browserWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.28f, 190.0f, 280.0f);
    ImGui::BeginChild("##bitmap_browser", ImVec2(browserWidth, 0), ImGuiChildFlags_Borders);
    for (auto* bitmap : bitmaps) {
        ImGui::PushID(static_cast<int>(bitmap->address));
        auto& thumbnail = m_thumbnails[bitmap->address];
        bool decoded = BuildPreview(*bitmap, pal, palCount, regions, thumbnail.texture,
                                    thumbnail.width, thumbnail.height, thumbnail.cache, emuFrame);
        const bool selected = bitmap->address == m_selBitmapAddr;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::BeginChild("##entry", ImVec2(0, 82), ImGuiChildFlags_Borders);
        if (decoded && thumbnail.texture) {
            const float scale = std::min(68.0f / std::max(1, thumbnail.width),
                                         68.0f / std::max(1, thumbnail.height));
            ImGui::Image((ImTextureID)(intptr_t)thumbnail.texture,
                         ImVec2(thumbnail.width * scale, thumbnail.height * scale));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                const float hoverScale = std::min(512.0f / std::max(1, thumbnail.width),
                                                  384.0f / std::max(1, thumbnail.height));
                ImGui::Image((ImTextureID)(intptr_t)thumbnail.texture,
                             ImVec2(thumbnail.width * hoverScale, thumbnail.height * hoverScale));
                ImGui::Text("%s  %dx%dx%d", bitmap->name[0] ? bitmap->name : "(unnamed)",
                            bitmap->width, bitmap->height, bitmap->numPlanes);
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", bitmap->name[0] ? bitmap->name : "(unnamed)");
        ImGui::TextDisabled("%dx%dx%d", bitmap->width, bitmap->height, bitmap->numPlanes);
        ImGui::TextDisabled("$%06X", bitmap->address);
        ImGui::EndGroup();
        ImGui::EndChild();
        if (selected)
            ImGui::PopStyleColor();
        if (ImGui::IsItemClicked()) {
            m_selBitmapAddr = bitmap->address;
            selBmp = bitmap;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##bitmap_workspace", ImVec2(0, 0), ImGuiChildFlags_None);

    // Details and palette controls stay above the large preview.
    if (selBmp) {
        if (ImGui::BeginTable("##bmpdetails", 2,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Field");
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            DetailRow("Name", "%s", selBmp->name[0] ? selBmp->name : "(unnamed)");
            DetailRow("Type", "%s", TypeName(selBmp->type));
            DetailRow("Dimensions", "%d x %d", selBmp->width, selBmp->height);
            DetailRow("Depth", "%d planes (%d colours)", selBmp->numPlanes, 1 << selBmp->numPlanes);
            DetailRow("Size", "%u bytes", selBmp->size);
            std::string fl;
            if (selBmp->flags & 0x1)
                fl += "interleaved ";
            if (selBmp->flags & 0x2)
                fl += "masked ";
            if (selBmp->flags & 0x4)
                fl += "ham ";
            if (fl.empty())
                fl = "none";
            DetailRow("Flags", "%s (0x%X)", fl.c_str(), selBmp->flags);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        DrawAddress("Bitmap data", selBmp->address, selBmp->size);
        if (m_selPaletteAddr != 0 && selPal)
            DrawAddress("Palette data", selPal->address, selPal->size);

        if (selBmp->flags & 0x6)
            ImGui::TextDisabled("Note: masked/HAM decoding is approximate in the preview.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Palette source");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##pal", palPreview)) {
        if (ImGui::Selectable(kLiveLabel, m_selPaletteAddr == kPalLiveCopper))
            m_selPaletteAddr = kPalLiveCopper;
        if (ImGui::Selectable(kEofLabel, m_selPaletteAddr == kPalEndOfFrame))
            m_selPaletteAddr = kPalEndOfFrame;
        if (ImGui::Selectable(kRampLabel, m_selPaletteAddr == kPalDebugRamp))
            m_selPaletteAddr = kPalDebugRamp;
        for (auto* p : palettes) {
            bool selected = p->address == m_selPaletteAddr;
            if (ImGui::Selectable(palLabel(p), selected))
                m_selPaletteAddr = p->address;
        }
        ImGui::EndCombo();
    }

    // --- Palette swatch strip --------------------------------------------------------------
    ImGui::Separator();
    ImGui::TextUnformatted("Palette");
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float sw = 14.0f;
        int perRow = std::max(1, (int)(ImGui::GetContentRegionAvail().x / sw));
        for (int i = 0; i < palCount; ++i) {
            int cx = i % perRow, cy = i / perRow;
            ImVec2 a(p0.x + cx * sw, p0.y + cy * sw);
            ImVec2 b(a.x + sw - 1, a.y + sw - 1);
            dl->AddRectFilled(a, b, pal[i]);
            dl->AddRect(a, b, IM_COL32(0, 0, 0, 128));
        }
        int rows = (palCount + perRow - 1) / perRow;
        ImGui::Dummy(ImVec2(0, rows * sw + 4));
    }

    // --- Preview ---------------------------------------------------------------------------
    ImGui::Separator();
    ImGui::TextUnformatted("Preview");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderInt("zoom", &m_zoom, 1, 8);

    bool ok = selBmp && BuildPreview(*selBmp, pal, palCount, regions, m_previewTex, m_previewW,
                                     m_previewH, m_previewCache, emuFrame);
    if (ok && m_previewTex) {
        ImVec2 sz(m_previewW * (float)m_zoom, m_previewH * (float)m_zoom);
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::Image((ImTextureID)(intptr_t)m_previewTex, sz);
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("Cannot decode this bitmap (out of range or not in RAM).");
    }

    ImGui::EndChild();

    ImGui::End();
}

} // namespace adm::ui
