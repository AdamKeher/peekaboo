#include "ExecIntrospect.h"

#include <array>

namespace adm::core::exec {

namespace {

// Names for the 68000 vectors that mean something specific. Everything else is
// either a TRAP (handled separately) or reserved.
struct NamedVector
{
    int num;
    const char* name;
};

constexpr std::array<NamedVector, 18> kNamedVectors = {{
    {2, "Bus Error"},
    {3, "Address Error"},
    {4, "Illegal Instruct."},
    {5, "Zero Divide"},
    {6, "CHK Instruct."},
    {7, "TRAPV"},
    {8, "Privilege Viol."},
    {9, "Trace"},
    {10, "Line-A (1010)"},
    {11, "Line-F (1111)"},
    {24, "Spurious Int"},
    {25, "Level 1 Int"},
    {26, "Level 2 Int"},
    {27, "Level 3 Int"},
    {28, "Level 4 Int"},
    {29, "Level 5 Int"},
    {30, "Level 6 Int"},
    {31, "Level 7 Int"},
}};

constexpr std::array<const char*, 16> kInterruptNames = {
    {"TBE", "DSKBLK", "SOFTINT", "PORTS", "COPER", "VERTB", "BLIT", "AUD0", "AUD1", "AUD2", "AUD3",
     "RBF", "DSKSYNC", "EXTER", "INTEN", "NMI"}};

} // namespace

uint32_t Readers::Long(uint32_t addr) const
{
    if (!readLong)
        return 0;
    bool ok = false;
    uint32_t v = readLong(addr, ok);
    return ok ? v : 0;
}

uint16_t Readers::Word(uint32_t addr) const
{
    return static_cast<uint16_t>((Byte(addr) << 8) | Byte(addr + 1));
}

uint8_t Readers::Byte(uint32_t addr) const
{
    if (!readByte)
        return 0;
    bool ok = false;
    uint8_t v = readByte(addr, ok);
    return ok ? v : 0;
}

std::optional<uint32_t> ExecBase(const Readers& r)
{
    if (!r.readLong)
        return std::nullopt;
    bool ok = false;
    uint32_t base = r.readLong(4, ok);
    if (!ok || base == 0 || (base & 1) != 0 || !r.Mapped(base))
        return std::nullopt;
    return base;
}

std::string ReadCString(const Readers& r, uint32_t addr, size_t maxLen)
{
    if (addr == 0 || !r.readByte)
        return {};
    std::string s;
    for (size_t i = 0; i < maxLen; ++i) {
        bool ok = false;
        uint8_t c = r.readByte(addr + static_cast<uint32_t>(i), ok);
        if (!ok || c == 0)
            break;
        s.push_back((c >= 32 && c < 127) ? static_cast<char>(c) : '.');
    }
    return s;
}

std::string ReadBString(const Readers& r, uint32_t addr, size_t maxLen)
{
    if (addr == 0 || !r.readByte)
        return {};
    bool ok = false;
    uint8_t len = r.readByte(addr, ok);
    if (!ok)
        return {};
    size_t n = len < maxLen ? len : maxLen;
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = r.readByte(addr + 1 + static_cast<uint32_t>(i), ok);
        if (!ok)
            break;
        s.push_back((c >= 32 && c < 127) ? static_cast<char>(c) : '.');
    }
    return s;
}

void ForEachNode(const Readers& r, uint32_t listAddr, int maxNodes,
                 const std::function<void(uint32_t)>& fn)
{
    if (!r.readLong || !fn)
        return;
    bool ok = false;
    uint32_t node = r.readLong(listAddr, ok); // lh_Head
    if (!ok)
        return;
    for (int n = 0; node != 0 && n < maxNodes; ++n) {
        uint32_t succ = r.readLong(node + LN_SUCC, ok);
        if (!ok || succ == 0) // a node whose ln_Succ is 0 is the list's tail sentinel
            break;
        fn(node);
        node = succ;
    }
}

std::vector<LibraryNode> ListLibraryNodes(const Readers& r, uint32_t listAddr)
{
    std::vector<LibraryNode> out;
    ForEachNode(r, listAddr, kListCap, [&](uint32_t node) {
        LibraryNode l;
        l.node = node;
        l.version = r.Word(node + LIB_VERSION);
        l.revision = r.Word(node + LIB_REVISION);
        l.openCount = r.Word(node + LIB_OPENCNT);
        l.name = ReadCString(r, r.Long(node + LN_NAME));
        l.idString = ReadCString(r, r.Long(node + LIB_IDSTRING));
        out.push_back(std::move(l));
    });
    return out;
}

