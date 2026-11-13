#include "MemoryLocks.h"

#include <algorithm>
#include <fstream>

#include <fmt/format.h>

#include "ArMemoryAccess.h"
#include "Logger.h"

namespace adm::core {

namespace {

std::string BytesToHex(const std::vector<uint8_t>& v)
{
    std::string s;
    s.reserve(v.size() * 2);
    for (uint8_t b : v)
        s += fmt::format("{:02X}", b);
    return s;
}

std::optional<std::vector<uint8_t>> HexToBytesImpl(const std::string& s, std::string& err)
{
    std::vector<uint8_t> out;
    std::string clean;
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            clean += c;
    }
    if (clean.size() % 2 != 0) {
        err = "hex byte string has an odd number of digits";
        return std::nullopt;
    }
    for (size_t i = 0; i < clean.size(); i += 2) {
        const std::string byte = clean.substr(i, 2);
        try {
            out.push_back(static_cast<uint8_t>(std::stoul(byte, nullptr, 16)));
        } catch (...) {
            err = fmt::format("'{}' is not a hex byte", byte);
            return std::nullopt;
        }
    }
    return out;
}

const char* AnchorKindToString(AnchorKind k)
{
    switch (k) {
        case AnchorKind::Absolute:
            return "absolute";
        case AnchorKind::Seglist:
            return "seglist";
        case AnchorKind::Signature:
            return "signature";
        case AnchorKind::Operand:
            return "operand";
    }
    return "absolute";
}

std::optional<AnchorKind> AnchorKindFromString(const std::string& s)
{
    if (s == "absolute")
        return AnchorKind::Absolute;
    if (s == "seglist")
        return AnchorKind::Seglist;
    if (s == "signature")
        return AnchorKind::Signature;
    if (s == "operand")
        return AnchorKind::Operand;
    return std::nullopt;
}

// Addresses are written as "0x0010ABCD" strings rather than JSON numbers so a trainer file
// stays readable and diffable by hand.
std::optional<uint32_t> ParseAddr(const nlohmann::json& j, std::string& err)
{
    try {
        if (j.is_string())
            return static_cast<uint32_t>(std::stoul(j.get<std::string>(), nullptr, 0));
        if (j.is_number_unsigned())
            return j.get<uint32_t>();
    } catch (...) {
    }
    err = "address is not a number or a hex string";
    return std::nullopt;
}

// nlohmann stores a JSON integer as signed or unsigned depending on how it was produced
// (a parsed "3" is unsigned, an assigned int 3 is signed). Checking only is_number_unsigned()
// therefore silently ignores perfectly good values, so accept any integer and range-check it.
bool ReadUInt(const nlohmann::json& j, const char* key, uint32_t& out)
{
    if (!j.contains(key))
        return false;
    const nlohmann::json& v = j[key];
    if (v.is_number_unsigned()) {
        out = v.get<uint32_t>();
        return true;
    }
    if (v.is_number_integer()) {
        const int64_t n = v.get<int64_t>();
        if (n < 0)
            return false;
        out = static_cast<uint32_t>(n);
        return true;
    }
    if (v.is_string()) {
        // Lets "value": "0xFF" work as well as "value": 255.
        try {
            out = static_cast<uint32_t>(std::stoul(v.get<std::string>(), nullptr, 0));
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

} // namespace

std::optional<std::vector<uint8_t>> MemoryLocks::ParseHexBytes(const std::string& text,
                                                               std::string& err)
{
    return HexToBytesImpl(text, err);
}

const char* ToString(LockMode m)
{
    switch (m) {
        case LockMode::Block:
            return "block";
        case LockMode::Set:
            return "set";
        case LockMode::Patch:
            return "patch";
    }
    return "block";
}

std::optional<LockMode> LockModeFromString(const std::string& s)
{
    if (s == "block")
        return LockMode::Block;
    if (s == "set")
        return LockMode::Set;
    if (s == "patch")
        return LockMode::Patch;
    return std::nullopt;
}

uint32_t LockEntry::Span() const
{
    switch (mode) {
        case LockMode::Block:
            return len > 0 ? len : 1;
        case LockMode::Set:
            return size > 0 ? size : 1;
        case LockMode::Patch:
            return bytes.empty() ? 1 : static_cast<uint32_t>(bytes.size());
    }
    return 1;
}

// --- lookup -----------------------------------------------------------------------------

const LockEntry* MemoryLocks::Find(const std::string& id) const
{
    for (const auto& e : m_entries) {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

LockEntry* MemoryLocks::Find(const std::string& id)
{
    return const_cast<LockEntry*>(std::as_const(*this).Find(id));
}

const LockEntry* MemoryLocks::FindCovering(uint32_t addr) const
{
    for (const auto& e : m_entries) {
        if (!e.resolved)
            continue;
        const uint32_t base = *e.resolved;
        if (addr >= base && addr - base < e.Span())
            return &e;
    }
    return nullptr;
}

// --- mutation ---------------------------------------------------------------------------

std::string MemoryLocks::MakeId()
{
    // Monotonic and short. Skips ids already in use so a loaded trainer file cannot collide
    // with a locally-added entry.
    for (;;) {
        std::string candidate = fmt::format("L{:03}", m_nextId++);
        if (!Find(candidate))
            return candidate;
    }
}

std::string MemoryLocks::Add(LockEntry e)
{
    if (e.id.empty() || Find(e.id))
        e.id = MakeId();
    const std::string id = e.id;
    m_entries.push_back(std::move(e));
    ++m_revision;
    Save();
    return id;
}

bool MemoryLocks::SetValue(const std::string& id, uint32_t value, uint8_t size, std::string& err)
{
    LockEntry* e = Find(id);
    if (!e) {
        err = fmt::format("no lock with id {}", id);
        return false;
    }
    if (e->mode != LockMode::Set) {
        err = fmt::format("lock {} is a {} lock - only a set lock forces a value", id,
                          ToString(e->mode));
        return false;
    }
    if (size != 1 && size != 2 && size != 4) {
        err = "size must be 1, 2 or 4 bytes";
        return false;
    }
    const uint32_t max = size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
    if (value > max) {
        err = fmt::format("${:X} does not fit in {} bits", value, size * 8);
        return false;
    }

    e->value = value;
    e->size = size;
    // A forced value protects exactly its own width, so len tracks size (same rule as the PR
    // command and the UI helpers).
    e->len = size;
    ++m_revision;
    Save();
    return true;
}

bool MemoryLocks::Remove(const std::string& id)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [&](const LockEntry& e) { return e.id == id; });
    if (it == m_entries.end())
        return false;
    m_entries.erase(it);
    ++m_revision;
    Save();
    return true;
}

size_t MemoryLocks::RemoveCovering(uint32_t addr)
{
    const size_t before = m_entries.size();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&](const LockEntry& e) {
                                       if (!e.resolved)
                                           return false;
                                       const uint32_t base = *e.resolved;
                                       return addr >= base && addr - base < e.Span();
                                   }),
                    m_entries.end());
    const size_t removed = before - m_entries.size();
    if (removed) {
        ++m_revision;
        Save();
    }
    return removed;
}

