#include "BreakpointsView.h"
#include "../FontRoles.h"
#include "../TableLayout.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <sstream>
#include "../AddressLink.h"

#include "../../core/BreakpointCommand.h"
#include "../../core/HexUtil.h"
#include "../../core/MemwatchSource.h"
#include "../IconsFontAwesome6.h"
#include "EmulatorView.h"
#include "WatchRegisters.h"

namespace adm::ui {

using core::FormatAddr8;
using core::ParseHexAddress;

// Resolve a UAE memwatch access mask to a short agent name (shared with the AR console).
std::string BreakpointsView::SourceFromAccessMask(uint32_t m)
{
    const char* n = core::MemwatchSourceName(m);
    return n ? std::string(n) : std::string{};
}

BreakpointsView::BreakpointsView(EmulatorView* emulatorView) : m_emulatorView(emulatorView) {}

void BreakpointsView::Restore(bool open)
{
    m_open = open;
    m_noFocus = open;
    if (open)
        m_needsRefresh = true;
}

void BreakpointsView::MarkNeedsRefresh() { m_needsRefresh = true; }

std::pair<bool, bool> BreakpointsView::GetBreakpointStatus(uint32_t address) const
{
    const std::string norm = FormatAddr8(address);
    for (const auto& e : m_entries) {
        if (e.address == norm && e.type == "PC")
            return {true, e.enabled};
    }
    return {false, false};
}

bool BreakpointsView::IsBreakpointAt(uint32_t address) const
{
    return GetBreakpointStatus(address).first;
}

bool BreakpointsView::IsBreakpointEnabledAt(uint32_t address) const
{
    auto [has, enabled] = GetBreakpointStatus(address);
    return has && enabled;
}

void BreakpointsView::ToggleBreakpointAt(uint32_t address)
{
    const std::string norm = FormatAddr8(address);
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        if (m_entries[i].address == norm && m_entries[i].type == "PC") {
            ExecuteToggle(i);
            return;
        }
    }
    // Not found: add it
    ExecuteAdd(norm, "PC");
}

void BreakpointsView::RemoveBreakpointAt(uint32_t address)
{
    const std::string norm = FormatAddr8(address);
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        if (m_entries[i].address == norm && m_entries[i].type == "PC") {
            ExecuteRemove(i);
            return;
        }
    }
}

std::pair<bool, bool> BreakpointsView::GetCopperBreakpointStatus(uint32_t address) const
{
    const std::string norm = FormatAddr8(address);
    for (const auto& e : m_entries) {
        if (e.address == norm && e.type == "Read" && e.source == "COP")
            return {true, e.enabled};
    }
    return {false, false};
}

void BreakpointsView::ToggleCopperBreakpointAt(uint32_t address)
{
    const std::string norm = FormatAddr8(address);
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        if (m_entries[i].address == norm && m_entries[i].type == "Read" &&
            m_entries[i].source == "COP") {
            ExecuteRemove(i); // a Copper breakpoint is a single click target: remove, not disable
            return;
        }
    }
    // A Copper breakpoint fires when the Copper *fetches* the instruction: Read watchpoint,
    // Source=Copper, 2 bytes at the instruction's first word (one hit per fetch).
    ExecuteAdd(norm, "Read", 2, "COP");
}

std::string BreakpointsView::NormalizeAddress(const std::string& token) const
{
    if (auto v = ParseHexAddress(token))
        return FormatAddr8(*v);
    return {};
}

bool BreakpointsView::RunCoreCommand(const std::string& uaeCmd)
{
    if (!m_emulatorView || !m_emulatorView->IsRunning())
        return false;
    auto* emu = m_emulatorView->GetEmulator();
    if (!emu || !emu->HasDebugCommand())
        return false;
    std::string output;
    return emu->ExecuteDebugCommand(uaeCmd, output);
}

int BreakpointsView::AllocateWatchpointSlot() const
{
    // UAE provides MEMWATCH_TOTAL (20) memwatch slots. Pick the lowest index not
    // already claimed by a watchpoint entry (enabled or disabled).
    static constexpr int kMemwatchTotal = 20;
    for (int slot = 0; slot < kMemwatchTotal; ++slot) {
        bool used = false;
        for (const auto& e : m_entries) {
            if (e.type != "PC" && e.slot == slot) {
                used = true;
                break;
            }
        }
        if (!used)
            return slot;
    }
    return -1; // all slots in use
}

int BreakpointsView::AllocateBreakpointSlot(const std::set<int>& exclude) const
{
    // UAE provides BREAKPOINT_TOTAL (20) bpnode slots. Allocate top-down so explicit-node
    // breakpoints (conditional / counted) don't collide with the plain "f <addr>" toggle path,
    // which the core auto-allocates bottom-up.
    static constexpr int kBreakpointTotal = 20;
    for (int slot = kBreakpointTotal - 1; slot >= 0; --slot) {
        if (exclude.count(slot))
            continue;
        bool used = false;
        for (const auto& e : m_entries) {
            if (e.type == "PC" && (e.bpSlot == slot || e.condSlot == slot)) {
                used = true;
                break;
            }
        }
        if (!used)
            return slot;
    }
    return -1; // all slots in use
}

bool BreakpointsView::IssueAdd(const BreakpointEntry& e)
{
    if (!e.needsExplicitNode())
        return RunCoreCommand(BuildAddCommand(e)); // plain PC ("f <addr>") or watchpoint ("w ...")

    // Conditional / counted PC breakpoint: drive UAE's explicit bpnode commands.
    auto cmds = core::BuildSmartBreakpointAddCommands(e.address, e.bpSlot, e.condEnabled, e.condReg,
                                                      e.condOper, e.condValue, e.condMask,
                                                      e.condSlot, e.ignoreCount);
    bool ok = true;
    for (const auto& c : cmds)
        ok = RunCoreCommand(c) && ok;
    return ok;
}

bool BreakpointsView::IssueRemove(const BreakpointEntry& e)
{
    if (!e.needsExplicitNode())
        return RunCoreCommand(BuildRemoveCommand(e));

    auto cmds = core::BuildSmartBreakpointRemoveCommands(e.bpSlot, e.condEnabled, e.condSlot);
    bool ok = true;
    for (const auto& c : cmds)
        ok = RunCoreCommand(c) && ok;
    return ok;
}

void BreakpointsView::TypeToAccess(const std::string& type, bool& read, bool& write, bool& exec)
{
    read = write = exec = false;
    if (type == "PC")
        return;
    // Types are '+'-joined tokens: Read/Write/Exec, or the short R/W/X forms.
    for (size_t i = 0; i < type.size();) {
        size_t plus = type.find('+', i);
        const std::string tok = type.substr(i, plus == std::string::npos ? plus : plus - i);
        if (tok == "Read" || tok == "R")
            read = true;
        else if (tok == "Write" || tok == "W")
            write = true;
        else if (tok == "Exec" || tok == "X")
            exec = true;
        if (plus == std::string::npos)
            break;
        i = plus + 1;
    }
}

std::string BreakpointsView::AccessToType(bool read, bool write, bool exec)
{
    // Single-access types keep their long-standing names so existing sessions, the AR
    // console and the "fl" refresh path all keep matching on them.
    if (read && !write && !exec)
        return "Read";
    if (!read && write && !exec)
        return "Write";
    if (!read && !write && exec)
        return "Exec";
    if (!read && !write && !exec)
        return "Write"; // a watchpoint with no access would never fire
    // Combinations use the short '+'-joined form: "R+W", "R+X", "W+X", "R+W+X".
    std::string out;
    if (read)
        out = "R";
    if (write)
        out += out.empty() ? "W" : "+W";
    if (exec)
        out += out.empty() ? "X" : "+X";
    return out;
}

std::string BreakpointsView::BuildAddCommand(const BreakpointEntry& e) const
{
    // UAE's "f <addr>" toggles (adds if absent) a PC breakpoint.
    if (e.type == "PC")
        return "f " + e.address;
    // Watchpoints use UAE's memwatch: "w <slot> <addr> <size> <accessmask> <flags>".
    // R=read, W=write, I=instruction fetch (execute); they combine. `size` is the watched
    // region length and UAE parses it as HEX — memwatch_func() breaks when an access
    // touches any byte in [addr, addr+size), so a length of 300 watches 768 bytes.
    bool wantRead = false, wantWrite = false, wantExec = false;
    TypeToAccess(e.type, wantRead, wantWrite, wantExec);
    std::string flagStr;
    if (wantRead)
        flagStr += "R";
    if (wantWrite)
        flagStr += "W";
    if (wantExec)
        flagStr += "I";
    if (flagStr.empty())
        flagStr = "RW";
    const char* flags = flagStr.c_str();
    const int sz = e.size > 0 ? e.size : 2;
    // Access-source token (CPU/COP/BLT/DMA/ALL): which agents trigger the watchpoint.
    // "CPU" restricts to CPU code accesses, which is what you almost always want — it
    // gives a meaningful break PC. Widening to Copper/Blitter/DMA catches async hardware
    // accesses, but then the break surfaces at an unrelated CPU instruction.
    const char* src = e.source.empty() ? "CPU" : e.source.c_str();
    char buf[96];
    if (e.valueEnabled) {
        // "V <value>": break only when the written value matches. UAE infers the value
        // width from the digit count, so pad to the access width for an unambiguous
        // compare. A range watch has no single access width, so clamp to a long.
        const int valueBytes = (sz == 1 || sz == 2 || sz == 4) ? sz : 4;
        const int hexDigits = valueBytes * 2;
        const uint32_t mask = valueBytes == 4 ? 0xFFFFFFFFu : ((1u << (valueBytes * 8)) - 1u);
        snprintf(buf, sizeof(buf), "w %d %s %X %s %s V %0*X", e.slot, e.address.c_str(), sz, src,
                 flags, hexDigits, e.value & mask);
    } else {
        snprintf(buf, sizeof(buf), "w %d %s %X %s %s", e.slot, e.address.c_str(), sz, src, flags);
    }
    return buf;
}

std::string BreakpointsView::BuildRemoveCommand(const BreakpointEntry& e) const
{
    // "f <addr>" toggles, so re-sending it removes the PC breakpoint.
    if (e.type == "PC")
        return "f " + e.address;
    // "w <slot>" with no address removes that memwatch slot.
    char buf[24];
    snprintf(buf, sizeof(buf), "w %d", e.slot);
    return buf;
}