std::vector<TaskNode> ListTasks(const Readers& r, uint32_t execBase)
{
    std::vector<TaskNode> out;
    auto decode = [&](uint32_t node, bool thisTask) {
        TaskNode t;
        t.node = node;
        t.pri = static_cast<int8_t>(r.Byte(node + LN_PRI));
        t.state = r.Byte(node + TC_STATE);
        t.sigWait = r.Long(node + TC_SIGWAIT);
        t.spLower = r.Long(node + TC_SPLOWER);
        t.spUpper = r.Long(node + TC_SPUPPER);
        t.name = ReadCString(r, r.Long(node + LN_NAME));
        t.isThisTask = thisTask;
        out.push_back(std::move(t));
    };
    uint32_t thisTask = r.Long(execBase + EB_THISTASK);
    if (thisTask)
        decode(thisTask, true);
    ForEachNode(r, execBase + EB_TASKREADY, kListCap, [&](uint32_t n) { decode(n, false); });
    ForEachNode(r, execBase + EB_TASKWAIT, kListCap, [&](uint32_t n) { decode(n, false); });
    return out;
}

std::vector<MemHeaderNode> ListMemHeaders(const Readers& r, uint32_t execBase)
{
    std::vector<MemHeaderNode> out;
    ForEachNode(r, execBase + EB_MEMLIST, kListCap, [&](uint32_t node) {
        MemHeaderNode m;
        m.node = node;
        m.attributes = r.Word(node + MH_ATTRIBUTES);
        m.first = r.Long(node + MH_FIRST);
        m.lower = r.Long(node + MH_LOWER);
        m.upper = r.Long(node + MH_UPPER);
        m.free = r.Long(node + MH_FREE);
        m.name = ReadCString(r, r.Long(node + LN_NAME));
        out.push_back(std::move(m));
    });
    return out;
}

std::vector<MemChunkNode> ListFreeChunks(const Readers& r, const MemHeaderNode& mh)
{
    std::vector<MemChunkNode> out;
    uint32_t chunk = mh.first;
    for (int n = 0; chunk != 0 && n < kListCap; ++n) {
        MemChunkNode c;
        c.addr = chunk;
        c.bytes = r.Long(chunk + MC_BYTES);
        // A chunk must live inside its header's span and have a sane length;
        // anything else means the free list is corrupt and the walk must stop.
        if (c.bytes == 0 || chunk < mh.lower || chunk >= mh.upper || c.bytes > (mh.upper - chunk))
            break;
        out.push_back(c);
        chunk = r.Long(chunk + MC_NEXT);
    }
    return out;
}

std::vector<PortNode> ListPorts(const Readers& r, uint32_t execBase)
{
    std::vector<PortNode> out;
    ForEachNode(r, execBase + EB_PORTLIST, kListCap, [&](uint32_t node) {
        PortNode p;
        p.node = node;
        p.pri = static_cast<int8_t>(r.Byte(node + LN_PRI));
        ForEachNode(r, node + MP_MSGLIST, kListCap, [&](uint32_t) { ++p.messages; });
        p.name = ReadCString(r, r.Long(node + LN_NAME));
        out.push_back(std::move(p));
    });
    return out;
}

const char* TaskStateName(uint8_t state)
{
    switch (state) {
        case 1:
            return "Added";
        case 2:
            return "Run";
        case 3:
            return "Ready";
        case 4:
            return "Wait";
        case 5:
            return "Except";
        case 6:
            return "Removed";
        default:
            return "?";
    }
}

const char* ExceptionVectorName(int num)
{
    for (const auto& v : kNamedVectors)
        if (v.num == num)
            return v.name;
    return nullptr;
}

std::string VectorName(int num)
{
    if (num == 0)
        return "Reset SSP";
    if (num == 1)
        return "Reset PC";
    if (const char* n = ExceptionVectorName(num))
        return n;
    if (num >= 32 && num <= 47)
        return "TRAP #" + std::to_string(num - 32);
    if (num >= 12 && num <= 23)
        return "reserved vector " + std::to_string(num);
    return "user interrupt vector " + std::to_string(num);
}

const char* InterruptName(int index)
{
    if (index < 0 || index >= static_cast<int>(kInterruptNames.size()))
        return nullptr;
    return kInterruptNames[static_cast<size_t>(index)];
}

} // namespace adm::core::exec