void MemoryLocks::Clear()
{
    if (m_entries.empty())
        return;
    m_entries.clear();
    ++m_revision;
    Save();
}

void MemoryLocks::ReplaceAll(std::vector<LockEntry> entries)
{
    m_entries = std::move(entries);
    for (auto& e : m_entries) {
        if (e.id.empty())
            e.id = MakeId();
    }
    ++m_revision;
    Save();
}

bool MemoryLocks::ApplyPatch(LockEntry& e, bool apply, const LockContext& ctx, std::string& err)
{
    if (!ctx.mem) {
        err = "memory is not available";
        return false;
    }
    if (!e.resolved) {
        err = "entry is not resolved";
        return false;
    }
    const uint32_t at = *e.resolved;

    if (apply) {
        if (e.bytes.empty()) {
            err = "patch has no bytes";
            return false;
        }
        // Snapshot what is there first, so disabling can put it back exactly.
        std::vector<uint8_t> before(e.bytes.size());
        std::vector<bool> valid;
        if (ctx.mem->Read(at, before.data(), before.size(), &valid) != before.size()) {
            err = fmt::format("cannot read the {} byte(s) at ${:08X} to save the original",
                              e.bytes.size(), at);
            return false;
        }
        if (!ctx.mem->Write(at, e.bytes.data(), e.bytes.size(), err))
            return false;
        e.original = std::move(before);
        e.applied = true;
        return true;
    }

    if (!e.applied)
        return true; // nothing to undo
    if (e.original.size() != e.bytes.size()) {
        err = "the saved original bytes do not match the patch length";
        return false;
    }
    if (!ctx.mem->Write(at, e.original.data(), e.original.size(), err))
        return false;
    e.applied = false;
    return true;
}

