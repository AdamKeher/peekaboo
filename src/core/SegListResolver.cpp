#include "SegListResolver.h"

namespace adm::core {

namespace {

// AmigaOS structure offsets (see exec/execbase.h, exec/tasks.h, dos/dosextens.h).
// These match the constants UAE's own debugger uses to walk the seglist.
constexpr uint32_t kExecBasePtr = 4;       // absolute long holding ExecBase
constexpr uint32_t kThisTaskOffset = 276;  // ExecBase->ThisTask
constexpr uint32_t kNodeTypeOffset = 8;    // Node.ln_Type
constexpr uint8_t kNtProcess = 13;         // NT_PROCESS
constexpr uint32_t kPrCliOffset = 172;     // Process->pr_CLI (BPTR)
constexpr uint32_t kCliModuleOffset = 60;  // CLI->cli_Module (BPTR to seglist)
constexpr uint32_t kPrSegListOffset = 128; // Process->pr_SegList (BPTR to array)
constexpr uint32_t kSegArrayEntry3 = 12;   // seglist array [3] (the loaded segs)

// A BPTR is a byte address shifted right by 2; convert back to an APTR.
inline uint32_t Bptr2Aptr(uint32_t bptr) { return bptr << 2; }

// Locate the head segment node (APTR) of the running process, or 0 on failure.
uint32_t FindSeglistHead(const SegListResolver::ReadLongFn& readLong,
                         const SegListResolver::ReadByteFn& readByte)
{
    bool ok = true;
    const uint32_t execBase = readLong(kExecBasePtr, ok);
    if (!ok || execBase == 0) {
        return 0;
    }
    const uint32_t task = readLong(execBase + kThisTaskOffset, ok);
    if (!ok || task == 0) {
        return 0;
    }
    // Only processes (not plain tasks) own a segment list.
    const uint8_t nodeType = readByte(task + kNodeTypeOffset, ok);
    if (!ok || nodeType != kNtProcess) {
        return 0;
    }

    // Prefer the CLI's module (a shell-launched program); fall back to pr_SegList.
    const uint32_t cli = Bptr2Aptr(readLong(task + kPrCliOffset, ok));
    if (ok && cli != 0) {
        const uint32_t seg = Bptr2Aptr(readLong(cli + kCliModuleOffset, ok));
        if (ok && seg != 0) {
            return seg;
        }
    }

    uint32_t segArray = Bptr2Aptr(readLong(task + kPrSegListOffset, ok));
    if (!ok || segArray == 0) {
        return 0;
    }
    const uint32_t seg = Bptr2Aptr(readLong(segArray + kSegArrayEntry3, ok));
    return ok ? seg : 0;
}

} // namespace

std::vector<AmigaSegment> SegListResolver::ResolveActiveProcess(const ReadLongFn& readLong,
                                                                const ReadByteFn& readByte)
{
    std::vector<AmigaSegment> segments;
    uint32_t seg = FindSeglistHead(readLong, readByte);

    for (int i = 0; seg != 0 && i < kMaxSegments; ++i) {
        bool ok = true;
        // AmigaDOS stores the allocation length in BYTES immediately before the segment node
        // -- it is the AllocMem size that UnLoadSeg hands back to FreeMem. The length longword
        // and the node's next-BPTR longword are both bookkeeping, so the usable payload is
        // eight bytes shorter than the allocation.
        const uint32_t allocBytes = readLong(seg - 4, ok);
        if (!ok || allocBytes < 8) {
            break; // too small to hold the length + next-BPTR longwords
        }
        AmigaSegment s;
        s.base = seg + 4;         // code/data follows the next-BPTR longword
        s.size = allocBytes - 8U; // omit the length and next-BPTR longwords
        segments.push_back(s);

        seg = Bptr2Aptr(readLong(seg, ok)); // node's first long is the next BPTR
        if (!ok) {
            break;
        }
    }
    return segments;
}

std::optional<uint32_t> SegListResolver::ProgramBase(const ReadLongFn& readLong,
                                                     const ReadByteFn& readByte)
{
    auto segments = ResolveActiveProcess(readLong, readByte);
    if (segments.empty()) {
        return std::nullopt;
    }
    return segments.front().base;
}

std::optional<AmigaSegment> SegListResolver::FindSegmentContaining(uint32_t pc,
                                                                   const ReadLongFn& readLong,
                                                                   const ReadByteFn& readByte)
{
    for (const auto& s : ResolveActiveProcess(readLong, readByte)) {
        if (pc >= s.base && pc < s.base + s.size) {
            return s;
        }
    }
    return std::nullopt;
}

} // namespace adm::core