void BreakpointsView::SetBreakpointOrigin(uint32_t address, const std::string& origin)
{
    if (origin.empty()) {
        ClearBreakpointOrigin(address);
        return;
    }
    m_origins[FormatAddr8(address)] = origin;
}

void BreakpointsView::ClearBreakpointOrigin(uint32_t address)
{
    m_origins.erase(FormatAddr8(address));
}

std::string BreakpointsView::GetBreakpointOrigin(uint32_t address) const
{
    auto it = m_origins.find(FormatAddr8(address));
    return it == m_origins.end() ? std::string{} : it->second;
}

void BreakpointsView::RefreshFromCore()
{
    m_needsRefresh = false;

    if (!m_emulatorView || !m_emulatorView->IsRunning())
        return;
    auto* emu = m_emulatorView->GetEmulator();
    if (!emu || !emu->HasDebugCommand())
        return;

    std::string output;
    if (!emu->ExecuteDebugCommand("fl", output))
        return;

    // Smart PC breakpoints (condition / ignore-count / one-shot) carry host-side metadata that
    // "fl" can't round-trip, so they are carried over from the current list rather than reparsed.
    // Their PC head node still appears in "fl" as "PC == addr", so skip those addresses below to
    // avoid creating a duplicate plain entry.
    std::set<std::string> smartAddrs;
    for (const auto& e : m_entries) {
        if (e.hasHostMeta())
            smartAddrs.insert(e.address);
    }

    // Parse active (enabled) breakpoints from "fl" output
    std::vector<BreakpointEntry> active;
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        // Match "N: PC == XXXXXXXX [...]"
        size_t pcPos = line.find("PC ==");
        if (pcPos != std::string::npos) {
            size_t addrStart = pcPos + 5;
            while (addrStart < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[addrStart])))
                ++addrStart;
            std::string addrHex;
            while (addrStart < line.size() &&
                   std::isxdigit(static_cast<unsigned char>(line[addrStart])))
                addrHex += line[addrStart++];
            if (auto v = ParseHexAddress(addrHex)) {
                const std::string norm = FormatAddr8(*v);
                if (smartAddrs.count(norm))
                    continue; // belongs to a carried-over smart breakpoint
                active.push_back({norm, "PC", true});
            }
        }
    }

    // Rebuild displayed list: enabled entries from core + disabled entries cached locally.
    // Prune disabled entries that somehow got re-added to the core (shouldn't happen, but be safe).
    for (auto it = m_disabledEntries.begin(); it != m_disabledEntries.end();) {
        const std::string& addr = it->first;
        bool inCore = std::any_of(active.begin(), active.end(),
                                  [&](const BreakpointEntry& e) { return e.address == addr; });
        if (inCore)
            it = m_disabledEntries.erase(it);
        else
            ++it;
    }

    // "fl" only reports PC breakpoints; carry ALL watchpoint entries (enabled and
    // disabled) over from the current list so they — and their size/slot — survive a
    // refresh. m_disabledEntries only tracks address+type, so reconstructing a disabled
    // watchpoint from it would reset its size to the default.
    std::vector<BreakpointEntry> watchpoints;
    std::vector<BreakpointEntry> smartPc;
    for (const auto& e : m_entries) {
        if (e.type != "PC")
            watchpoints.push_back(e);
        else if (e.hasHostMeta())
            smartPc.push_back(e); // carry verbatim (enabled or disabled)
    }

    m_entries = active;
    m_entries.insert(m_entries.end(), watchpoints.begin(), watchpoints.end());
    m_entries.insert(m_entries.end(), smartPc.begin(), smartPc.end());
    // Disabled plain PC breakpoints aren't present in the core; restore them from the cache.
    // (Disabled watchpoints and smart PC breakpoints were already carried over above.)
    for (const auto& [addr, type] : m_disabledEntries) {
        if (type == "PC" && !smartAddrs.count(addr))
            m_entries.push_back({addr, type, false});
    }

    if (m_selectedRow >= static_cast<int>(m_entries.size()))
        m_selectedRow = -1;
}

bool BreakpointsView::ExecuteAdd(const std::string& addr, const std::string& type, int size,
                                 const std::string& source, bool valueEnabled, uint32_t value,
                                 const std::string& regName)
{
    const std::string norm = NormalizeAddress(addr);
    if (norm.empty())
        return false;

    // Check not already present (enabled or disabled)
    for (const auto& e : m_entries) {
        if (e.address == norm && e.type == type)
            return false;
    }

    BreakpointEntry entry{norm, type, true};
    entry.size = size > 0 ? size : 2;
    entry.source = source.empty() ? "CPU" : source;
    entry.valueEnabled = valueEnabled && type != "PC";
    entry.value = value;
    entry.regName = regName;
    return InsertEntry(std::move(entry));
}

bool BreakpointsView::InsertEntry(BreakpointEntry entry)
{
    if (entry.type != "PC") {
        entry.slot = AllocateWatchpointSlot();
        if (entry.slot < 0)
            return false; // all 20 UAE memwatch slots are in use
    }
    if (entry.needsExplicitNode()) {
        entry.bpSlot = AllocateBreakpointSlot();
        if (entry.condEnabled)
            entry.condSlot = AllocateBreakpointSlot({entry.bpSlot});
    }
    bool success = true;
    if (m_emulatorView && m_emulatorView->IsRunning()) {
        success = IssueAdd(entry);
        if (success)
            m_entries.push_back(entry); // visible immediately; refresh confirms
    } else {
        // If not running, just add it to our local list. It will be injected when the emulator
        // starts.
        m_entries.push_back(entry);
    }

    if (success && m_emulatorView && m_emulatorView->IsRunning()) {
        auto* emu = m_emulatorView->GetEmulator();
        if (emu && emu->HasArmDebug())
            emu->ArmDebugTrace();
        m_needsRefresh = true; // re-query fl to confirm
    }
    return success;
}

void BreakpointsView::ExecuteRemove(int index)
{
    if (index < 0 || index >= static_cast<int>(m_entries.size()))
        return;
    const BreakpointEntry e = m_entries[static_cast<size_t>(index)];
    if (e.enabled) {
        if (m_emulatorView && m_emulatorView->IsRunning()) {
            IssueRemove(e); // removes from core
        }
    }
    // Always erase from local caches immediately so the UI reflects the removal
    // even when the emulator is off (RefreshFromCore won't run in that case).
    m_disabledEntries.erase({e.address, e.type});
    if (e.type == "PC")
        m_origins.erase(e.address);
    m_entries.erase(m_entries.begin() + index);
    if (m_selectedRow >= static_cast<int>(m_entries.size()))
        m_selectedRow = static_cast<int>(m_entries.size()) - 1;
    m_needsRefresh = true; // re-query fl when running to confirm
}

void BreakpointsView::NotifyEmulatorReset()
{
    // Reset() reloads only the game, so the core's breakpoint nodes survive but lose their armed
    // state. Re-arm the debug trace so they fire again; do NOT re-send the add commands ("f <addr>"
    // toggles, which would delete the still-present breakpoints). Re-query "fl" to resync the UI.
    if (m_emulatorView) {
        auto* emu = m_emulatorView->GetEmulator();
        if (emu && emu->HasArmDebug())
            emu->ArmDebugTrace();
    }
    m_needsRefresh = true;
}

void BreakpointsView::ReinjectionToCore()
{
    // Smart breakpoints need valid bpnode slots before injection (a fresh core / imported set
    // clears them). Assign any that are missing.
    for (auto& e : m_entries) {
        if (e.needsExplicitNode() && e.bpSlot < 0) {
            e.bpSlot = AllocateBreakpointSlot();
            if (e.condEnabled)
                e.condSlot = AllocateBreakpointSlot({e.bpSlot});
        }
    }
    // Push all previously-enabled breakpoints into the fresh core instance.
    // Disabled entries stay disabled (not injected).
    for (const auto& e : m_entries) {
        if (e.enabled)
            IssueAdd(e);
    }
    // Arm the CPU trace so re-injected breakpoints fire on the next retro_run() frame.
    if (m_emulatorView) {
        auto* emu = m_emulatorView->GetEmulator();
        if (emu && emu->HasArmDebug())
            emu->ArmDebugTrace();
        // Re-arm exception / beam breaks (they live in the core, cleared on reload).
        if (emu) {
            if (m_exceptionMask != 0)
                emu->ArmExceptionBreak(m_exceptionMask);
            if (m_beamVpos >= 0)
                emu->ArmBeamBreak(m_beamVpos, m_beamHpos);
            if (m_diskBreak.Armed())
                emu->ArmDiskBreak(m_diskBreak.access, m_diskBreak.cyl, m_diskBreak.side,
                                  m_diskBreak.sector, m_diskBreak.addrLo, m_diskBreak.addrHi);
        }
    }
    m_needsRefresh = true; // re-query fl to confirm
}

void BreakpointsView::SetExceptionMask(uint64_t mask)
{
    m_exceptionMask = mask;
    if (m_emulatorView && m_emulatorView->IsRunning()) {
        if (auto* emu = m_emulatorView->GetEmulator())
            emu->ArmExceptionBreak(mask);
    }
}

void BreakpointsView::SetBeamBreak(int vpos, int hpos)
{
    m_beamVpos = vpos;
    m_beamHpos = hpos;
    if (m_emulatorView && m_emulatorView->IsRunning()) {
        if (auto* emu = m_emulatorView->GetEmulator())
            emu->ArmBeamBreak(vpos, hpos);
    }
}

void BreakpointsView::SetDiskBreak(const DiskBreak& db)
{
    m_diskBreak = db;
    if (m_emulatorView && m_emulatorView->IsRunning()) {
        if (auto* emu = m_emulatorView->GetEmulator())
            emu->ArmDiskBreak(db.access, db.cyl, db.side, db.sector, db.addrLo, db.addrHi);
    }
}

