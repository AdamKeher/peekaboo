#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

/**
 * @brief Read-only introspection of AmigaOS Exec structures in emulated memory.
 *
 * The AR console has walked ExecBase since the EXECBASE/TASKS/LIBS commands were
 * written, but it did so through private methods on ArCommandInterpreter. Several
 * newer features (describe_address, the library-call tracer, the resident scanner)
 * need the same walks, so the primitives live here instead of being forked.
 *
 * Like SegListResolver, this reads through caller-supplied big-endian accessors
 * rather than touching the emulator, so it is unit-testable against a synthetic
 * memory image.
 */
namespace exec {

// --- Amiga Exec structure offsets (exec/execbase.i, exec/nodes.i) -----------
// struct Node (LN): ln_Succ(0) ln_Pred(4) ln_Type(8.b) ln_Pri(9.b) ln_Name(10)
inline constexpr uint32_t LN_SUCC = 0, LN_PRED = 4, LN_TYPE = 8, LN_PRI = 9, LN_NAME = 10;
// struct Library: lib_Version(20.w) lib_Revision(22.w) lib_IdString(24) lib_OpenCnt(32.w)
inline constexpr uint32_t LIB_VERSION = 20, LIB_REVISION = 22, LIB_IDSTRING = 24, LIB_OPENCNT = 32;
// struct Task: tc_State(15.b) tc_SigWait(22.l) tc_SPLower(58.l) tc_SPUpper(62.l)
inline constexpr uint32_t TC_STATE = 15, TC_SIGWAIT = 22, TC_SPREG = 54, TC_SPLOWER = 58,
                          TC_SPUPPER = 62;
// struct MsgPort: mp_MsgList is a List at offset 20
inline constexpr uint32_t MP_MSGLIST = 20;
// struct MemHeader: mh_First(16.l) mh_Lower(20.l) mh_Upper(24.l) mh_Free(28.l)
inline constexpr uint32_t MH_ATTRIBUTES = 14, MH_FIRST = 16, MH_LOWER = 20, MH_UPPER = 24,
                          MH_FREE = 28;
// struct MemChunk: mc_Next(0.l) mc_Bytes(4.l)
inline constexpr uint32_t MC_NEXT = 0, MC_BYTES = 4;
// struct ExecBase (LIB_SIZE=34): selected fields
inline constexpr uint32_t EB_MAXLOCMEM = 62, EB_THISTASK = 276, EB_ATTNFLAGS = 296,
                          EB_SYSFLAGS = 292;
inline constexpr uint32_t EB_INTVECTS = 84; // 16 * IV_SIZE(12)
inline constexpr uint32_t EB_MEMLIST = 322, EB_RESOURCELIST = 336, EB_DEVICELIST = 350;
inline constexpr uint32_t EB_INTRLIST = 364, EB_LIBLIST = 378, EB_PORTLIST = 392,
                          EB_TASKREADY = 406, EB_TASKWAIT = 420, EB_SEMAPHORELIST = 532;
inline constexpr uint32_t IV_SIZE = 12, IV_CODE = 4, IV_NODE = 8; // struct IntVector
// Reset-survival fields (exec/execbase.i).
inline constexpr uint32_t EB_CHKBASE = 38, EB_COLDCAPTURE = 42, EB_COOLCAPTURE = 46,
                          EB_WARMCAPTURE = 50, EB_SYSSTKUPPER = 54, EB_SYSSTKLOWER = 58,
                          EB_CHKSUM = 82;
inline constexpr uint32_t EB_KICKMEMPTR = 546, EB_KICKTAGPTR = 550, EB_KICKCHECKSUM = 554;
inline constexpr uint32_t EB_RESMODULES = 300;

// struct Resident (exec/resident.i)
inline constexpr uint16_t RTAG_MATCHWORD_VALUE = 0x4AFC;
inline constexpr uint32_t RTAG_MATCHWORD = 0, RTAG_MATCHTAG = 2, RTAG_ENDSKIP = 6, RTAG_FLAGS = 10,
                          RTAG_VERSION = 11, RTAG_TYPE = 12, RTAG_PRI = 13, RTAG_NAME = 14,
                          RTAG_IDSTRING = 18, RTAG_INIT = 22;

// Safety bound for list walks (a corrupt ln_Succ chain must not hang the UI).
inline constexpr int kListCap = 512;

// Returns the 32-bit big-endian value at `addr`; sets ok=false if unmapped.
using ReadLongFn = std::function<uint32_t(uint32_t addr, bool& ok)>;
// Returns the byte at `addr`; sets ok=false if unmapped.
using ReadByteFn = std::function<uint8_t(uint32_t addr, bool& ok)>;
// True when `addr` is inside a mapped region. Optional; when null every address
// is treated as plausible (useful for tests over a synthetic image).
using IsMappedFn = std::function<bool(uint32_t addr)>;

// The accessors the walks need, bundled so callers pass one object around.
struct Readers
{
    ReadLongFn readLong;
    ReadByteFn readByte;
    IsMappedFn isMapped; // optional

