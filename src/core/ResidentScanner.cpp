#include "ResidentScanner.h"

#include <algorithm>

#include "HunkParser.h"

namespace adm::core {

namespace {

// The bit-31 flag on a ResModules entry means "this is not a Resident, it is the
// address of another array to keep reading from".
constexpr uint32_t kIndirectFlag = 0x80000000u;

// A KickMem MemList entry is a struct MemList: ml_Node(14) ml_NumEntries(14.w)
// then ml_ME[] of {me_Addr(4) me_Length(4)}.
constexpr uint32_t ML_NUMENTRIES = 14, ML_ME = 16, ME_SIZE = 8;

std::string TypeName(uint8_t type)
{
    return HunkParser::GetResidentTypeName(static_cast<ResidentType>(type));
}

} // namespace

ResidentModule ResidentScanner::ReadResident(const exec::Readers& r, uint32_t addr)
{
    ResidentModule m;
    m.addr = addr;
    m.matchWord = r.Word(addr + exec::RTAG_MATCHWORD);
    m.matchTag = r.Long(addr + exec::RTAG_MATCHTAG);
    m.endSkip = r.Long(addr + exec::RTAG_ENDSKIP);
    m.flags = r.Byte(addr + exec::RTAG_FLAGS);
    m.version = r.Byte(addr + exec::RTAG_VERSION);
    m.type = r.Byte(addr + exec::RTAG_TYPE);
    m.pri = static_cast<int8_t>(r.Byte(addr + exec::RTAG_PRI));
    m.initFn = r.Long(addr + exec::RTAG_INIT);
    m.name = exec::ReadCString(r, r.Long(addr + exec::RTAG_NAME));
    m.idString = exec::ReadCString(r, r.Long(addr + exec::RTAG_IDSTRING));
    m.typeName = TypeName(m.type);
    // A RomTag is only a RomTag if it says so and points at itself; without both
    // checks any longword that happens to read $4AFC would be reported.
    m.valid = m.matchWord == exec::RTAG_MATCHWORD_VALUE && m.matchTag == addr;
    return m;
}

std::vector<ResidentModule> ResidentScanner::ListResidentModules(const exec::Readers& r,
                                                                 uint32_t execBase)
{
    std::vector<ResidentModule> out;
    uint32_t array = r.Long(execBase + exec::EB_RESMODULES);
    if (array == 0)
        return out;

    bool indirect = false;
    for (int guard = 0; guard < kMaxModules; ++guard) {
        bool ok = false;
        const uint32_t entry = r.readLong ? r.readLong(array, ok) : 0;
        if (!ok || entry == 0)
            break;
        if (entry & kIndirectFlag) {
            // Continue in the array this points at, rather than treating the
            // value as a Resident.
            array = entry & ~kIndirectFlag;
            indirect = true;
            continue;
        }
        ResidentModule m = ReadResident(r, entry);
        m.indirect = indirect;
        out.push_back(std::move(m));
        array += 4;
    }
    return out;
}

namespace {

const ResidentScanner::Region* FindRegionImpl(const std::vector<ResidentScanner::Region>& regions,
                                              uint32_t addr)
{
    for (const auto& reg : regions)
        if (reg.size != 0 && addr >= reg.base && addr - reg.base < reg.size)
            return &reg;
    return nullptr;
}

} // namespace

RebootSurvivors ResidentScanner::ScanRebootSurvivors(const exec::Readers& r, uint32_t execBase,
                                                     const std::vector<Region>& regions)
{
    RebootSurvivors s;
    if (execBase == 0) {
        s.hint = "ExecBase unavailable — no AmigaOS is booted, so nothing can be registered to "
                 "survive a reset.";
        return s;
    }
    s.available = true;
    s.execBase = execBase;

    struct CaptureSpec
    {
        const char* name;
        uint32_t offset;
    };
    const CaptureSpec specs[] = {{"ColdCapture", exec::EB_COLDCAPTURE},
                                 {"CoolCapture", exec::EB_COOLCAPTURE},
                                 {"WarmCapture", exec::EB_WARMCAPTURE}};
    for (const auto& spec : specs) {
        CaptureVector cv;
        cv.name = spec.name;
        cv.addr = execBase + spec.offset;
        cv.value = r.Long(cv.addr);
        cv.set = cv.value != 0;
        if (const Region* reg = FindRegionImpl(regions, cv.value))
            cv.pointsToRom = reg->isRom;
        s.captures.push_back(std::move(cv));
    }

    s.kickMemPtr = r.Long(execBase + exec::EB_KICKMEMPTR);
    s.kickTagPtr = r.Long(execBase + exec::EB_KICKTAGPTR);
    s.kickCheckSum = r.Long(execBase + exec::EB_KICKCHECKSUM);

    // Exec validates KickCheckSum on reset and drops both pointers if it does
    // not agree. The exact sum differs between Kickstart versions, so report the
    // one-line pointer sum for comparison rather than pronouncing a verdict the
    // machine might disagree with.
    if (s.kickMemPtr != 0 || s.kickTagPtr != 0) {
        uint32_t sum = 0;
        for (uint32_t v : {s.kickMemPtr, s.kickTagPtr}) {
            const uint32_t prev = sum;
            sum += v;
            if (sum < prev)
                ++sum; // add the carry back in
        }
        s.pointerSumCheckSum = ~sum;
        s.matchesPointerSum = s.pointerSumCheckSum == s.kickCheckSum;
    }

    // KickTagPtr is a null-terminated array of Resident pointers, with the same
    // bit-31 indirection rule as ResModules.
    if (s.kickTagPtr) {
        uint32_t array = s.kickTagPtr;
        for (int guard = 0; guard < kMaxModules; ++guard) {
            bool ok = false;
            const uint32_t entry = r.readLong ? r.readLong(array, ok) : 0;
            if (!ok || entry == 0)
                break;
            if (entry & kIndirectFlag) {
                array = entry & ~kIndirectFlag;
                continue;
            }
            s.kickTags.push_back(ReadResident(r, entry));
            array += 4;
        }
    }

    // KickMemPtr is a chain of MemLists describing the blocks to AllocAbs on the
    // way back up — i.e. the memory the tags live in.
    if (s.kickMemPtr) {
        uint32_t node = s.kickMemPtr;
        for (int guard = 0; guard < kMaxModules && node != 0; ++guard) {
            const uint16_t count = r.Word(node + ML_NUMENTRIES);
            for (uint16_t i = 0; i < count && i < 64; ++i) {
                const uint32_t base = node + ML_ME + static_cast<uint32_t>(i) * ME_SIZE;
                s.kickMemEntries.emplace_back(r.Long(base), r.Long(base + 4));
            }
            node = r.Long(node + exec::LN_SUCC);
        }
    }

    for (const auto& mh : exec::ListMemHeaders(r, execBase)) {
        const Region* reg = FindRegionImpl(regions, mh.lower);
        if (!reg)
            continue;
        const uint32_t regionEnd = reg->base + reg->size;
        if (mh.upper >= regionEnd)
            continue;
        StolenMemory sm;
        sm.memHeader = mh.node;
        sm.name = mh.name.empty() ? reg->name : mh.name;
        sm.upper = mh.upper;
        sm.regionEnd = regionEnd;
        sm.stolenBytes = regionEnd - mh.upper;
        s.stolenMemory.push_back(std::move(sm));
    }

    return s;
}

bool RebootSurvivors::AnySurvivors() const
{
    if (std::any_of(captures.begin(), captures.end(), [](const CaptureVector& c) { return c.set; }))
        return true;
    return kickTagPtr != 0 || kickMemPtr != 0 || !stolenMemory.empty();
}

} // namespace adm::core
