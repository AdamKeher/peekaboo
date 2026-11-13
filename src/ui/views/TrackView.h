#pragma once

#include <vector>

#include "../../core/IpfTypes.h"

namespace adm::ui {

/**
 * @brief Renders the per-track preservation map of an IPF image.
 *
 * Embedded as a "Track Map" tab inside the document window (FileListView), so it
 * is self-contained per document rather than a separate floating panel. Unlike
 * the filesystem block views it is keyed on physical tracks and works even when
 * a disk has no AmigaDOS filesystem (the common case for copy-protected IPFs):
 * a cylinder x head grid coloured by what each track holds — standard layout,
 * named copy-protection, weak/fuzzy-bit zones, partial, or unformatted/custom —
 * with per-track details on hover.
 */
class TrackView
{
public:
    TrackView() = default;

    void SetData(const std::vector<core::IpfTrackInfo>& tracks);
    bool HasData() const { return !m_tracks.empty(); }

    // Draws the summary + grid into the current window/tab (no Begin/End).
    void DrawContent();

private:
    void DrawSummary();
    void DrawGrid();

    std::vector<core::IpfTrackInfo> m_tracks;
    uint32_t m_maxCylinder = 0;
    uint32_t m_maxHead = 0;
    uint32_t m_standardCount = 0;
    uint32_t m_weakCount = 0;
    uint32_t m_protectionCount = 0;
};

} // namespace adm::ui