nlohmann::json BreakpointsView::ExportEntries() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : m_entries) {
        nlohmann::json node;
        node["address"] = e.address;
        node["type"] = e.type;
        node["enabled"] = e.enabled;
        if (e.type != "PC") {
            node["size"] = e.size;
            node["source"] = e.source;
            if (e.valueEnabled) {
                node["valueEnabled"] = true;
                node["value"] = e.value;
            }
            if (!e.regName.empty())
                node["regName"] = e.regName;
            // Only written when set, so a session without log-mode watchpoints still
            // loads in an older build that ignores these keys.
            if (e.logOnly) {
                node["logOnly"] = true;
                if (e.maxHits > 0)
                    node["maxHits"] = e.maxHits;
            }
            if (e.oneShot)
                node["oneShot"] = true;
        } else if (e.hasHostMeta()) {
            // Smart PC breakpoint metadata (bpnode slots are not persisted — reallocated on load).
            if (e.condEnabled) {
                node["condEnabled"] = true;
                node["condReg"] = e.condReg;
                node["condOper"] = e.condOper;
                node["condValue"] = e.condValue;
                node["condMask"] = e.condMask;
            }
            if (e.ignoreCount > 0)
                node["ignoreCount"] = e.ignoreCount;
            if (e.oneShot)
                node["oneShot"] = true;
        }
        arr.push_back(std::move(node));
    }
    nlohmann::json root;
    root["version"] = 2;
    root["entries"] = std::move(arr);
    if (m_exceptionMask != 0)
        root["exceptionMask"] = m_exceptionMask;
    if (m_beamVpos >= 0) {
        root["beamVpos"] = m_beamVpos;
        root["beamHpos"] = m_beamHpos;
    }
    if (m_diskBreak.Armed()) {
        root["diskBreak"] = {{"access", m_diskBreak.access}, {"cyl", m_diskBreak.cyl},
                             {"side", m_diskBreak.side},     {"sector", m_diskBreak.sector},
                             {"addrLo", m_diskBreak.addrLo}, {"addrHi", m_diskBreak.addrHi}};
    }
    if (auto* kp = GetKprintfBreakpoints(); kp && !kp->Empty())
        root["kprintf"] = kp->Export();
    return root;
}

void BreakpointsView::ImportEntries(const nlohmann::json& j)
{
    // Drop any existing breakpoints from the core and our caches so the imported set is
    // authoritative (ClearAllByType("") removes from the core and clears m_entries).
    ClearAllByType("");
    m_entries.clear();
    m_disabledEntries.clear();
    m_selectedRow = -1;

    // Exception / beam break state (re-armed by ReinjectionToCore below).
    m_exceptionMask = j.value("exceptionMask", 0ull);
    m_beamVpos = j.value("beamVpos", -1);
    m_beamHpos = j.value("beamHpos", -1);
    m_diskBreak = DiskBreak{};
    if (j.contains("diskBreak")) {
        const auto& d = j["diskBreak"];
        m_diskBreak.access = d.value("access", -1);
        m_diskBreak.cyl = d.value("cyl", -1);
        m_diskBreak.side = d.value("side", -1);
        m_diskBreak.sector = d.value("sector", -1);
        m_diskBreak.addrLo = d.value("addrLo", 0u);
        m_diskBreak.addrHi = d.value("addrHi", 0u);
    }

    // KPrintF traps are matched host-side, so they need no re-injection — just replace the store.
    if (auto* kp = GetKprintfBreakpoints()) {
        if (j.contains("kprintf"))
            kp->Import(j["kprintf"]);
        else
            kp->Clear();
    }

    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& e : j["entries"]) {
            if (!e.contains("address"))
                continue;
            BreakpointEntry entry;
            entry.address = NormalizeAddress(e.value("address", std::string{}));
            entry.type = e.value("type", std::string("PC"));
            entry.enabled = e.value("enabled", true);
            // Any positive length is valid: watchpoints can cover an arbitrary range, not
            // just byte/word/long.
            entry.size = e.value("size", 2);
            if (entry.size <= 0)
                entry.size = 2;
            entry.source = e.value("source", std::string("CPU"));
            if (entry.source.empty())
                entry.source = "CPU";
            entry.valueEnabled = e.value("valueEnabled", false);
            entry.value = e.value("value", 0u);
            entry.regName = e.value("regName", std::string{});
            entry.logOnly = e.value("logOnly", false);
            entry.maxHits = e.value("maxHits", 0u);
            entry.slot = -1;
            // Smart PC breakpoint metadata (v2+). Slots are reallocated by ReinjectionToCore.
            entry.condEnabled = e.value("condEnabled", false);
            entry.condReg = e.value("condReg", std::string("D0"));
            entry.condOper = e.value("condOper", std::string("=="));
            entry.condValue = e.value("condValue", 0u);
            entry.condMask = e.value("condMask", 0xFFFFFFFFu);
            entry.ignoreCount = e.value("ignoreCount", 0);
            entry.oneShot = e.value("oneShot", false);
            entry.bpSlot = -1;
            entry.condSlot = -1;
            if (entry.address.empty())
                continue;
            // Skip duplicates of an already-imported (address,type) pair.
            bool dup = false;
            for (const auto& existing : m_entries) {
                if (existing.address == entry.address && existing.type == entry.type) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            // Watchpoints need a memwatch slot; AllocateWatchpointSlot scans m_entries, so
            // assign before pushing this entry.
            if (entry.type != "PC")
                entry.slot = AllocateWatchpointSlot();
            m_entries.push_back(entry);
            if (!entry.enabled)
                m_disabledEntries.insert({entry.address, entry.type});
        }
    }

    // Inject the enabled entries into the now-running core.
    ReinjectionToCore();

    // Claim the current running state so Draw()'s not-running→running auto-reinjection does
    // not fire and double-toggle these PC breakpoints (UAE's "f <addr>" is a toggle) right
    // back off on the first frame after a session is opened from a cold start.
    m_wasRunning = (m_emulatorView && m_emulatorView->IsRunning());
}

void BreakpointsView::ExecuteToggle(int index)
{
    if (index < 0 || index >= static_cast<int>(m_entries.size()))
        return;
    BreakpointEntry& e = m_entries[static_cast<size_t>(index)];
    if (e.enabled) {
        // Disable: remove from core, cache locally as disabled.
        if (m_emulatorView && m_emulatorView->IsRunning()) {
            IssueRemove(e); // removes from core
        }
        m_disabledEntries.insert({e.address, e.type});
        e.enabled = false;
        // UAE's "f" remove command clears SPCFLAG_BRK globally, which would prevent
        // any remaining enabled breakpoints from firing. Re-arm if there are still
        // enabled breakpoints so they continue to work after this one is disabled.
        if (m_emulatorView && m_emulatorView->IsRunning()) {
            bool anyStillEnabled = false;
            for (const auto& entry : m_entries) {
                if (entry.enabled) {
                    anyStillEnabled = true;
                    break;
                }
            }
            if (anyStillEnabled) {
                auto* emu = m_emulatorView->GetEmulator();
                if (emu && emu->HasArmDebug())
                    emu->ArmDebugTrace();
            }
        }
    } else {
        // Re-enable: inject back into core, remove from disabled cache.
        // A disabled watchpoint may have lost its slot across a RefreshFromCore
        // (m_disabledEntries doesn't track slots); reassign one if needed.
        if (e.type != "PC" && e.slot < 0)
            e.slot = AllocateWatchpointSlot();
        // A re-enabled smart breakpoint may have lost its bpnode slot(s) across a refresh.
        if (e.needsExplicitNode() && e.bpSlot < 0) {
            e.bpSlot = AllocateBreakpointSlot();
            if (e.condEnabled)
                e.condSlot = AllocateBreakpointSlot({e.bpSlot});
        }
        bool success = true;
        if (m_emulatorView && m_emulatorView->IsRunning()) {
            success = IssueAdd(e);
        }
        if (success) {
            m_disabledEntries.erase({e.address, e.type});
            if (m_emulatorView && m_emulatorView->IsRunning()) {
                auto* emu = m_emulatorView->GetEmulator();
                if (emu && emu->HasArmDebug())
                    emu->ArmDebugTrace();
            }
            e.enabled = true;
        }
    }
}

bool BreakpointsView::AddBreakpointAt(uint32_t address, const std::string& type, int size,
                                      const std::string& source)
{
    return ExecuteAdd(FormatAddr8(address), type, size, source);
}

int BreakpointsView::FindEntryBySlot(int slot) const
{
    if (slot < 0)
        return -1;
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        const auto& e = m_entries[static_cast<size_t>(i)];
        if (e.type != "PC" && e.slot == slot)
            return i;
    }
    return -1;
}

int BreakpointsView::AddWatchpoint(const core::WatchpointSpec& spec, std::string& outError)
{
    if (!spec.read && !spec.write && !spec.exec) {
        outError = "access must include at least one of read/write/exec";
        return -1;
    }
    if (spec.len == 0) {
        outError = "len must be >= 1";
        return -1;
    }

    const std::string type = AccessToType(spec.read, spec.write, spec.exec);
    const std::string norm = FormatAddr8(spec.addr);
    for (const auto& e : m_entries) {
        if (e.address == norm && e.type == type) {
            outError = "a " + type + " watchpoint already exists at $" + norm;
            return -1;
        }
    }
    if (AllocateWatchpointSlot() < 0) {
        outError = "no free watchpoint slots (the core provides 20)";
        return -1;
    }

    BreakpointEntry entry{norm, type, true};
    entry.size = static_cast<int>(spec.len);
    entry.source = spec.source.empty() ? "CPU" : spec.source;
    entry.valueEnabled = spec.valueEnabled;
    entry.value = spec.value;
    entry.logOnly = spec.logOnly;
    entry.maxHits = spec.maxHits;
    entry.oneShot = spec.oneShot;

    if (!InsertEntry(entry)) {
        outError = "the core rejected the watchpoint command";
        return -1;
    }
    // InsertEntry assigned the slot on its own copy; find the entry it stored.
    const int idx = static_cast<int>(m_entries.size()) - 1;
    return idx >= 0 ? m_entries[static_cast<size_t>(idx)].slot : -1;
}

bool BreakpointsView::RemoveWatchpointSlot(int slot)
{
    const int idx = FindEntryBySlot(slot);
    if (idx < 0)
        return false;
    ExecuteRemove(idx);
    return true;
}

