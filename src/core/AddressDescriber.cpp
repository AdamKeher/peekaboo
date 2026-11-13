#include "AddressDescriber.h"

#include <array>

#include <fmt/core.h>

#include "SegListResolver.h"

namespace adm::core {

namespace {

constexpr uint32_t kCustomBase = 0xDFF000;
constexpr uint32_t kCustomEnd = 0xDFF200;

// CIA-A sits on the odd bytes at $BFE001, CIA-B on the even bytes at $BFD000;
// both stride one register every $100 bytes.
constexpr uint32_t kCiaABase = 0xBFE001;
constexpr uint32_t kCiaBBase = 0xBFD000;

constexpr std::array<const char*, 16> kCiaRegs = {{"PRA", "PRB", "DDRA", "DDRB", "TALO", "TAHI",
                                                   "TBLO", "TBHI", "TODLOW", "TODMID", "TODHI",
                                                   "(unused)", "SDR", "ICR", "CRA", "CRB"}};

// ExecBase fields worth naming. Ordered by offset; the lookup takes the last
// field whose span contains the address.
struct ExecField
{
    uint32_t offset;
    uint32_t size;
    const char* name;
};

constexpr std::array<ExecField, 26> kExecFields = {{
    {exec::LIB_VERSION, 2, "LibNode.lib_Version"},
    {exec::LIB_REVISION, 2, "LibNode.lib_Revision"},
    {exec::LIB_IDSTRING, 4, "LibNode.lib_IdString"},
    {exec::LIB_OPENCNT, 2, "LibNode.lib_OpenCnt"},
    {34, 2, "SoftVer"},
    {36, 2, "LowMemChkSum"},
    {exec::EB_CHKBASE, 4, "ChkBase"},
    {exec::EB_COLDCAPTURE, 4, "ColdCapture"},
    {exec::EB_COOLCAPTURE, 4, "CoolCapture"},
    {exec::EB_WARMCAPTURE, 4, "WarmCapture"},
    {exec::EB_SYSSTKUPPER, 4, "SysStkUpper"},
    {exec::EB_SYSSTKLOWER, 4, "SysStkLower"},
    {exec::EB_MAXLOCMEM, 4, "MaxLocMem"},
    {66, 4, "DebugEntry"},
    {70, 4, "DebugData"},
    {74, 4, "AlertData"},
    {78, 4, "MaxExtMem"},
    {exec::EB_CHKSUM, 2, "ChkSum"},
    {exec::EB_INTVECTS, 16 * exec::IV_SIZE, "IntVects[16]"},
    {exec::EB_THISTASK, 4, "ThisTask"},
    {exec::EB_SYSFLAGS, 2, "SysFlags"},
    {exec::EB_ATTNFLAGS, 2, "AttnFlags"},
    {exec::EB_RESMODULES, 4, "ResModules"},
    {exec::EB_KICKMEMPTR, 4, "KickMemPtr"},
    {exec::EB_KICKTAGPTR, 4, "KickTagPtr"},
    {exec::EB_KICKCHECKSUM, 4, "KickCheckSum"},
}};

struct ExecList
{
    uint32_t offset;
    const char* name;
};

constexpr std::array<ExecList, 9> kExecLists = {{
    {exec::EB_MEMLIST, "MemList"},
    {exec::EB_RESOURCELIST, "ResourceList"},
    {exec::EB_DEVICELIST, "DeviceList"},
    {exec::EB_INTRLIST, "IntrList"},
    {exec::EB_LIBLIST, "LibList"},
    {exec::EB_PORTLIST, "PortList"},
    {exec::EB_TASKREADY, "TaskReady"},
    {exec::EB_TASKWAIT, "TaskWait"},
    {exec::EB_SEMAPHORELIST, "SemaphoreList"},
}};

// How far below a library base the jump table can plausibly stretch. Real
// libraries top out well short of this; it only bounds the search.
constexpr uint32_t kMaxJumpTableBytes = 0x600;

void Add(AddressDescription& d, std::string kind, std::string name, std::string detail,
         uint32_t base, uint32_t size, uint32_t addr)
{
    AddressLayer l;
    l.kind = std::move(kind);
    l.name = std::move(name);
    l.detail = std::move(detail);
    l.base = base;
    l.size = size;
    // A layer with no base of its own (the allocated/free verdict, for instance)
    // has no meaningful offset either — reporting addr-0 would read as one.
    l.offset = (base == 0 && size == 0) ? 0 : (base <= addr ? addr - base : 0);
    d.layers.push_back(std::move(l));
}

void DescribeRegion(AddressDescription& d, uint32_t addr, const AddressDescriber::Context& ctx)
{
    for (const auto& r : ctx.regions) {
        if (r.size == 0 || addr < r.base || addr - r.base >= r.size)
            continue;
        d.mapped = true;
        Add(d, "region", r.name, r.writable ? "writable" : "read-only", r.base, r.size, addr);
        return;
    }
}

void DescribeChipset(AddressDescription& d, uint32_t addr, const AddressDescriber::Context& ctx)
{
    if (addr >= kCustomBase && addr < kCustomEnd) {
        const uint32_t offset = (addr - kCustomBase) & ~1u;
        std::string name = ctx.customRegName ? ctx.customRegName(offset) : std::string{};
        if (name.empty())
            name = fmt::format("$DFF{:03X}", offset);
        std::string detail = ctx.customRegDesc ? ctx.customRegDesc(offset) : std::string{};
        Add(d, "customRegister", name, std::move(detail), kCustomBase + offset, 2, addr);
        d.mapped = true;
        return;
    }

    char chip = 0;
    std::string reg;
    uint32_t index = 0;
    if (AddressDescriber::DescribeCia(addr, chip, reg, index)) {
        Add(d, "cia", fmt::format("CIA-{} {}", chip, reg),
            fmt::format("register {} of CIA-{}", index, chip), addr, 1, addr);
        d.mapped = true;
    }
}

void DescribeVector(AddressDescription& d, uint32_t addr)
{
    if (addr >= 0x400)
        return;
    const int num = static_cast<int>(addr / 4);
    Add(d, "vector", AddressDescriber::VectorName(num),
        fmt::format("68k exception vector {} (byte {} of the longword)", num, addr % 4), num * 4u,
        4, addr);
}

void DescribeProgram(AddressDescription& d, uint32_t addr, const AddressDescriber::Context& ctx)
{
    auto readLong = [&](uint32_t a, bool& ok) { return ctx.readers.readLong(a, ok); };
    auto readByte = [&](uint32_t a, bool& ok) { return ctx.readers.readByte(a, ok); };
    if (!ctx.readers.readLong || !ctx.readers.readByte)
        return;

    auto segments = SegListResolver::ResolveActiveProcess(readLong, readByte);
    if (segments.empty())
        return;

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& s = segments[i];
        if (s.size == 0 || addr < s.base || addr - s.base >= s.size)
            continue;
        Add(d, "segment", fmt::format("segment {}", i),
            fmt::format("hunk {} of the running program", i), s.base, s.size, addr);

        // Only the first hunk's addresses line up with link-time symbol
        // addresses, which is the case the ELF reader supports.
        if (i == 0 && ctx.resolveSymbol) {
            const uint32_t linkAddr = addr - segments[0].base;
            std::string fn;
            uint32_t fnBase = 0, fnSize = 0;
            if (ctx.resolveSymbol(linkAddr, fn, fnBase, fnSize))
                Add(d, "function", fn, fmt::format("link address ${:08X}", linkAddr),
                    segments[0].base + fnBase, fnSize, addr);
        }
        return;
    }
}

void DescribeExecBase(AddressDescription& d, uint32_t addr, uint32_t execBase)
{
    // ExecBase's tail is version-dependent; 1024 comfortably covers every field
    // worth naming without claiming unrelated memory behind it.
    constexpr uint32_t kExecBaseSpan = 1024;
    if (addr < execBase || addr - execBase >= kExecBaseSpan)
        return;
    const uint32_t off = addr - execBase;

    for (const auto& l : kExecLists) {
        if (off >= l.offset && off - l.offset < 14) {
            Add(d, "execBase", fmt::format("ExecBase.{}", l.name),
                fmt::format("struct List at ExecBase+{}", l.offset), execBase + l.offset, 14, addr);
            return;
        }
    }
    for (const auto& f : kExecFields) {
        if (off >= f.offset && off - f.offset < f.size) {
            Add(d, "execBase", fmt::format("ExecBase.{}", f.name),
                fmt::format("ExecBase+{}", f.offset), execBase + f.offset, f.size, addr);
            return;
        }
    }
    Add(d, "execBase", "ExecBase", fmt::format("unnamed field at ExecBase+{}", off), execBase,
        kExecBaseSpan, addr);
}

void DescribeMemory(AddressDescription& d, uint32_t addr, const exec::Readers& r, uint32_t execBase)
{
    for (const auto& mh : exec::ListMemHeaders(r, execBase)) {
        if (addr < mh.lower || addr >= mh.upper)
            continue;
        const char* type = (mh.attributes & 0x2) ? "CHIP" : (mh.attributes & 0x4) ? "FAST" : "MEM";
        Add(d, "memoryHeader", mh.name.empty() ? std::string(type) : mh.name,
            fmt::format("{} pool ${:08X}-${:08X}, {} bytes free", type, mh.lower, mh.upper,
                        mh.free),
            mh.lower, mh.upper - mh.lower, addr);

        for (const auto& c : exec::ListFreeChunks(r, mh)) {
            if (addr >= c.addr && addr - c.addr < c.bytes) {
                Add(d, "allocation", "free",
                    fmt::format("inside a {}-byte free chunk — not owned by anything", c.bytes),
                    c.addr, c.bytes, addr);
                return;
            }
        }
        Add(d, "allocation", "allocated",
            "outside every free chunk of this pool, so it belongs to some allocation", 0, 0, addr);
        return;
    }
}

void DescribeTaskStack(AddressDescription& d, uint32_t addr, const exec::Readers& r,
                       uint32_t execBase)
{
    for (const auto& t : exec::ListTasks(r, execBase)) {
        if (t.spLower == 0 || t.spUpper <= t.spLower)
            continue;
        if (addr < t.spLower || addr >= t.spUpper)
            continue;
        Add(d, "taskStack", t.name.empty() ? fmt::format("task ${:08X}", t.node) : t.name,
            fmt::format("stack of {} ({}{})", t.name.empty() ? "an unnamed task" : t.name,
                        exec::TaskStateName(t.state), t.isThisTask ? ", current task" : ""),
            t.spLower, t.spUpper - t.spLower, addr);
        return;
    }
}

void DescribeExecNodes(AddressDescription& d, uint32_t addr, const exec::Readers& r,
                       uint32_t execBase)
{
    struct ListSpec
    {
        uint32_t offset;
        const char* kind;
        bool hasJumpTable;
    };
    const ListSpec lists[] = {
        {exec::EB_LIBLIST, "library", true},
        {exec::EB_DEVICELIST, "device", true},
        {exec::EB_RESOURCELIST, "resource", false},
    };

    for (const auto& spec : lists) {
        for (const auto& lib : exec::ListLibraryNodes(r, execBase + spec.offset)) {
            if (addr == lib.node) {
                Add(d, "execNode", lib.name,
                    fmt::format("base of the {} node (v{}.{}, {} opens)", spec.kind, lib.version,
                                lib.revision, lib.openCount),
                    lib.node, 0, addr);
                return;
            }
            if (!spec.hasJumpTable || addr >= lib.node || lib.node - addr > kMaxJumpTableBytes)
                continue;
            const uint32_t lvo = lib.node - addr;
            if (lvo % 6 != 0)
                continue;
            // Each jump-table slot is a 6-byte JMP, so a multiple of 6 below the
            // base is exactly an LVO entry — which is how a call through the
            // library lands here.
            Add(d, "libraryVector", fmt::format("{} LVO -{}", lib.name, lvo),
                fmt::format("jump-table entry {} of {}", lvo / 6, lib.name), addr, 6, addr);
            return;
        }
    }

    for (const auto& p : exec::ListPorts(r, execBase)) {
        if (addr == p.node) {
            Add(d, "execNode", p.name,
                fmt::format("base of a public message port ({} queued)", p.messages), p.node, 0,
                addr);
            return;
        }
    }
}

} // namespace

