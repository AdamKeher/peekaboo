#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ExecIntrospect.h"

namespace adm::core {

// One entry of ExecBase->ResModules, or of a KickTag list.
struct ResidentModule
{
    uint32_t addr = 0;      // address of the struct Resident
    bool valid = false;     // matchword and matchtag both check out
    bool indirect = false;  // reached via a bit-31 "continue at this list" pointer
    uint16_t matchWord = 0; // should be $4AFC
    uint32_t matchTag = 0;  // should equal addr
    uint32_t endSkip = 0;
    uint8_t flags = 0;
    uint8_t version = 0;
    uint8_t type = 0;
    int8_t pri = 0;
    uint32_t initFn = 0;
    std::string name;
    std::string idString;
    std::string typeName; // "Library", "Device", "Resource", ...
};

// A capture vector that fires on reset.
struct CaptureVector
{
    std::string name;  // "ColdCapture" / "CoolCapture" / "WarmCapture"
    uint32_t addr = 0; // where the field lives
    uint32_t value = 0;
    bool set = false;         // non-zero
    bool pointsToRom = false; // inside the Kickstart region we were given
};

// A memory pool whose mh_Upper sits below the end of its region — the classic
// "steal memory off the top so it survives a reset" trick.
struct StolenMemory
{
    uint32_t memHeader = 0;
    std::string name;
    uint32_t upper = 0;
    uint32_t regionEnd = 0;
    uint32_t stolenBytes = 0;
};

struct RebootSurvivors
{
    bool available = false;
    std::string hint;

    uint32_t execBase = 0;
    std::vector<CaptureVector> captures;

    uint32_t kickMemPtr = 0;
    uint32_t kickTagPtr = 0;
    uint32_t kickCheckSum = 0; // as stored in ExecBase
    // The one's-complement sum-with-carry of KickMemPtr and KickTagPtr. Exec's
    // own SumKickData varies between Kickstart versions, so this is offered for
    // comparison rather than as a verdict on whether the reset will honour them.
    uint32_t pointerSumCheckSum = 0;
    bool matchesPointerSum = false;
    // Resident tags reachable through KickTagPtr.
    std::vector<ResidentModule> kickTags;
    // MemList entries reachable through KickMemPtr, as (address, bytes) pairs.
    std::vector<std::pair<uint32_t, uint32_t>> kickMemEntries;

    std::vector<StolenMemory> stolenMemory;

    // True when anything at all would survive a reset.
    bool AnySurvivors() const;
};

/**
 * @brief Finds the things that outlive a reset.
 *
 * If a bootblock menu reboots the machine rather than chaining straight into the
 * DOS boot, whatever it wants to keep has to travel through one of a short list
 * of channels: a resident module, a KickTag, one of the three capture vectors,
 * or memory stolen off the top of a pool. This enumerates exactly that list.
 */
class ResidentScanner
{
public:
    // A memory region, used to decide whether a capture vector points at ROM and
    // to spot memory stolen off the end of a pool.
    struct Region
    {
        uint32_t base = 0;
        uint32_t size = 0;
        std::string name;
        bool isRom = false;
    };

    // Walks ExecBase->ResModules. The array is null-terminated; an entry with
    // bit 31 set is not a Resident but a pointer to another array to continue in.
    static std::vector<ResidentModule> ListResidentModules(const exec::Readers& r,
                                                           uint32_t execBase);

    static RebootSurvivors ScanRebootSurvivors(const exec::Readers& r, uint32_t execBase,
                                               const std::vector<Region>& regions);

    // Parses one struct Resident, validating its matchword/matchtag.
    static ResidentModule ReadResident(const exec::Readers& r, uint32_t addr);

private:
    static constexpr int kMaxModules = 512;
};

} // namespace adm::core