std::vector<core::WatchpointInfo> BreakpointsView::ListWatchpoints() const
{
    std::vector<core::WatchpointInfo> out;
    for (const auto& e : m_entries) {
        if (e.type == "PC")
            continue;
        core::WatchpointInfo info;
        TypeToAccess(e.type, info.read, info.write, info.exec);
        auto addr = ParseHexAddress(e.address);
        info.addr = addr ? *addr : 0;
        info.len = static_cast<uint32_t>(e.size > 0 ? e.size : 1);
        info.source = e.source;
        info.valueEnabled = e.valueEnabled;
        info.value = e.value;
        info.logOnly = e.logOnly;
        info.maxHits = e.maxHits;
        info.oneShot = e.oneShot;
        info.slot = e.slot;
        info.enabled = e.enabled;
        auto hitIt = m_hitCounts.find(e.address);
        info.hits = e.logOnly ? e.logHits : (hitIt != m_hitCounts.end() ? hitIt->second : 0);
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<core::WatchpointHit> BreakpointsView::GetWatchpointLog(int slot, int limit, bool clear)
{
    std::vector<core::WatchpointHit> out;
    for (const auto& h : m_watchLog) {
        if (slot >= 0 && h.slot != slot)
            continue;
        out.push_back(h);
    }
    // Keep the newest when a limit is given: with a wide watchpoint the tail is what you
    // are usually looking at.
    if (limit > 0 && out.size() > static_cast<size_t>(limit))
        out.erase(out.begin(), out.end() - limit);

    if (clear) {
        if (slot < 0) {
            m_watchLog.clear();
        } else {
            for (auto it = m_watchLog.begin(); it != m_watchLog.end();)
                it = (it->slot == slot) ? m_watchLog.erase(it) : it + 1;
        }
    }
    return out;
}

bool BreakpointsView::ConsumeLogModeBreak()
{
    if (!m_emulatorView || !m_emulatorView->IsRunning())
        return false;
    auto* emu = m_emulatorView->GetEmulator();
    if (!emu)
        return false;

    adm::AmigaEmulator::MemwatchHit wh;
    if (!emu->GetLastMemwatchHit(wh))
        return false; // not a watchpoint break (PC breakpoint, KPrintF, beam, ...)

    // Find the log-mode entry whose watched range overlaps the access. A break caused by
    // a normal (breaking) watchpoint must fall through so the machine still stops.
    const uint32_t accLo = wh.addr;
    const uint32_t accHi = wh.addr + (wh.size > 0 ? static_cast<uint32_t>(wh.size) : 1u);
    for (auto& e : m_entries) {
        if (e.type == "PC" || !e.enabled || !e.logOnly)
            continue;
        auto eaOpt = ParseHexAddress(e.address);
        if (!eaOpt)
            continue;
        const uint32_t ea = *eaOpt;
        const uint32_t eHi = ea + static_cast<uint32_t>(e.size > 0 ? e.size : 1);
        if (ea >= accHi || accLo >= eHi)
            continue;

        core::WatchpointHit hit;
        hit.slot = e.slot;
        hit.pc = wh.pc;
        hit.addr = wh.addr;
        hit.value = wh.val;
        hit.rwi = wh.rwi;
        hit.size = wh.size;
        // Name the true triggering agent when the core reports it: a watchpoint stops at
        // the next CPU instruction boundary, so blaming wh.pc for a Copper or Blitter
        // access would point at an unrelated instruction.
        std::string src = SourceFromAccessMask(wh.accessMask);
        hit.source = src.empty() ? e.source : src;
        hit.frame = emu->GetTotalFrameCount();

        if (m_watchLog.size() >= kMaxWatchLog)
            m_watchLog.pop_front();
        m_watchLog.push_back(std::move(hit));
        e.logHits++;

        // maxHits disarms the watchpoint rather than merely stopping the recording, so a
        // finished capture costs nothing in emulation speed.
        if (e.maxHits > 0 && e.logHits >= e.maxHits) {
            const int idx = FindEntryBySlot(e.slot);
            if (idx >= 0)
                ExecuteRemove(idx);
        }
        return true;
    }
    return false;
}

bool BreakpointsView::AddSmartBreakpointAt(uint32_t address, bool condEnabled,
                                           const std::string& condReg, const std::string& condOper,
                                           uint32_t condValue, uint32_t condMask, int ignoreCount,
                                           bool oneShot)
{
    const std::string norm = FormatAddr8(address);
    // One PC breakpoint per address (enabled or disabled).
    for (const auto& e : m_entries) {
        if (e.address == norm && e.type == "PC")
            return false;
    }

    BreakpointEntry entry{norm, "PC", true};
    entry.condEnabled = condEnabled;
    if (condEnabled) {
        entry.condReg = condReg.empty() ? "D0" : condReg;
        entry.condOper = condOper.empty() ? "==" : condOper;
        entry.condValue = condValue;
        entry.condMask = condMask;
    }
    entry.ignoreCount = ignoreCount > 0 ? ignoreCount : 0;
    entry.oneShot = oneShot;
    if (entry.needsExplicitNode()) {
        entry.bpSlot = AllocateBreakpointSlot();
        if (entry.condEnabled)
            entry.condSlot = AllocateBreakpointSlot({entry.bpSlot});
    }

    bool success = true;
    if (m_emulatorView && m_emulatorView->IsRunning()) {
        success = IssueAdd(entry);
        if (success) {
            m_entries.push_back(entry);
            auto* emu = m_emulatorView->GetEmulator();
            if (emu && emu->HasArmDebug())
                emu->ArmDebugTrace();
            m_needsRefresh = true;
        }
    } else {
        m_entries.push_back(entry); // injected when the emulator starts
    }
    return success;
}

bool BreakpointsView::RemoveEntryAt(uint32_t address, const std::string& type)
{
    const std::string norm = FormatAddr8(address);
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        if (m_entries[i].address == norm && m_entries[i].type == type) {
            ExecuteRemove(i);
            return true;
        }
    }
    return false;
}

void BreakpointsView::ClearAllByType(const std::string& type)
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (!type.empty() && it->type != type) {
            ++it;
            continue;
        }
        if (it->enabled && m_emulatorView && m_emulatorView->IsRunning())
            IssueRemove(*it); // removes from core
        m_disabledEntries.erase({it->address, it->type});
        m_origins.erase(it->address);
        m_hitCounts.erase(it->address);
        m_lastHit.erase(it->address);
        it = m_entries.erase(it);
    }
    m_selectedRow = -1;
    // Removing breakpoints clears SPCFLAG_BRK globally; re-arm if any remain enabled.
    if (m_emulatorView && m_emulatorView->IsRunning()) {
        bool anyEnabled = std::any_of(m_entries.begin(), m_entries.end(),
                                      [](const BreakpointEntry& e) { return e.enabled; });
        if (anyEnabled) {
            auto* emu = m_emulatorView->GetEmulator();
            if (emu && emu->HasArmDebug())
                emu->ArmDebugTrace();
        }
    }
    m_needsRefresh = true; // re-query fl when running to confirm
}

void BreakpointsView::Draw()
{
    const bool emuRunning = m_emulatorView && m_emulatorView->IsRunning();

    // Track running-state transitions even when the window is closed, so that
    // a stop → restart cycle is detected correctly regardless of window visibility.
    if (!emuRunning) {
        m_wasRunning = false;
    } else if (!m_wasRunning) {
        // Emulator just started (or restarted). Re-inject any breakpoints we
        // were tracking so they are present in the fresh core instance.
        m_wasRunning = true;
        if (!m_entries.empty())
            ReinjectionToCore();
    }

    // Refresh from core if flagged, even when the window is closed, so that
    // other views (e.g. the disassembly gutter) always see up-to-date state.
    if (m_needsRefresh)
        RefreshFromCore();

    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(520, 300), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = 0;
    if (m_noFocus) {
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;
        m_noFocus = false;
    }

    if (m_requestFocus) {
        m_requestFocus = false;
        ImGui::SetNextWindowFocus();
    }

    if (!ImGui::Begin(ICON_FA_CIRCLE_DOT " Breakpoints###Breakpoints##debugger", &m_open, flags)) {
        ImGui::End();
        return;
    }

    // Refresh when window first opens
    if (ImGui::IsWindowAppearing())
        m_needsRefresh = true;

    // Track hits: detect when the emulator pauses at a breakpoint
    if (emuRunning && m_emulatorView->IsPaused()) {
        adm::AmigaEmulator::CpuState cpuState;
        if (m_emulatorView->GetEmulator()->GetCpuState(cpuState)) {
            std::string pcStr = FormatAddr8(cpuState.pc);
            if (pcStr != m_lastPausePc) {
                // Check if we hit an enabled PC breakpoint
                auto [hasBp, bpEnabled] = GetBreakpointStatus(cpuState.pc);
                int oneShotRemoveIdx = -1;
                if (hasBp && bpEnabled) {
                    const uint64_t n = ++m_hitCounts[pcStr];
                    // Record it for the status row too. Without this the footer only ever spoke
                    // about watchpoints, so a run full of PC breakpoint hits still read
                    // "No breakpoint hit yet".
                    HitInfo hi;
                    hi.pcBreak = true;
                    hi.accessAddr = cpuState.pc;
                    hi.pc = cpuState.pc;
                    hi.rwi = 4; // instruction fetch
                    hi.source = "CPU";
                    hi.hitCount = n;
                    m_lastHit[pcStr] = hi;
                    m_lastHitAddr = pcStr;
                    // One-shot: now that it has fired (we stay paused here), schedule its removal
                    // so resuming doesn't stop here again.
                    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
                        const auto& e = m_entries[static_cast<size_t>(i)];
                        if (e.type == "PC" && e.enabled && e.oneShot && e.address == pcStr) {
                            oneShotRemoveIdx = i;
                            break;
                        }
                    }
                }

                // Memory watchpoints break at the *next* PC and are keyed by the
                // accessed data address, so GetBreakpointStatus(pc) never matches
                // them. Match the access range against enabled watch entries instead.
                adm::AmigaEmulator::MemwatchHit wh;
                if (m_emulatorView->GetEmulator()->GetLastMemwatchHit(wh)) {
                    const uint32_t accLo = wh.addr;
                    const uint32_t accHi =
                        wh.addr + (wh.size > 0 ? static_cast<uint32_t>(wh.size) : 1u);
                    for (const auto& e : m_entries) {
                        if (e.type == "PC" || !e.enabled)
                            continue;
                        auto eaOpt = ParseHexAddress(e.address);
                        if (!eaOpt)
                            continue;
                        const uint32_t ea = *eaOpt;
                        const uint32_t eHi = ea + static_cast<uint32_t>(e.size > 0 ? e.size : 1);
                        // Proper interval overlap: the watched region [ea, eHi) and the
                        // access [accLo, accHi) intersect. (The old start-in-range test
                        // missed a Long watch whose start is below the accessed byte.)
                        if (ea < accHi && accLo < eHi) {
                            // Record hit details for the info column / tooltip (#2).
                            HitInfo hi;
                            hi.hitCount = ++m_hitCounts[e.address];
                            hi.accessAddr = wh.addr;
                            hi.value = wh.val;
                            hi.rwi = wh.rwi;
                            hi.size = wh.size;
                            hi.pc = wh.pc;
                            // True triggering agent if the core exposes the access mask;
                            // otherwise fall back to the watch's configured source.
                            std::string src = SourceFromAccessMask(wh.accessMask);
                            hi.source = src.empty() ? e.source : src;
                            m_lastHit[e.address] = hi;
                            m_lastHitAddr = e.address;
                            break; // the core fires one slot per access
                        }
                    }
                }
                // Remove a one-shot breakpoint after all hit bookkeeping (keeps indices valid).
                if (oneShotRemoveIdx >= 0)
                    ExecuteRemove(oneShotRemoveIdx);
                m_lastPausePc = pcStr;
            }
        }
    } else {
        m_lastPausePc.clear();
    }

    // ----- Breakpoint list -----
    // Collapsible like the Exceptions / KPrintF sections below, so a long list can be folded
    // away when you are working with one of those instead.
    if (ImGui::CollapsingHeader("Breakpoints / watchpoints", ImGuiTreeNodeFlags_DefaultOpen))
        DrawBreakpointList();

    // Drawn outside the collapsing section: the modal owns the whole window while it is up,
    // so it must keep being submitted regardless of the section state.
    DrawAddPopup();

    // ----- Exceptions & raster breaks -----
    DrawExceptionAndBeamSection();

    // ----- Break on KPrintF debug output -----
    DrawKprintfSection();

    // ----- Last-hit status row: communicates which breakpoint fired and its source -----
    DrawLastHitStatus();

    ImGui::End();
}