bool AddressDescriber::DescribeCia(uint32_t addr, char& outChip, std::string& outReg,
                                   uint32_t& outIndex)
{
    auto match = [&](uint32_t base, char chip) {
        if (addr < base)
            return false;
        const uint32_t delta = addr - base;
        if ((delta & 0xFF) != 0)
            return false; // registers only appear every $100 bytes
        const uint32_t index = delta >> 8;
        if (index > 15)
            return false;
        outChip = chip;
        outIndex = index;
        outReg = kCiaRegs[index];
        return true;
    };
    return match(kCiaABase, 'A') || match(kCiaBBase, 'B');
}

std::string AddressDescriber::VectorName(int num) { return exec::VectorName(num); }

AddressDescription AddressDescriber::Describe(uint32_t addr, const Context& ctx)
{
    AddressDescription d;
    d.addr = addr;

    DescribeRegion(d, addr, ctx);
    DescribeChipset(d, addr, ctx);
    DescribeVector(d, addr);

    if (ctx.inspectOs && ctx.readers.readLong && ctx.readers.readByte) {
        DescribeProgram(d, addr, ctx);
        if (auto execBase = exec::ExecBase(ctx.readers)) {
            DescribeExecBase(d, addr, *execBase);
            DescribeExecNodes(d, addr, ctx.readers, *execBase);
            DescribeMemory(d, addr, ctx.readers, *execBase);
            DescribeTaskStack(d, addr, ctx.readers, *execBase);
        }
    }

    if (ctx.annotationLabel) {
        std::string label = ctx.annotationLabel(addr);
        if (!label.empty())
            Add(d, "annotation", label, "label you set earlier", addr, 0, addr);
    }
    if (ctx.annotationComment) {
        std::string comment = ctx.annotationComment(addr);
        if (!comment.empty())
            Add(d, "annotation", "comment", comment, addr, 0, addr);
    }

    return d;
}

} // namespace adm::core
