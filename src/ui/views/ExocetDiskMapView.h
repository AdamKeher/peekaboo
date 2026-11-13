#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <imgui.h>

#include "../../core/ExocetDiskMap.h"

namespace adm::core {
class ExocetSession;
}

namespace adm::ui {

// Exocet Disk Map: the active disk's own layout, read off the
// ADF by the map in its `.exocet`'s "disk" section (core/ExocetDiskMap.h) - the regions, the
// directory's header, the blocks by kind, the runs they lie in on the disk and every entry. Read
// again when the disk, the `.exocet` or the map changes.
class ExocetDiskMapView
{
public:
    explicit ExocetDiskMapView(core::ExocetSession* session) : m_session(session) {}

    void Open() { m_isOpen = true; }
    void Restore()
    {
        m_isOpen = true;
        m_noFocusOnAppearing = true;
    }
    void Close() { m_isOpen = false; }
    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    void Draw();

private:
    void Refresh(bool force);
    void DrawRegions();
    void DrawHeader();
    void DrawKinds();
    void DrawRuns();
    void DrawEntries();
    std::string SectorPlace(uint32_t sector) const;

    core::ExocetSession* m_session;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;

    // what the map was read from: the .exocet (and its edits), and the ADF as it stood on disk
    std::string m_packPath, m_diskPath;
    uint64_t m_packRevision = ~0ull;
    std::filesystem::file_time_type m_diskTime{};
    uintmax_t m_diskSize = 0;

    std::string m_status; // why there is no map, when there is none
    bool m_haveSpec = false;
    core::ExocetDiskSpec m_spec;
    core::ExocetDiskMap m_map;
    int m_selectedKind = -1; // a kind picked in the Kinds table: its runs and entries marked
};

} // namespace adm::ui