void BreakpointsView::DrawBreakpointList()
{
    const bool emuRunning = m_emulatorView && m_emulatorView->IsRunning();
    const bool hasSelection =
        m_selectedRow >= 0 && m_selectedRow < static_cast<int>(m_entries.size());

    std::string currentPcStr;
    if (emuRunning && m_emulatorView->IsPaused()) {
        adm::AmigaEmulator::CpuState cpuState;
        if (m_emulatorView->GetEmulator()->GetCpuState(cpuState)) {
            currentPcStr = FormatAddr8(cpuState.pc);
        }
    }

    // ----- Toolbar -----
    if (ImGui::Button(ICON_FA_PLUS " Add")) {
        m_addAddress[0] = '\0';
        m_addTypeIndex = 0;
        m_addSizeIndex = 1;   // word
        m_addSourceIndex = 0; // CPU
        m_addUseRegister = false;
        m_addValueEnabled = false;
        m_addValue[0] = '\0';
        m_addLength[0] = '\0';
        m_addLogOnly = false;
        m_addMaxHits[0] = '\0';
        m_addCondEnabled = false;
        m_addCondRegIndex = 0;
        m_addCondOperIndex = 0;
        m_addCondValue[0] = '\0';
        m_addCondMask[0] = '\0';
        m_addIgnoreCount[0] = '\0';
        m_addOneShot = false;
        m_showAddPopup = true;
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button(ICON_FA_MINUS " Remove"))
        ExecuteRemove(m_selectedRow);
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(m_entries.empty());
    if (ImGui::Button(ICON_FA_TRASH " Clear All")) {
        ClearAllByType("");
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // ----- Table -----
    // kAdjustableColumns makes ImGui persist this table's column widths, order and visibility
    // (keyed by the "##bptable" id); TableLayoutStore is what carries them across sessions.
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_SizingFixedFit | kAdjustableColumns;

    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = lineH + 2.0f;

    // Height: fit the rows we actually have, but never shrink below a few rows (an empty or
    // one-entry list still needs somewhere to drop the next breakpoint), and never grow so tall
    // that the Exceptions / KPrintF sections and the status footer below get pushed off-screen.
    // Beyond that the list scrolls *inside* the table rather than scrolling the whole window.
    const float hdrH = ImGui::GetFrameHeight(); // the table's own header row
    const float minTableH = hdrH + rowH * kMinTableRows;
    const float wantTableH = hdrH +
                             rowH * std::max(kMinTableRows, static_cast<float>(m_entries.size())) +
                             ImGui::GetStyle().CellPadding.y * 2.0f;
    // What has to survive underneath: two collapsed section headers + the status footer
    // (a separator plus one text line).
    const float reservedH = 2.0f * ImGui::GetFrameHeightWithSpacing() +
                            ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    const float roomH = ImGui::GetContentRegionAvail().y - reservedH;
    const float tableH = std::min(wantTableH, std::max(minTableH, roomH));

    PushFontRole(FontRole::Code);
    if (ImGui::BeginTable("##bptable", 5, tableFlags, ImVec2(0.0f, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Empty label on the status column removes the header text and lets the
        // column be as narrow as the fixed width allows. It's the enable/disable toggle, so keep
        // it pinned (not hideable / reorderable).
        ImGui::TableSetupColumn("",
                                ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide |
                                    ImGuiTableColumnFlags_NoReorder,
                                rowH);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        // Who set it, and the source line it sits on when the debug info can say.
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 60.0f);

        // --- Manual Header Row ---
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

        // Column 0 Header: All/None/Mixed toggle
        ImGui::TableSetColumnIndex(0);
        ImGui::TableHeader("");

        int enabledCount = 0;
        for (const auto& e : m_entries) {
            if (e.enabled)
                enabledCount++;
        }
        int totalCount = static_cast<int>(m_entries.size());

        if (ImGui::IsItemClicked()) {
            bool anyDisabled = (enabledCount < totalCount);
            // Logic:
            // All enabled (anyDisabled=false) -> Disable all (target=false)
            // All disabled (anyDisabled=true) -> Enable all (target=true)
            // Mixed (anyDisabled=true) -> Enable all (target=true)
            bool target = anyDisabled;
            if (totalCount > 0) {
                for (int i = 0; i < totalCount; ++i) {
                    if (m_entries[i].enabled != target)
                        ExecuteToggle(i);
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to Enable/Disable all breakpoints");
        }

        // Draw a decorative circle in the header to indicate state
        {
            ImVec2 headerPos = ImGui::GetCursorScreenPos();
            float cellW = ImGui::GetContentRegionAvail().x;
            ImVec2 center(headerPos.x + cellW * 0.5f,
                          headerPos.y - ImGui::GetTextLineHeightWithSpacing() * 0.5f);

            const ImU32 color = IM_COL32(200, 200, 200, 200);
            const float radius = rowH * 0.3f;

            if (totalCount > 0) {
                if (enabledCount == totalCount) {
                    // All enabled: fill
                    ImGui::GetWindowDrawList()->AddCircleFilled(center, radius, color);
                } else if (enabledCount == 0) {
                    // None enabled: hollow
                    ImGui::GetWindowDrawList()->AddCircle(center, radius, color, 0, 1.0f);
                } else {
                    // Mixed: hollow with small filled circle inside
                    ImGui::GetWindowDrawList()->AddCircle(center, radius, color, 0, 1.0f);
                    ImGui::GetWindowDrawList()->AddCircleFilled(center, radius * 0.5f, color);
                }
            } else {
                // No breakpoints: faint hollow circle
                ImGui::GetWindowDrawList()->AddCircle(center, radius, IM_COL32(200, 200, 200, 60),
                                                      0, 1.0f);
            }
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::TableHeader("Type");
        ImGui::TableSetColumnIndex(2);
        ImGui::TableHeader("Address");
        ImGui::TableSetColumnIndex(3);
        ImGui::TableHeader("Source");
        ImGui::TableSetColumnIndex(4);
        ImGui::TableHeader("Hits");

        const float circleR = rowH * 0.3f;
        const ImU32 colEnabled = IM_COL32(210, 50, 50, 255);
        const ImU32 colDisabled = IM_COL32(210, 50, 50, 180);

        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
            const auto& e = m_entries[static_cast<size_t>(i)];
            const bool rowSelected = (m_selectedRow == i);

            ImGui::TableNextRow();

            if (e.enabled && e.type == "PC" && e.address == currentPcStr) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(200, 150, 0, 100));
            }

            ImGui::PushID(i);

            // --- Column 0: status circle ---
            // Capture the column 0 cursor position BEFORE drawing the SpanAllColumns
            // Selectable so we can reposition back to column 0 for the InvisibleButton.
            ImGui::TableSetColumnIndex(0);
            const ImVec2 col0Pos = ImGui::GetCursorScreenPos();
            const float col0W = ImGui::GetContentRegionAvail().x;

            // Row-spanning selectable for row selection and background highlight.
            // AllowOverlap lets items drawn after it (the InvisibleButton) take priority.
            if (ImGui::Selectable("##rowsel", rowSelected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap,
                                  ImVec2(0, rowH)))
                m_selectedRow = i;

            // Right-click context menu on the row selectable
            if (ImGui::BeginPopupContextItem("##bp_ctx")) {
                m_selectedRow = i;
                if (ImGui::MenuItem(e.enabled ? "Disable" : "Enable"))
                    ExecuteToggle(i);
                if (ImGui::MenuItem("Remove"))
                    ExecuteRemove(i);
                if (ImGui::MenuItem("Reset Hits")) {
                    m_hitCounts.erase(e.address);
                    m_lastHit.erase(e.address);
                }
                ImGui::EndPopup();
            }

            // Reposition to column 0 so the InvisibleButton is drawn there.
            // Using SetCursorScreenPos (not SameLine) because SameLine after
            // SpanAllColumns places the cursor at the end of the last column.
            ImGui::SetCursorScreenPos(col0Pos);
            const ImVec2 btnSize(col0W, rowH);
            const ImVec2 center(col0Pos.x + col0W * 0.5f, col0Pos.y + rowH * 0.5f);

            if (ImGui::InvisibleButton("##toggle", btnSize))
                ExecuteToggle(i);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                ExecuteRemove(i);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(e.enabled ? "Enabled" : "Disabled");

            // Draw the circle (decorative, on top of everything)
            if (e.enabled)
                ImGui::GetWindowDrawList()->AddCircleFilled(center, circleR, colEnabled);
            else
                ImGui::GetWindowDrawList()->AddCircle(center, circleR, colDisabled, 0, 1.5f);

            // --- Column 1: type (with size suffix for watchpoints: .b/.w/.l) ---
            ImGui::TableSetColumnIndex(1);
            if (e.type == "PC") {
                // Suffix flags: ? = condition, N = ignore-count, 1 = one-shot.
                char suffix[8] = {};
                int sp = 0;
                if (e.condEnabled)
                    suffix[sp++] = '?';
                if (e.ignoreCount > 0)
                    suffix[sp++] = 'N';
                if (e.oneShot)
                    suffix[sp++] = '1';
                if (sp > 0)
                    ImGui::Text("PC %s", suffix);
                else
                    ImGui::TextUnformatted("PC");
                if (e.hasHostMeta() && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    if (e.condEnabled)
                        ImGui::Text("Condition: (%s & $%X) %s $%X", e.condReg.c_str(), e.condMask,
                                    e.condOper.c_str(), e.condValue);
                    if (e.ignoreCount > 0)
                        ImGui::Text("Ignore first %d hit%s", e.ignoreCount,
                                    e.ignoreCount == 1 ? "" : "s");
                    if (e.oneShot)
                        ImGui::TextUnformatted("One-shot (removed after first hit)");
                    ImGui::EndTooltip();
                }
            } else {
                // Byte/word/long keep their familiar suffix; a wider range shows its length
                // in hex instead, since ".w" would be a lie for a 0x300-byte watch.
                char szc[16];
                if (e.size == 1)
                    snprintf(szc, sizeof(szc), "b");
                else if (e.size == 2)
                    snprintf(szc, sizeof(szc), "w");
                else if (e.size == 4)
                    snprintf(szc, sizeof(szc), "l");
                else
                    snprintf(szc, sizeof(szc), "$%X", e.size);
                const bool cpuSrc = (e.source.empty() || e.source == "CPU");
                // Append a '*' for non-CPU sources so async watchpoints stand out (their
                // break PC is the unrelated CPU instruction, not the accessing agent), '='
                // when a value condition is attached, and '~' for log mode (never stops).
                ImGui::Text("%s.%s%s%s%s", e.type.c_str(), szc, cpuSrc ? "" : "*",
                            e.valueEnabled ? "=" : "", e.logOnly ? "~" : "");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s watchpoint, %d byte%s, source %s", e.type.c_str(), e.size,
                                e.size == 1 ? "" : "s", cpuSrc ? "CPU" : e.source.c_str());
                    if (auto a = ParseHexAddress(e.address))
                        ImGui::Text("Range:   [$%08X, $%08X)", *a, *a + e.size);
                    if (e.valueEnabled)
                        ImGui::Text("Only when value = $%X", e.value);
                    if (e.logOnly) {
                        ImGui::Separator();
                        ImGui::TextUnformatted("Log mode: records hits and keeps running.");
                        if (e.maxHits > 0)
                            ImGui::Text("Disarms after %u hits.", e.maxHits);
                    }
                    ImGui::EndTooltip();
                }
            }

            // --- Column 2: address (clickable hyperlink, with register name if symbolic) ---
            ImGui::TableSetColumnIndex(2);
            if (m_addrCtx && m_addrCtx->valid()) {
                if (auto a = ParseHexAddress(e.address))
                    DrawAddressLink("bp", *a, "$%06X", *m_addrCtx);
                else
                    ImGui::TextUnformatted(e.address.c_str());
            } else {
                ImGui::TextUnformatted(e.address.c_str());
            }
            const std::string regName =
                !e.regName.empty() ? e.regName : (e.type != "PC" ? [&] {
                    if (auto a = ParseHexAddress(e.address))
                        return WatchRegisterNameAt(*a);
                    return std::string{};
                }()
                                                                 : std::string{});
            if (!regName.empty()) {
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextDisabled("%s", regName.c_str());
            }

            // --- Column 3: origin + source line ---
            //
            // GDB inserts its breakpoints on every resume and removes them on every stop, so
            // rows appear here that were never set in Peekaboo. Labelling them is what stops
            // that looking like the breakpoint list corrupting itself.
            ImGui::TableSetColumnIndex(3);
            {
                const auto originIt = m_origins.find(e.address);
                const bool fromGdb = originIt != m_origins.end() && originIt->second == kOriginGdb;
                if (fromGdb) {
                    // Same blue the debugger uses for hyperlinks: this row came from outside.
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.70f, 1.00f, 1.0f));
                    ImGui::TextUnformatted("GDB");
                    ImGui::PopStyleColor();
                } else if (originIt != m_origins.end()) {
                    ImGui::TextUnformatted(originIt->second.c_str());
                } else {
                    ImGui::TextDisabled("user");
                }

                // The file:line is resolved live rather than remembered, so it stays right after
                // a reload puts the same program at a different address.
                std::string file;
                uint32_t srcLine = 0;
                if (e.type == "PC" && m_sourceResolver) {
                    if (auto a = ParseHexAddress(e.address)) {
                        if (m_sourceResolver(*a, file, srcLine) && srcLine > 0) {
                            const size_t slash = file.find_last_of("/\\");
                            const std::string base =
                                slash == std::string::npos ? file : file.substr(slash + 1);
                            ImGui::SameLine(0.0f, 6.0f);
                            ImGui::TextDisabled("%s:%u", base.c_str(), srcLine);
                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::Text("%s:%u", file.c_str(), srcLine);
                                if (fromGdb)
                                    ImGui::TextDisabled(
                                        "Set by GDB (VS Code). Removed and reinserted on every "
                                        "stop and resume, so it may vanish while halted.");
                                ImGui::EndTooltip();
                            }
                        }
                    }
                }
            }

            // --- Column 4: hits ---
            ImGui::TableSetColumnIndex(4);
            auto hitsIt = m_hitCounts.find(e.address);
            // A log-mode watchpoint never pauses, so it has no break count — show the
            // number of accesses it has recorded instead.
            uint64_t hits =
                e.logOnly ? e.logHits : (hitsIt != m_hitCounts.end() ? hitsIt->second : 0);
            if (hits > 0)
                ImGui::Text("%llu%s", (unsigned long long)hits, e.logOnly ? " logged" : "");
            else
                ImGui::TextDisabled("0");

            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                m_hitCounts.erase(e.address);
                m_lastHit.erase(e.address);
            }
            if (ImGui::IsItemHovered() && hits > 0) {
                ImGui::BeginTooltip();
                auto hi = m_lastHit.find(e.address);
                if (hi != m_lastHit.end()) {
                    const HitInfo& h = hi->second;
                    if (h.pcBreak) {
                        ImGui::Text("Last hit: stopped at PC $%08X", h.pc);
                    } else {
                        const char* dir = (h.rwi & 2) ? "write" : ((h.rwi & 1) ? "read" : "access");
                        std::string accReg = WatchRegisterNameAt(h.accessAddr);
                        ImGui::Text("Last hit: %s of $%08X%s%s", dir, h.accessAddr,
                                    accReg.empty() ? "" : " ", accReg.c_str());
                        if (h.rwi & 2)
                            ImGui::Text("Value:   $%X (%d-byte)", h.value, h.size);
                        if (!h.source.empty())
                            ImGui::Text("Source:  %s", h.source.c_str());
                        ImGui::Text("By PC:   $%08X", h.pc);
                    }
                    ImGui::Separator();
                }
                ImGui::TextDisabled("Right-click to reset");
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
    PopFontRole();
}