bool MemoryLocks::SetEnabled(const std::string& id, bool enabled, const LockContext& ctx,
                             std::string& err)
{
    LockEntry* e = Find(id);
    if (!e) {
        err = fmt::format("no entry '{}'", id);
        return false;
    }
    if (e->enabled == enabled && !(enabled && e->mode == LockMode::Patch && !e->applied))
        return true;

    if (enabled) {
        if (!ctx.mem) {
            err = "memory is not available (is the emulator running?)";
            return false;
        }
        // Resolve first. An entry that does not resolve, or whose guard bytes do not match,
        // must never be turned on - that is the whole point of carrying verify bytes.
        const ResolveResult r =
            AnchorResolver::Resolve(e->anchor, *ctx.mem, ctx.segments, e->verify);
        e->state = r.state;
        e->note = r.note;
        e->resolved =
            r.state == ResolveState::Resolved ? std::optional<uint32_t>(r.addr) : std::nullopt;
        if (r.state != ResolveState::Resolved) {
            err = r.note.empty() ? fmt::format("anchor {}", ToString(r.state))
                                 : fmt::format("{} ({})", ToString(r.state), r.note);
            ++m_revision;
            Save();
            return false;
        }
        if (e->mode == LockMode::Patch && !ApplyPatch(*e, true, ctx, err)) {
            ++m_revision;
            Save();
            return false;
        }
    } else {
        if (e->mode == LockMode::Patch && e->applied && !ApplyPatch(*e, false, ctx, err)) {
            // Leave it enabled: the bytes are still in memory, and pretending otherwise
            // would lose the only record of what needs reverting.
            return false;
        }
    }

    e->enabled = enabled;
    ++m_revision;
    Save();
    return true;
}

void MemoryLocks::ResolveAll(const LockContext& ctx)
{
    if (!ctx.mem)
        return;
    bool changed = false;
    for (auto& e : m_entries) {
        const ResolveResult r = AnchorResolver::Resolve(e.anchor, *ctx.mem, ctx.segments, e.verify);
        const auto before = e.resolved;
        e.state = r.state;
        e.note = r.note;
        e.resolved =
            r.state == ResolveState::Resolved ? std::optional<uint32_t>(r.addr) : std::nullopt;
        if (e.enabled && r.state != ResolveState::Resolved) {
            // It was on and is no longer findable - switch it off rather than protecting a
            // stale address that now belongs to something else.
            e.enabled = false;
            e.applied = false;
            changed = true;
        }
        if (before != e.resolved)
            changed = true;
    }
    ++m_revision; // resolution state moved even if nothing was disabled
    if (changed)
        Save();
}

std::vector<ProtectSpec> MemoryLocks::BuildProtections() const
{
    std::vector<ProtectSpec> out;
    for (const auto& e : m_entries) {
        if (!e.enabled || !e.resolved || e.mode == LockMode::Patch)
            continue;
        ProtectSpec s;
        s.addr = *e.resolved;
        if (e.mode == LockMode::Block) {
            s.mode = 0;
            s.len = e.len > 0 ? e.len : 1;
            s.size = 1;
            s.value = 0;
        } else {
            s.mode = 1;
            s.size = e.size == 2 || e.size == 4 ? e.size : 1;
            s.len = s.size; // a forced value protects exactly its own width
            s.value = e.value;
        }
        out.push_back(s);
    }
    return out;
}

// --- serialisation ----------------------------------------------------------------------

