#include "ExocetDiskMap.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>

#include <fmt/format.h>

#include "ExocetPack.h"

namespace adm::core {

using ojson = nlohmann::ordered_json;

namespace {

std::string Str(const ojson& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool Refuse(std::string* error, const std::string& why)
{
    if (error)
        *error = why;
    return false;
}

// A number in the map: a whole, non-negative one.
bool Whole(const ojson& j, const char* key, uint32_t* out)
{
    auto it = j.find(key);
    if (it == j.end())
        return true; // keep the default
    if (!it->is_number_integer() && !it->is_number_unsigned())
        return false;
    const int64_t v = it->get<int64_t>();
    if (v < 0 || v > 0xFFFFFFFFll)
        return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

bool ParseFields(const ojson& list, const char* what, std::vector<ExocetDiskField>* out,
                 size_t* bytes, std::string* error)
{
    if (!list.is_array() || list.empty())
        return Refuse(error, fmt::format("the directory's {} lists no fields", what));
    size_t at = 0;
    for (const auto& x : list) {
        if (!x.is_object())
            return Refuse(error, fmt::format("a {} field is not an object", what));
        ExocetDiskField f;
        f.name = Str(x, "name");
        f.type = Str(x, "type");
        f.unit = Str(x, "unit");
        f.role = Str(x, "role");
        f.text = Str(x, "text");
        if (f.name.empty())
            return Refuse(error, fmt::format("a {} field has no name", what));
        if (f.type == "u8")
            f.size = 1;
        else if (f.type == "u16")
            f.size = 2;
        else if (f.type == "u32")
            f.size = 4;
        else if (f.type.size() > 4 && f.type.compare(0, 4, "char") == 0) {
            const std::string n = f.type.substr(4);
            if (n.find_first_not_of("0123456789") != std::string::npos || n.size() > 3)
                return Refuse(error, fmt::format("{} field {}: type {}", what, f.name, f.type));
            f.size = static_cast<size_t>(std::stoi(n));
            f.isText = true;
        }
        if (f.size == 0)
            return Refuse(error, fmt::format("{} field {}: type \"{}\" is not u8, u16, u32 or "
                                             "charN",
                                             what, f.name, f.type));
        f.offset = at;
        at += f.size;
        out->push_back(std::move(f));
    }
    *bytes = at;
    return true;
}

uint32_t ReadBE(const uint8_t* p, size_t size)
{
    uint32_t v = 0;
    for (size_t i = 0; i < size; ++i)
        v = (v << 8) | p[i];
    return v;
}

// two blocks' packers as one: the same, or mixed
std::string Merge(const std::string& a, const std::string& b)
{
    return a.empty() || a == b ? b : "mixed";
}

std::string ReadText(const uint8_t* p, size_t size)
{
    std::string s;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t c = p[i];
        if (c >= 0x20 && c < 0x7F)
            s += static_cast<char>(c);
        else
            s += fmt::format("\\x{:02X}", c);
    }
    return s;
}

} // namespace

const ExocetDiskKind* ExocetDiskSpec::FindKind(int value) const
{
    for (const auto& k : kinds)
        if (k.value == value)
            return &k;
    return nullptr;
}

const ExocetDiskField* ExocetDiskSpec::FindHeader(const std::string& name) const
{
    for (const auto& f : header)
        if (f.name == name)
            return &f;
    return nullptr;
}

std::string ExocetDetectPacker(const ExocetDiskSpec& spec, const uint8_t* block, uint32_t packed,
                               uint32_t unpacked, int* slices)
{
    if (slices)
        *slices = 0;
    // walk the slices: every one's header, its bytes and its pad inside the block, and the
    // unpacked sizes adding up to the entry's
    int zx0 = 0, lz4 = 0;
    uint64_t at = 0, total = 0;
    bool walked = spec.slices && packed >= 4;
    while (walked && at < packed) {
        if (at + 4 > packed) {
            walked = false;
            break;
        }
        const uint16_t size = static_cast<uint16_t>((block[at] << 8) | block[at + 1]);
        const uint16_t raw = static_cast<uint16_t>((block[at + 2] << 8) | block[at + 3]);
        const uint16_t bytes = size & static_cast<uint16_t>(~spec.zx0Flag);
        if (bytes == 0 || raw == 0) {
            walked = false;
            break;
        }
        (size & spec.zx0Flag ? zx0 : lz4)++;
        at += 4 + bytes;
        at += at & 1;
        total += raw;
    }
    if (walked && at == packed && total == unpacked) {
        if (slices)
            *slices = zx0 + lz4;
        return zx0 && lz4 ? "mixed" : zx0 ? "zx0" : "lz4";
    }
    return packed == unpacked ? "none" : "?";
}

bool ParseExocetDiskSpec(const ojson& disk, ExocetDiskSpec* out, std::string* error)
{
    if (!disk.is_object())
        return Refuse(error, "\"disk\" is not an object");
    ExocetDiskSpec s;
    s.name = Str(disk, "name");
    s.text = Str(disk, "text");
    if (!Whole(disk, "sector_bytes", &s.sectorBytes) || !Whole(disk, "sectors", &s.sectors) ||
        !Whole(disk, "sectors_per_track", &s.sectorsPerTrack) || s.sectorBytes == 0 ||
        s.sectors == 0 || s.sectorsPerTrack == 0)
        return Refuse(error, "sector_bytes, sectors and sectors_per_track must be whole numbers "
                             "above 0");

    if (auto r = disk.find("regions"); r != disk.end()) {
        if (!r->is_array())
            return Refuse(error, "\"regions\" is not a list");
        for (const auto& x : *r) {
            if (!x.is_object())
                return Refuse(error, "a region is not an object");
            ExocetDiskRegionSpec reg;
            reg.name = Str(x, "name");
            reg.text = Str(x, "text");
            reg.first = x.value("first", ojson());
            reg.count = x.value("count", ojson());
            reg.end = x.value("end", ojson());
            if (reg.name.empty() || reg.first.is_null() ||
                (reg.count.is_null() == reg.end.is_null()))
                return Refuse(error, fmt::format("region \"{}\" needs a name, a first, and a count "
                                                 "or an end",
                                                 reg.name));
            s.regions.push_back(std::move(reg));
        }
    }

    auto dir = disk.find("directory");
    if (dir == disk.end() || !dir->is_object())
        return Refuse(error, "no \"directory\"");
    if (!Whole(*dir, "sector", &s.dirSector) || !Whole(*dir, "sectors", &s.dirSectors) ||
        s.dirSectors == 0)
        return Refuse(error, "the directory's sector and sectors must be whole numbers");
    s.magic = Str(*dir, "magic");
    if (auto v = dir->find("version"); v != dir->end()) {
        if (!v->is_number_integer())
            return Refuse(error, "the directory's version is not a number");
        s.version = v->get<int>();
    }
    s.count = Str(*dir, "count");
    if (!ParseFields(dir->value("header", ojson()), "header", &s.header, &s.headerBytes, error) ||
        !ParseFields(dir->value("entry", ojson()), "entry", &s.entry, &s.entryBytes, error))
        return false;
    if (s.count.empty() || !s.FindHeader(s.count) || s.FindHeader(s.count)->isText)
        return Refuse(error, "the directory's \"count\" must name a number field of the header");
    for (const char* role : {"sector", "packed", "kind"}) {
        const bool found = std::any_of(s.entry.begin(), s.entry.end(),
                                       [&](const auto& f) { return f.role == role && !f.isText; });
        if (!found)
            return Refuse(error, fmt::format("no entry field has the role \"{}\"", role));
    }

    if (auto sl = disk.find("slices"); sl != disk.end()) {
        if (!sl->is_object())
            return Refuse(error, "\"slices\" is not an object");
        uint32_t flag = s.zx0Flag;
        if (!Whole(*sl, "zx0_flag", &flag) || flag == 0 || flag > 0xFFFF || (flag & (flag - 1)))
            return Refuse(error, "the slices' zx0_flag must be one bit of a word");
        s.slices = true;
        s.zx0Flag = static_cast<uint16_t>(flag);
        s.slicesText = Str(*sl, "text");
    }

    if (auto k = disk.find("kinds"); k != disk.end()) {
        if (!k->is_array())
            return Refuse(error, "\"kinds\" is not a list");
        for (const auto& x : *k) {
            if (!x.is_object() || !x.contains("value") || !x["value"].is_number_integer())
                return Refuse(error, "a kind needs a whole-number value");
            ExocetDiskKind kind{x["value"].get<int>(), Str(x, "name"), Str(x, "packer"),
                                Str(x, "text")};
            if (!kind.packer.empty() && kind.packer != "zx0" && kind.packer != "lz4" &&
                kind.packer != "none")
                return Refuse(error, fmt::format("kind {}: packer \"{}\" is not zx0, lz4 or none",
                                                 kind.name, kind.packer));
            s.kinds.push_back(std::move(kind));
        }
    }
    *out = std::move(s);
    return true;
}

ExocetDiskMap ReadExocetDiskMap(const ExocetDiskSpec& spec, const std::vector<uint8_t>& adf)
{
    ExocetDiskMap m;
    const uint64_t diskBytes = uint64_t(spec.sectorBytes) * spec.sectors;
    if (adf.size() != diskBytes) {
        m.error = fmt::format("The image is {} bytes; the map is for {} sectors of {} ({} bytes).",
                              adf.size(), spec.sectors, spec.sectorBytes, diskBytes);
        return m;
    }
    const uint64_t dirAt = uint64_t(spec.dirSector) * spec.sectorBytes;
    const uint64_t dirEnd = dirAt + uint64_t(spec.dirSectors) * spec.sectorBytes;
    if (dirEnd > adf.size() || spec.headerBytes > dirEnd - dirAt) {
        m.error = "The directory is not inside the disk.";
        return m;
    }
    const uint8_t* dir = adf.data() + dirAt;

    // the header
    std::map<std::string, uint32_t> values;
    for (const auto& f : spec.header) {
        ExocetDiskHeaderValue v;
        v.field = &f;
        if (f.isText) {
            v.text = ReadText(dir + f.offset, f.size);
        } else {
            v.value = ReadBE(dir + f.offset, f.size);
            values[f.name] = v.value;
        }
        m.header.push_back(std::move(v));
    }
    if (!spec.magic.empty()) {
        const ExocetDiskField* f = spec.FindHeader(spec.magicField);
        const std::string got =
            f && f->isText ? ReadText(dir + f->offset, f->size)
                           : ReadText(dir, std::min<size_t>(spec.magic.size(), dirEnd - dirAt));
        if (got != spec.magic) {
            m.error = fmt::format("Not a {} disk: the directory at sector {} starts \"{}\", not "
                                  "\"{}\".",
                                  spec.name.empty() ? spec.magic : spec.name, spec.dirSector, got,
                                  spec.magic);
            return m;
        }
    }
    if (spec.version >= 0) {
        auto it = values.find(spec.versionField);
        if (it != values.end() && static_cast<int>(it->second) != spec.version)
            m.warnings.push_back(fmt::format("The disk's directory is version {}; the map is for "
                                             "version {}: fields may have moved.",
                                             it->second, spec.version));
    }

    // the entries
    const uint32_t count = values[spec.count];
    const uint64_t room = (dirEnd - dirAt - spec.headerBytes) / spec.entryBytes;
    if (count > room) {
        m.error =
            fmt::format("The header says {} entries; the directory has room for {}.", count, room);
        return m;
    }
    std::map<int, uint32_t> streamAt;
    std::map<int, ExocetDiskKindTotal> totals;
    std::map<int, int> packerMisses; // blocks of a kind whose packer is not the map's
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = dir + spec.headerBytes + size_t(i) * spec.entryBytes;
        ExocetDiskEntry e;
        e.index = static_cast<int>(i);
        bool haveSectors = false;
        for (const auto& f : spec.entry) {
            if (f.isText)
                continue;
            const uint32_t v = ReadBE(p + f.offset, f.size);
            if (f.role == "sector")
                e.sector = v;
            else if (f.role == "packed")
                e.packed = v;
            else if (f.role == "unpacked")
                e.unpacked = v;
            else if (f.role == "kind")
                e.kind = static_cast<int>(v);
            else if (f.role == "sectors") {
                e.sectors = v;
                haveSectors = true;
            }
        }
        const uint32_t need = (e.packed + spec.sectorBytes - 1) / spec.sectorBytes;
        if (!haveSectors)
            e.sectors = need;
        if (e.unpacked == 0 && std::none_of(spec.entry.begin(), spec.entry.end(),
                                            [](const auto& f) { return f.role == "unpacked"; }))
            e.unpacked = e.packed;
        e.fits = uint64_t(e.sector) + e.sectors <= spec.sectors && e.sectors == need;
        if (!e.fits)
            m.warnings.push_back(fmt::format("Entry {} (sector {}, {} sectors, {} bytes) is not a "
                                             "block this disk could hold.",
                                             i, e.sector, e.sectors, e.packed));
        const ExocetDiskKind* kind = spec.FindKind(e.kind);
        e.kindName = kind ? kind->name : fmt::format("kind {}", e.kind);
        e.streamOffset = streamAt[e.kind];
        streamAt[e.kind] += e.unpacked;

        // its packer: off the disk when the map says how, else the map's word for it
        const std::string mapped = kind ? kind->packer : std::string();
        if (spec.slices) {
            const uint64_t at = uint64_t(e.sector) * spec.sectorBytes;
            e.packer =
                at + e.packed <= adf.size()
                    ? ExocetDetectPacker(spec, adf.data() + at, e.packed, e.unpacked, &e.slices)
                    : "?";
            e.packerOk = mapped.empty() || e.packer == mapped;
            if (!e.packerOk)
                ++packerMisses[e.kind];
        } else {
            e.packer = mapped;
        }

        auto& t = totals[e.kind];
        t.kind = e.kind;
        t.name = e.kindName;
        if (kind) {
            t.text = kind->text;
            t.packer = kind->packer;
        }
        t.found = Merge(t.found, e.packer);
        t.blocks++;
        t.sectors += e.sectors;
        t.packed += e.packed;
        t.unpacked += e.unpacked;

        // a run: the same kind, straight on from the last
        if (!m.runs.empty() && m.runs.back().kind == e.kind &&
            m.runs.back().endSector == e.sector) {
            auto& r = m.runs.back();
            r.lastEntry = e.index;
            r.endSector = e.sector + e.sectors;
            r.blocks++;
            r.packed += e.packed;
            r.unpacked += e.unpacked;
            r.packer = Merge(r.packer, e.packer);
            r.packerOk = r.packerOk && e.packerOk;
        } else {
            m.runs.push_back({e.index, e.index, e.kind, e.kindName, e.sector, e.sector + e.sectors,
                              1, e.packed, e.unpacked, e.packer, e.packerOk});
        }
        m.usedEnd = std::max(m.usedEnd, e.sector + e.sectors);
        m.entries.push_back(std::move(e));
    }
    for (auto& [kind, t] : totals) {
        if (auto miss = packerMisses.find(kind); miss != packerMisses.end())
            m.warnings.push_back(fmt::format("{}: {} of its {} blocks are {} on the disk; the map "
                                             "says {}.",
                                             t.name, miss->second, t.blocks, t.found, t.packer));
        m.kinds.push_back(std::move(t));
    }