void BreakpointsView::DrawAddPopup()
{
    // ----- Add popup -----
    if (m_showAddPopup)
        ImGui::OpenPopup("Add Breakpoint##bp_add");

    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Add Breakpoint##bp_add", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        static const char* kTypes[] = {"PC (execution)", "Read watchpoint", "Write watchpoint",
                                       "Read+Write watchpoint", "Exec watchpoint (opcode fetch)"};
        ImGui::Text("Type:");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::Combo("##bp_type", &m_addTypeIndex, kTypes, 5);
        if (m_addTypeIndex == 4 && ImGui::IsItemHovered())
            ImGui::SetTooltip("Fires when the CPU fetches an instruction from the watched\n"
                              "range — use it to catch the moment control enters a segment.");

        // Size/source/value/register apply to watchpoints only. A PC breakpoint just
        // matches the instruction address.
        const bool isWatchpoint = (m_addTypeIndex != 0);
        static const int kSizeBytes[] = {1, 2, 4};

        // --- Target: a named hardware register, or a raw address ---
        const WatchRegister* selReg = nullptr;
        if (isWatchpoint) {
            ImGui::Checkbox("Named register", &m_addUseRegister);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pick a Custom/CIA register by name instead of typing an "
                                  "address (sets address + width automatically).");
        }

        bool enterPressed = false;
        if (isWatchpoint && m_addUseRegister) {
            const auto& groups = WatchRegisterGroups();
            if (m_addRegGroup >= static_cast<int>(groups.size()))
                m_addRegGroup = 0;
            ImGui::Text("Group:");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##bp_reggroup", groups[m_addRegGroup].label.c_str())) {
                for (int g = 0; g < static_cast<int>(groups.size()); ++g)
                    if (ImGui::Selectable(groups[g].label.c_str(), g == m_addRegGroup)) {
                        m_addRegGroup = g;
                        m_addRegIndex = 0;
                    }
                ImGui::EndCombo();
            }
            const auto& regs = groups[m_addRegGroup].regs;
            if (m_addRegIndex >= static_cast<int>(regs.size()))
                m_addRegIndex = 0;
            ImGui::Text("Register:");
            ImGui::SetNextItemWidth(-1.0f);
            const char* preview = regs.empty() ? "" : regs[m_addRegIndex].name.c_str();
            if (ImGui::BeginCombo("##bp_reg", preview)) {
                for (int r = 0; r < static_cast<int>(regs.size()); ++r) {
                    char lbl[48];
                    snprintf(lbl, sizeof(lbl), "%-8s $%06X", regs[r].name.c_str(), regs[r].address);
                    if (ImGui::Selectable(lbl, r == m_addRegIndex))
                        m_addRegIndex = r;
                }
                ImGui::EndCombo();
            }
            if (!regs.empty())
                selReg = &regs[m_addRegIndex];
        } else if (isWatchpoint) {
            ImGui::Text("Size:");
            ImGui::SetNextItemWidth(-1.0f);
            static const char* kSizes[] = {"Byte (1)", "Word (2)", "Long (4)", "Range (hex)"};
            ImGui::Combo("##bp_size", &m_addSizeIndex, kSizes, 4);
            if (m_addSizeIndex == 3) {
                ImGui::Text("Length:");
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##bp_len", m_addLength, sizeof(m_addLength),
                                 ImGuiInputTextFlags_CharsHexadecimal);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Watched region length in bytes (hex). One slot covers\n"
                                      "the whole range: $300 watches 768 bytes.");
            }
        }

        if (isWatchpoint) {
            ImGui::Text("Source:");
            ImGui::SetNextItemWidth(-1.0f);
            static const char* kSources[] = {"CPU code", "Copper", "Blitter", "DMA (all)",
                                             "Any source"};
            ImGui::Combo("##bp_source", &m_addSourceIndex, kSources, 5);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Which agent's accesses trigger the watchpoint.\n"
                                  "CPU code is the usual choice and gives a meaningful break PC.\n"
                                  "Copper/Blitter/DMA/Any catch async hardware accesses, but the\n"
                                  "break then stops at an unrelated CPU instruction.");

            // Value condition: break only when the written value matches.
            ImGui::Checkbox("Only when value =", &m_addValueEnabled);
            ImGui::SameLine();
            ImGui::BeginDisabled(!m_addValueEnabled);
            ImGui::SetNextItemWidth(90.0f);
            ImGui::InputText("##bp_value", m_addValue, sizeof(m_addValue),
                             ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Break only when the access value (masked to the watch width)\n"
                                  "equals this hex value, e.g. a DMACON write that sets a bit.");

            // Log mode: record hits and keep running. Essential for wide ranges — a read
            // watchpoint over a few hundred bytes fires constantly, so breaking is useless.
            ImGui::Checkbox("Log hits, don't break", &m_addLogOnly);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Record every access and keep running, instead of stopping.\n"
                                  "Read the recorded hits back with the get_watchpoint_log MCP\n"
                                  "tool. This is what makes a wide read watchpoint usable.");
            if (m_addLogOnly) {
                ImGui::SameLine();
                ImGui::TextUnformatted("max");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.0f);
                ImGui::InputText("##bp_maxhits", m_addMaxHits, sizeof(m_addMaxHits),
                                 ImGuiInputTextFlags_CharsDecimal);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Disarm after this many recorded hits (blank = unlimited).");
            }
        }

        // Condition register / operator token tables (shared by the controls and the Add handler).
        static const char* kCondRegs[] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6",
                                          "D7", "A0", "A1", "A2", "A3", "A4", "A5",
                                          "A6", "A7", "PC", "SR", "USP"};
        static const char* kCondOpers[] = {"==", "!=", "<", ">", "<=", ">="};

        // --- Smart controls (PC breakpoints only): register condition, ignore-count, one-shot ---
        if (!isWatchpoint) {
            ImGui::Checkbox("Condition", &m_addCondEnabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Break only when a CPU register compares true at this PC.\n"
                                  "Evaluated in-core every instruction (no slowdown).");
            if (m_addCondEnabled) {
                ImGui::Indent();
                ImGui::SetNextItemWidth(60.0f);
                ImGui::Combo("##cond_reg", &m_addCondRegIndex, kCondRegs, IM_ARRAYSIZE(kCondRegs));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(48.0f);
                ImGui::Combo("##cond_oper", &m_addCondOperIndex, kCondOpers,
                             IM_ARRAYSIZE(kCondOpers));
                ImGui::SameLine();
                ImGui::TextUnformatted("$");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::SetNextItemWidth(80.0f);
                ImGui::InputText("##cond_val", m_addCondValue, sizeof(m_addCondValue),
                                 ImGuiInputTextFlags_CharsHexadecimal);
                ImGui::SameLine();
                ImGui::TextUnformatted("& $");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::SetNextItemWidth(80.0f);
                ImGui::InputText("##cond_mask", m_addCondMask, sizeof(m_addCondMask),
                                 ImGuiInputTextFlags_CharsHexadecimal);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "AND-mask applied before the compare: (reg & mask) %s value.\n"
                        "Leave blank for no mask (FFFFFFFF).",
                        kCondOpers[m_addCondOperIndex]);
                ImGui::Unindent();
            }

            ImGui::TextUnformatted("Ignore count:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            ImGui::InputText("##ignore", m_addIgnoreCount, sizeof(m_addIgnoreCount),
                             ImGuiInputTextFlags_CharsDecimal);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Skip this many hits before breaking (0 = break immediately).");

            ImGui::Checkbox("One-shot (remove after first hit)", &m_addOneShot);
        }

        // Address field (raw-address mode, or PC breakpoints).
        const bool manualAddr = !(isWatchpoint && m_addUseRegister);
        if (manualAddr) {
            ImGui::Text("Address (hex):");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            enterPressed = ImGui::InputText("##bp_addr", m_addAddress, sizeof(m_addAddress),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        }

        // Resolve the effective target + width.
        std::string addrStr;
        int sizeBytes;
        std::string regName;
        if (selReg) {
            addrStr = FormatAddr8(selReg->address);
            sizeBytes = selReg->size;
            regName = selReg->name;
        } else {
            addrStr = m_addAddress;
            if (!isWatchpoint) {
                sizeBytes = 2;
            } else if (m_addSizeIndex == 3) {
                // Custom range: an unparseable or empty length falls back to a word so the
                // Add button stays usable while the field is being typed.
                auto len = ParseHexAddress(m_addLength);
                sizeBytes = (len && *len > 0) ? static_cast<int>(*len) : 2;
            } else {
                sizeBytes = kSizeBytes[m_addSizeIndex];
            }
        }
        const std::string normAddr = NormalizeAddress(addrStr);
        const bool addrOk = !normAddr.empty();

        // Value validity (only gates Add when the condition is enabled).
        uint32_t valNum = 0;
        bool valOk = true;
        if (isWatchpoint && m_addValueEnabled) {
            if (auto v = ParseHexAddress(m_addValue))
                valNum = *v;
            else
                valOk = false;
        }

        // Smart PC condition validity + parsed ignore-count.
        uint32_t condValNum = 0;
        uint32_t condMaskNum = 0xFFFFFFFFu;
        bool condOk = true;
        if (!isWatchpoint && m_addCondEnabled) {
            if (auto v = ParseHexAddress(m_addCondValue))
                condValNum = *v;
            else
                condOk = false;
            if (m_addCondMask[0] != '\0') {
                if (auto m = ParseHexAddress(m_addCondMask))
                    condMaskNum = *m;
                else
                    condOk = false;
            }
        }
        int ignoreNum = 0;
        if (!isWatchpoint && m_addIgnoreCount[0] != '\0')
            ignoreNum = std::atoi(m_addIgnoreCount);

        // Size hint — the trap commonly missed by 2 bytes on a copper MOVE.
        if (isWatchpoint && addrOk) {
            if (auto a = ParseHexAddress(normAddr)) {
                ImGui::TextDisabled("Breaks on access to [$%08X, $%08X)", *a, *a + sizeBytes);
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_showAddPopup = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::Spacing();
        ImGui::BeginDisabled(!addrOk || !valOk || !condOk);
        if (ImGui::Button("Add", ImVec2(80, 0)) || (enterPressed && addrOk && valOk && condOk)) {
            if (m_addTypeIndex == 0) {
                const bool smart = m_addCondEnabled || ignoreNum > 0 || m_addOneShot;
                if (smart) {
                    if (auto a = ParseHexAddress(normAddr))
                        AddSmartBreakpointAt(*a, m_addCondEnabled, kCondRegs[m_addCondRegIndex],
                                             kCondOpers[m_addCondOperIndex], condValNum,
                                             condMaskNum, ignoreNum, m_addOneShot);
                } else {
                    ExecuteAdd(addrStr, "PC");
                }
            } else {
                static const char* kTypeNames[] = {"PC", "Read", "Write", "R+W", "Exec"};
                static const char* kSourceTokens[] = {"CPU", "COP", "BLT", "DMA", "ALL"};
                if (m_addLogOnly) {
                    // Log mode carries host-side state the plain ExecuteAdd path does not,
                    // so build the spec and go through the full watchpoint API.
                    core::WatchpointSpec spec;
                    if (auto a = ParseHexAddress(normAddr))
                        spec.addr = *a;
                    spec.len = static_cast<uint32_t>(sizeBytes > 0 ? sizeBytes : 2);
                    TypeToAccess(kTypeNames[m_addTypeIndex], spec.read, spec.write, spec.exec);
                    spec.source = kSourceTokens[m_addSourceIndex];
                    spec.valueEnabled = m_addValueEnabled;
                    spec.value = valNum;
                    spec.logOnly = true;
                    spec.maxHits = m_addMaxHits[0] != '\0'
                                       ? static_cast<uint32_t>(std::atoi(m_addMaxHits))
                                       : 0;
                    std::string err;
                    AddWatchpoint(spec, err);
                } else {
                    ExecuteAdd(addrStr, kTypeNames[m_addTypeIndex], sizeBytes,
                               kSourceTokens[m_addSourceIndex], m_addValueEnabled, valNum, regName);
                }
            }
            m_showAddPopup = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            m_showAddPopup = false;
            ImGui::CloseCurrentPopup();
        }

        if (manualAddr && !addrOk && m_addAddress[0] != '\0')
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Invalid hex address");
        else if (isWatchpoint && m_addValueEnabled && !valOk)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Invalid hex value");

        ImGui::EndPopup();
    }
}

void BreakpointsView::DrawExceptionAndBeamSection()
{
    const bool emuRunning = m_emulatorView && m_emulatorView->IsRunning();
    auto* emu = emuRunning ? m_emulatorView->GetEmulator() : nullptr;
    const bool excAvail = emu && emu->HasExceptionBreak();
    const bool beamAvail = emu && emu->HasBeamBreak();

    if (ImGui::CollapsingHeader("Exceptions / Raster")) {
        // --- CPU exception vectors ---
        struct Vec
        {
            const char* label;
            int vec;
        };
        static const Vec kVecs[] = {
            {"Bus", 2},   {"Address", 3},   {"Illegal", 4}, {"ZeroDiv", 5}, {"CHK", 6},
            {"TRAPV", 7}, {"Privilege", 8}, {"Trace", 9},   {"Line-A", 10}, {"Line-F", 11},
        };
        ImGui::BeginDisabled(!excAvail);
        ImGui::TextUnformatted("Break on CPU exception:");
        uint64_t newMask = m_exceptionMask;
        int col = 0;
        for (const auto& v : kVecs) {
            bool on = (m_exceptionMask >> v.vec) & 1u;
            if (ImGui::Checkbox(v.label, &on)) {
                if (on)
                    newMask |= (1ull << v.vec);
                else
                    newMask &= ~(1ull << v.vec);
            }
            if (++col % 4 != 0)
                ImGui::SameLine();
        }
        if (col % 4 != 0)
            ImGui::NewLine();
        if (newMask != m_exceptionMask)
            SetExceptionMask(newMask);
        ImGui::EndDisabled();
        if (!excAvail)
            ImGui::TextDisabled("(needs a rebuilt core)");

        ImGui::Separator();

        // --- Raster (beam) break ---
        ImGui::BeginDisabled(!beamAvail);
        bool beamOn = m_beamVpos >= 0;
        if (ImGui::Checkbox("Break at scanline", &beamOn)) {
            if (beamOn)
                SetBeamBreak(m_beamVpos >= 0 ? m_beamVpos : 0, m_beamHpos);
            else
                SetBeamBreak(-1, -1);
        }
        if (beamOn) {
            ImGui::SameLine();
            int vp = m_beamVpos < 0 ? 0 : m_beamVpos;
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::InputInt("##beamline", &vp)) {
                if (vp < 0)
                    vp = 0;
                if (vp > 624)
                    vp = 624;
                SetBeamBreak(vp, m_beamHpos);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("clock");
            ImGui::SameLine();
            // -1 means "anywhere on the line". The raster overlay sets an exact colour clock, so
            // it has to be visible and editable here or the armed breakpoint would be misreported.
            int hp = m_beamHpos;
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::InputInt("##beamclock", &hp)) {
                if (hp < -1)
                    hp = -1;
                if (hp > 227)
                    hp = 227;
                SetBeamBreak(m_beamVpos, hp);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Colour clock within the scanline (0-227). "
                                  "-1 breaks anywhere on the line.");
            ImGui::SameLine();
            ImGui::TextDisabled("(one-shot)");
        }
        ImGui::EndDisabled();
        if (!beamAvail)
            ImGui::TextDisabled("(needs a rebuilt core)");
    }
}