    uint32_t Long(uint32_t addr) const;
    uint16_t Word(uint32_t addr) const;
    uint8_t Byte(uint32_t addr) const;
    bool Mapped(uint32_t addr) const { return !isMapped || isMapped(addr); }
};

// ExecBase from the longword at $000004, validated against the memory map.
// nullopt when no OS has booted or the pointer is implausible.
std::optional<uint32_t> ExecBase(const Readers& r);

// NUL-terminated string at `addr`, with non-printable bytes rendered as '.'.
// Empty for a null pointer or an unmapped first byte.
std::string ReadCString(const Readers& r, uint32_t addr, size_t maxLen = 80);

// BCPL string at `addr` (a leading length byte, no terminator). `addr` is a raw
// pointer, not a BPTR — shift a BPTR left by 2 before calling.
std::string ReadBString(const Readers& r, uint32_t addr, size_t maxLen = 255);

// Walk an Exec List at `listAddr` from lh_Head via ln_Succ, stopping at the tail
// sentinel (the node whose ln_Succ is 0) or after maxNodes.
void ForEachNode(const Readers& r, uint32_t listAddr, int maxNodes,
                 const std::function<void(uint32_t node)>& fn);

// --- Decoded views ---------------------------------------------------------

struct LibraryNode
{
    uint32_t node = 0;
    uint16_t version = 0;
    uint16_t revision = 0;
    uint16_t openCount = 0;
    std::string name;
    std::string idString;
};

struct TaskNode
{
    uint32_t node = 0;
    int pri = 0;
    uint8_t state = 0;
    uint32_t sigWait = 0;
    uint32_t spLower = 0;
    uint32_t spUpper = 0;
    std::string name;
    bool isThisTask = false;
};

struct MemHeaderNode
{
    uint32_t node = 0;
    uint16_t attributes = 0;
    uint32_t lower = 0;
    uint32_t upper = 0;
    uint32_t free = 0;
    uint32_t first = 0; // mc_First: head of the free-chunk chain
    std::string name;
};

struct MemChunkNode
{
    uint32_t addr = 0;
    uint32_t bytes = 0;
};

struct PortNode
{
    uint32_t node = 0;
    int pri = 0;
    int messages = 0;
    std::string name;
};

// Libraries, devices and resources all hang off a List of Library-shaped nodes.
std::vector<LibraryNode> ListLibraryNodes(const Readers& r, uint32_t listAddr);

// ThisTask (flagged) followed by TaskReady then TaskWait.
std::vector<TaskNode> ListTasks(const Readers& r, uint32_t execBase);

std::vector<MemHeaderNode> ListMemHeaders(const Readers& r, uint32_t execBase);

// The free-chunk chain of one MemHeader, in address order as stored.
std::vector<MemChunkNode> ListFreeChunks(const Readers& r, const MemHeaderNode& mh);

std::vector<PortNode> ListPorts(const Readers& r, uint32_t execBase);

// Task state name ("Run", "Wait", ...) or "?" for an unknown code.
const char* TaskStateName(uint8_t state);

// 68k exception vector name for vector number `num` ("Bus Error", "Level 3 Int",
// ...), or nullptr when the vector has no well-known meaning of its own.
const char* ExceptionVectorName(int num);

// Same lookup, but always produces something: TRAPs, the reset vectors and the
// reserved/user ranges all get a name. This is the one to show a user.
std::string VectorName(int num);

// Amiga interrupt name for INTB bit `index` (0-15), e.g. "VERTB". Null out of range.
const char* InterruptName(int index);

} // namespace exec
} // namespace adm::core
