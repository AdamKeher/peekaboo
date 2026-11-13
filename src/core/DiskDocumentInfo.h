#pragma once

#include <cstdint>
#include <string>

namespace adm {
namespace ui {

/**
 * @brief Lightweight DTO describing an open disk document for display in menus and lists.
 *
 * Populated by Application and consumed by UI layers. Kept separate from DiskDocument
 * to avoid pulling UI-facing state into the core domain model.
 */
struct DiskDocumentInfo
{
    uint32_t id;
    std::string name;
    bool isActive;
    bool isLoadedInEmulator;
    int mountedOnDrive = -1;      // drive index (0-3) if mounted, -1 if not
    bool isHardDiskImage = false; // .hdf/.hdz: belongs on DH0/DH1, never in a floppy drive
};

} // namespace ui
} // namespace adm