core::KprintfBreakpoints* BreakpointsView::GetKprintfBreakpoints() const
{
    // The store lives in AmigaEmulator, which EmulatorView owns for the whole app lifetime — so
    // rules can be added (and survive) with no emulation running.
    if (!m_emulatorView)
        return nullptr;
    auto* emu = m_emulatorView->GetEmulator();
    return emu ? &emu->KprintfBreakpoints() : nullptr;
}

void BreakpointsView::DrawKprintfSection()
{
    auto* store = GetKprintfBreakpoints();

    if (!ImGui::CollapsingHeader("KPrintF text"))
        return;

    if (!store) {
        ImGui::TextDisabled("(no emulator instance)");
        return;
    }

    ImGui::TextDisabled("Break when the program's serial debug output (KPrintF) matches:");

    static const char* kMatchModes[] = {"Contains", "Exact", "Starts with", "Regex"};
    static const core::KprintfMatch kMatchValues[] = {
        core::KprintfMatch::Contains, core::KprintfMatch::Exact, core::KprintfMatch::Prefix,
        core::KprintfMatch::Regex};

    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##kp_mode", &m_kprintfMatchIndex, kMatchModes, IM_ARRAYSIZE(kMatchModes));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-90.0f);
    const bool entered =
        ImGui::InputTextWithHint("##kp_text", "text to match", m_kprintfText, sizeof(m_kprintfText),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool canAdd = m_kprintfText[0] != '\0';
    ImGui::BeginDisabled(!canAdd);
    const bool addClicked = ImGui::Button(ICON_FA_PLUS " Add##kp");
    ImGui::EndDisabled();

    ImGui::Checkbox("Match case", &m_kprintfCaseSensitive);
    ImGui::SameLine();
    ImGui::Checkbox("One-shot##kp", &m_kprintfOneShot);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Remove the trap once it has fired.");

    if ((addClicked || (entered && canAdd)) && canAdd) {
        const int id = store->Add(m_kprintfText, kMatchValues[m_kprintfMatchIndex],
                                  m_kprintfCaseSensitive, m_kprintfOneShot);
        m_kprintfAddFailed = (id == 0);
        if (id != 0)
            m_kprintfText[0] = '\0';
    }
    if (m_kprintfAddFailed) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), m_kprintfMatchIndex == 3
                                                               ? "Duplicate rule or invalid regex"
                                                               : "That rule already exists");
    }

    const auto& entries = store->Entries();
    if (entries.empty()) {
        ImGui::TextDisabled("No KPrintF traps set.");
        return;
    }

    constexpr ImGuiTableFlags kpFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingFixedFit | kAdjustableColumns;
    PushFontRole(FontRole::Code);
    if (ImGui::BeginTable("##kptable", 5, kpFlags)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 26.0f);
        ImGui::TableSetupColumn("Match", ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide,
                                26.0f);
        ImGui::TableHeadersRow();

        // Mutations are deferred so the table iteration isn't invalidated mid-draw.
        int toggleId = 0, removeId = 0;
        bool toggleTo = false;
        for (const auto& e : entries) {
            ImGui::TableNextRow();
            ImGui::PushID(e.id);

            ImGui::TableSetColumnIndex(0);
            bool on = e.enabled;
            if (ImGui::Checkbox("##kp_en", &on)) {
                toggleId = e.id;
                toggleTo = on;
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(core::KprintfBreakpoints::MatchName(e.match));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "%s match, case %s%s", core::KprintfBreakpoints::MatchName(e.match),
                    e.caseSensitive ? "sensitive" : "insensitive", e.oneShot ? ", one-shot" : "");
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(e.text.c_str());
            if (!e.lastLine.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("Last match: %s", e.lastLine.c_str());

            ImGui::TableSetColumnIndex(3);
            if (e.hits > 0)
                ImGui::Text("%llu", (unsigned long long)e.hits);
            else
                ImGui::TextDisabled("0");

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton(ICON_FA_XMARK))
                removeId = e.id;

            ImGui::PopID();
        }
        ImGui::EndTable();

        if (toggleId != 0)
            store->SetEnabled(toggleId, toggleTo);
        if (removeId != 0)
            store->Remove(removeId);
    }
    PopFontRole();

    if (ImGui::Button(ICON_FA_TRASH " Clear##kp"))
        store->Clear();
    ImGui::SameLine();
    if (ImGui::Button("Reset hits##kp"))
        store->ResetHits();

    // Most recent match across all rules — this is the line that stopped execution.
    if (m_emulatorView) {
        if (auto* emu = m_emulatorView->GetEmulator()) {
            core::KprintfHit hit;
            if (emu->GetLastKprintfHit(hit)) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.35f, 1.0f), "Last match: %s",
                                   hit.line.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Matched rule: \"%s\" (%s)", hit.text.c_str(),
                                      core::KprintfBreakpoints::MatchName(hit.match));
            }
        }
    }
}