    // the regions: the map's, then the blocks and what is left
    auto resolve = [&](const ojson& v, uint32_t* out) {
        if (v.is_number_integer() || v.is_number_unsigned()) {
            const int64_t n = v.get<int64_t>();
            if (n < 0)
                return false;
            *out = static_cast<uint32_t>(n);
            return true;
        }
        if (v.is_string()) {
            auto it = values.find(v.get<std::string>());
            if (it == values.end())
                return false;
            *out = it->second;
            return true;
        }
        return false;
    };
    for (const auto& r : spec.regions) {
        ExocetDiskRegion reg;
        reg.name = r.name;
        reg.text = r.text;
        uint32_t first = 0, n = 0;
        bool ok = resolve(r.first, &first);
        if (ok && !r.count.is_null())
            ok = resolve(r.count, &n);
        else if (ok) {
            uint32_t end = 0;
            ok = resolve(r.end, &end) && end >= first;
            n = end - first;
        }
        if (!ok) {
            m.warnings.push_back(fmt::format("Region {}: its first, count or end is not a number "
                                             "or a header field.",
                                             r.name));
            continue;
        }
        reg.first = first;
        reg.count = n;
        m.regions.push_back(std::move(reg));
    }
    if (!m.entries.empty()) {
        uint32_t first = m.usedEnd;
        for (const auto& e : m.entries)
            first = std::min(first, e.sector);
        m.regions.push_back({"blocks", fmt::format("{} entries", m.entries.size()), first,
                             m.usedEnd - first, true});
    }
    if (m.usedEnd < spec.sectors)
        m.regions.push_back({"unused", "", m.usedEnd, spec.sectors - m.usedEnd, true});
    std::stable_sort(m.regions.begin(), m.regions.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 1; i < m.regions.size(); ++i) {
        const auto& a = m.regions[i - 1];
        const auto& b = m.regions[i];
        if (a.first + a.count > b.first)
            m.warnings.push_back(
                fmt::format("Regions {} and {} overlap at sector {}.", a.name, b.name, b.first));
        else if (a.first + a.count < b.first)
            m.warnings.push_back(
                fmt::format("Sectors {}-{} are in no region.", a.first + a.count, b.first - 1));
    }
    m.ok = true;
    return m;
}

ExocetDiskMapLoad LoadExocetDiskMap(const ExocetPack* pack, const std::string& packError,
                                    const std::string& diskPath)
{
    ExocetDiskMapLoad out;
    if (!pack) {
        out.status = packError.empty() ? "No .exocet beside the disk." : packError;
        return out;
    }
    const auto& doc = pack->Document();
    auto disk = doc.find("disk");
    if (disk == doc.end()) {
        out.status = "The .exocet has no \"disk\" map.";
        return out;
    }
    std::string error;
    if (!ParseExocetDiskSpec(*disk, &out.spec, &error)) {
        out.status = "The .exocet's disk map: " + error;
        return out;
    }
    out.haveSpec = true;
    if (diskPath.empty()) {
        out.status = "No disk image.";
        return out;
    }
    std::ifstream in(diskPath, std::ios::binary);
    if (!in) {
        out.status = "Cannot read " + diskPath;
        return out;
    }
    const std::vector<uint8_t> adf((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    out.map = ReadExocetDiskMap(out.spec, adf);
    if (!out.map.ok)
        out.status = out.map.error;
    return out;
}

} // namespace adm::core
