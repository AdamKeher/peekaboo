#include "LibraryCallTracer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

#include <fmt/core.h>
#include <nlohmann/json.hpp>

namespace adm::core {

namespace {

std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Reads the register named by an argument spec out of a trace record.
bool ReadReg(const CpuTraceRecord& rec, const std::string& name, uint32_t& out)
{
    return CpuTraceHistory::ReadRegister(rec, name, out);
}

// Every jump-table entry is a 6-byte JMP, so this is the furthest below a base
// a real vector can sit. Only used to bound the reverse lookup.
constexpr uint32_t kMaxLvo = 0x600;

} // namespace

bool LibraryCallTracer::ParseArgType(const std::string& text, LvoArgType& out)
{
    const std::string t = ToLower(text);
    if (t == "long") {
        out = LvoArgType::Long;
        return true;
    }
    if (t == "ulong") {
        out = LvoArgType::ULong;
        return true;
    }
    if (t == "ptr") {
        out = LvoArgType::Ptr;
        return true;
    }
    if (t == "cstr") {
        out = LvoArgType::CStr;
        return true;
    }
    if (t == "bstr") {
        out = LvoArgType::BStr;
        return true;
    }
    if (t == "bptr") {
        out = LvoArgType::BPtr;
        return true;
    }
    return false;
}

bool LibraryCallTracer::LoadArgTables(const std::string& path)
{
    m_argTables.clear();
    try {
        std::ifstream f(path);
        if (!f.is_open())
            return false;
        auto json = nlohmann::json::parse(f);
        if (!json.is_object())
            return false;

        // `requireReg` is false for the result, which never names a register:
        // the ABI always returns in D0.
        auto parseArg = [](const nlohmann::json& j, LvoArgSpec& out, bool requireReg) {
            if (!j.is_object())
                return false;
            out.reg = j.value("reg", std::string{});
            out.name = j.value("name", std::string{});
            if (requireReg && out.reg.empty())
                return false;
            if (!ParseArgType(j.value("type", std::string{"ulong"}), out.type))
                out.type = LvoArgType::ULong;
            return true;
        };

        for (auto& [libName, table] : json.items()) {
            if (libName.empty() || libName[0] == '_' || !table.is_object())
                continue; // skip "_comment"
            auto& dst = m_argTables[libName];
            for (auto& [offStr, fn] : table.items()) {
                if (!fn.is_object())
                    continue;
                const long off = std::strtol(offStr.c_str(), nullptr, 10);
                const uint32_t lvo = static_cast<uint32_t>(off < 0 ? -off : off);
                LvoSpec spec;
                spec.name = fn.value("name", std::string{});
                if (fn.contains("args") && fn["args"].is_array()) {
                    for (const auto& a : fn["args"]) {
                        LvoArgSpec arg;
                        if (parseArg(a, arg, true))
                            spec.args.push_back(std::move(arg));
                    }
                }
                if (fn.contains("result") && parseArg(fn["result"], spec.result, false)) {
                    spec.result.reg = "d0";
                    spec.haveResult = true;
                }
                dst[lvo] = std::move(spec);
            }
        }
    } catch (const std::exception&) {
        m_argTables.clear();
        return false;
    }
    return !m_argTables.empty();
}

bool LibraryCallTracer::LoadNameTables(const std::string& path)
{
    m_nameTables.clear();
    try {
        std::ifstream f(path);
        if (!f.is_open())
            return false;
        auto json = nlohmann::json::parse(f);
        if (!json.is_object())
            return false;
        for (auto& [libName, table] : json.items()) {
            if (libName.empty() || libName[0] == '_' || !table.is_object())
                continue;
            auto& dst = m_nameTables[libName];
            for (auto& [offStr, fn] : table.items()) {
                if (!fn.is_string())
                    continue;
                const long off = std::strtol(offStr.c_str(), nullptr, 10);
                dst[static_cast<uint32_t>(off < 0 ? -off : off)] = fn.get<std::string>();
            }
        }
    } catch (const std::exception&) {
        m_nameTables.clear();
        return false;
    }
    return !m_nameTables.empty();
}

const LvoSpec* LibraryCallTracer::FindSpec(const std::string& library, uint32_t lvo) const
{
    auto lib = m_argTables.find(library);
    if (lib == m_argTables.end())
        return nullptr;
    auto fn = lib->second.find(lvo);
    return fn == lib->second.end() ? nullptr : &fn->second;
}

std::optional<uint32_t> LibraryCallTracer::ResolveEntry(const exec::Readers& r, uint32_t execBase,
                                                        const std::string& library,
                                                        const std::string& function,
                                                        uint32_t* outLvo) const
{
    if (execBase == 0)
        return std::nullopt;

    // Find the LVO by name in either table — the metadata file is small, so the
    // generated name table is the one that usually answers.
    const std::string wantFn = ToLower(function);
    uint32_t lvo = 0;
    bool found = false;
    auto scanNames = [&](const std::map<uint32_t, std::string>& table) {
        for (const auto& [off, name] : table)
            if (ToLower(name) == wantFn) {
                lvo = off;
                found = true;
                return;
            }
    };
    if (auto it = m_nameTables.find(library); it != m_nameTables.end())
        scanNames(it->second);
    if (!found) {
        if (auto it = m_argTables.find(library); it != m_argTables.end())
            for (const auto& [off, spec] : it->second)
                if (ToLower(spec.name) == wantFn) {
                    lvo = off;
                    found = true;
                    break;
                }
    }
    if (!found)
        return std::nullopt;

    const uint32_t lists[] = {exec::EB_LIBLIST, exec::EB_DEVICELIST, exec::EB_RESOURCELIST};
    for (uint32_t listOff : lists)
        for (const auto& node : exec::ListLibraryNodes(r, execBase + listOff))
            if (node.name == library) {
                if (outLvo)
                    *outLvo = lvo;
                return node.node - lvo;
            }
    return std::nullopt;
}

LibraryCallValue LibraryCallTracer::DecodeValue(const LvoArgSpec& spec, uint32_t raw,
                                                const exec::Readers& r)
{
    LibraryCallValue v;
    v.name = spec.name;
    v.reg = spec.reg;
    v.value = raw;
    switch (spec.type) {
        case LvoArgType::CStr:
            v.text = exec::ReadCString(r, raw);
            break;
        case LvoArgType::BStr:
            // A dos.library "name" argument is a BPTR to a BCPL string.
            v.text = exec::ReadBString(r, raw << 2);
            break;
        case LvoArgType::BPtr:
            v.text = raw ? fmt::format("${:08X}", raw << 2) : std::string{"NULL"};
            break;
        case LvoArgType::Ptr:
            v.text = raw ? fmt::format("${:08X}", raw) : std::string{"NULL"};
            break;
        case LvoArgType::Long:
            v.text = fmt::format("{}", static_cast<int32_t>(raw));
            break;
        case LvoArgType::ULong:
            break;
    }
    return v;
}

LibraryCallSnapshot LibraryCallTracer::Trace(const CpuTraceHistory& history, const exec::Readers& r,
                                             uint32_t execBase, const LibraryCallQuery& query) const
{
    LibraryCallSnapshot snap;
    if (execBase == 0) {
        snap.hint = "ExecBase unavailable — no AmigaOS is booted, so there are no library jump "
                    "tables to watch.";
        return snap;
    }

    // Build the reverse map: jump-table entry address -> which call it is.
    struct EntryInfo
    {
        std::string library;
        std::string function;
        uint32_t lvo = 0;
        const LvoSpec* spec = nullptr;
    };
    std::unordered_map<uint32_t, EntryInfo> entries;

    const uint32_t lists[] = {exec::EB_LIBLIST, exec::EB_DEVICELIST, exec::EB_RESOURCELIST};
    std::vector<std::string> wantFns;
    wantFns.reserve(query.functions.size());
    for (const auto& f : query.functions)
        wantFns.push_back(ToLower(f));

    for (uint32_t listOff : lists) {
        for (const auto& node : exec::ListLibraryNodes(r, execBase + listOff)) {
            if (!query.library.empty() && node.name != query.library)
                continue;
            ++snap.librariesScanned;

            auto add = [&](uint32_t lvo, const std::string& fnName) {
                if (lvo == 0 || lvo > kMaxLvo || lvo > node.node)
                    return;
                if (!wantFns.empty() &&
                    std::find(wantFns.begin(), wantFns.end(), ToLower(fnName)) == wantFns.end())
                    return;
                EntryInfo info;
                info.library = node.name;
                info.function = fnName;
                info.lvo = lvo;
                info.spec = FindSpec(node.name, lvo);
                entries[node.node - lvo] = std::move(info);
            };

            if (auto it = m_nameTables.find(node.name); it != m_nameTables.end())
                for (const auto& [lvo, fnName] : it->second)
                    add(lvo, fnName);
            if (auto it = m_argTables.find(node.name); it != m_argTables.end())
                for (const auto& [lvo, spec] : it->second)
                    if (entries.find(node.node - lvo) == entries.end())
                        add(lvo, spec.name.empty() ? fmt::format("LVO-{}", lvo) : spec.name);
        }
    }

    if (entries.empty()) {
        snap.hint =
            query.library.empty()
                ? "No open library matched a known LVO table."
                : fmt::format("'{}' is not open, or no LVO table covers it.", query.library);
        return snap;
    }

    CpuTraceQuery all;
    all.limit = 0;
    all.startSeq = query.sinceSeq;
    CpuTraceSnapshot trace = history.Query(all);
    if (!trace.available) {
        snap.hint = trace.hint;
        return snap;
    }
    snap.available = true;

    const size_t limit = query.limit <= 0 ? SIZE_MAX : static_cast<size_t>(query.limit);

    for (size_t i = 0; i < trace.matches.size(); ++i) {
        const CpuTraceRecord& rec = trace.matches[i].record;
        auto hit = entries.find(rec.pc);
        if (hit == entries.end())
            continue;
        const EntryInfo& info = hit->second;

        ++snap.totalCalls;
        if (snap.calls.size() >= limit) {
            snap.truncated = true;
            continue;
        }

        LibraryCallEntry call;
        call.seq = rec.seq;
        call.frame = rec.frame;
        call.cycle = rec.cycle;
        call.lRef = rec.lRef;
        call.entryPc = rec.pc;
        call.library = info.library;
        call.function = info.function;
        call.lvo = info.lvo;
        // The JSR already pushed the return address, so the top of the stack at
        // the jump-table entry is where control comes back to.
        call.callerPc = r.Long(rec.a[7]);

        if (info.spec) {
            for (const auto& arg : info.spec->args) {
                uint32_t raw = 0;
                if (ReadReg(rec, arg.reg, raw))
                    call.args.push_back(DecodeValue(arg, raw, r));
            }
            if (info.spec->haveResult) {
                // D0 at the instruction control returns to is the result.
                for (size_t j = i + 1; j < trace.matches.size(); ++j) {
                    const CpuTraceRecord& back = trace.matches[j].record;
                    if (back.pc != call.callerPc)
                        continue;
                    call.result = DecodeValue(info.spec->result, back.d[0], r);
                    call.haveResult = true;
                    break;
                }
            }
        }
        snap.calls.push_back(std::move(call));
    }

    if (snap.calls.empty() && snap.totalCalls == 0)
        snap.hint = "No matching library calls in the recorded trace. Enable the CPU trace and "
                    "history, then run the frames you care about.";
    return snap;
}

} // namespace adm::core