nlohmann::json MemoryLocks::EntryToJson(const LockEntry& e)
{
    nlohmann::json j;
    j["id"] = e.id;
    j["desc"] = e.desc;
    if (!e.group.empty())
        j["group"] = e.group;
    j["enabled"] = e.enabled;
    j["mode"] = ToString(e.mode);

    nlohmann::json a;
    a["kind"] = AnchorKindToString(e.anchor.kind);
    switch (e.anchor.kind) {
        case AnchorKind::Absolute:
            a["addr"] = fmt::format("0x{:08X}", e.anchor.addr);
            break;
        case AnchorKind::Seglist:
            a["seg"] = e.anchor.seg;
            a["offset"] = fmt::format("0x{:X}", e.anchor.offset);
            break;
        case AnchorKind::Signature:
            a["pattern"] = e.anchor.pattern.ToString();
            a["target_offset"] = e.anchor.targetOffset;
            break;
        case AnchorKind::Operand:
            a["pattern"] = e.anchor.pattern.ToString();
            a["operand_at"] = e.anchor.operandAt;
            a["operand_size"] = e.anchor.operandSize;
            break;
    }
    j["anchor"] = a;

    if (!e.verify.Empty())
        j["verify"] = e.verify.ToString();

    switch (e.mode) {
        case LockMode::Block:
            j["len"] = e.len;
            break;
        case LockMode::Set:
            j["size"] = e.size;
            j["value"] = e.value;
            break;
        case LockMode::Patch:
            j["bytes"] = BytesToHex(e.bytes);
            // The originals ride along so a session that ends with a patch applied can still
            // revert it next time, rather than leaving the guest permanently modified.
            if (e.applied && !e.original.empty()) {
                j["original"] = BytesToHex(e.original);
                j["applied"] = true;
            }
            break;
    }
    return j;
}