void BreakpointsView::DrawLastHitStatus()
{
    ImGui::Separator();

    auto it = m_lastHit.find(m_lastHitAddr);
    if (m_lastHitAddr.empty() || it == m_lastHit.end()) {
        // Say why there is nothing to report rather than a bare "no hit yet", which reads like a
        // fault when the real answer is "nothing is armed" or "it hasn't run into one".
        size_t armed = 0;
        for (const auto& e : m_entries)
            if (e.enabled)
                ++armed;
        if (m_entries.empty())
            ImGui::TextDisabled("No breakpoints set");
        else if (armed == 0)
            ImGui::TextDisabled("%zu breakpoint%s, none enabled", m_entries.size(),
                                m_entries.size() == 1 ? "" : "s");
        else
            ImGui::TextDisabled("%zu breakpoint%s armed - none hit yet", armed,
                                armed == 1 ? "" : "s");
        return;
    }
    const HitInfo& h = it->second;

    // A coloured dot keyed to the source makes the agent scannable at a glance.
    const bool cpu = h.source.empty() || h.source.rfind("CPU", 0) == 0;
    ImU32 dot = cpu ? IM_COL32(120, 200, 120, 255) : IM_COL32(230, 170, 80, 255);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = ImGui::GetTextLineHeight() * 0.3f;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        {p.x + r + 1.0f, p.y + ImGui::GetTextLineHeight() * 0.5f}, r, dot);
    ImGui::Dummy(ImVec2(r * 2.0f + 6.0f, 0));
    ImGui::SameLine();

    // Hit count, when the same breakpoint has fired more than once. Repeats are the useful
    // signal in a loop, and the count is otherwise only visible in the table's Hits column.
    char times[32] = {};
    if (h.hitCount > 1)
        snprintf(times, sizeof(times), "  x%llu", (unsigned long long)h.hitCount);

    if (h.pcBreak) {
        std::string file;
        uint32_t line = 0;
        if (m_sourceResolver && m_sourceResolver(h.pc, file, line) && !file.empty())
            ImGui::Text("Last hit: PC $%08X  (%s:%u)%s", h.pc, file.c_str(), line, times);
        else
            ImGui::Text("Last hit: PC $%08X%s", h.pc, times);
        return;
    }

    // Resolve a friendly name for the hit address: register (e.g. COLOR00) else raw hex.
    std::string regName = WatchRegisterNameAt(h.accessAddr);
    char addrLabel[40];
    if (!regName.empty())
        snprintf(addrLabel, sizeof(addrLabel), "%s ($%08X)", regName.c_str(), h.accessAddr);
    else
        snprintf(addrLabel, sizeof(addrLabel), "$%08X", h.accessAddr);

    const char* src = h.source.empty() ? "CPU" : h.source.c_str();
    const char* verb = (h.rwi & 2) ? "wrote" : ((h.rwi & 1) ? "read" : "hit");

    if (h.rwi & 2)
        ImGui::Text("Last hit: %s %s %s $%X  (PC $%08X)%s", src, verb, addrLabel, h.value, h.pc,
                    times);
    else
        ImGui::Text("Last hit: %s %s %s  (PC $%08X)%s", src, verb, addrLabel, h.pc, times);
}

} // namespace adm::ui