std::optional<LockEntry> MemoryLocks::EntryFromJson(const nlohmann::json& j, std::string& err)
{
    if (!j.is_object()) {
        err = "entry is not an object";
        return std::nullopt;
    }
    LockEntry e;
    if (j.contains("id") && j["id"].is_string())
        e.id = j["id"].get<std::string>();
    if (j.contains("desc") && j["desc"].is_string())
        e.desc = j["desc"].get<std::string>();
    if (j.contains("group") && j["group"].is_string())
        e.group = j["group"].get<std::string>();
    if (j.contains("enabled") && j["enabled"].is_boolean())
        e.enabled = j["enabled"].get<bool>();

    if (j.contains("mode") && j["mode"].is_string()) {
        const auto m = LockModeFromString(j["mode"].get<std::string>());
        if (!m) {
            err = fmt::format("unknown mode '{}'", j["mode"].get<std::string>());
            return std::nullopt;
        }
        e.mode = *m;
    }

    // Anchor. A bare "addr" at the top level is accepted as sugar for an absolute anchor so
    // hand-written files and imported .cht entries stay terse.
    if (j.contains("anchor") && j["anchor"].is_object()) {
        const auto& a = j["anchor"];
        const std::string kindStr =
            a.contains("kind") && a["kind"].is_string() ? a["kind"].get<std::string>() : "absolute";
        const auto kind = AnchorKindFromString(kindStr);
        if (!kind) {
            err = fmt::format("unknown anchor kind '{}'", kindStr);
            return std::nullopt;
        }
        e.anchor.kind = *kind;
        switch (*kind) {
            case AnchorKind::Absolute: {
                if (!a.contains("addr")) {
                    err = "absolute anchor has no addr";
                    return std::nullopt;
                }
                const auto v = ParseAddr(a["addr"], err);
                if (!v)
                    return std::nullopt;
                e.anchor.addr = *v;
                break;
            }
            case AnchorKind::Seglist: {
                if (a.contains("seg") && a["seg"].is_number())
                    e.anchor.seg = a["seg"].get<int>();
                if (a.contains("offset")) {
                    const auto v = ParseAddr(a["offset"], err);
                    if (!v)
                        return std::nullopt;
                    e.anchor.offset = *v;
                }
                break;
            }
            case AnchorKind::Signature:
            case AnchorKind::Operand: {
                if (!a.contains("pattern") || !a["pattern"].is_string()) {
                    err = "signature/operand anchor has no pattern";
                    return std::nullopt;
                }
                auto p = BytePattern::Parse(a["pattern"].get<std::string>(), err);
                if (!p)
                    return std::nullopt;
                e.anchor.pattern = *p;
                if (a.contains("target_offset") && a["target_offset"].is_number())
                    e.anchor.targetOffset = a["target_offset"].get<int>();
                if (a.contains("operand_at") && a["operand_at"].is_number())
                    e.anchor.operandAt = a["operand_at"].get<int>();
                if (a.contains("operand_size") && a["operand_size"].is_number())
                    e.anchor.operandSize = a["operand_size"].get<int>();
                break;
            }
        }
    } else if (j.contains("addr")) {
        const auto v = ParseAddr(j["addr"], err);
        if (!v)
            return std::nullopt;
        e.anchor.kind = AnchorKind::Absolute;
        e.anchor.addr = *v;
    } else {
        err = "entry has no anchor";
        return std::nullopt;
    }

    if (j.contains("verify") && j["verify"].is_string()) {
        auto p = BytePattern::Parse(j["verify"].get<std::string>(), err);
        if (!p)
            return std::nullopt;
        e.verify = *p;
    }

    uint32_t u = 0;
    if (ReadUInt(j, "len", u))
        e.len = u;
    if (ReadUInt(j, "size", u)) {
        if (u > 255) {
            err = fmt::format("size {} is out of range", u);
            return std::nullopt;
        }
        e.size = static_cast<uint8_t>(u);
    }
    if (ReadUInt(j, "value", u))
        e.value = u;

    if (j.contains("bytes") && j["bytes"].is_string()) {
        auto b = HexToBytesImpl(j["bytes"].get<std::string>(), err);
        if (!b)
            return std::nullopt;
        e.bytes = *b;
    }
    if (j.contains("original") && j["original"].is_string()) {
        auto b = HexToBytesImpl(j["original"].get<std::string>(), err);
        if (!b)
            return std::nullopt;
        e.original = *b;
    }
    if (j.contains("applied") && j["applied"].is_boolean())
        e.applied = j["applied"].get<bool>();

    // Validation that would otherwise only surface as a confusing core-side no-op.
    if (e.mode == LockMode::Set && e.size != 1 && e.size != 2 && e.size != 4) {
        err = fmt::format("set size must be 1, 2 or 4 (got {})", e.size);
        return std::nullopt;
    }
    if (e.mode == LockMode::Block && e.len == 0) {
        err = "block length must be at least 1";
        return std::nullopt;
    }
    if (e.mode == LockMode::Patch && e.bytes.empty()) {
        err = "patch has no bytes";
        return std::nullopt;
    }
    return e;
}

void MemoryLocks::Load()
{
    m_entries.clear();
    try {
        std::ifstream f(m_path);
        if (!f.is_open())
            return;
        nlohmann::json j;
        f >> j;
        const nlohmann::json& arr =
            j.is_array() ? j : (j.contains("entries") ? j["entries"] : nlohmann::json());
        if (!arr.is_array())
            return;
        for (const auto& item : arr) {
            std::string err;
            if (auto e = EntryFromJson(item, err)) {
                if (e->id.empty())
                    e->id = MakeId();
                m_entries.push_back(std::move(*e));
            }
            // A malformed entry is skipped rather than discarding the whole file: one bad
            // hand-edit should not cost the user every other lock they had.
        }
    } catch (...) {
        m_entries.clear();
        // Losing every lock silently would look like the user's trainer file vanished.
        Logger::Warn("MemoryLocks: failed to parse {} — no locks loaded", m_path);
    }
}

void MemoryLocks::Save() const
{
    try {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : m_entries)
            arr.push_back(EntryToJson(e));
        std::ofstream f(m_path);
        if (!f.is_open())
            return;
        f << arr.dump(4);
    } catch (...) {
        // Persistence is best-effort; failing to save must not take the session down.
        Logger::Warn("MemoryLocks: failed to save {}", m_path);
    }
}

} // namespace adm::core
