#include "McpToolHandler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <fmt/format.h>

#include "AmigaCustom.h"
#include "AmigaEmulator.h"
#include "ArMemoryAccess.h"
#include "BitplaneDecoder.h"
#include "Constants.h"
#include "CopperDisassembler.h"
#include "DisplayMasks.h"
#include "EmulatorStateOps.h"
#include "EmulatorStateStore.h"
#include "ExocetPack.h"
#include "ExocetQuery.h"
#include "ExocetSession.h"
#include "HexUtil.h"
#include "Logger.h"
#include "McpConsoleTools.h"
#include "McpReTools.h"
#include "McpServer.h"
#include "McpUiTools.h"
#include "MemoryAnnotations.h"
#include "PrivateFeatures.h"
#include "RetroKeyNames.h"

namespace adm::core {

using nlohmann::json;

namespace {

std::string ToHex(uint32_t v, int width) { return fmt::format("{:0{}X}", v, width); }

} // namespace

json BuildExtendedCpuRegistersJson(const CpuExtendedState& ext, const std::string& group)
{
    json j;
    j["cpu"] = {{"model", ext.cpuModel},
                {"fpu", FpuName(ext.fpuModel, ext.cpuModel)},
                {"mmu", CpuMmuName(ext)}};
    j["activeStack"] = ActiveStackName(ext);
    j["intmask"] = ext.intmask;
    j["stopped"] = ext.stopped;

    json unsupported = json::array();
    auto wanted = [&](CpuRegGroup g) {
        if (group == "all")
            return true;
        if (group == "control")
            return g == CpuRegGroup::Stack || g == CpuRegGroup::System;
        return (group == "mmu" && g == CpuRegGroup::Mmu) ||
               (group == "fpu" && g == CpuRegGroup::Fpu);
    };
    auto key = [](CpuRegGroup g) {
        switch (g) {
            case CpuRegGroup::Stack:
                return "stack";
            case CpuRegGroup::System:
                return "control";
            case CpuRegGroup::Mmu:
                return "mmu";
            default:
                return "fpu";
        }
    };
    for (const auto& def : CpuRegisterCatalog()) {
        if (!wanted(def.group))
            continue;
        const char* name = def.id == CpuRegId::Isp ? IspName(ext.cpuModel) : def.name;
        auto reading = ReadCpuRegister(ext, def);
        if (!reading) {
            unsupported.push_back({{"name", name}, {"supportedOn", SupportedOnText(def)}});
            continue;
        }
        json value;
        if (reading->fp) {
            value = {{"hex", FormatFpRaw(*reading->fp)}, {"value", FormatFpValue(*reading->fp)}};
        } else if (def.id == CpuRegId::Fpcr) {
            value = {{"hex", FormatCpuRegister(*reading)}, {"decoded", DecodeFpcr(ext.fpcr)}};
        } else if (def.id == CpuRegId::Fpsr) {
            value = {{"hex", FormatCpuRegister(*reading)}, {"decoded", DecodeFpsr(ext.fpsr)}};
        } else {
            value = FormatCpuRegister(*reading);
        }
        j[key(def.group)][name] = value;
    }
    if (!unsupported.empty())
        j["unsupported"] = unsupported;
    return j;
}

namespace {

// How long run_until_frame(wait: true) will hold its caller. Kept under the HTTP request
// timeout so the wait ends with a real answer rather than the transport giving up on it.
constexpr int kFrameWaitBudgetSeconds = McpServer::kRequestTimeoutSeconds - 2;

// Accepts a JSON number, or a string parsed as hex ("0x1234", "$1234" or bare
// "1234" — bare strings are treated as hex to match the debugger's habit).
bool ParseAddr(const json& j, uint32_t& out)
{
    if (j.is_number_integer() || j.is_number_unsigned()) {
        out = j.get<uint32_t>();
        return true;
    }
    if (j.is_string()) {
        if (auto v = ParseHexAddress(j.get<std::string>())) {
            out = *v;
            return true;
        }
    }
    return false;
}

bool GetAddrArg(const json& args, const char* key, uint32_t& out)
{
    return args.contains(key) && ParseAddr(args[key], out);
}

json Err(const std::string& msg) { return json{{"error", msg}}; }

// "addr required" for a genuinely absent key, but something that names the offending text when
// the key *was* supplied and simply didn't parse — a malformed address used to be reported as a
// missing one, which sends the caller re-reading the schema for a field they already passed.
json AddrArgError(const json& args, const char* key)
{
    if (!args.contains(key) || args[key].is_null())
        return Err(fmt::format("{} required", key));
    return Err(fmt::format("could not parse {} {} — expected an integer or a hex string like "
                           "\"0x1234\" / \"$1234\"",
                           key, args[key].dump()));
}

// True when an optional addr-style argument was supplied but is unusable, so a typo in one is
// reported rather than silently ignored.
bool BadOptionalAddr(const json& args, const char* key, uint32_t& out)
{
    return args.contains(key) && !args[key].is_null() && !ParseAddr(args[key], out);
}

// Where the raster beam stands right now, as reported by the core. Every sampled-state tool
// carries this: a read of the custom registers, the blitter log or the display only means
// something once you know how far into the field it was taken.
json BeamJson(const adm::AmigaEmulator* emu)
{
    int vpos = 0, hpos = 0;
    if (!emu || !emu->GetBeamPosition(vpos, hpos))
        return json{};
    return json{{"vpos", vpos}, {"hpos", hpos}};
}

// Beam-based "is this capture partial?", using the live DIWSTRT. See
// adm::core::BeamHasOverwrittenVisibleField for why the test is not "vpos != 0".
bool BeamHasOverwrittenTheVisibleField(const adm::AmigaEmulator* emu, int vpos)
{
    core::Custom custom{};
    if (!emu || !emu->GetCustomRegisters(custom))
        custom = core::Custom{}; // falls back to the default display window
    return core::BeamHasOverwrittenVisibleField(custom, vpos);
}

// Build an ArMemoryAccess view onto the live emulator memory for a single tool call.
void RefreshMem(ArMemoryAccess& mem, adm::AmigaEmulator* emu)
{
    std::vector<ArMemoryAccess::Region> regions;
    if (emu) {
        for (const auto& r : emu->GetMemoryRegions())
            regions.push_back({r.hostPtr, r.amigaAddr, r.size, r.name, r.writable});
    }
    mem.SetRegions(std::move(regions));
    mem.readCustomReg = [emu](uint32_t offset, uint16_t& out) {
        return emu && emu->ReadCustomRegisterWord(offset, out);
    };
    mem.writeCustomReg = [emu](uint32_t offset, uint16_t value) {
        return emu && emu->WriteCustomRegister(ArMemoryAccess::kCustomBase + offset, value);
    };
    // CIA registers come from the same side-effect-free snapshot get_cia_registers uses,
    // so read_memory over $BFDxxx/$BFExxx returns the port values (fire buttons, drive
    // status, the keyboard serial line) instead of "??".
    mem.readCiaReg = [emu](bool chipB, int reg, uint8_t& out) {
        return emu && emu->ReadCIARegisterByte(chipB, reg, out);
    };
    // Writes go straight to the chip (cia_bank.bput), unlike the snapshot the reads use.
    mem.writeCiaReg = [emu](bool chipB, int reg, uint8_t value) {
        if (!emu)
            return false;
        const uint32_t addr = chipB ? (0xBFD000u + static_cast<uint32_t>(reg) * 0x100u)
                                    : (0xBFE001u + static_cast<uint32_t>(reg) * 0x100u);
        return emu->WriteCIARegister(addr, value);
    };
}

// Serialize a profiler snapshot for the requested view. Summary fields are always present;
// the view-specific array is included when a trace is available.
json ProfileToJson(const ProfileSnapshot& snap, ProfileView view)
{
    json j;
    j["view"] = ProfileViewName(view);
    j["available"] = snap.available;
    j["haveSymbols"] = snap.haveSymbols;
    j["totalCycles"] = snap.totalCycles;
    j["resolvedCycles"] = snap.resolvedCycles;
    j["unresolvedCycles"] = snap.unresolvedCycles;
    // `available` describes the CPU *trace*, so it cannot gate the callstack view: its live
    // half is a DWARF unwind of the halted CPU and needs no trace at all. Every other view is
    // trace-derived and has nothing to say without one.
    if (!snap.available && !(view == ProfileView::CallStack && snap.liveAvailable)) {
        j["hint"] = "No CPU trace this frame. Enable tracing (Disassembly Trace/Profile) "
                    "and run a frame.";
        if (view != ProfileView::CallStack)
            return j;
        if (!snap.liveAvailable) {
            j["liveAvailable"] = false;
            j["hint"] = "No CPU trace, and no live unwind either: pause the machine and load "
                        "a program with a sibling .elf carrying DWARF call-frame info.";
            return j;
        }
    }
    switch (view) {
        case ProfileView::Instructions: {
            json arr = json::array();
            for (const auto& s : snap.instructions)
                arr.push_back({{"pc", ToHex(s.pc, 8)},
                               {"mnemonic", s.mnemonic},
                               {"count", s.count},
                               {"totalCycles", s.totalCycles},
                               {"minCycles", s.minCycles},
                               {"maxCycles", s.maxCycles},
                               {"avgCycles", s.AvgCycles()}});
            j["instructions"] = arr;
            break;
        }
        case ProfileView::Functions: {
            if (!snap.haveSymbols)
                j["hint"] = "No debug symbols (needs a sibling .elf). Use view=instructions "
                            "for per-PC hot spots.";
            json arr = json::array();
            for (const auto& f : snap.functions)
                arr.push_back({{"name", f.name},
                               {"address", ToHex(f.address, 8)},
                               {"totalCycles", f.totalCycles},
                               {"count", f.count},
                               {"pctFrame", f.pctFrame * 100.0f},
                               {"avgCycles", f.AvgCycles()}});
            j["functions"] = arr;
            break;
        }
        case ProfileView::Flat:
        case ProfileView::Flame: {
            json arr = json::array();
            for (const auto& fr : snap.frames)
                arr.push_back({{"depth", fr.depth},
                               {"startLRef", fr.startLRef},
                               {"endLRef", fr.endLRef},
                               {"address", ToHex(fr.funcAddress, 8)},
                               {"name", fr.name}});
            j["frames"] = arr;
            break;
        }
        case ProfileView::CallStack: {
            // Two independent stacks. `liveStack` is a real DWARF unwind of the halted CPU
            // and is the one to trust for "where am I now"; `callStack` is the frame
            // scrubber's trace reconstruction at the playhead.
            j["liveAvailable"] = snap.liveAvailable;
            if (snap.liveAvailable) {
                j["liveStop"] = snap.liveStop;
                json live = json::array();
                int depth = 0;
                for (const auto& f : snap.liveStack) {
                    json row = {{"depth", depth++},
                                {"pc", ToHex(f.pc, 8)},
                                {"name", f.name},
                                {"inlined", f.inlined}};
                    if (!f.inlined) {
                        row["sp"] = ToHex(f.sp, 8);
                        row["cfa"] = ToHex(f.cfa, 8);
                    }
                    if (!f.file.empty()) {
                        row["file"] = f.file;
                        row["line"] = f.line;
                    }
                    live.push_back(std::move(row));
                }
                j["liveStack"] = live;
            } else {
                j["hint"] = "Live unwind needs the machine paused and a sibling .elf with "
                            "DWARF call-frame info. Falling back to the traced stack.";
            }

            j["playheadLRef"] = snap.playheadLRef;
            json arr = json::array();
            for (const auto& f : snap.callStack)
                arr.push_back(
                    {{"depth", f.depth}, {"address", ToHex(f.funcAddress, 8)}, {"name", f.name}});
            j["callStack"] = arr;
            break;
        }
    }
    return j;
}

// Serialize the current frame's blitter log. Summary fields are always present; the per-blit
// array is included when a frame's worth of data is available (emulator running).
json BlitLogToJson(const BlitLogSnapshot& snap)
{
    json j;
    j["available"] = snap.available;
    if (!snap.available) {
        j["hint"] = "No blitter data. Start the emulator and run a frame.";
        return j;
    }
    j["totalBlits"] = snap.totalBlits;
    j["cpuBlits"] = snap.cpuBlits;
    j["copperBlits"] = snap.copperBlits;
    j["totalDmaCycles"] = snap.totalDmaCycles;
    j["busPercent"] = snap.busPercent;
    j["truncated"] = snap.truncated;

    json arr = json::array();
    for (const auto& e : snap.blits) {
        json ch = json::object();
        auto addCh = [&ch](const char* name, bool used, uint32_t ptr, int16_t mod) {
            if (used)
                ch[name] = {{"addr", ToHex(ptr, 6)}, {"mod", mod}};
        };
        addCh("A", e.useA, e.aptr, e.amod);
        addCh("B", e.useB, e.bptr, e.bmod);
        addCh("C", e.useC, e.cptr, e.cmod);
        addCh("D", e.useD, e.dptr, e.dmod);
        arr.push_back({{"startCycle", e.startCycle},
                       {"lRef", e.lRef},
                       {"line", e.startLine},
                       {"clock", e.startClock},
                       {"trigger", e.fromCopper ? "copper" : "cpu"},
                       {"op", e.op},
                       {"lineMode", e.lineMode},
                       {"widthPx", e.widthPx},
                       {"rows", e.rows},
                       {"wordsPerRow", e.wordsPerRow},
                       {"minterm", ToHex(e.minterm, 2)},
                       {"bltcon0", ToHex(e.bltcon0, 4)},
                       {"bltcon1", ToHex(e.bltcon1, 4)},
                       {"dmaCycles", e.dmaCycles},
                       {"channels", ch}});
    }
    j["blits"] = arr;
    return j;
}

// Serialize the current frame's custom-register writes. Covers CPU and Copper writes only —
// chipset activity that bypasses the register-write path (sprite DMA, bitplane pointer
// auto-increment) is not represented.
json RegLogToJson(const RegLogSnapshot& snap)
{
    json j;
    j["available"] = snap.available;
    if (!snap.available) {
        j["hint"] = "No register data. Start the emulator and run a frame.";
        return j;
    }
    j["coreSupported"] = snap.coreSupported;
    if (!snap.coreSupported) {
        j["hint"] = "The loaded PuAE core predates the register-log patch. Rebuild it with "
                    "scripts/build_puae.ps1 or scripts/build_puae.sh.";
        return j;
    }
    j["totalWrites"] = snap.totalWrites;
    j["cpuWrites"] = snap.cpuWrites;
    j["copperWrites"] = snap.copperWrites;
    j["changedWrites"] = snap.changedWrites;
    j["distinctRegisters"] = snap.distinctRegisters;
    j["truncated"] = snap.truncated;

    json arr = json::array();
    for (const auto& e : snap.writes) {
        json entry = {{"lRef", e.lRef},
                      {"line", e.line},
                      {"clock", e.clock},
                      {"source", e.fromCopper ? "copper" : "cpu"},
                      {"register", e.name},
                      {"offset", ToHex(e.reg, 3)},
                      {"value", ToHex(e.value, 4)},
                      {"changed", e.changed},
                      {"firstWrite", e.firstWrite},
                      {"pc", ToHex(e.pc, 6)}};
        // Only meaningful once the register has a previous value this frame.
        if (!e.firstWrite)
            entry["prevValue"] = ToHex(e.prevValue, 4);
        if (!e.description.empty())
            entry["description"] = e.description;
        arr.push_back(std::move(entry));
    }
    j["writes"] = arr;
    return j;
}

// Serialize the session's disk DMA traffic. The summary fields answer "did this program
// write anything back to the disk?" on their own; `transfers` carries the detail.
json DiskLogToJson(const DiskLogSnapshot& snap)
{
    json j;
    j["available"] = snap.available;
    if (!snap.available) {
        j["hint"] = "No disk DMA data. Start the emulator; a core built before PuAE 1.19.0 "
                    "does not export the disk log (rebuild via scripts/build_puae).";
        return j;
    }
    // Two different scopes live in this object, so name them rather than leaving the
    // caller to infer which is which (they diverge as soon as a filter is applied).
    j["matched"] = snap.matched;               // passed the filter, before `limit`
    j["returned"] = snap.transfers.size();     // actually in `transfers`
    j["totalTransfers"] = snap.totalTransfers; // whole log, filter ignored
    j["reads"] = snap.reads;                   // whole log
    j["writes"] = snap.writes;                 // whole log
    j["bytesRead"] = snap.bytesRead;           // whole log
    j["bytesWritten"] = snap.bytesWritten;     // whole log
    j["anyWrites"] = snap.anyWrites;           // whole log
    j["scope"] = {{"matched", "filter"},     {"returned", "filter+limit"},
                  {"totalTransfers", "log"}, {"reads", "log"},
                  {"writes", "log"},         {"bytesRead", "log"},
                  {"bytesWritten", "log"},   {"anyWrites", "log"}};
    j["truncated"] = snap.truncated;
    j["currentSession"] = snap.currentSession;
    j["sessionCount"] = snap.sessionCount;
    j["order"] = snap.order;
    if (snap.sessionCount > 1) {
        j["note"] = "the log spans more than one run of the machine. `seq` and `frame` "
                    "both restart at each one - order by `ordinal`, and compare `session` "
                    "against currentSession to tell this run from an earlier one.";
    }
    if (snap.droppedEvents)
        j["droppedEvents"] = true; // core ring overflowed; the log has gaps

    json arr = json::array();
    for (const auto& e : snap.transfers) {
        json t = {{"seq", e.seq},
                  {"session", e.session},
                  {"ordinal", e.ordinal},
                  {"frame", e.frame},
                  {"startCycle", e.startCycle},
                  {"endCycle", e.endCycle},
                  {"lRef", e.lRef},
                  {"direction", e.direction},
                  {"trigger", e.trigger},
                  {"pc", ToHex(e.pc, 6)},
                  {"drive", e.drive},
                  {"cyl", e.cyl},
                  {"side", e.side},
                  {"track", e.cyl * 2 + e.side},
                  {"dskpt", ToHex(e.dskpt, 6)},
                  {"dskptEnd", ToHex(e.dskptEnd, 6)},
                  {"dsklen", ToHex(e.dsklen, 4)},
                  {"words", e.words},
                  {"bytes", e.bytes},
                  {"sha1", e.sha1},
                  {"standardTrack", e.standardTrack},
                  {"sectorCount", e.sectorCount},
                  {"sectorsChecksumOk", e.sectorsChecksumOk}};
        // Only surface the exceptional flags, to keep the common row compact.
        if (e.turbo)
            t["turbo"] = true;
        if (e.aborted)
            t["aborted"] = true;
        if (e.direction == "write")
            t["persistedToFile"] = e.persistedToFile;
        if (!e.sectors.empty()) {
            json sec = json::array();
            for (const auto& s : e.sectors)
                sec.push_back({{"track", s.track},
                               {"sector", s.sector},
                               {"checksumOk", s.checksumOk},
                               {"sha1", s.sha1}});
            t["sectors"] = sec;
        }
        arr.push_back(std::move(t));
    }
    j["transfers"] = arr;
    return j;
}

json CopperInstructionToJson(uint32_t addr, uint16_t word1, uint16_t word2,
                             const json& customRegsDef)
{
    const CopperInstruction inst = CopperDisassembler::Disassemble(word1, word2, customRegsDef);
    const std::string text = inst.details.empty() ? inst.name : inst.name + " " + inst.details;
    return {
        {"addr", ToHex(addr, 8)},
        {"rawBytes", json::array({static_cast<uint8_t>(word1 >> 8), static_cast<uint8_t>(word1),
                                  static_cast<uint8_t>(word2 >> 8), static_cast<uint8_t>(word2)})},
        {"hex", fmt::format("{:04X}{:04X}", word1, word2)},
        {"words", json::array({ToHex(word1, 4), ToHex(word2, 4)})},
        {"text", text},
        {"instruction", inst.name},
        {"details", inst.details},
        {"registerName", inst.registerName}};
}

std::string UpperTrimmed(std::string value)
{
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    size_t last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
    for (char& c : value)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

// --- Display masks (get_display_masks / set_display_masks) ------------------------------

// Describes one toggle group so the parser/serializer can be shared by all three.
struct MaskGroupInfo
{
    const char* key;  // JSON key / argument name ("bitplanes")
    const char* item; // per-item name prefix ("BPL" -> BPL1..BPL8)
    int count;        // number of toggles
    int firstIndex;   // index of bit 0 (BPL1, SPR0, PF1)
    uint8_t allMask;  // mask with every toggle visible
    uint8_t DisplayMasks::* field;
};

constexpr MaskGroupInfo kMaskGroups[] = {
    {"bitplanes", "BPL", 8, 1, 0xFF, &DisplayMasks::bitplanes},
    {"sprites", "SPR", 8, 0, 0xFF, &DisplayMasks::sprites},
    {"playfields", "PF", 2, 1, 0x03, &DisplayMasks::playfields},
};

json MaskGroupToJson(const MaskGroupInfo& g, uint8_t mask)
{
    mask &= g.allMask;
    json items = json::object();
    json hidden = json::array();
    for (int i = 0; i < g.count; ++i) {
        const bool on = (mask & (1u << i)) != 0;
        items[fmt::format("{}{}", g.item, g.firstIndex + i)] = on;
        if (!on)
            hidden.push_back(g.firstIndex + i);
    }
    return json{{"mask", mask},
                {"maskHex", ToHex(mask, 2)},
                {"allVisible", mask == g.allMask},
                {"hidden", hidden},
                {"items", items}};
}

// Parses one display-mask argument: an integer or hex-string mask, "all"/"none", or an array of
// item indices naming exactly the items that stay visible.
bool ParseMaskArg(const json& j, const MaskGroupInfo& g, uint8_t& out, std::string& err)
{
    const int lastIndex = g.firstIndex + g.count - 1;
    if (j.is_array()) {
        uint8_t mask = 0;
        for (const auto& e : j) {
            if (!e.is_number_integer() && !e.is_number_unsigned()) {
                err = fmt::format("{}: array entries must be {} indices ({}-{})", g.key, g.item,
                                  g.firstIndex, lastIndex);
                return false;
            }
            const int64_t idx = e.get<int64_t>();
            if (idx < g.firstIndex || idx > lastIndex) {
                err = fmt::format("{}: index {} out of range ({}-{})", g.key, idx, g.firstIndex,
                                  lastIndex);
                return false;
            }
            mask |= static_cast<uint8_t>(1u << (idx - g.firstIndex));
        }
        out = mask;
        return true;
    }
    if (j.is_string()) {
        const std::string s = UpperTrimmed(j.get<std::string>());
        if (s == "ALL") {
            out = g.allMask;
            return true;
        }
        if (s == "NONE") {
            out = 0;
            return true;
        }
    }
    uint32_t value = 0;
    if (!ParseAddr(j, value)) {
        err = fmt::format("{}: expected a mask (integer or hex string), \"all\", \"none\", or an "
                          "array of {} indices ({}-{})",
                          g.key, g.item, g.firstIndex, lastIndex);
        return false;
    }
    if (value > g.allMask) {
        err = fmt::format("{}: mask {} out of range (0-{})", g.key, value, g.allMask);
        return false;
    }
    out = static_cast<uint8_t>(value) & g.allMask;
    return true;
}

// The decoded screen mode, in the pixels the program actually draws. DIW/DDF registers are
// always in lores units, so reporting them verbatim makes a 640x256 hires screen look like a
// 320x256 one — every surface that answers "what is on screen" goes through DescribeScreenMode.
json ScreenModeToJson(const Custom& c)
{
    const ScreenMode m = DescribeScreenMode(c);
    json j;
    j["resolution"] = ResolutionName(m.resolution);
    j["pixelsPerLores"] = m.pixelsPerLores;
    j["interlaced"] = m.interlaced;
    j["displayMode"] = DisplayModeName(m.displayMode);
    j["bitplanesInUse"] = m.bitplanes;
    j["bitplaneDma"] = m.bitplaneDma;
    j["spriteDma"] = m.spriteDma;
    j["dualPlayfield"] = m.displayMode == BitplaneDisplayMode::DualPlayfield;
    // 0 = the registers do not describe a usable window (not yet set up, or mid-change).
    j["displayWidth"] = m.displayWidth;
    j["displayHeight"] = m.displayHeight;
    j["fetchWidth"] = m.fetchWidth;
    j["summary"] = ScreenModeSummary(m);
    return j;
}

// Full display-mask state plus the hardware context that says what is actually on screen
// (screen mode, how many bitplanes the program enabled, whether sprite DMA is on).
json DisplayMasksToJson(const DisplayMasks& masks, adm::AmigaEmulator* emu)
{
    json j;
    for (const auto& g : kMaskGroups)
        j[g.key] = MaskGroupToJson(g, masks.*(g.field));
    const uint8_t effective = EffectiveBitplaneMask(masks);
    j["effectiveBitplaneMask"] = effective;
    j["effectiveBitplaneMaskHex"] = ToHex(effective, 2);
    j["allVisible"] = masks.bitplanes == 0xFF && masks.sprites == 0xFF && masks.playfields == 0x3;

    Custom c{};
    if (emu && emu->GetCustomRegisters(c))
        j["hardware"] = ScreenModeToJson(c);
    return j;
}

} // namespace

McpToolHandler::McpToolHandler(McpHost host) : m_host(std::move(host))
{
    std::ifstream f("assets/custom_registers.json");
    if (f.is_open())
        m_customRegsDef = json::parse(f, nullptr, false);
    if (!m_customRegsDef.is_object())
        m_customRegsDef = json::object();
    BuildManifest();
}

nlohmann::json McpToolHandler::Handle(const std::string& name, const nlohmann::json& args)
{
    auto t0 = std::chrono::steady_clock::now();
    json result = Dispatch(name, args);
    double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // A deferred tool is re-dispatched every frame until it finishes; only the call that
    // actually completes belongs in the activity log.
    if (!(result.is_object() && result.value(McpServer::kRetryKey, false)))
        Record(name, args, result, ms);
    return result;
}

void McpToolHandler::Record(const std::string& tool, const json& args, const json& result,
                            double ms)
{
    ActivityEntry e;
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    e.time = buf;
    e.tool = tool;
    e.args = args.is_null() ? "" : args.dump();
    if (e.args.size() > 200)
        e.args = e.args.substr(0, 197) + "...";
    e.ok = !(result.is_object() && result.contains("error"));
    if (!e.ok) {
        e.result = result.value("error", std::string("error"));
    } else {
        std::string r = result.dump();
        e.result = r.size() > 200 ? r.substr(0, 197) + "..." : r;
    }
    e.durationMs = ms;

    Logger::Info("MCP {} {} ({:.1f} ms)", tool, e.ok ? "ok" : ("error: " + e.result), ms);

    m_activity.push_back(std::move(e));
    while (m_activity.size() > kMaxActivity)
        m_activity.pop_front();
}

std::optional<nlohmann::json>
McpToolHandler::DispatchStatusTools(const std::string& name, const json& args, AmigaEmulator* emu)
{
    [[maybe_unused]] auto requireEmu = [&]() -> bool { return emu != nullptr; };

    // ---- Status / read ----
    if (name == "get_status") {
        json j;
        j["coreLoaded"] = emu && emu->IsCoreLoaded();
        j["gameLoaded"] = emu && emu->IsGameLoaded();
        j["running"] = m_host.isRunning && m_host.isRunning();
        j["paused"] = m_host.isPaused && m_host.isPaused();
        if (emu) {
            j["model"] = GetAmigaModelInfo(emu->GetCurrentModel()).displayName;
            j["coreVersion"] = emu->GetCoreVersion();
            j["frameCount"] = emu->GetTotalFrameCount();
            // frameCount restarts at 0 on every start() / compile-and-run, so it only
            // identifies a moment together with the session it was taken in. Compare
            // this against a frameSession you saved earlier before reusing a frame
            // number as a run_until_frame target.
            j["frameSession"] = emu->GetSessionId();
            j["cpuTrace"] = emu->IsCpuTraceEnabled();
            // Where the beam stands. Every sampled-state tool — read_memory,
            // get_custom_registers, get_blitter_log, screenshot — means something different
            // depending on how far into the field it was called, and this is the only way to ask.
            const json beam = BeamJson(emu);
            if (!beam.is_null())
                j["beam"] = beam;
            j["partialFrame"] = emu->IsPartialFrame() ||
                                (!beam.is_null() && (m_host.isPaused && m_host.isPaused()) &&
                                 BeamHasOverwrittenTheVisibleField(emu, beam.value("vpos", 0)));
            // Most recent KPrintF trap match, so a client that finds the emulator paused can
            // tell whether a KPrintF breakpoint is what stopped it (and on which line).
            KprintfHit kh;
            if (emu->GetLastKprintfHit(kh))
                j["lastKprintfMatch"] = {{"text", kh.text}, {"line", kh.line}};

            // DH0/DH1 as configured, with what the running machine actually has and whether a
            // restart is pending. Configure them with manage_hard_drives (the HD console command).
            const auto& service = emu->GetHostMountService();
            const auto stateName = [](core::HostMountRuntimeState state) {
                switch (state) {
                    case core::HostMountRuntimeState::Disabled:
                        return "disabled";
                    case core::HostMountRuntimeState::Configured:
                        return "configured";
                    case core::HostMountRuntimeState::PendingRestart:
                        return "pendingRestart";
                    case core::HostMountRuntimeState::Effective:
                        return "effective";
                    case core::HostMountRuntimeState::Confirmed:
                        return "confirmed";
                    case core::HostMountRuntimeState::Failed:
                        return "failed";
                }
                return "unknown";
            };
            json drives = json::array();
            for (size_t index = 0; index < 2; ++index) {
                const auto& mount = service.GetConfiguredMounts()[index];
                const auto& effective = service.GetEffectiveMounts()[index];
                json drive = {{"device", core::HostMountDeviceName(mount.device)},
                              {"enabled", mount.enabled},
                              {"backing", mount.IsHardfile() ? "hardfile" : "directory"},
                              {"path", mount.HostPath().generic_string()},
                              {"readOnly", mount.readOnly},
                              {"bootPriority", mount.bootPriority},
                              {"state", stateName(service.GetRuntimeState(mount.device))},
                              {"inMachine", effective.enabled},
                              {"reads", service.GetActivity()[index].reads},
                              {"writes", service.GetActivity()[index].writes}};
                // What the running machine actually has in this slot, which differs from the
                // configuration while a restart is pending or a WHDLoad/Bartman launch overrides
                // it.
                if (effective.enabled) {
                    drive["effective"] = {
                        {"backing", effective.IsHardfile() ? "hardfile"
                                    : effective.backingType == core::HostMountBackingType::Archive
                                        ? "archive"
                                        : "directory"},
                        {"path", effective.HostPath().generic_string()},
                        {"volumeLabel", effective.volumeLabel},
                        {"readOnly", effective.readOnly},
                        {"transient",
                         effective.source == core::HostMountSource::TransientWhdload ||
                             effective.source == core::HostMountSource::TransientBartman ||
                             effective.source == core::HostMountSource::TransientDocument}};
                }
                if (mount.IsHardfile()) {
                    drive["controller"] = core::HardDriveControllerId(mount.controller);
                    drive["controllerUnit"] = mount.controllerUnit;
                    drive["geometry"] = core::HardfileGeometryModeId(mount.geometry.mode);
                } else {
                    drive["volumeLabel"] = mount.volumeLabel;
                }
                drives.push_back(std::move(drive));
            }
            j["hardDrives"] = std::move(drives);
            j["cd"] = {{"image", emu->GetConfiguredCdImage().generic_string()},
                       {"hasDrive", emu->HasCdDrive()},
                       {"inMachine", emu->GetCdImage()},
                       {"lamp", emu->GetCdLed() != 0}};
            // A WHDLoad launch: the effective DH0 came from launch_whdload rather than the
            // hard-drive configuration.
            const auto& effectiveProfile = service.GetEffectiveProfile();
            if (effectiveProfile &&
                effectiveProfile->mounts[0].source == core::HostMountSource::TransientWhdload) {
                const auto& game = effectiveProfile->mounts[0];
                j["whdload"] = {
                    {"active", true},
                    {"volumeLabel", game.volumeLabel},
                    {"path", game.hostDirectory.generic_string()},
                    {"archive", game.backingType == core::HostMountBackingType::Archive}};
            } else {
                j["whdload"] = {{"active", false}};
            }
            j["hardDrivesRestartPending"] = service.HasPendingRestart();

            // The chipset revisions as configured (configure_machine / MACHINE) beside what the
            // running machine really has. "auto" follows the model preset.
            if (!emu->IsCoreLoaded() || emu->HasMachineConfig()) {
                json machine;
                json settings;
                for (const auto& setting : core::kMachineSettingKeys)
                    settings[setting.keyword] = emu->ResolveCoreVariable(setting.key);
                machine["configured"] = std::move(settings);
                if (const auto config = emu->GetMachineConfig()) {
                    machine["running"] = {
                        {"chipset", core::ChipsetName(config->chipsetMask)},
                        {"chipsetExtra", core::ChipsetCompatibleName(config->compatible)},
                        {"agnusModel", core::AgnusModelName(config->agnusModel)},
                        {"agnusSize", core::AgnusSizeName(config->agnusSize)},
                        {"deniseModel", core::DeniseModelName(config->deniseModel)},
                        {"cpu", config->cpuModel},
                        {"fpu", config->fpuModel},
                        {"mmu", core::MmuName(*config)},
                        {"addressBits", core::AddressBits(*config)}};
                }
                j["machine"] = std::move(machine);
            }

            // Serial and parallel ports (manage_serial_port / manage_parallel_port).
            if (!emu->IsCoreLoaded() || emu->HasHostPorts()) {
                json serial = {{"target", emu->GetSerialPort().ToCoreName()},
                               {"mode", core::SerialPortModeName(emu->GetSerialPort().mode)}};
                const auto parallelU8 = emu->GetParallelOutput().u8string();
                json parallel = {{"path", std::string(parallelU8.begin(), parallelU8.end())}};
                if (const auto ports = emu->GetHostPortsStatus()) {
                    serial["open"] = ports->serialOpen;
                    serial["connected"] = ports->serialConnected;
                    serial["peer"] = ports->serialPeer;
                    serial["bytesOut"] = ports->serialTx;
                    serial["bytesIn"] = ports->serialRx;
                    serial["bytesDropped"] = ports->serialDropped;
                    serial["baud"] = ports->serialBaud;
                    serial["error"] = ports->serialError;
                    parallel["open"] = ports->parallelOpen;
                    parallel["bytesWritten"] = ports->parallelBytes;
                    parallel["error"] = ports->parallelError;
                }
                j["serial"] = std::move(serial);
                j["parallel"] = std::move(parallel);
            }
            if (emu->HasSoundBoard())
                j["soundCard"] = emu->ResolveCoreVariable("puae_sound_board");
            if (emu->HasBsdsocket()) {
                json network = {
                    {"bsdsocket", emu->ResolveCoreVariable("puae_bsdsocket") == "enabled"}};
                if (emu->HasNetworkCard()) {
                    network["a2065"] = emu->ResolveCoreVariable("puae_a2065");
                    network["uaenet"] = emu->ResolveCoreVariable("puae_sana2") == "enabled";
                }
                j["network"] = std::move(network);
            }
            if (!emu->IsCoreLoaded() || emu->HasMidi()) {
                json midi = {{"outDevice", emu->GetMidiOutDevice()},
                             {"inDevice", emu->GetMidiInDevice()},
                             {"out", core::MidiDeviceName(emu->GetMidiOutDevice(), true)},
                             {"in", core::MidiDeviceName(emu->GetMidiInDevice(), false)}};
                if (const auto ports = emu->GetHostPortsStatus(); ports && ports->hasMidi) {
                    midi["active"] = ports->midiActive;
                    midi["messagesOut"] = ports->midiOutMessages;
                    midi["bytesIn"] = ports->midiInBytes;
                    midi["error"] = ports->midiError;
                }
                j["midi"] = std::move(midi);
            }
        }
        j["appVersion"] = m_host.appVersion;
        j["consoleVersion"] = std::string(constants::ConsoleVersion);
        return j;
    }

    if (name == "get_registers") {
        if (!requireEmu())
            return Err("emulator not available");
        adm::AmigaEmulator::CpuState st;
        if (!emu->GetCpuState(st))
            return Err("CPU state unavailable (emulator not running?)");
        json j;
        for (int i = 0; i < 8; ++i)
            j["d"][i] = ToHex(st.d[i], 8);
        for (int i = 0; i < 8; ++i)
            j["a"][i] = ToHex(st.a[i], 8);
        j["pc"] = ToHex(st.pc, 8);
        j["sr"] = ToHex(st.sr, 4);
        json f;
        f["T1"] = (st.sr >> 15) & 1;
        f["T0"] = (st.sr >> 14) & 1;
        f["S"] = (st.sr >> 13) & 1;
        f["M"] = (st.sr >> 12) & 1;
        f["IPL"] = (st.sr >> 8) & 7;
        f["X"] = (st.sr >> 4) & 1;
        f["N"] = (st.sr >> 3) & 1;
        f["Z"] = (st.sr >> 2) & 1;
        f["V"] = (st.sr >> 1) & 1;
        f["C"] = st.sr & 1;
        j["flags"] = f;
        CpuExtendedState ext;
        if (emu->GetCpuExtState(ext)) {
            f["STOP"] = ext.stopped ? 1 : 0;
            j["flags"] = f;
            j["cpu"] = {{"model", ext.cpuModel},
                        {"fpu", FpuName(ext.fpuModel, ext.cpuModel)},
                        {"mmu", CpuMmuName(ext)}};
            if (const auto config = emu->GetMachineConfig())
                j["cpu"]["addressBits"] = AddressBits(*config);
            json stack;
            stack["USP"] = ToHex(ext.usp, 8);
            stack[IspName(ext.cpuModel)] = ToHex(ext.isp, 8);
            if (ReadCpuRegister(ext, GetCpuRegister(CpuRegId::Msp)))
                stack["MSP"] = ToHex(ext.msp, 8);
            stack["active"] = ActiveStackName(ext);
            j["stack"] = stack;
        }
        char mnemo[128] = {}, hex[64] = {};
        uint32_t nextPc = 0;
        if (emu->Disassemble(st.pc, mnemo, sizeof(mnemo), hex, sizeof(hex), nextPc))
            j["disasmAtPc"] = fmt::format("{}: {}  {}", ToHex(st.pc, 8), hex, mnemo);
        return j;
    }

    if (name == "get_extended_cpu_registers") {
        if (!requireEmu())
            return Err("emulator not available");
        std::string group = args.value("group", std::string("all"));
        // The console-era spelling, kept so older callers still work.
        const std::string variant = args.value("variant", std::string());
        if (variant == "RC")
            group = "control";
        else if (variant == "RF")
            group = "fpu";
        else if (variant == "RM")
            group = "mmu";
        if (group != "all" && group != "control" && group != "mmu" && group != "fpu")
            return Err("group must be control, mmu, fpu or all");
        CpuExtendedState ext;
        if (!emu->GetCpuExtState(ext))
            return Err("extended CPU registers unavailable (emulator not running, or a core "
                       "older than 1.102.0)");
        return BuildExtendedCpuRegistersJson(ext, group);
    }

    if (name == "get_custom_registers") {
        if (!requireEmu())
            return Err("emulator not available");
        Custom c{};
        if (!emu->GetCustomRegisters(c))
            return Err("custom registers unavailable (emulator not running?)");
        json r;
        auto w = [&](const char* n, uint16_t v) { r[n] = ToHex(v, 4); };
        w("dmaconr", c.dmaconr);
        w("dmacon", c.dmacon);
        w("intenar", c.intenar);
        w("intena", c.intena);
        w("intreqr", c.intreqr);
        w("intreq", c.intreq);
        w("adkconr", c.adkconr);
        w("adkcon", c.adkcon);
        w("bplcon0", c.bplcon0);
        w("bplcon1", c.bplcon1);
        w("bplcon2", c.bplcon2);
        w("bplcon3", c.bplcon3);
        w("bpl1mod", c.bpl1mod);
        w("bpl2mod", c.bpl2mod);
        w("diwstrt", c.diwstrt);
        w("diwstop", c.diwstop);
        w("diwhigh", c.diwhigh);
        w("ddfstrt", c.ddfstrt);
        w("ddfstop", c.ddfstop);
        w("bltcon0", c.bltcon0);
        w("bltcon1", c.bltcon1);
        w("bltsize", c.bltsize);
        // Input registers. Unlike most of this block these are read back from the
        // chipset rather than from the write shadow, so they carry live controller
        // state: JOY0DAT is the Amiga's port 0 (mouse port), JOY1DAT its port 1
        // (joystick port). Both are 0000 with nothing held, so sample them with a
        // direction applied - a rest reading proves nothing.
        w("joy0dat", c.joy0dat);
        w("joy1dat", c.joy1dat);
        w("potgo", c.potgo);
        w("potinp", c.potinp);
        json ptrs;
        ptrs["cop1lc"] = ToHex(DecodeAmigaPtr(c.cop1lc), 6);
        ptrs["cop2lc"] = ToHex(DecodeAmigaPtr(c.cop2lc), 6);
        for (int i = 0; i < 8; ++i)
            ptrs["bplpt"][i] = ToHex(DecodeAmigaPtr(c.bplpt[i]), 6);
        for (int i = 0; i < 8; ++i)
            ptrs["sprpt"][i] = ToHex(DecodeAmigaPtr(c.sprpt[i]), 6);
        json colors = json::array();
        for (int i = 0; i < 32; ++i)
            colors.push_back(ToHex(c.color[i], 4));
        r["pointers"] = ptrs;
        r["colors"] = colors;
        // DIWSTRT/DDFSTRT are in lores units regardless of the screen's own resolution, so the
        // raw values above can't be read as a screen size. Decode it once here.
        r["screenMode"] = ScreenModeToJson(c);
        // Several of these registers are moving targets — bitplane DMA advances bplpt as it
        // fetches, the copper rewrites colors per scanline — so a snapshot is only meaningful
        // alongside the raster position it was taken at.
        if (const json beam = BeamJson(emu); !beam.is_null())
            r["beam"] = beam;
        return r;
    }

    if (name == "get_cia_registers") {
        if (!requireEmu())
            return Err("emulator not available");
        CIAState cia{};
        if (!emu->GetCIARegisters(cia))
            return Err("CIA registers unavailable (old core or not running?)");
        auto dump = [](const CIAChip& c) {
            json j;
            auto h = [](uint8_t v) { return ToHex(v, 2); };
            j["pra"] = h(c.pra);
            j["prb"] = h(c.prb);
            j["dra"] = h(c.dra);
            j["drb"] = h(c.drb);
            j["talo"] = h(c.talo);
            j["tahi"] = h(c.tahi);
            j["tblo"] = h(c.tblo);
            j["tbhi"] = h(c.tbhi);
            j["todlo"] = h(c.todlo);
            j["todmid"] = h(c.todmid);
            j["todhi"] = h(c.todhi);
            j["sdr"] = h(c.sdr);
            j["icr"] = h(c.icr);
            j["imask"] = h(c.imask);
            j["cra"] = h(c.cra);
            j["crb"] = h(c.crb);
            return j;
        };
        return json{{"ciaa", dump(cia.a)}, {"ciab", dump(cia.b)}};
    }

    if (name == "list_memory_regions") {
        if (!requireEmu())
            return Err("emulator not available");
        json arr = json::array();
        for (const auto& reg : emu->GetMemoryRegions()) {
            arr.push_back({{"name", reg.name},
                           {"base", ToHex(reg.amigaAddr, 8)},
                           {"size", reg.size},
                           {"writable", reg.writable}});
        }
        return json{{"regions", arr}};
    }

    if (name == "read_memory") {
        if (!requireEmu())
            return Err("emulator not available");
        uint32_t addr = 0;
        if (!GetAddrArg(args, "addr", addr))
            return AddrArgError(args, "addr");
        uint32_t len = args.value("len", 256u);
        if (len == 0)
            return Err("len must be > 0");
        if (len > 8192)
            len = 8192;
        ArMemoryAccess mem;
        RefreshMem(mem, emu);
        std::vector<uint8_t> buf(len);
        std::vector<bool> valid;
        mem.Read(addr, buf.data(), len, &valid);
        // Surface unmapped bytes rather than passing filler off as real data: render
        // them as "??" in hex and ' ' in ASCII, and flag the response as partial.
        std::string hex, ascii;
        hex.reserve(len * 2);
        ascii.reserve(len);
        bool partial = false;
        for (uint32_t i = 0; i < len; ++i) {
            bool ok = (i >= valid.size()) || valid[i];
            if (!ok) {
                partial = true;
                hex += "??";
                ascii += ' ';
                continue;
            }
            hex += fmt::format("{:02X}", buf[i]);
            ascii += (buf[i] >= 32 && buf[i] < 127) ? static_cast<char>(buf[i]) : '.';
        }
        json r{{"addr", ToHex(addr, 8)}, {"len", len}, {"hex", hex}, {"ascii", ascii}};
        if (partial) {
            r["partial"] = true; // some bytes were unmapped (shown as "??")
            // A "??" inside CIA space is a decode miss, not a missing feature: only the
            // odd addresses of $BFExxx (CIA-A) and the even ones of $BFDxxx (CIA-B) are
            // answered by a chip, and register $B does not exist. Say so, because "??"
            // over a fire-button register otherwise reads as "the button did nothing".
            if (addr >= ArMemoryAccess::kCiaBase && addr < ArMemoryAccess::kCiaEnd) {
                r["hint"] = "CIA space is served, but only where a chip select decodes: "
                            "CIA-A on odd addresses $BFE001+reg*$100, CIA-B on even "
                            "addresses $BFD000+reg*$100, registers 0-15 except $B. "
                            "Reads come from a side-effect-free snapshot (ICR is not "
                            "cleared). get_cia_registers dumps both chips by name.";
            }
        }
        return r;
    }

    if (name == "disassemble") {
        if (!requireEmu())
            return Err("emulator not available");
        uint32_t addr = 0;
        if (!GetAddrArg(args, "addr", addr))
            return AddrArgError(args, "addr");
        uint32_t count = args.value("count", 16u);
        if (count == 0)
            return Err("count must be > 0");
        if (count > 512)
            count = 512;
        json lines = json::array();
        uint32_t pc = addr;
        for (uint32_t i = 0; i < count; ++i) {
            char mnemo[128] = {}, hex[64] = {};
            uint32_t nextPc = 0;
            if (!emu->Disassemble(pc, mnemo, sizeof(mnemo), hex, sizeof(hex), nextPc))
                break;
            json line;
            line["addr"] = ToHex(pc, 8);
            line["hex"] = hex;
            line["text"] = mnemo;
            if (m_host.annotations) {
                if (auto l = m_host.annotations->GetLabel(pc))
                    line["label"] = *l;
                if (auto cm = m_host.annotations->GetComment(pc))
                    line["comment"] = *cm;
                if (auto b = m_host.annotations->GetBlock(pc))
                    line["block"] = *b;
            }
            lines.push_back(line);
            if (nextPc <= pc)
                break; // guard against non-advancing disassembly
            pc = nextPc;
        }
        return json{{"start", ToHex(addr, 8)}, {"lines", lines}};
    }

    if (name == "disassemble_copper") {
        if (!requireEmu())
            return Err("emulator not available");
        if (!args.contains("source") ||
            !(args["source"].is_string() || args["source"].is_number_integer() ||
              args["source"].is_number_unsigned()))
            return Err("source required (COP1, COP2, Trace, or an address)");

        enum class Source { Cop1, Cop2, Address, Trace };
        Source source = Source::Address;
        uint32_t startAddr = 0;
        std::string sourceName;
        if (args["source"].is_string()) {
            sourceName = UpperTrimmed(args["source"].get<std::string>());
            if (sourceName == "COP1" || sourceName == "COP1LC") {
                source = Source::Cop1;
                sourceName = "COP1";
            } else if (sourceName == "COP2" || sourceName == "COP2LC") {
                source = Source::Cop2;
                sourceName = "COP2";
            } else if (sourceName == "TRACE") {
                source = Source::Trace;
                sourceName = "Trace";
            } else if (!ParseAddr(args["source"], startAddr)) {
                return Err("source must be COP1, COP2, Trace, or an address");
            } else {
                sourceName = "Address";
            }
        } else if (args["source"].is_number_integer() && !args["source"].is_number_unsigned() &&
                   args["source"].get<int64_t>() < 0) {
            return Err("invalid copper address");
        } else if (ParseAddr(args["source"], startAddr)) {
            sourceName = "Address";
        } else {
            return Err("invalid copper address");
        }

        int requestedCount = 256;
        if (args.contains("count")) {
            if (!args["count"].is_number_integer() && !args["count"].is_number_unsigned())
                return Err("count must be an integer");
            if (args["count"].is_number_unsigned()) {
                requestedCount =
                    static_cast<int>(std::min<uint64_t>(args["count"].get<uint64_t>(), 16384));
            } else {
                const int64_t count = args["count"].get<int64_t>();
                if (count < 0)
                    return Err("count must be >= 0");
                requestedCount = static_cast<int>(std::min<int64_t>(count, 16384));
            }
        }

        json result{{"source", sourceName}};
        json lines = json::array();
        if (source == Source::Trace) {
            const int limit =
                requestedCount == 0
                    ? adm::AmigaEmulator::kMaxCopperExecEvents
                    : std::min(requestedCount, adm::AmigaEmulator::kMaxCopperExecEvents);
            std::vector<adm::AmigaEmulator::CopperExecEvent> events(
                adm::AmigaEmulator::kMaxCopperExecEvents);
            const int available =
                emu->GetCopperExecEvents(events.data(), adm::AmigaEmulator::kMaxCopperExecEvents);
            const int count = std::min(available, limit);

            int activeList = 1;
            for (int i = 0; i < count; ++i) {
                const auto& ev = events[static_cast<size_t>(i)];
                json line = CopperInstructionToJson(ev.addr, ev.word1, ev.word2, m_customRegsDef);
                line["list"] = activeList == 1 ? "COP1" : "COP2";
                line["line"] = ev.vpos;
                line["clock"] = ev.hpos;
                line["lRef"] = static_cast<uint32_t>(ev.vpos) * constants::beam::CCKPerLine +
                               static_cast<uint32_t>(ev.hpos);
                line["absoluteCycle"] = ev.absolute_cycle;
                lines.push_back(std::move(line));

                if ((ev.word1 & 1) == 0) {
                    const uint16_t reg = ev.word1 & 0x01FE;
                    if (reg == 0x0088)
                        activeList = 1;
                    else if (reg == 0x008A)
                        activeList = 2;
                }
            }
            result["available"] = available;
            result["overflow"] = emu->GetCopperExecOverflow();
            result["truncated"] = available > count || emu->GetCopperExecOverflow();
            result["instructions"] = std::move(lines);
            return result;
        }

        if (source == Source::Cop1 || source == Source::Cop2) {
            Custom regs{};
            if (!emu->GetCustomRegisters(regs))
                return Err("copper pointers unavailable (emulator not running?)");
            startAddr = DecodeAmigaPtr(source == Source::Cop1 ? regs.cop1lc : regs.cop2lc);
        }
        startAddr &= ~1u;
        result["start"] = ToHex(startAddr, 8);

        const int limit = requestedCount == 0 ? 4096 : std::min(requestedCount, 4096);
        ArMemoryAccess mem;
        RefreshMem(mem, emu);
        uint32_t addr = startAddr;
        std::string stopReason = "limit";
        for (int i = 0; i < limit; ++i) {
            bool ok1 = false, ok2 = false;
            const uint16_t word1 = mem.ReadWord(addr, &ok1);
            const uint16_t word2 = mem.ReadWord(addr + 2, &ok2);
            if (!ok1 || !ok2) {
                stopReason = "unmapped";
                result["unmappedAt"] = ToHex(addr, 8);
                break;
            }
            lines.push_back(CopperInstructionToJson(addr, word1, word2, m_customRegsDef));
            addr += 4;
            if (word1 == 0xFFFF && word2 == 0xFFFE) {
                stopReason = "end";
                break;
            }
            if ((word1 & 1) == 0) {
                const uint16_t reg = word1 & 0x01FE;
                if (reg == 0x0088 || reg == 0x008A) {
                    stopReason = "jump";
                    break;
                }
            }
        }
        result["stopReason"] = stopReason;
        result["truncated"] = stopReason == "limit";
        result["instructions"] = std::move(lines);
        return result;
    }

    if (name == "get_annotations") {
        if (!m_host.annotations)
            return Err("annotations unavailable");
        uint32_t start = 0, end = 0xFFFFFFFFu;
        if (BadOptionalAddr(args, "start", start))
            return AddrArgError(args, "start");
        if (BadOptionalAddr(args, "end", end))
            return AddrArgError(args, "end");
        json arr = json::array();
        for (const auto& [addr, ann] : m_host.annotations->All()) {
            if (addr < start || addr > end)
                continue;
            json a;
            a["addr"] = ToHex(addr, 8);
            if (!ann.label.empty())
                a["label"] = ann.label;
            if (!ann.comment.empty())
                a["comment"] = ann.comment;
            if (!ann.block.empty())
                a["block"] = ann.block;
            arr.push_back(a);
        }
        return json{{"annotations", arr}};
    }

    if (name == "screenshot") {
        if (!requireEmu())
            return Err("emulator not available");
        std::vector<uint8_t> png;
        if (!emu->CaptureScreenshotPNG(png) || png.empty())
            return Err("screenshot failed (no frame?)");
        json j{{"image_png_base64", Base64Encode(png)},
               {"width", emu->GetWidth()},
               {"height", emu->GetHeight()}};

        // A capture taken part-way through a field only contains the scanlines drawn so far;
        // whatever was underneath fills the rest. For anything drawn late in the field that is
        // indistinguishable from "the program renders nothing", so it has to be said out loud.
        // The frame is deliberately NOT completed here — capturing the machine exactly as it
        // stands is the right default for a debugger, and silently stepping it would break
        // anyone screenshotting at a breakpoint on purpose.
        const json beam = BeamJson(emu);
        const bool paused = m_host.isPaused && m_host.isPaused();
        if (!beam.is_null())
            j["beam"] = beam;
        const bool midField = !beam.is_null() && paused &&
                              BeamHasOverwrittenTheVisibleField(emu, beam.value("vpos", 0));
        const bool partial = emu->IsPartialFrame() || midField;
        j["partialFrame"] = partial;
        std::string note;
        if (partial) {
            const int vpos = beam.is_null() ? emu->GetBreakVpos() : beam.value("vpos", 0);
            note = fmt::format(
                "Captured mid-frame: scanlines below vpos {} are left over from the previous "
                "frame. Call end_frame (while paused) and screenshot again for a complete field.",
                vpos);
        }

        // The capture comes off the display texture, i.e. after the core applied the debug
        // display masks — say so whenever something is hidden, so a missing layer in the image
        // isn't mistaken for a bug in the program under test.
        const DisplayMasks masks = emu->GetDisplayMasks();
        if (masks.bitplanes != 0xFF || masks.sprites != 0xFF || masks.playfields != 0x3) {
            j["displayMasks"] = DisplayMasksToJson(masks, emu);
            if (!note.empty())
                note += " ";
            note += "Display masks are active: this image shows only the un-masked "
                    "bitplanes/sprites/playfields. Call set_display_masks with "
                    "bitplanes/sprites/playfields = \"all\" for the unfiltered picture.";
        }
        if (!note.empty())
            j["note"] = note;
        return j;
    }

    if (name == "get_display_masks") {
        if (!requireEmu())
            return Err("emulator not available");
        return DisplayMasksToJson(emu->GetDisplayMasks(), emu);
    }

    if (name == "set_display_masks") {
        if (!requireEmu())
            return Err("emulator not available");
        DisplayMasks masks = emu->GetDisplayMasks();
        bool any = false;
        if (args.value("reset", false)) {
            masks = DisplayMasks{};
            any = true;
        }
        for (const auto& g : kMaskGroups) {
            if (!args.contains(g.key))
                continue;
            uint8_t value = 0;
            std::string err;
            if (!ParseMaskArg(args[g.key], g, value, err))
                return Err(err);
            masks.*(g.field) = value;
            any = true;
        }
        if (!any)
            return Err("nothing to set (pass bitplanes, sprites, playfields and/or reset:true)");
        // Re-renders the current frame, so a following screenshot shows the new masking even
        // while the emulator is paused.
        emu->SetDisplayMasks(masks);
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return DisplayMasksToJson(emu->GetDisplayMasks(), emu);
    }

    return std::nullopt; // not one of this category's tools
}

std::optional<nlohmann::json> McpToolHandler::DispatchAnnotationTools(const std::string& name,
                                                                      const json& args,
                                                                      AmigaEmulator* emu)
{
    [[maybe_unused]] auto requireEmu = [&]() -> bool { return emu != nullptr; };

    // ---- Annotations (write) ----
    if (name == "set_label" || name == "set_comment" || name == "set_block") {
        if (!m_host.annotations)
            return Err("annotations unavailable");
        uint32_t addr = 0;
        if (!GetAddrArg(args, "addr", addr))
            return AddrArgError(args, "addr");
        std::string text = args.value("text", std::string{});
        if (name == "set_label")
            m_host.annotations->SetLabel(addr, text);
        else if (name == "set_comment")
            m_host.annotations->SetComment(addr, text);
        else
            m_host.annotations->SetBlock(addr, text);
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return json{{"ok", true}, {"addr", ToHex(addr, 8)}};
    }

    if (name == "remove_annotation") {
        if (!m_host.annotations)
            return Err("annotations unavailable");
        uint32_t addr = 0;
        if (!GetAddrArg(args, "addr", addr))
            return AddrArgError(args, "addr");
        bool removed = false;
        if (args.contains("fields") && args["fields"].is_array()) {
            for (const auto& f : args["fields"]) {
                if (!f.is_string())
                    continue;
                std::string fs = f.get<std::string>();
                if (fs == "label")
                    removed |= m_host.annotations->RemoveLabel(addr);
                else if (fs == "comment")
                    removed |= m_host.annotations->RemoveComment(addr);
                else if (fs == "block")
                    removed |= m_host.annotations->RemoveBlock(addr);
            }
        } else {
            removed = m_host.annotations->Remove(addr);
        }
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return json{{"ok", true}, {"removed", removed}};
    }

    return std::nullopt; // not one of this category's tools
}

std::optional<nlohmann::json>
McpToolHandler::DispatchWriteTools(const std::string& name, const json& args, AmigaEmulator* emu)
{
    [[maybe_unused]] auto requireEmu = [&]() -> bool { return emu != nullptr; };

    // ---- Memory / chipset writes ----
    if (name == "write_memory") {
        if (!requireEmu())
            return Err("emulator not available");
        uint32_t addr = 0;
        if (!GetAddrArg(args, "addr", addr))
            return AddrArgError(args, "addr");
        std::string hex = args.value("hex", std::string{});
        // strip spaces
        std::string clean;
        for (char ch : hex)
            if (!std::isspace(static_cast<unsigned char>(ch)))
                clean += ch;
        if (clean.empty() || clean.size() % 2 != 0)
            return Err("hex must be an even-length hex byte string");
        std::vector<uint8_t> bytes;
        try {
            for (size_t i = 0; i < clean.size(); i += 2)
                bytes.push_back(static_cast<uint8_t>(std::stoul(clean.substr(i, 2), nullptr, 16)));
        } catch (...) {
            return Err("invalid hex");
        }
        ArMemoryAccess mem;
        RefreshMem(mem, emu);
        std::string werr;
        if (!mem.Write(addr, bytes.data(), bytes.size(), werr))
            return Err("write failed: " + werr);
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return json{{"ok", true}, {"addr", ToHex(addr, 8)}, {"written", bytes.size()}};
    }

    if (name == "write_custom_register") {
        if (!requireEmu())
            return Err("emulator not available");
        uint32_t addr = 0;
        if (!GetAddrArg(args, "addr", addr))
            return AddrArgError(args, "addr");
        uint32_t value = 0;
        if (!GetAddrArg(args, "value", value))
            return AddrArgError(args, "value");
        if (!emu->WriteCustomRegister(addr, static_cast<uint16_t>(value)))
            return Err("write_custom_register failed");
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return json{{"ok", true}};
    }

    return std::nullopt; // not one of this category's tools
}

std::optional<nlohmann::json> McpToolHandler::DispatchExecutionTools(const std::string& name,
                                                                     const json& args,
                                                                     AmigaEmulator* emu)
{
    [[maybe_unused]] auto requireEmu = [&]() -> bool { return emu != nullptr; };

    // ---- Execution control ----
    if (name == "pause") {
        if (m_host.pause)
            m_host.pause();
        return json{{"ok", true}, {"paused", m_host.isPaused && m_host.isPaused()}};
    }
    if (name == "resume") {
        if (m_host.resume)
            m_host.resume();
        return json{{"ok", true}, {"paused", m_host.isPaused && m_host.isPaused()}};
    }
    if (name == "step") {
        if (!m_host.stepInto)
            return Err("step unavailable");
        if (args.contains("count") && !args["count"].is_number_integer())
            return Err("count must be an integer");
        int count = args.value("count", 1);
        if (count < 1 || count > 65536)
            return Err("count must be between 1 and 65536");
        for (int i = 0; i < count; ++i)
            m_host.stepInto();
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return json{{"ok", true}, {"stepped", count}};
    }
    if (name == "step_over") {
        if (!m_host.stepOver)
            return Err("step_over unavailable");
        return json{{"ok", m_host.stepOver()}};
    }
    if (name == "step_frame") {
        if (!m_host.stepFrame)
            return Err("step_frame unavailable");
        if (!(m_host.isRunning && m_host.isRunning()))
            return Err("emulator not running");
        if (!(m_host.isPaused && m_host.isPaused()))
            return Err("emulator not paused (pause first)");
        int count = args.value("count", 1);
        if (count < 1)
            count = 1;
        for (int i = 0; i < count; ++i)
            m_host.stepFrame();
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        json j{{"ok", true}, {"stepped", count}};
        if (m_host.getFrameCount)
            j["frameCount"] = m_host.getFrameCount();
        return j;
    }
    if (name == "run_until_frame") {
        if (!m_host.runUntilFrame)
            return Err("run_until_frame unavailable");
        if (!(m_host.isRunning && m_host.isRunning())) {
            m_frameWait = {};
            return Err("emulator not running");
        }
        if (!args.contains("frame") || !args["frame"].is_number_integer())
            return Err("frame (integer) required");
        const uint64_t target = args["frame"].get<uint64_t>();
        const bool turbo = args.value("turbo", false);
        const bool wait = args.value("wait", false);
        const uint64_t cur = m_host.getFrameCount ? m_host.getFrameCount() : 0;
        const bool paused = m_host.isPaused && m_host.isPaused();

        // Re-entry: this request was deferred on a previous frame and the emulator has been
        // running in between. Decide whether the run has finished.
        if (m_frameWait.active && m_frameWait.target == target) {
            const bool expired = std::chrono::steady_clock::now() >= m_frameWait.deadline;
            if (!paused && !expired)
                return json{{McpServer::kRetryKey, true}};
            m_frameWait = {};
            json j{{"ok", true}, {"current", cur}, {"target", target}, {"turbo", turbo}};
            j["reached"] = cur >= target;
            if (cur >= target) {
                j["note"] = "target reached; emulator is paused";
            } else if (paused) {
                // A breakpoint / watchpoint got there first. Say so rather than reporting a
                // bare "didn't reach it", which reads like a failure.
                j["stoppedEarly"] = true;
                j["note"] = "stopped before the target frame — a breakpoint fired. Check "
                            "get_status and get_registers.";
            } else {
                j["timedOut"] = true;
                j["note"] = fmt::format(
                    "still running after {}s; the run is not cancelled. Poll get_status until "
                    "paused with frameCount >= target, or pass turbo:true to get there faster.",
                    kFrameWaitBudgetSeconds);
            }
            return j;
        }

        // "Run until at least frame N" is trivially satisfied when N is already behind us.
        // This used to be an error, which turned a routine "advance a bit" into a failure the
        // caller had to detect and work around.
        if (target <= cur)
            return json{{"ok", true},
                        {"current", cur},
                        {"target", target},
                        {"alreadyPast", true},
                        {"frameSession", emu ? emu->GetSessionId() : 0},
                        {"note", "target frame is at/behind the current frame; nothing to run. "
                                 "If you computed this target before a start(), it is stale: "
                                 "start() rebases frameCount to 0 and bumps frameSession."}};

        if (!m_host.runUntilFrame(target, turbo))
            return Err("run_until_frame failed");
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        if (!wait)
            return json{{"ok", true},
                        {"target", target},
                        {"current", cur},
                        {"turbo", turbo},
                        {"frameSession", emu ? emu->GetSessionId() : 0},
                        {"note", "runs asynchronously; poll get_status until paused with "
                                 "frameCount >= target, or pass wait:true to have this call "
                                 "return only once it is there"}};

        m_frameWait.active = true;
        m_frameWait.target = target;
        m_frameWait.deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(kFrameWaitBudgetSeconds);
        return json{{McpServer::kRetryKey, true}};
    }

    if (name == "start") {
        if (!m_host.startEmulator)
            return Err("start unavailable");
        if (m_host.isRunning && m_host.isRunning())
            return Err("emulator already running");
        m_host.startEmulator();
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        json j{{"ok", true}, {"running", m_host.isRunning && m_host.isRunning()}};
        if (emu) {
            j["frameCount"] = emu->GetTotalFrameCount();
            j["frameSession"] = emu->GetSessionId();
            j["note"] = "start() reloads the game, so frameCount restarts at 0 and "
                        "frameSession advances. Any frame number computed before this "
                        "call is stale. (reset_emulator does NOT rebase the counter.)";
        }
        return j;
    }
    if (name == "stop") {
        if (!m_host.stopEmulator)
            return Err("stop unavailable");
        if (!(m_host.isRunning && m_host.isRunning()))
            return Err("emulator not running");
        m_host.stopEmulator();
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return json{{"ok", true}, {"running", m_host.isRunning && m_host.isRunning()}};
    }
    if (name == "end_frame") {
        if (!m_host.endFrame)
            return Err("end_frame unavailable");
        if (!(m_host.isRunning && m_host.isRunning()))
            return Err("emulator not running");
        if (!(m_host.isPaused && m_host.isPaused()))
            return Err("emulator not paused (pause first)");
        m_host.endFrame();
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        json j{{"ok", true}};
        if (m_host.getFrameCount)
            j["frameCount"] = m_host.getFrameCount();
        return j;
    }

    if (name == "click_mouse") {
        if (!requireEmu() || !emu->IsGameLoaded())
            return Err("emulator not running");
        if (!args.contains("button") || !args["button"].is_string())
            return Err("button required (left or right)");
        const std::string button = UpperTrimmed(args["button"].get<std::string>());
        if (button != "LEFT" && button != "RIGHT")
            return Err("button must be left or right");

        uint32_t durationFrames = 1;
        if (args.contains("durationFrames")) {
            if (!args["durationFrames"].is_number_integer() &&
                !args["durationFrames"].is_number_unsigned())
                return Err("durationFrames must be an integer");
            if (args["durationFrames"].is_number_unsigned()) {
                durationFrames = static_cast<uint32_t>(
                    std::min<uint64_t>(args["durationFrames"].get<uint64_t>(), 10000));
            } else {
                const int64_t duration = args["durationFrames"].get<int64_t>();
                if (duration < 1)
                    return Err("durationFrames must be >= 1");
                durationFrames = static_cast<uint32_t>(std::min<int64_t>(duration, 10000));
            }
            if (durationFrames == 0)
                return Err("durationFrames must be >= 1");
        }

        const auto mouseButton = button == "LEFT" ? adm::AmigaEmulator::MouseButton::Left
                                                  : adm::AmigaEmulator::MouseButton::Right;
        const uint64_t releaseFrame = emu->ClickMouseButton(mouseButton, durationFrames);
        return json{{"ok", true},
                    {"button", button == "LEFT" ? "left" : "right"},
                    {"durationFrames", durationFrames},
                    {"releaseFrame", releaseFrame}};
    }

    if (name == "press_key") {
        if (!requireEmu() || !emu->IsGameLoaded())
            return Err("emulator not running");
        if (!args.contains("key"))
            return Err("key required (a key name like \"F1\" / \"Return\", or a RETROK number)");

        int keyCode = 0;
        if (args["key"].is_string()) {
            const std::string keyName = args["key"].get<std::string>();
            if (!LookupRetroKey(keyName, keyCode))
                return Err("unknown key '" + keyName +
                           "'. Names are case-insensitive, e.g. F1, Return, Escape, Space, "
                           "Num1, Kp5, Up, LeftBracket.");
        } else if (args["key"].is_number_integer() || args["key"].is_number_unsigned()) {
            keyCode = args["key"].get<int>();
            if (keyCode <= 0 || keyCode > 322)
                return Err("key number out of range (1-322)");
        } else {
            return Err("key must be a string name or an integer RETROK code");
        }

        uint32_t durationFrames = 2;
        if (args.contains("durationFrames")) {
            if (!args["durationFrames"].is_number_integer() &&
                !args["durationFrames"].is_number_unsigned())
                return Err("durationFrames must be an integer");
            const int64_t duration = args["durationFrames"].get<int64_t>();
            if (duration < 1)
                return Err("durationFrames must be >= 1");
            durationFrames = static_cast<uint32_t>(std::min<int64_t>(duration, 10000));
        }

        std::vector<int> modifiers;
        if (args.contains("modifiers")) {
            if (!args["modifiers"].is_array())
                return Err("modifiers must be an array of strings");
            for (const auto& m : args["modifiers"]) {
                if (!m.is_string())
                    return Err("modifiers must be an array of strings");
                int modCode = 0;
                const std::string modName = m.get<std::string>();
                if (!LookupModifierKey(modName, modCode))
                    return Err("unknown modifier '" + modName +
                               "' (shift, ctrl, alt, lalt, ralt, amiga/meta, and l/r variants)");
                modifiers.push_back(modCode);
            }
        }

        const auto press = emu->PressKey(keyCode, durationFrames, modifiers);
        const char* canonical = RetroKeyName(keyCode);
        json j{{"ok", true},
               {"key", canonical ? canonical : std::to_string(keyCode)},
               {"code", keyCode},
               {"durationFrames", durationFrames},
               {"pressFrame", press.pressFrame},
               {"releaseFrame", press.releaseFrame},
               {"queued", press.queued}};
        if (m_host.getFrameCount)
            j["frameCount"] = m_host.getFrameCount();
        if (press.queued) {
            j["note"] = "the same key was still held by an earlier press, so this one was "
                        "queued behind it and goes down at pressFrame. The machine must reach "
                        "that frame (resume / step_frame / run_until_frame) for it to land.";
        } else if (m_host.isPaused && m_host.isPaused()) {
            j["note"] = "emulator is paused; the press lands once frames advance "
                        "(resume / step_frame / run_until_frame).";
        }
        return j;
    }

    if (name == "type_text") {
        if (!requireEmu() || !emu->IsGameLoaded())
            return Err("emulator not running");
        if (!args.contains("text") || !args["text"].is_string())
            return Err("text required");
        const std::string text = args["text"].get<std::string>();
        if (text.empty())
            return Err("text must not be empty");
        if (text.size() > 1024)
            return Err("text too long (max 1024 characters)");

        uint32_t framesPerKey = 2;
        if (args.contains("framesPerKey")) {
            if (!args["framesPerKey"].is_number_integer() &&
                !args["framesPerKey"].is_number_unsigned())
                return Err("framesPerKey must be an integer");
            const int64_t fpk = args["framesPerKey"].get<int64_t>();
            if (fpk < 2)
                return Err("framesPerKey must be >= 2 (one frame held, one frame released, so "
                           "repeated characters register as separate presses)");
            framesPerKey = static_cast<uint32_t>(std::min<int64_t>(fpk, 1000));
        }

        // Validate the whole string before queuing anything, so a bad character can't leave
        // half a line typed into the guest.
        std::vector<KeyStroke> strokes;
        strokes.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            KeyStroke stroke;
            if (!AsciiToKeyStroke(text[i], stroke))
                return Err("no key mapping for character '" + std::string(1, text[i]) +
                           "' at index " + std::to_string(i) +
                           " (printable ASCII, newline, tab and backspace only)");
            strokes.push_back(stroke);
        }

        // Each character owns a framesPerKey window: held for all but the last frame, then
        // released, so "aa" is two presses rather than one long hold.
        const uint64_t startFrame = emu->GetTotalFrameCount();
        const uint32_t hold = framesPerKey - 1;
        const std::vector<int> shift{static_cast<int>(adm::AmigaEmulator::RetroKey::LShift)};
        uint64_t releaseFrame = startFrame;
        for (size_t i = 0; i < strokes.size(); ++i) {
            const uint64_t pressFrame = startFrame + static_cast<uint64_t>(i) * framesPerKey;
            releaseFrame = emu->PressKeyAt(strokes[i].code, pressFrame, hold,
                                           strokes[i].shift ? shift : std::vector<int>{})
                               .releaseFrame;
        }

        return json{{"ok", true},
                    {"characters", strokes.size()},
                    {"framesPerKey", framesPerKey},
                    {"startFrame", startFrame},
                    {"releaseFrame", releaseFrame},
                    {"note", "queued; advance the machine with step_frame or resume until "
                             "frameCount reaches releaseFrame"}};
    }

    if (name == "set_joystick") {
        if (!requireEmu() || !emu->IsGameLoaded())
            return Err("emulator not running");

        // `port` is an AMIGA port number, not a libretro one. Amiga port 1 is the
        // joystick port (JOY1DAT, CIA-A PRA bit 7) and is the default; Amiga port 0 is
        // the mouse port (JOY0DAT, CIA-A PRA bit 6). PuAE swaps the two when it maps
        // libretro ports onto Amiga ports, so translate here and keep every value the
        // caller ever sees in Amiga terms.
        if (args.contains("port") &&
            !(args["port"].is_number_integer() || args["port"].is_number_unsigned()))
            return Err("port must be 0 or 1");
        const int amigaPort = args.value("port", 1);
        if (amigaPort != 0 && amigaPort != 1)
            return Err("port must be 0 (mouse port, JOY0DAT) or 1 (joystick port, JOY1DAT)");
        const int retroPort = amigaPort == 1 ? 0 : 1;

        if (!emu->EnsureJoypadPortDevice(retroPort))
            return Err("could not attach a joypad to Amiga port " + std::to_string(amigaPort));

        // RETRO_DEVICE_ID_JOYPAD_*: B=0, UP=4, DOWN=5, LEFT=6, RIGHT=7, A=8.
        static const std::pair<const char*, int> kButtons[] = {
            {"up", 4}, {"down", 5}, {"left", 6}, {"right", 7}, {"fire", 0}, {"fire2", 8},
        };

        bool any = false;
        for (const auto& [argName, padId] : kButtons) {
            if (!args.contains(argName))
                continue;
            if (!args[argName].is_boolean())
                return Err(std::string(argName) + " must be a boolean");
            emu->SetInjectedJoypadButton(retroPort, padId, args[argName].get<bool>());
            any = true;
        }
        if (!args.contains("release") && !any)
            return Err("give at least one of up/down/left/right/fire/fire2, or release:true");
        if (args.value("release", false))
            emu->ClearInjectedJoypad(retroPort);

        json state = json::object();
        const uint32_t mask = emu->GetInjectedJoypadMask(retroPort);
        for (const auto& [argName, padId] : kButtons)
            state[argName] = (mask & (1u << padId)) != 0;

        // Echo the hardware this drove. Without it the only way to confirm which port
        // moved is to dump JOY0DAT/JOY1DAT and CIA-A PRA by hand.
        json j{{"ok", true},
               {"port", amigaPort},
               {"dataRegister", amigaPort == 1 ? "JOY1DAT" : "JOY0DAT"},
               {"dataRegisterAddr", amigaPort == 1 ? "DFF00C" : "DFF00A"},
               {"fireBit", amigaPort == 1 ? "CIA-A PRA bit 7" : "CIA-A PRA bit 6"},
               {"retroPort", retroPort},
               {"state", state}};
        if (amigaPort == 0) {
            // Real hardware cannot have both either: one DE-9 socket, one set of pins.
            j["note"] = "Amiga port 0 is shared with the emulated mouse. A joystick is now "
                        "attached there, so click_mouse / mouse motion no longer reach the "
                        "guest on this port until the emulator is restarted (start/stop).";
        }
        return j;
    }

    if (name == "save_state" || name == "load_state" || name == "delete_state") {
        if (!m_host.states)
            return Err("state snapshots unavailable");
        if (!args.contains("name") || !args["name"].is_string())
            return Err("name required");
        const std::string stateName = args["name"].get<std::string>();
        if (!EmulatorStateStore::IsValidName(stateName))
            return Err("name must be 1-64 characters of letters, digits, '-', '_' or '.'");

        if (name == "delete_state") {
            if (!m_host.states->Remove(stateName))
                return Err("no snapshot named '" + stateName + "'");
            return json{{"ok", true}, {"name", stateName}};
        }

        if (!requireEmu())
            return Err("emulator not available");
        const std::string document =
            m_host.activeDocumentPath ? m_host.activeDocumentPath() : std::string{};

        if (name == "save_state") {
            const bool persist = args.value("persist", false);
            const auto r = SaveEmulatorState(*emu, *m_host.states, stateName, document, persist);
            if (!r.ok)
                return Err(r.error);
            json j{{"ok", true},
                   {"name", stateName},
                   {"bytes", r.info.bytes},
                   {"frame", r.info.frame},
                   {"persisted", r.info.persisted}};
            if (!r.warning.empty())
                j["warning"] = r.warning;
            return j;
        }

        // load_state
        const auto r = LoadEmulatorState(*emu, *m_host.states, stateName, document);
        if (!r.ok)
            return Err(r.error);

        // The restore may have replaced the core's breakpoint/watchpoint tables, so make
        // the host re-read them rather than trusting its cached view.
        if (m_host.refreshBreakpoints)
            m_host.refreshBreakpoints();
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return json{{"ok", true},
                    {"name", stateName},
                    {"bytes", r.info.bytes},
                    {"frame", r.info.frame},
                    {"frameCount", emu->GetTotalFrameCount()}};
    }

    if (name == "list_states") {
        if (!m_host.states)
            return Err("state snapshots unavailable");
        json arr = json::array();
        for (const auto& s : m_host.states->List()) {
            arr.push_back({{"name", s.name},
                           {"bytes", s.bytes},
                           {"frame", s.frame},
                           {"persisted", s.persisted},
                           {"coreVersion", s.coreVersion},
                           {"document", s.document},
                           {"created", s.created}});
        }
        return json{{"states", arr}, {"directory", m_host.states->Directory().string()}};
    }

    if (name == "add_breakpoint" || name == "remove_breakpoint") {
        uint32_t addr = 0;
        if (!GetAddrArg(args, "addr", addr))
            return AddrArgError(args, "addr");
        if (name == "remove_breakpoint") {
            if (!m_host.removeBreakpoint)
                return Err("breakpoints unavailable");
            return json{{"ok", m_host.removeBreakpoint(addr)}};
        }

        const bool hasCondition = args.contains("register") || args.contains("operator") ||
                                  args.contains("value") || args.contains("mask");
        if (hasCondition && (!args.contains("register") || !args["register"].is_string() ||
                             !args.contains("operator") || !args["operator"].is_string() ||
                             !args.contains("value")))
            return Err("register, operator, and value are all required for a condition");
        if (args.contains("ignoreCount") && !args["ignoreCount"].is_number_integer())
            return Err("ignoreCount must be an integer");
        if (args.contains("oneShot") && !args["oneShot"].is_boolean())
            return Err("oneShot must be a boolean");
        const int ignoreCount = args.value("ignoreCount", 0);
        if (ignoreCount < 0)
            return Err("ignoreCount must be >= 0");
        const bool oneShot = args.value("oneShot", false);
        const bool smart = hasCondition || ignoreCount > 0 || oneShot;
        if (!smart) {
            if (!m_host.addBreakpoint)
                return Err("breakpoints unavailable");
            const bool added = m_host.addBreakpoint(addr);
            // ok:false has exactly one meaning here - an identical PC breakpoint was
            // already set - so say that rather than leaving a bare false to be read as
            // "the core refused it".
            json j{{"ok", added}, {"addr", ToHex(addr, 8)}, {"type", "PC"}};
            if (!added)
                j["reason"] = "a PC breakpoint already exists at this address";
            const bool running = m_host.isRunning && m_host.isRunning();
            j["armed"] = running;
            if (!running)
                j["note"] = "the emulator is not running; the breakpoint is held by the "
                            "host and injected into the core when it starts";
            return j;
        }
        if (!m_host.addSmartBreakpoint)
            return Err("conditional breakpoints unavailable");

        uint32_t value = 0;
        uint32_t mask = 0xFFFFFFFFu;
        if (hasCondition && !GetAddrArg(args, "value", value))
            return AddrArgError(args, "value");
        if (args.contains("mask") && !GetAddrArg(args, "mask", mask))
            return AddrArgError(args, "mask");
        const std::string reg =
            hasCondition ? UpperTrimmed(args["register"].get<std::string>()) : "";
        const std::string oper = hasCondition ? args["operator"].get<std::string>() : "";
        const bool ok = m_host.addSmartBreakpoint(addr, hasCondition, reg, oper, value, mask,
                                                  ignoreCount, oneShot);
        return json{{"ok", ok},
                    {"conditional", hasCondition},
                    {"ignoreCount", ignoreCount},
                    {"oneShot", oneShot}};
    }
    if (name == "list_breakpoints") {
        if (!m_host.listBreakpoints)
            return Err("breakpoints unavailable");
        json arr = json::array();
        for (auto& [addr, enabled] : m_host.listBreakpoints())
            arr.push_back({{"addr", ToHex(addr, 8)}, {"enabled", enabled}});
        return json{{"breakpoints", arr}};
    }
    if (name == "add_watchpoint") {
        WatchpointSpec spec;
        if (!GetAddrArg(args, "addr", spec.addr))
            return AddrArgError(args, "addr");

        if (args.contains("len")) {
            uint32_t len = 0;
            if (!ParseAddr(args["len"], len))
                return Err("len must be an integer or hex string");
            if (len == 0 || len > 0x1000000)
                return Err("len must be between 1 and 0x1000000");
            spec.len = len;
        } else {
            spec.len = 1;
        }

        // access defaults to write, which is what the old address-only tool did.
        std::string access = "w";
        if (args.contains("access")) {
            if (!args["access"].is_string())
                return Err("access must be a string");
            access = args["access"].get<std::string>();
            std::transform(access.begin(), access.end(), access.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        spec.read = spec.write = spec.exec = false;
        for (char c : access) {
            if (c == 'r')
                spec.read = true;
            else if (c == 'w')
                spec.write = true;
            else if (c == 'x' || c == 'i')
                spec.exec = true;
            else
                return Err("access may only contain r, w and x (e.g. \"r\", \"rw\", \"x\")");
        }
        if (!spec.read && !spec.write && !spec.exec)
            return Err("access must include at least one of r, w, x");

        if (args.contains("source")) {
            if (!args["source"].is_string())
                return Err("source must be a string");
            spec.source = UpperTrimmed(args["source"].get<std::string>());
            static const std::array<const char*, 11> kSources = {
                "CPU", "CPUD", "CPUI", "COP", "BLT", "DSK", "BPL", "SPR", "AUD", "DMA", "ALL"};
            if (std::find_if(kSources.begin(), kSources.end(),
                             [&](const char* s) { return spec.source == s; }) == kSources.end())
                return Err("source must be one of CPU, CPUD, CPUI, COP, BLT, DSK, BPL, SPR, "
                           "AUD, DMA, ALL");
        }

        if (args.contains("value")) {
            uint32_t value = 0;
            if (!ParseAddr(args["value"], value))
                return Err("value must be an integer or hex string");
            spec.valueEnabled = true;
            spec.value = value;
        }

        if (args.contains("mode")) {
            if (!args["mode"].is_string())
                return Err("mode must be \"break\" or \"log\"");
            const std::string mode = UpperTrimmed(args["mode"].get<std::string>());
            if (mode == "LOG")
                spec.logOnly = true;
            else if (mode != "BREAK")
                return Err("mode must be \"break\" or \"log\"");
        }
        if (args.contains("maxHits")) {
            if (!args["maxHits"].is_number_integer() && !args["maxHits"].is_number_unsigned())
                return Err("maxHits must be an integer");
            const int64_t maxHits = args["maxHits"].get<int64_t>();
            if (maxHits < 0)
                return Err("maxHits must be >= 0");
            spec.maxHits = static_cast<uint32_t>(std::min<int64_t>(maxHits, 1000000));
        }
        if (args.contains("oneShot")) {
            if (!args["oneShot"].is_boolean())
                return Err("oneShot must be a boolean");
            spec.oneShot = args["oneShot"].get<bool>();
        }

        if (!m_host.addWatchpointFull)
            return Err("watchpoints unavailable");
        std::string error;
        const int slot = m_host.addWatchpointFull(spec, error);
        if (slot < 0)
            return Err(error.empty() ? "could not add watchpoint" : error);
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return json{{"ok", true},
                    {"slot", slot},
                    {"addr", ToHex(spec.addr, 8)},
                    {"len", spec.len},
                    {"access", spec.AccessString()},
                    {"source", spec.source},
                    {"mode", spec.logOnly ? "log" : "break"}};
    }
    if (name == "remove_watchpoint") {
        if (args.contains("slot")) {
            if (!args["slot"].is_number_integer() && !args["slot"].is_number_unsigned())
                return Err("slot must be an integer");
            if (!m_host.removeWatchpointSlot)
                return Err("watchpoints unavailable");
            const int slot = args["slot"].get<int>();
            if (!m_host.removeWatchpointSlot(slot))
                return Err("no watchpoint in slot " + std::to_string(slot));
            if (m_host.onStateChanged)
                m_host.onStateChanged();
            return json{{"ok", true}, {"slot", slot}};
        }
        uint32_t addr = 0;
        if (!args.contains("addr") || args["addr"].is_null())
            return Err("addr or slot required");
        if (!GetAddrArg(args, "addr", addr))
            return AddrArgError(args, "addr");
        // Address form: remove whichever watchpoint is registered at that address,
        // whatever its access type.
        if (m_host.listWatchpointsFull && m_host.removeWatchpointSlot) {
            for (const auto& w : m_host.listWatchpointsFull()) {
                if (w.addr != addr)
                    continue;
                if (!m_host.removeWatchpointSlot(w.slot))
                    return Err("could not remove watchpoint at " + ToHex(addr, 8));
                if (m_host.onStateChanged)
                    m_host.onStateChanged();
                return json{{"ok", true}, {"slot", w.slot}, {"addr", ToHex(addr, 8)}};
            }
            return Err("no watchpoint at " + ToHex(addr, 8));
        }
        if (!m_host.removeWatchpoint)
            return Err("watchpoints unavailable");
        return json{{"ok", m_host.removeWatchpoint(addr)}};
    }
    if (name == "list_watchpoints") {
        if (m_host.listWatchpointsFull) {
            json arr = json::array();
            for (const auto& w : m_host.listWatchpointsFull()) {
                arr.push_back(
                    {{"slot", w.slot},
                     {"addr", ToHex(w.addr, 8)},
                     {"len", w.len},
                     {"access", w.AccessString()},
                     {"source", w.source},
                     {"mode", w.logOnly ? "log" : "break"},
                     {"enabled", w.enabled},
                     {"hits", w.hits},
                     {"maxHits", w.maxHits},
                     {"oneShot", w.oneShot},
                     {"value", w.valueEnabled ? json(ToHex(w.value, 8)) : json(nullptr)}});
            }
            return json{{"watchpoints", arr}};
        }
        if (!m_host.listWatchpoints)
            return Err("watchpoints unavailable");
        json arr = json::array();
        for (auto addr : m_host.listWatchpoints())
            arr.push_back(ToHex(addr, 8));
        return json{{"watchpoints", arr}};
    }
    if (name == "get_watchpoint_log") {
        if (!m_host.getWatchpointLog)
            return Err("watchpoints unavailable");
        int slot = -1;
        if (args.contains("slot")) {
            if (!args["slot"].is_number_integer() && !args["slot"].is_number_unsigned())
                return Err("slot must be an integer");
            slot = args["slot"].get<int>();
        }
        int limit = 0;
        if (args.contains("limit")) {
            if (!args["limit"].is_number_integer() && !args["limit"].is_number_unsigned())
                return Err("limit must be an integer");
            limit = args["limit"].get<int>();
        }
        const bool clear = args.value("clear", false);
        const auto hits = m_host.getWatchpointLog(slot, limit, clear);
        json arr = json::array();
        for (const auto& h : hits) {
            std::string access;
            if (h.rwi & 1)
                access += 'r';
            if (h.rwi & 2)
                access += 'w';
            if (h.rwi & 4)
                access += 'x';
            arr.push_back({{"slot", h.slot},
                           {"pc", ToHex(h.pc, 8)},
                           {"addr", ToHex(h.addr, 8)},
                           {"value", ToHex(h.value, 8)},
                           {"access", access},
                           {"size", h.size},
                           {"source", h.source},
                           {"frame", h.frame}});
        }
        return json{{"hits", arr}, {"count", arr.size()}};
    }

    if (name == "add_kprintf_breakpoint") {
        if (!m_host.addKprintfBreakpoint)
            return Err("KPrintF breakpoints unavailable");
        const std::string text = args.value("text", std::string{});
        if (text.empty())
            return Err("text required");
        const std::string modeName = args.value("match", std::string("contains"));
        auto mode = KprintfBreakpoints::ParseMatch(modeName);
        if (!mode)
            return Err("match must be contains, exact, prefix or regex");
        const int id = m_host.addKprintfBreakpoint(text, *mode, args.value("caseSensitive", false),
                                                   args.value("oneShot", false));
        if (id == 0) {
            return Err(*mode == KprintfMatch::Regex
                           ? "duplicate rule, or the regex failed to compile"
                           : "a KPrintF breakpoint with that text and match mode already exists");
        }
        return json{{"ok", true}, {"id", id}};
    }
    if (name == "remove_kprintf_breakpoint") {
        if (!m_host.removeKprintfBreakpoint || !m_host.listKprintfBreakpoints)
            return Err("KPrintF breakpoints unavailable");
        // Accept either the id from add_kprintf_breakpoint or the rule text verbatim.
        int id = args.value("id", 0);
        if (id == 0) {
            const std::string text = args.value("text", std::string{});
            if (text.empty())
                return Err("id or text required");
            for (const auto& e : m_host.listKprintfBreakpoints()) {
                if (e.text == text) {
                    id = e.id;
                    break;
                }
            }
            if (id == 0)
                return Err("no KPrintF breakpoint with that text");
        }
        return json{{"ok", m_host.removeKprintfBreakpoint(id)}};
    }
    if (name == "list_kprintf_breakpoints") {
        if (!m_host.listKprintfBreakpoints)
            return Err("KPrintF breakpoints unavailable");
        json arr = json::array();
        for (const auto& e : m_host.listKprintfBreakpoints()) {
            json node{{"id", e.id},
                      {"text", e.text},
                      {"match", KprintfBreakpoints::MatchName(e.match)},
                      {"caseSensitive", e.caseSensitive},
                      {"oneShot", e.oneShot},
                      {"enabled", e.enabled},
                      {"hits", e.hits}};
            if (!e.lastLine.empty())
                node["lastLine"] = e.lastLine;
            arr.push_back(std::move(node));
        }
        return json{{"kprintfBreakpoints", arr}};
    }
    if (name == "clear_kprintf_breakpoints") {
        if (!m_host.clearKprintfBreakpoints)
            return Err("KPrintF breakpoints unavailable");
        m_host.clearKprintfBreakpoints();
        return json{{"ok", true}};
    }

    return std::nullopt; // not one of this category's tools
}

std::optional<nlohmann::json>
McpToolHandler::DispatchDocumentTools(const std::string& name, const json& args, AmigaEmulator* emu)
{
    [[maybe_unused]] auto requireEmu = [&]() -> bool { return emu != nullptr; };

    // ---- Documents ----
    if (name == "load_document") {
        if (!m_host.loadDocument)
            return Err("load_document unavailable");
        std::string path = args.value("path", std::string{});
        if (path.empty())
            return Err("path required");
        bool force = args.value("force", false);
        // Always re-read from disk. This used to focus an already-open tab and return
        // ok:true without touching the file, which silently served the previous build
        // of a rebuilt ADF for the rest of the session.
        const bool reload = args.value("reload", true);
        bool ok = m_host.loadDocument(path, force, reload);
        if (!ok)
            return Err("load failed (the open document may have unsaved changes; pass "
                       "force:true to replace it)");
        json j{{"ok", true}, {"path", path}, {"reloadedFromDisk", reload}};
        if (!reload)
            j["note"] = "reload:false - if this path was already open, the in-memory copy "
                        "was reused and any on-disk changes are NOT loaded";
        return j;
    }
    if (name == "load_document_bytes") {
        // For a caller that has the image but no file - a page in a browser, which cannot
        // write one. The bytes go to a file of that name in Peekaboo's own temp folder and
        // are loaded from there exactly as load_document would, so the same name again is
        // the same path: a disk swap into whatever drive holds it.
        if (!m_host.loadDocument)
            return Err("load_document_bytes unavailable");
        const std::string fileName = args.value("name", std::string{});
        const bool nameOk = !fileName.empty() && fileName.size() <= 64 && fileName.front() != '.' &&
                            fileName.find('.') != std::string::npos &&
                            std::all_of(fileName.begin(), fileName.end(), [](char c) {
                                return std::isalnum(static_cast<unsigned char>(c)) || c == '.' ||
                                       c == '-' || c == '_';
                            });
        if (!nameOk)
            return Err("name required: a plain file name with an extension (letters, digits, "
                       ". - _), which says what the image is - game.adf, demo.exe");
        auto bytes = Base64Decode(args.value("data", std::string{}));
        if (!bytes || bytes->empty())
            return Err("data required: the image, base64");
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec) / "peekaboo-documents";
        if (ec || (std::filesystem::create_directories(dir, ec), ec))
            return Err("cannot make the temp folder: " + ec.message());
        const std::string path = (dir / fileName).string();
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes->data()),
                      static_cast<std::streamsize>(bytes->size()));
            if (!out)
                return Err("cannot write " + path);
        }
        if (!m_host.loadDocument(path, args.value("force", false), true))
            return Err("load failed (the open document may have unsaved changes; pass "
                       "force:true to replace it)");
        return json{{"ok", true}, {"path", path}, {"bytes", bytes->size()}};
    }
    if (name == "list_documents") {
        if (!m_host.listDocuments)
            return Err("list_documents unavailable");
        json arr = json::array();
        int idx = 0;
        for (auto& d : m_host.listDocuments())
            arr.push_back({{"index", idx++}, {"name", d}});
        return json{{"documents", arr}};
    }
    if (name == "run_document") {
        if (!m_host.runDocument)
            return Err("run_document unavailable");
        if (!args.contains("index") || !args["index"].is_number_integer())
            return Err("index required");
        m_host.runDocument(args["index"].get<int>());
        return json{{"ok", true}};
    }
    if (name == "close_document") {
        if (!m_host.closeDocument)
            return Err("close_document unavailable");
        if (!args.contains("index") || !args["index"].is_number_integer())
            return Err("index required");
        int idx = args["index"].get<int>();
        if (m_host.listDocuments) {
            int count = static_cast<int>(m_host.listDocuments().size());
            if (idx < 0 || idx >= count)
                return Err(count == 0 ? std::string("no open documents")
                                      : fmt::format("index out of range (0-{})", count - 1));
        }
        bool force = args.value("force", false);
        if (!m_host.closeDocument(idx, force))
            return Err("close refused (document has unsaved changes; pass force:true)");
        if (m_host.onStateChanged)
            m_host.onStateChanged();
        return json{{"ok", true}, {"closed", idx}};
    }

    return std::nullopt; // not one of this category's tools
}

std::optional<nlohmann::json>
McpToolHandler::DispatchConsoleTools(const std::string& name, const json& args, AmigaEmulator* emu)
{
    [[maybe_unused]] auto requireEmu = [&]() -> bool { return emu != nullptr; };

    // ---- Console passthrough ----
    if (name == "exec_console") {
        if (!m_host.execConsole)
            return Err("console unavailable");
        std::string cmd = args.value("cmd", std::string{});
        if (cmd.empty())
            return Err("cmd required");
        return json{{"output", m_host.execConsole(cmd)}};
    }

    // Console-specific tools are declarative adapters over ArCommandInterpreter. This preserves
    // one implementation of every command while still giving MCP clients discoverable,
    // operation-specific tools instead of forcing them through exec_console.
    if (auto result = DispatchMcpConsoleTool(name, args, m_host.execConsole))
        return std::move(*result);

    // Reverse-engineering tools (memory diffs, address explanation, library-call
    // tracing, segment/hunk mapping, ADF filesystem, reset survivors, trace
    // search) live in their own file so this one does not keep growing.
    if (auto result = DispatchMcpReTool(name, args, m_host))
        return std::move(*result);

    if (auto result = DispatchMcpUiTool(name, args, m_host))
        return std::move(*result);

    return std::nullopt; // not one of this category's tools
}

std::optional<nlohmann::json>
McpToolHandler::DispatchProfilerTools(const std::string& name, const json& args, AmigaEmulator* emu)
{
    [[maybe_unused]] auto requireEmu = [&]() -> bool { return emu != nullptr; };

    // ---- Profiler ----
    if (name == "set_cpu_trace") {
        if (!requireEmu())
            return Err("emulator not available");
        if (!args.contains("enabled") || !args["enabled"].is_boolean())
            return Err("enabled (boolean) required");
        emu->SetCpuTraceEnabled(args["enabled"].get<bool>());
        return json{{"ok", true}, {"cpuTrace", emu->IsCpuTraceEnabled()}};
    }

    if (name == "get_profile") {
        if (!m_host.getProfile)
            return Err("profiler unavailable");
        ProfileView view = ProfileView::Functions;
        if (args.contains("view") && args["view"].is_string()) {
            if (!ParseProfileView(args["view"].get<std::string>(), view))
                return Err("unknown view (functions|instructions|flat|flame|callstack)");
        }
        ProfileQuery q;
        q.view = view;
        int limit = args.value("limit", 32);
        q.limit = limit < 0 ? 0 : (limit > 1000 ? 1000 : limit);
        return ProfileToJson(m_host.getProfile(q), view);
    }

    if (name == "get_blitter_log") {
        if (!m_host.getBlitLog)
            return Err("blitter log unavailable");
        int limit = args.value("limit", 64);
        limit = limit < 0 ? 0 : (limit > 2000 ? 2000 : limit);
        json r = BlitLogToJson(m_host.getBlitLog(limit));
        // The log covers the current frame up to *now*, so how much of the frame it represents
        // depends entirely on where the beam is.
        if (const json beam = BeamJson(emu); !beam.is_null())
            r["beam"] = beam;
        return r;
    }

    if (name == "get_crass_profile") {
        if (!m_host.getCrassProfile)
            return Err("crass profile unavailable");
        const CrassProfileStats p = m_host.getCrassProfile();
        json r;
        r["present"] = p.present;
        if (!p.present) {
            // The usual reason is simply that the program was not built with PROFILE. It is
            // also what a stale registry entry reports once the block fails validation, which
            // is indistinguishable from here and equally means "there are no numbers".
            r["reason"] = "no valid PROFILE block registered by the running program";
            return r;
        }
        r["blockAddress"] = ToHex(p.blockAddress, 8);
        r["version"] = p.version;
        r["windowFields"] = p.windowFields;
        r["linesPer2Fields"] = p.linesPer2Fields;
        r["timingOk"] = p.timingOk;
        if (!p.timingOk)
            r["hint"] = "the program's raster-line counter (CIA-B TOD/HSYNC) was not running at "
                        "profile_init(), so every line figure below is meaningless";

        json frames;
        frames["rendered"] = p.rendered; // live, current window
        frames["worstGap"] = p.worstGap; // live, current window, in fields
        frames["lastFields"] = p.lastFields;
        frames["lastRendered"] = p.lastRendered;
        frames["lastDropped"] = p.lastDropped;
        frames["lastWorstGap"] = p.lastWorstGap;
        frames["totalRendered"] = p.totalRendered;
        frames["totalDropped"] = p.totalDropped;
        frames["windowComplete"] = p.lastFields != 0;
        r["frames"] = frames;

        json sections = json::array();
        for (const auto& s : p.sections) {
            json js;
            js["name"] = s.name;
            js["lastLines"] = s.lastLines; // most recent STOP, live
            js["called"] = s.lastCount != 0;
            js["lastMin"] = s.lastMin;
            js["lastAvg"] = s.lastAvg;
            js["lastMax"] = s.lastMax;
            js["lastCount"] = s.lastCount;
            // The in-progress window, for a caller that does not want to wait for the next
            // report. `min` reads 65535 until the window's first STOP.
            js["windowTotal"] = s.total;
            js["windowCount"] = s.count;
            js["windowMin"] = s.min;
            js["windowMax"] = s.max;
            sections.push_back(js);
        }
        r["sections"] = sections;
        if (p.sectionsTruncated)
            r["sectionsTruncated"] = true;
        return r;
    }

    if (name == "get_exocet") {
        if (!m_host.getExocet)
            return Err("exocet unavailable");
        return ExocetStatusJson(m_host.getExocet());
    }

    if (name == "get_exocet_memory") {
        if (!m_host.getExocetMemory)
            return Err("exocet unavailable");
        return ExocetMemoryJson(m_host.getExocetMemory(), args);
    }

    // The rest work on the Exocet window's session (core/ExocetQuery.h, shared with the
    // console's EXO command)
    if (name.rfind("get_exocet_", 0) == 0 || name.find("_exocet") != std::string::npos ||
        name == "inject_music") {
        ExocetSession* s = m_host.exocetSession ? m_host.exocetSession() : nullptr;
        if (!s)
            return Err("exocet unavailable");
        // ExocetQuery reads its arguments with value(), which throws on a null
        static const json kNoArgs = json::object();
        const json& a = args.is_object() ? args : kNoArgs;
        if (name == "get_exocet_timeline")
            return ExocetTimelineJson(*s, a);
        if (name == "get_exocet_grid")
            return ExocetGridJson(*s, a);
        if (name == "get_exocet_keys")
            return ExocetKeysJson(*s, a);
        if (name == "get_exocet_music")
            return ExocetMusicJson(*s, a);
        if (name == "get_exocet_disk_map")
            return ExocetDiskMapJson(*s, a);
        if (name == "get_exocet_proof")
            return ExocetProofJson(*s);
        if (name == "get_exocet_issues")
            return ExocetIssuesJson(*s);
        if (name == "set_exocet_key" || name == "delete_exocet_key")
            return ExocetSetKeyJson(*s, a, name == "delete_exocet_key");
        if (name == "set_exocet_keys")
            return ExocetSetKeysJson(*s, a);
        if (name == "key_exocet_every_hit")
            return ExocetEveryHitJson(*s, a);
        if (name == "clear_exocet_track")
            return ExocetClearTrackJson(*s, a);
        if (name == "undo_exocet")
            return ExocetEditJson(*s, "undo");
        if (name == "redo_exocet")
            return ExocetEditJson(*s, "redo");
        if (name == "save_exocet")
            return ExocetEditJson(*s, "save");
        if (name == "check_exocet")
            return ExocetCheckJson(*s, a);
        if (name == "push_exocet")
            return ExocetPushJson(*s);
        if (name == "inject_music")
            return ExocetInjectJson(*s, a);
        if (name == "write_exocet_adf")
            return ExocetWriteJson(*s, a);
        if (name == "seek_exocet")
            return ExocetSeekJson(*s, a, m_host.frameHistorySeek);
        if (name == "manage_exocet_breakpoints")
            return ExocetBreakpointsJson(*s, a);
        if (name == "manage_exocet_bookmarks")
            return ExocetBookmarksJson(*s, a);
    }

    if (name == "get_frame_history") {
        if (!m_host.getFrameHistory)
            return Err("frame history unavailable");
        const FrameHistoryStats st = m_host.getFrameHistory();
        json r;
        r["recording"] = st.recording;
        r["empty"] = st.empty;
        r["oldestFrame"] = st.oldestFrame;
        r["newestFrame"] = st.newestFrame;
        r["frameCount"] = st.frameCount;
        r["keyframeCount"] = st.keyframeCount;
        r["bytesUsed"] = st.bytesUsed;
        r["bytesBudget"] = st.bytesBudget;
        r["saveStateBytes"] = st.blobSize;
        r["evictedFrames"] = st.evictedFrames;
        // Places where the recorded frame numbers skip. Playback steps over them, but a
        // seek snaps to the nearest frame that exists, so this explains a goto_frame that
        // reports landing a frame or two away from the one asked for.
        r["gapCount"] = st.gapCount;
        r["avgDeltaBytes"] = st.avgDeltaBytes;
        r["avgRecordMs"] = st.avgRecordMs;
        r["estimatedCapacityFrames"] = st.estimatedCapacityFrames;
        if (m_host.getFrameHistoryPosition)
            r["position"] = m_host.getFrameHistoryPosition();
        if (m_host.frameHistoryPlayback) {
            const auto pb = m_host.frameHistoryPlayback();
            static const char* kModes[] = {"forward", "reverse", "pingpong"};
            r["playing"] = pb.playing;
            r["playDirection"] = kModes[(pb.mode >= 0 && pb.mode <= 2) ? pb.mode : 0];
            r["playSpeed"] = pb.speed;
            r["looping"] = pb.looping;
            if (pb.looping) {
                r["loopStart"] = pb.loopStart;
                r["loopEnd"] = pb.loopEnd;
            }
        }
        if (!st.hint.empty())
            r["hint"] = st.hint;
        return r;
    }

    if (name == "rewind" || name == "goto_frame") {
        const bool isGoto = (name == "goto_frame");
        if (isGoto ? !m_host.frameHistorySeek : !m_host.frameHistoryRewind)
            return Err("frame history unavailable");

        std::string err;
        if (isGoto) {
            if (!args.contains("frame"))
                return Err("goto_frame requires `frame`");
            err = m_host.frameHistorySeek(args["frame"].get<uint64_t>());
        } else {
            uint64_t count = args.value("frames", static_cast<uint64_t>(1));
            if (count == 0)
                count = 1;
            err = m_host.frameHistoryRewind(count);
        }
        if (!err.empty())
            return Err(err);

        json r;
        r["ok"] = true;
        if (m_host.getFrameHistoryPosition) {
            const uint64_t at = m_host.getFrameHistoryPosition();
            r["frame"] = at;
            // A seek snaps onto the nearest frame actually recorded, so say when that is
            // not the frame that was asked for rather than let the caller assume it was.
            if (isGoto && at != args["frame"].get<uint64_t>()) {
                r["note"] = "landed on the nearest recorded frame; frame " +
                            std::to_string(args["frame"].get<uint64_t>()) +
                            " was never recorded (see gapCount in get_frame_history)";
            }
        }
        return r;
    }

    if (name == "clear_frame_history") {
        if (!m_host.frameHistoryClear)
            return Err("frame history unavailable");
        m_host.frameHistoryClear();
        json r;
        r["ok"] = true;
        return r;
    }

    if (name == "loop_frames") {
        if (!m_host.frameHistoryLoop)
            return Err("frame history unavailable");
        const uint64_t a = args.value("start", static_cast<uint64_t>(0));
        const uint64_t b = args.value("end", static_cast<uint64_t>(0));
        // The range is optional: the transport arguments alone act on whatever is already
        // playing, which is how you slow down or reverse a loop that is already running.
        if (a || b) {
            const std::string err = m_host.frameHistoryLoop(a, b);
            if (!err.empty())
                return Err(err);
        }
        if (args.contains("direction") && m_host.frameHistoryPlayMode) {
            const std::string d = args.value("direction", std::string("forward"));
            if (d != "forward" && d != "reverse" && d != "pingpong")
                return Err("direction must be forward, reverse or pingpong");
            m_host.frameHistoryPlayMode(d == "reverse" ? 1 : d == "pingpong" ? 2 : 0);
        }
        if (args.contains("speed") && m_host.frameHistoryPlaySpeed)
            m_host.frameHistoryPlaySpeed(args.value("speed", 1.0));
        if (args.contains("step") && m_host.frameHistoryStep) {
            const int count = args.value("step", 0);
            const int delta = count < 0 ? -1 : 1;
            for (int i = 0, n = count < 0 ? -count : count; i < n; ++i) {
                const std::string err = m_host.frameHistoryStep(delta);
                if (!err.empty())
                    return Err(err);
            }
        }
        if (args.contains("playing") && m_host.frameHistoryPlay)
            m_host.frameHistoryPlay(args.value("playing", true));

        json r;
        r["ok"] = true;
        if (m_host.frameHistoryPlayback) {
            const auto pb = m_host.frameHistoryPlayback();
            r["looping"] = pb.looping;
            r["playing"] = pb.playing;
            r["playSpeed"] = pb.speed;
        } else {
            r["looping"] = (b != 0);
        }
        return r;
    }

    if (name == "diff_frames") {
        if (!m_host.frameHistoryDiff)
            return Err("frame history unavailable");
        if (!args.contains("frameA") || !args.contains("frameB"))
            return Err("diff_frames requires `frameA` and `frameB`");

        int limit = args.value("limit", 256);
        limit = limit <= 0 ? 4096 : (limit > 4096 ? 4096 : limit);

        std::vector<FrameDiffRun> runs;
        bool truncated = false;
        const std::string err =
            m_host.frameHistoryDiff(args["frameA"].get<uint64_t>(), args["frameB"].get<uint64_t>(),
                                    runs, static_cast<size_t>(limit), truncated);
        if (!err.empty())
            return Err(err);

        json out = json::array();
        uint64_t total = 0;
        for (const auto& run : runs) {
            total += run.length;
            out.push_back({{"address", run.address},
                           {"addressHex", ToHex(run.address, 8)},
                           {"length", run.length},
                           {"region", run.region}});
        }
        json r;
        r["runs"] = out;
        r["runCount"] = runs.size();
        r["bytesChanged"] = total;
        r["truncated"] = truncated;
        return r;
    }

    if (name == "get_register_log") {
        if (!m_host.getRegLog)
            return Err("register log unavailable");
        int limit = args.value("limit", 64);
        limit = limit < 0 ? 0 : (limit > 2000 ? 2000 : limit);
        json r = RegLogToJson(m_host.getRegLog(limit));
        // Same reasoning as the blitter log: the log covers the current frame up to *now*.
        if (const json beam = BeamJson(emu); !beam.is_null())
            r["beam"] = beam;
        return r;
    }

    if (name == "get_audio_stats") {
        if (!m_host.getAudioStats)
            return Err("audio statistics unavailable");
        const AudioLogSnapshot s = m_host.getAudioStats();
        if (!s.available)
            return Err("emulator is not running");

        auto tapJson = [](const AudioTapStatus& t) {
            return json{{"recording", t.recording},
                        {"path", t.path},
                        {"sampleRate", t.sampleRate},
                        {"channels", t.channels},
                        {"framesWritten", t.framesWritten},
                        {"framesDroppedStaging", t.framesDroppedStaging},
                        {"seconds", t.seconds}};
        };

        return json{
            {"initialized", s.initialized},
            {"muted", s.muted},
            {"resampling", s.resampling},
            {"backend", s.backend},
            {"coreSampleRate", s.coreSampleRate},
            {"deviceSampleRate", s.deviceSampleRate},
            {"targetFps", s.targetFps},
            {"latencyMs", s.latencyMs},
            {"device",
             {{"periodFrames", s.devicePeriodFrames}, {"bufferFrames", s.deviceBufferFrames}}},
            {"producer",
             {{"framesProduced", s.framesProduced},
              {"framesDroppedOverrun", s.framesDroppedOverrun},
              {"blockCount", s.blockCount},
              {"lastBlockFrames", s.lastBlockFrames},
              {"minBlockFrames", s.minBlockFrames},
              {"maxBlockFrames", s.maxBlockFrames},
              {"meanBlockFrames", s.meanBlockFrames},
              {"gapMeanMs", s.produceGapMeanMs},
              {"gapMaxMs", s.produceGapMaxMs},
              {"stalls", s.produceStalls},
              {"gapHistogram", {{"edgesMs", s.produceGapEdgesMs}, {"counts", s.produceGapHist}}},
              {"updateMeanMs", s.updateMeanMs},
              {"updateMaxMs", s.updateMaxMs},
              {"updateSlow", s.updateSlow}}},
            {"discardedTime",
             {{"events", s.timeDiscardEvents},
              {"frames", s.timeDiscardedFrames},
              {"elapsedClamp", s.timeDiscardElapsedClamp},
              {"accumulatorClamp", s.timeDiscardAccumulatorClamp},
              {"frameStopCap", s.timeDiscardFrameStopCap}}},
            {"consumer",
             {{"framesConsumed", s.framesConsumed},
              {"framesHeld", s.framesHeld},
              {"framesDroppedResampler", s.framesDroppedResampler},
              {"deviceCallbacks", s.deviceCallbacks},
              {"underrunCallbacks", s.underrunCallbacks}}},
            {"ring",
             {{"size", s.ringSize},
              {"level", s.ringLevel},
              {"levelMin", s.ringLevelMin},
              {"levelMax", s.ringLevelMax},
              {"levelMean", s.ringLevelMean},
              {"target", s.ringTarget},
              {"framesTrimmed", s.framesTrimmed},
              {"trimEvents", s.trimEvents},
              {"levelMinPlaying", s.ringLevelMinPlaying},
              {"pacePpm", s.pacePpm}}},
            {"taps", {{"core", tapJson(s.coreTap)}, {"device", tapJson(s.deviceTap)}}}};
    }

    if (name == "get_paula_state") {
        if (!m_host.getPaulaState)
            return Err("Paula state unavailable");
        const PaulaStateSnapshot s = m_host.getPaulaState();
        if (!s.available)
            return Err("emulator is not running");

        json channels = json::array();
        for (const auto& c : s.channels) {
            channels.push_back(json{{"channel", c.index},
                                    {"lc", c.lc},
                                    {"len", c.len},
                                    {"per", c.per},
                                    {"vol", c.vol},
                                    {"hz", c.hz},
                                    {"note", c.note},
                                    {"dmaOn", c.dmaOn},
                                    {"audible", c.audible},
                                    {"intPending", c.intPending},
                                    {"intEnabled", c.intEnabled},
                                    {"attachVolume", c.attachVolume},
                                    {"attachPeriod", c.attachPeriod},
                                    {"sampleCount", c.sampleCount},
                                    {"peak", c.peak},
                                    {"rms", c.rms}});
        }

        static const char* kModels[4] = {"none", "A500", "A1200", "A500_fixed"};
        return json{
            {"traceAvailable", s.traceAvailable},
            {"masterDmaOn", s.masterDmaOn},
            {"hostMuted", s.hostMuted},
            {"channelMask", s.channelMask},
            {"ledFilterOn", s.ledFilterOn},
            {"filterModel", kModels[s.filterModel >= 0 && s.filterModel < 4 ? s.filterModel : 0]},
            {"channels", channels}};
    }

    if (name == "start_audio_record" || name == "stop_audio_record") {
        if (!m_host.audioRecord)
            return Err("audio recording unavailable");

        AudioRecordRequest req;
        if (name == "stop_audio_record") {
            req.stop = true;
        } else {
            const std::string tap = args.value("tap", std::string("both"));
            if (tap == "core") {
                req.tapCore = true;
            } else if (tap == "device") {
                req.tapDevice = true;
            } else if (tap == "both") {
                req.tapCore = true;
                req.tapDevice = true;
            } else {
                return Err("tap must be one of: core, device, both");
            }
            req.path = args.value("path", std::string());
            if (req.path.empty())
                return Err("path is required");
            req.seconds = args.value("seconds", 0.0);
            if (req.seconds < 0.0)
                return Err("seconds must not be negative");
        }

        const AudioRecordResult r = m_host.audioRecord(req);
        if (!r.ok)
            return Err(r.message);
        return json{{"ok", true}, {"message", r.message}};
    }

    if (name == "get_disk_log") {
        if (!m_host.getDiskLog)
            return Err("disk log unavailable");
        DiskLogQuery q;
        q.limit = args.value("limit", 64);
        q.limit = q.limit < 0 ? 0 : (q.limit > 2000 ? 2000 : q.limit);
        const std::string dir = args.value("direction", std::string("any"));
        if (dir == "read")
            q.direction = 2;
        else if (dir == "write")
            q.direction = 3;
        else if (dir != "any")
            return Err("direction must be read, write or any");
        q.includeSectors = args.value("includeSectors", true);
        q.sinceSeq = args.value("sinceSeq", 0u);
        q.sinceSession = args.value("sinceSession", 0u);
        q.sinceOrdinal = args.value("sinceOrdinal", 0ull);
        const std::string order = args.value("order", std::string("newestFirst"));
        if (order == "newestFirst")
            q.newestFirst = true;
        else if (order == "oldestFirst")
            q.newestFirst = false;
        else
            return Err("order must be newestFirst or oldestFirst");
        return DiskLogToJson(m_host.getDiskLog(q));
    }

    if (name == "add_disk_breakpoint" || name == "remove_disk_breakpoint") {
        if (!m_host.armDiskBreak)
            return Err("disk breakpoints unavailable");
        if (name == "remove_disk_breakpoint")
            return json{{"ok", m_host.armDiskBreak(-1, -1, -1, -1, 0, 0)}};

        int access = 0;
        const std::string acc = args.value("access", std::string("any"));
        if (acc == "read")
            access = 1;
        else if (acc == "write")
            access = 2;
        else if (acc != "any")
            return Err("access must be read, write or any");

        // `track` is the cyl*2+side form the disk log reports; cyl/side may be given
        // separately instead. Both spellings resolve to the same pair of predicates.
        int cyl = args.value("cyl", -1);
        int side = args.value("side", -1);
        if (args.contains("track")) {
            const int track = args.value("track", -1);
            if (track < 0 || track > 159)
                return Err("track must be 0-159");
            cyl = track / 2;
            side = track % 2;
        }
        if (cyl > 82 || side > 1)
            return Err("cyl must be 0-82 and side 0-1");

        const int sector = args.value("sector", -1);
        if (sector > 10)
            return Err("sector must be 0-10");

        uint32_t addrLo = 0, addrHi = 0;
        if (BadOptionalAddr(args, "addrLo", addrLo))
            return AddrArgError(args, "addrLo");
        if (BadOptionalAddr(args, "addrHi", addrHi))
            return AddrArgError(args, "addrHi");
        if (addrHi != 0 && addrHi < addrLo)
            return Err("addrHi must be >= addrLo");

        const bool ok = m_host.armDiskBreak(access, cyl, side, sector, addrLo, addrHi);
        json r{{"ok", ok}, {"access", acc}};
        if (cyl >= 0)
            r["cyl"] = cyl;
        if (side >= 0)
            r["side"] = side;
        if (sector >= 0) {
            r["sector"] = sector;
            // Sector headers only exist once the data has arrived, so this predicate
            // is evaluated at completion; the reported pc is still the arming one.
            r["note"] = "sector filter breaks at transfer completion, not at the "
                        "arming instruction";
        }
        if (addrHi != 0)
            r["addrRange"] = ToHex(addrLo, 6) + "-" + ToHex(addrHi, 6);
        return r;
    }

    return std::nullopt; // not one of this category's tools
}

nlohmann::json McpToolHandler::Dispatch(const std::string& name, const json& args)
{
    // Hidden tools are unknown ones, as they are absent from the manifest
    if (IsPrivateTool(name) && !PrivateFeatures())
        return Err("unknown tool: " + name);

    auto* emu = m_host.emulator ? m_host.emulator() : nullptr;

    // One dispatcher per tool category (the former 1,900-line if/else chain, split along
    // its section comments). Each returns nullopt for names it does not own.
    if (auto r = DispatchStatusTools(name, args, emu))
        return *r;
    if (auto r = DispatchAnnotationTools(name, args, emu))
        return *r;
    if (auto r = DispatchWriteTools(name, args, emu))
        return *r;
    if (auto r = DispatchExecutionTools(name, args, emu))
        return *r;
    if (auto r = DispatchDocumentTools(name, args, emu))
        return *r;
    if (auto r = DispatchConsoleTools(name, args, emu))
        return *r;
    if (auto r = DispatchProfilerTools(name, args, emu))
        return *r;

    return Err("unknown tool: " + name);
}

void McpToolHandler::BuildManifest()
{
    json tools = json::array();

    auto addrSchema = []() {
        return json{{"type", json::array({"integer", "string"})},
                    {"description", "address: integer or hex string like \"0x1234\" / \"$1234\""}};
    };
    auto tool = [&](const char* name, const std::string& desc, json properties = json::object(),
                    json required = json::array()) {
        // A call site that writes `{}` for a parameterless tool hands us a null, not an empty
        // object (nlohmann reads an empty initializer list as null). MCP clients validate
        // inputSchema.properties as a record and reject the whole tools/list over one null, so
        // normalise here rather than trusting every call site to spell it json::object().
        if (!properties.is_object())
            properties = json::object();
        json schema = {{"type", "object"}, {"properties", properties}};
        if (!required.empty())
            schema["required"] = required;
        tools.push_back({{"name", name}, {"description", desc}, {"inputSchema", schema}});
    };

    tool("get_status",
         "Emulator status: core/game loaded, running/paused, model, versions, frame count, and — "
         "only once a core is loaded — `beam` ({vpos, hpos}, the current raster position) and "
         "`partialFrame`, true when the machine is stopped part-way through a frame. Those two "
         "keys are absent rather than null when there is no core, so check `coreLoaded` before "
         "reading them. Any result that depends on WHEN in the frame it was sampled (screenshot, "
         "read_memory, get_custom_registers, get_blitter_log) should be read against that beam "
         "position. `frameCount` is a host-side counter of frames run since the emulator started, "
         "not emulated time: load_state does not rewind it. It IS rebased to 0 by start() and by "
         "compile-and-run, but NOT by reset_emulator, which keeps counting - so a frame number "
         "only identifies a moment together with `frameSession`, which advances on exactly those "
         "rebases. Save both if you intend to reuse a frame number as a run_until_frame target.");
    tool("get_registers", "68k CPU registers (D0-7, A0-7, PC, SR + decoded flags incl. IPL "
                          "and STOP), the instruction at PC, the configured `cpu` (model, fpu, "
                          "mmu, addressBits) and the three `stack` pointers with the one A7 "
                          "currently is. Control/MMU/FPU registers: get_extended_cpu_registers.");
    tool("get_extended_cpu_registers",
         "Every CPU register beyond D/A/PC/SR, for the configured CPU (68000-68060): `stack` "
         "(USP, ISP - SSP on a 68000/010 - and MSP), `control` (VBR, SFC, DFC, CACR, CAAR, "
         "BUSCR, PCR), `mmu` (68030: TC, CRP, SRP as hi:lo, TT0/1, MMUSR; 68040/060: TC, URP, "
         "SRP, ITT0/1, DTT0/1, MMUSR) and `fpu` (FP0-FP7 as raw 96-bit extended plus decimal "
         "value, FPCR/FPSR decoded, FPIAR). Only registers this CPU has carry values; "
         "`unsupported` lists the others with the CPUs that have them.",
         {{"group",
           {{"type", "string"},
            {"enum", json::array({"all", "control", "mmu", "fpu"})},
            {"description", "which registers (default all); control includes the stacks"}}}});
    tool("get_custom_registers",
         "Amiga custom chip (DFF000) registers: DMA/interrupt/bitplane control, "
         "copper/bitplane/sprite pointers, colors, and the input registers JOY0DAT "
         "(Amiga port 0 / mouse port), JOY1DAT (port 1 / joystick port), POTGO and "
         "POTINP. The JOY registers are live chipset reads, so they track a stick set "
         "by set_joystick - but both read 0000 with nothing held, so sample them with a "
         "direction applied. Fire buttons live in CIA-A PRA, not here (get_cia_registers, "
         "or read_memory at $BFE001). Also returns `screenMode` — the decoded "
         "resolution (lores/hires/shres), interlace, bitplane count and display size in the "
         "pixels the program actually draws. Prefer that over deriving a size from DIWSTRT/"
         "DDFSTRT: those registers are in lores units at every resolution, so a 640x256 hires "
         "screen looks like 320x256 if you read them literally. Also returns `beam` "
         "({vpos, hpos}) when a core is loaded. Read the snapshot against it: several of these "
         "registers move during a frame — bitplane DMA advances bplpt as it fetches, and the "
         "copper rewrites colors per scanline — so the same list sampled at a different raster "
         "position legitimately holds different values.");
    tool("get_cia_registers", "CIA-A and CIA-B register snapshot.");
    tool("list_memory_regions", "List host-mapped Amiga memory regions (chip/slow/fast/ROM).");
    tool("read_memory",
         "Read bytes from emulated memory as hex + ASCII (len capped at 8192). Covers mapped "
         "RAM/ROM regions, the custom chip registers at $DFF000-$DFF1FF, and CIA space: CIA-A "
         "on odd addresses from $BFE001 (so $BFE001 is PRA - the fire buttons and drive "
         "status), CIA-B on even addresses from $BFD000, each register at +$100. CIA and "
         "custom reads come from a side-effect-free snapshot, so reading ICR does not clear it "
         "the way a real bus cycle would; get_cia_registers dumps both chips by name. Bytes no "
         "chip or region answers for come back as \"??\" with partial:true (and, inside CIA "
         "space, a `hint` naming the addresses that do decode) rather than as fake zeroes.",
         {{"addr", addrSchema()},
          {"len", {{"type", "integer"}, {"description", "byte count (default 256)"}}}},
         json::array({"addr"}));
    tool("disassemble",
         "Disassemble 68k instructions starting at addr (count capped at 512). Each line includes "
         "any existing label/comment/block annotation. Caution: UAE resolves indirect operands "
         "against the CPU's CURRENT register file, so a trailing '== $xxxxxxxx' on a form like "
         "(a3,$0002) is only correct for code the CPU is actually stopped in. When disassembling "
         "anywhere else, treat those resolved addresses as meaningless.",
         {{"addr", addrSchema()},
          {"count", {{"type", "integer"}, {"description", "instruction count (default 16)"}}}},
         json::array({"addr"}));
    tool("disassemble_copper",
         "Disassemble Copper instructions from COP1, COP2, an explicit address, or the current "
         "frame's execution Trace. Each instruction includes its four raw bytes, packed hex, "
         "words, text disassembly, and MOVE destination register name. Trace rows also include "
         "COP1/COP2 list assignment and raster timing. count defaults to 256, is capped at 4096 "
         "for lists and 16384 for Trace, and 0 requests the full capped result.",
         {{"source",
           {{"type", json::array({"integer", "string"})},
            {"description", "COP1, COP2, Trace, or an address such as $400 or 0x400"}}},
          {"count",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 256},
            {"description", "instruction count; 0 requests the full capped result"}}}},
         json::array({"source"}));
    tool("get_annotations", "List user/AI annotations (label/comment/block) in an optional range.",
         {{"start", addrSchema()}, {"end", addrSchema()}});
    tool("screenshot",
         "Capture the current emulator display as a PNG (base64). The capture is taken from the "
         "display texture *after* the core composited it, so it honours the debug display masks "
         "set by set_display_masks: any bitplane, sprite or playfield that is masked off is "
         "absent from the image. When masking is active the response also carries a "
         "`displayMasks` block and a `note`, so a missing layer is never mistaken for a bug in "
         "the program. Masking is cosmetic only — it changes what is drawn, never what the "
         "machine executes. Works while paused (changing a mask re-renders the current frame). "
         "Alongside the image the response carries `partialFrame`, and `beam` ({vpos, hpos}) "
         "when the loaded core exports it — these ride next to the PNG, so there is no need to "
         "call get_status to find out whether the picture can be trusted. When "
         "partialFrame is true the machine was stopped part-way through a field, so the image "
         "holds only the scanlines drawn so far and the previous frame shows through below them "
         "— a band drawn late in the field will be missing. That is a capture artefact, not a "
         "bug in the program: call end_frame (while paused) and screenshot again. The frame is "
         "never auto-completed, so a capture taken at a breakpoint stays exactly what the "
         "machine had drawn at that instant.");

    tool("set_label", "Set the symbolic label rendered above the instruction at addr.",
         {{"addr", addrSchema()}, {"text", {{"type", "string"}}}}, json::array({"addr", "text"}));
    tool("set_comment", "Set the trailing ';' comment on the instruction at addr.",
         {{"addr", addrSchema()}, {"text", {{"type", "string"}}}}, json::array({"addr", "text"}));
    tool("set_block", "Set the multi-line comment block (header) above the label at addr.",
         {{"addr", addrSchema()}, {"text", {{"type", "string"}}}}, json::array({"addr", "text"}));
    tool("remove_annotation",
         "Remove annotation at addr. Optional fields=[\"label\",\"comment\",\"block\"]; omit to "
         "remove all.",
         {{"addr", addrSchema()}, {"fields", {{"type", "array"}, {"items", {{"type", "string"}}}}}},
         json::array({"addr"}));

    tool("write_memory", "Write a hex byte string into emulated memory (fails on unmapped/ROM).",
         {{"addr", addrSchema()},
          {"hex", {{"type", "string"}, {"description", "even-length hex bytes, e.g. \"4e75\""}}}},
         json::array({"addr", "hex"}));
    tool("write_custom_register",
         "Write a 16-bit value to a custom chip register (full DFF000 addr).",
         {{"addr", addrSchema()}, {"value", addrSchema()}}, json::array({"addr", "value"}));

    tool("pause", "Pause the running emulator.");
    tool("resume", "Resume (arm trace + un-pause) the emulator.");
    tool("step", "Step one or more instructions (step into).",
         {{"count",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 65536},
            {"description", "instruction count (default 1)"}}}});
    tool("step_over", "Step over the instruction at PC (over BSR/JSR).");
    tool("step_frame",
         "Step whole emulated frames (emulator must be paused). Steps `count` frames "
         "(default 1) and returns the new frameCount.",
         {{"count", {{"type", "integer"}, {"description", "frames to step (default 1)"}}}});
    tool("run_until_frame",
         "Run the emulator until the emulated frame counter reaches at least `frame`, then "
         "auto-pause (like a breakpoint stop). Set turbo=true to run in warp mode. "
         "Prefer wait=true: the call then returns only once the run has ended, which is one "
         "round trip instead of polling get_status. Without it the call returns immediately and "
         "you must poll. A target at/behind the current frame is not an error — it returns "
         "alreadyPast:true, since \"run until at least frame N\" is already satisfied. "
         "With wait=true the reply says how it ended: reached:true at the target, "
         "stoppedEarly:true if a breakpoint got there first, or timedOut:true if the wait budget "
         "(" +
             std::to_string(kFrameWaitBudgetSeconds) +
             "s) ran out — a timeout does not cancel the run, so poll get_status or use turbo. "
             "`frame` is an absolute frame number on the CURRENT session's clock. start() "
             "rebases that clock to 0 (reset_emulator does not), so a target computed before a "
             "start() no longer means what it did: compare the `frameSession` in the reply "
             "against the one you saw when you computed the target.",
         {{"frame", {{"type", "integer"}, {"description", "target emulated frame number"}}},
          {"turbo", {{"type", "boolean"}, {"description", "run in warp mode (default false)"}}},
          {"wait",
           {{"type", "boolean"},
            {"default", false},
            {"description", "block until the run ends and report the final frame count, instead "
                            "of returning immediately"}}}},
         json::array({"frame"}));
    tool("start", "Start the emulator with the active document (same as the Run/F5 button). "
                  "Fails if the emulator is already running. This reloads the game, so it also "
                  "REBASES the frame counter: frameCount restarts at 0 and `frameSession` "
                  "(reported here and by get_status) advances. Any frame number captured "
                  "before this call is stale - recompute run_until_frame targets afterwards. "
                  "reset_emulator does the opposite: it keeps counting. The active document is "
                  "mounted into DF0 from its in-memory contents, so load the file first if it "
                  "has changed on disk (load_document re-reads it by default).");
    tool("stop", "Stop the running emulator session.");
    tool("end_frame", "Complete the current partially-executed frame, stepping past any "
                      "breakpoints, and stop at the frame boundary. The emulator must be running "
                      "and paused. Returns the new frameCount.");
    tool("click_mouse",
         "Click the Amiga mouse's left or right button for a configurable number of emulated "
         "frames. The mouse lives on the Amiga's port 0; if set_joystick has attached a "
         "joystick there (port:0), this no longer reaches the guest until the emulator is "
         "restarted. The injected button is independent of physical mouse capture and releases "
         "automatically after durationFrames (default 1, maximum 10000). This also works while "
         "paused; use step_frame or resume to advance the click.",
         {{"button", {{"type", "string"}, {"enum", json::array({"left", "right"})}}},
          {"durationFrames",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 10000},
            {"default", 1},
            {"description", "click duration in emulated frames"}}}},
         json::array({"button"}));
    tool("press_key",
         "Press an Amiga keyboard key for a number of emulated frames, then release it "
         "automatically. The injected hold is independent of the physical keyboard and survives "
         "losing window focus. Works while paused: the press only advances under step_frame or "
         "resume. Keys are named (case-insensitive, e.g. \"F1\", \"Return\", \"Escape\", "
         "\"Space\", \"Num1\" for the top-row 1, \"Kp5\" for the keypad, \"Up\"), or given as a "
         "raw RETROK number. Raw Amiga/CIA keycodes are not accepted: the core translates a key "
         "press into the correct raw code on the emulated keyboard line, so a program reading "
         "raw codes still sees the right one.\n"
         "Repeated presses of the same key QUEUE rather than merge. Two press_key calls made "
         "on the same frame - which is every call made while paused, since the frame counter "
         "is not advancing - used to collapse into one longer hold, so the guest saw a single "
         "keystroke while both calls returned ok:true. The second press is now scheduled after "
         "the first has been released, with one frame of key-up between them. The reply carries "
         "`pressFrame` (when the key actually goes down), `releaseFrame`, and `queued`:true "
         "when it had to wait behind an earlier press. Nothing lands until frames advance: "
         "run the machine to at least releaseFrame with resume, step_frame or run_until_frame.",
         {{"key",
           {{"type", json::array({"string", "integer"})},
            {"description", "key name or RETROK code"}}},
          {"durationFrames",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 10000},
            {"default", 2},
            {"description", "hold duration in emulated frames"}}},
          {"modifiers",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "modifier keys held for the same window: shift, ctrl, alt, lalt, "
                            "ralt, amiga/meta (l/r variants accepted)"}}}},
         json::array({"key"}));
    tool("type_text",
         "Type an ASCII string on the Amiga keyboard. The whole string is queued up front as "
         "timed key presses, so call this once and then advance the machine with step_frame or "
         "resume until frameCount reaches the returned releaseFrame. Assumes a US keyboard "
         "layout (which is what the core's default translation uses). Supports printable ASCII "
         "plus newline (Return), tab and backspace; any unmappable character is rejected before "
         "anything is queued.",
         {{"text", {{"type", "string"}, {"description", "text to type (max 1024 chars)"}}},
          {"framesPerKey",
           {{"type", "integer"},
            {"minimum", 2},
            {"maximum", 1000},
            {"default", 2},
            {"description", "emulated frames per character; the key is held for all but the "
                            "last frame so repeated characters register separately"}}}},
         json::array({"text"}));
    tool("set_joystick",
         "Set an emulated joystick's directions and fire buttons. Level-triggered: each named "
         "button stays held until you change it, unlike press_key's auto-release. Omitted "
         "buttons keep their current state; pass release:true to let go of everything. Works "
         "while paused; the state is sampled as the machine runs.\n"
         "`port` is an AMIGA PORT NUMBER, matching the hardware:\n"
         "  port 1 (DEFAULT) = the joystick port: JOY1DAT ($DFF00C), fire on CIA-A PRA bit 7.\n"
         "  port 0           = the mouse port:    JOY0DAT ($DFF00A), fire on CIA-A PRA bit 6.\n"
         "Port 1 is where a joystick normally goes and is what a one-player game reads, which "
         "is why it is the default - calls that omit `port` behave exactly as before. NOTE for "
         "anyone carrying over older code: this tool used to call the joystick port \"0\" and "
         "reject port 1 outright. The numbers now follow the hardware, so an explicit port:0 "
         "means the MOUSE port and drives different registers than it used to. Set both ports "
         "to test two-player input.\n"
         "The Amiga's port 0 is shared with the emulated mouse - one socket, one set of pins, "
         "exactly as on real hardware. Driving a joystick there attaches one in the mouse's "
         "place, and click_mouse / mouse motion stop reaching the guest on that port until the "
         "emulator is restarted (stop then start). Port 1 never disturbs the mouse.\n"
         "To confirm a port moved, read its data register: JOY0DAT ($DFF00A) and JOY1DAT "
         "($DFF00C) are live chipset reads, available through read_memory and in "
         "get_custom_registers, and a held direction shows up in the matching one while "
         "the other stays at rest. Both read 0000 with nothing held, so sample them WITH a "
         "direction applied - a rest reading tells you nothing. Fire buttons are not in "
         "those registers; they are CIA-A PRA bits 7 (port 1) and 6 (port 0), readable at "
         "$BFE001.\n"
         "The reply echoes the hardware it drove - port, dataRegister (JOY0DAT|JOY1DAT), "
         "dataRegisterAddr, fireBit - plus `state`, the level of every button after the call, "
         "so there is no need to dump registers to find out which port moved.",
         {{"port",
           {{"type", "integer"},
            {"enum", json::array({0, 1})},
            {"default", 1},
            {"description", "Amiga port number: 1 = joystick port (JOY1DAT, PRA bit 7, "
                            "default), 0 = mouse port (JOY0DAT, PRA bit 6, displaces the "
                            "emulated mouse)"}}},
          {"up", {{"type", "boolean"}}},
          {"down", {{"type", "boolean"}}},
          {"left", {{"type", "boolean"}}},
          {"right", {{"type", "boolean"}}},
          {"fire", {{"type", "boolean"}, {"description", "primary fire button"}}},
          {"fire2", {{"type", "boolean"}, {"description", "second button"}}},
          {"release", {{"type", "boolean"}, {"description", "release every button"}}}});
    tool("save_state",
         "Snapshot the entire machine (CPU, chip state, RAM) under a name, so you can return to "
         "this moment later instead of re-booting. The snapshot is frame-granular: CPU registers "
         "and RAM round-trip exactly, but the raster position does not — UAE's state format "
         "stores VPOSR/VHPOSR and discards them on restore, and a restore always resumes at "
         "vpos 0 / hpos 0. So a snapshot is only a faithful round trip if it was taken at "
         "vpos 0 too; taken anywhere else, the restored machine re-runs every scanline "
         "between vpos 0 and where the beam actually was, on top of a CPU that already "
         "executed them. Measured on one demo, restoring and running 5 frames: captured at "
         "vpos 0, 0 bytes of RAM differ from an uninterrupted run; captured at vpos 3 (where "
         "a frame step leaves the beam), 6,892 bytes; at vpos 150, 2,074. It is not a small "
         "or self-correcting error — a beam-racing demo resumed from a mid-field snapshot "
         "can break down and stay broken. Note that `end_frame` leaves the beam at vpos 3, "
         "NOT vpos 0, so \"snapshot at a frame boundary\" is not sufficient on its own. "
         "Kept in memory by default; pass persist:true to also write it to disk "
         "so it survives an app restart. The core build and mounted document are recorded and "
         "checked on restore.",
         {{"name",
           {{"type", "string"}, {"description", "1-64 chars: letters, digits, '-', '_', '.'"}}},
          {"persist",
           {{"type", "boolean"},
            {"default", false},
            {"description", "also write the snapshot to disk"}}}},
         json::array({"name"}));
    tool("load_state",
         "Restore a snapshot taken with save_state. The CPU comes back exactly — PC, registers "
         "and RAM — but the restore is frame-granular, not cycle-exact: the machine always "
         "resumes at the start of a frame (beam vpos 0), whatever the beam position was when the "
         "snapshot was taken. So chipset state that the copper rewrites during a frame (colors, "
         "BPLCON3, bitplane pointers) will legitimately differ either side of a round trip if the "
         "snapshot was taken mid-frame — compare those at a frame boundary, not across a "
         "restore, before concluding the chipset restore is broken. The same gap is a real "
         "emulation error, not just a reporting one: everything between vpos 0 and the beam "
         "position at capture time is executed a second time, so a program that races the beam "
         "or does its work off the vertical blank interrupt can resume desynchronised and "
         "degrade from there. Only a snapshot taken at vpos 0 resumes exactly; see save_state "
         "for the measured numbers. "
         "This does not change the run/pause state: a restore made while paused (the usual case, "
         "since you snapshot at a stop) leaves the machine paused, and injected input from "
         "click_mouse / press_key is duration-based and does not advance until you resume. "
         "`frameCount` is a host-side counter and is NOT rewound by a restore; the snapshot's own "
         "frame comes back in the reply's `frame` field. Refuses a snapshot taken on a different "
         "core build or with a different document mounted. Breakpoint/watchpoint state is re-read "
         "from the core afterwards.",
         {{"name", {{"type", "string"}}}}, json::array({"name"}));
    tool("list_states", "List saved machine-state snapshots (name, size, frame, whether "
                        "persisted, core version, document, capture time), newest first.");
    tool("delete_state", "Delete a saved snapshot and its file.", {{"name", {{"type", "string"}}}},
         json::array({"name"}));
    tool("add_breakpoint",
         "Add a PC breakpoint: the machine stops when the program counter EQUALS addr exactly, "
         "at an instruction fetch. Optional register/operator/value/mask create a conditional "
         "breakpoint; ignoreCount skips hits and oneShot removes it after the first stop.\n"
         "It is safe to set one while the emulator is paused, on an address that has not been "
         "written yet, and before the code that will occupy it has been loaded or decrunched - "
         "none of those drop the breakpoint, and it survives stop/start. If a breakpoint never "
         "fires on an address you believe ran, the usual cause is that the address is not an "
         "instruction BOUNDARY the CPU fetched from - a load address is not necessarily the "
         "entry point, and a breakpoint mid-instruction never matches. Use add_watchpoint with "
         "access:\"x\" and a `len` covering the range to catch control entering it anywhere.\n"
         "The reply reports `armed` - false when the emulator is not running, in which case the "
         "breakpoint is held by the host and injected on start - and, on ok:false, a `reason` "
         "(the only one being that an identical PC breakpoint already exists).",
         {{"addr", addrSchema()},
          {"register", {{"type", "string"}, {"description", "D0-D7, A0-A7, PC, SR, ..."}}},
          {"operator",
           {{"type", "string"}, {"enum", json::array({"==", "!=", "<", ">", "<=", ">="})}}},
          {"value", addrSchema()},
          {"mask", addrSchema()},
          {"ignoreCount", {{"type", "integer"}, {"minimum", 0}, {"default", 0}}},
          {"oneShot", {{"type", "boolean"}, {"default", false}}}},
         json::array({"addr"}));
    tool("remove_breakpoint", "Remove the PC breakpoint at addr.", {{"addr", addrSchema()}},
         json::array({"addr"}));
    tool("list_breakpoints", "List PC breakpoints.");
    tool("add_watchpoint",
         "Watch a region of memory. `len` makes it a range — the watchpoint fires on any "
         "access touching [addr, addr+len), so one slot can cover a whole vector table or "
         "code segment. `access` selects read, write and/or execute (x = instruction fetch, "
         "which tells you the moment control enters a range). `source` restricts which agent "
         "triggers it. With mode=\"log\" the hit is recorded and the machine keeps running — "
         "use that for wide read watchpoints, which would otherwise stop on every access — "
         "then read the hits back with get_watchpoint_log. The core provides 20 slots.",
         {{"addr", addrSchema()},
          {"len",
           {{"type", json::array({"integer", "string"})},
            {"default", 1},
            {"description", "watched region length in bytes (default 1, max 0x1000000)"}}},
          {"access",
           {{"type", "string"},
            {"default", "w"},
            {"description", "any combination of r (read), w (write), x (execute/opcode "
                            "fetch), e.g. \"r\", \"rw\", \"x\""}}},
          {"source",
           {{"type", "string"},
            {"enum", json::array({"CPU", "CPUD", "CPUI", "COP", "BLT", "DSK", "BPL", "SPR", "AUD",
                                  "DMA", "ALL"})},
            {"default", "CPU"},
            {"description", "which agent triggers it. CPU is the default because it gives a "
                            "meaningful break PC; widening to Copper/Blitter/DMA catches "
                            "async accesses but the break then surfaces at an unrelated "
                            "CPU instruction (the log records the real agent either way)"}}},
          {"value",
           {{"type", json::array({"integer", "string"})},
            {"description", "fire only when the written value matches"}}},
          {"mode",
           {{"type", "string"},
            {"enum", json::array({"break", "log"})},
            {"default", "break"},
            {"description", "break stops the machine; log records the hit and continues"}}},
          {"maxHits",
           {{"type", "integer"},
            {"minimum", 0},
            {"default", 0},
            {"description", "disarm after this many logged hits (0 = unlimited)"}}},
          {"oneShot", {{"type", "boolean"}, {"default", false}}}},
         json::array({"addr"}));
    tool("remove_watchpoint", "Remove a watchpoint by slot, or by the address it watches.",
         {{"addr", addrSchema()}, {"slot", {{"type", "integer"}}}});
    tool("list_watchpoints", "List memory watchpoints with their slot, watched range, access, "
                             "source, mode and hit count.");
    tool("get_watchpoint_log",
         "Read hits recorded by log-mode watchpoints: for each access, the PC that performed "
         "it, the exact address and value, the access type and width, the emulated frame, and "
         "the agent responsible (CPU/Copper/Blitter/DMA) — which is what tells you who really "
         "touched the memory, since a watchpoint stops at the next CPU instruction boundary "
         "and the PC alone would blame an unrelated instruction for a Copper or Blitter "
         "access.",
         {{"slot", {{"type", "integer"}, {"description", "only this slot (default: all)"}}},
          {"limit",
           {{"type", "integer"},
            {"description", "return at most this many of the most recent hits (default: all)"}}},
          {"clear",
           {{"type", "boolean"},
            {"default", false},
            {"description", "remove the returned hits from the buffer"}}}});

    tool("add_kprintf_breakpoint",
         "Break when the running program's KPrintF (serial) debug output matches `text`. "
         "Matching happens on each completed output line, so the emulator stops at the end of "
         "the line that matched — the PC is wherever the program had got to, not inside "
         "KPrintF. match = contains (default), exact, prefix or regex (ECMAScript). Matching is "
         "case-insensitive unless caseSensitive is true. Set oneShot to drop the trap after it "
         "fires. Returns the rule id; poll get_status for `lastKprintfMatch` after a pause.",
         {{"text", {{"type", "string"}, {"description", "text or regex to match"}}},
          {"match",
           {{"type", "string"},
            {"enum", json::array({"contains", "exact", "prefix", "regex"})},
            {"description", "how text is compared (default contains)"}}},
          {"caseSensitive", {{"type", "boolean"}, {"description", "default false"}}},
          {"oneShot", {{"type", "boolean"}, {"description", "remove after the first hit"}}}},
         json::array({"text"}));
    tool("remove_kprintf_breakpoint",
         "Remove a KPrintF text breakpoint by id (from add_kprintf_breakpoint / "
         "list_kprintf_breakpoints) or by its exact text.",
         {{"id", {{"type", "integer"}}}, {"text", {{"type", "string"}}}});
    tool("list_kprintf_breakpoints",
         "List KPrintF text breakpoints with their match mode, hit count and the last line each "
         "one matched.");
    tool("clear_kprintf_breakpoints", "Remove all KPrintF text breakpoints.");

    tool("load_document",
         "Load an ADF/EXE/DMS/etc. document by file path, ALWAYS re-reading it from disk. "
         "Replaces the currently open document; if that one has unsaved changes the load is "
         "refused unless force is true. `force` is about discarding unsaved edits - it has "
         "nothing to do with re-reading, which now happens unconditionally. Re-loading a path "
         "that is already open used to focus the existing tab and return ok:true WITHOUT "
         "touching the file, so an edit-build-test loop silently kept testing the previous "
         "build; that is fixed, and close_document is no longer needed in between. "
         "Loading a document that is currently in a drive performs a DISK SWAP: the new "
         "image is ejected and re-inserted into every drive that held the old copy, "
         "immediately, without stopping the machine. So the loop for a rebuilt image is "
         "load_document then reset_emulator (a warm reset keeps the disk in the drive and "
         "reboots off it). A full stop/start also works but is not required.",
         {{"path", {{"type", "string"}}},
          {"force",
           {{"type", "boolean"},
            {"default", false},
            {"description", "replace the open document even if it has unsaved changes"}}},
          {"reload",
           {{"type", "boolean"},
            {"default", true},
            {"description", "re-read the file when this path is already open (default true). "
                            "Only set false to deliberately keep in-memory edits."}}}},
         json::array({"path"}));
    tool("load_document_bytes",
         "Load an image sent as bytes rather than a path - for a caller with no file to point "
         "at, such as a page in a browser (which cannot write one). The bytes are written to "
         "`name` in Peekaboo's temp folder (peekaboo-documents) and loaded as load_document "
         "loads a path, so sending the same name again re-reads it and swaps it into any drive "
         "that holds it: load_document_bytes then reset_emulator reboots off the new image. "
         "The reply gives the path it was written to.",
         {{"name",
           {{"type", "string"},
            {"description", "a plain file name whose extension says what it is: sync.adf, "
                            "demo.exe (letters, digits, . - _; 64 at most)"}}},
          {"data", {{"type", "string"}, {"description", "the image, base64"}}},
          {"force",
           {{"type", "boolean"},
            {"default", false},
            {"description", "replace the open document even if it has unsaved changes"}}}},
         json::array({"name", "data"}));
    tool("list_documents", "List currently open documents (with indices).");
    tool("run_document", "Boot the open document at the given index in the emulator.",
         {{"index", {{"type", "integer"}}}}, json::array({"index"}));
    tool("close_document",
         "Close the open document at the given 0-based index (see list_documents). If it has "
         "unsaved changes the close is refused unless force is true.",
         {{"index", {{"type", "integer"}}},
          {"force",
           {{"type", "boolean"},
            {"description", "close even if the document has unsaved changes"}}}},
         json::array({"index"}));

    tool(
        "exec_console",
        "Run an AR5/UAE debugger console command. Returns the captured text output and also echoes "
        "it into Peekaboo's AR console window.",
        {{"cmd", {{"type", "string"}}}}, json::array({"cmd"}));

    tool("set_cpu_trace",
         "Enable/disable the per-instruction CPU trace buffer. Required before get_profile has "
         "data (records every 68k instruction each frame; ~14-17k entries/frame on an A500). "
         "Run at least one frame after enabling.",
         {{"enabled", {{"type", "boolean"}, {"description", "true to record, false to stop"}}}},
         json::array({"enabled"}));

    tool("get_profile",
         "Source-level CPU profiler snapshot for the last traced frame. view=functions "
         "(per-function cycle rollup, default), instructions (per-PC hot spots), flat "
         "(leaf-function timeline bands), flame (call-stack timeline bands), or callstack. "
         "view=callstack returns two stacks: liveStack, a real DWARF unwind of the halted CPU "
         "(needs the machine paused, but NO CPU trace) and callStack, the traced "
         "reconstruction at the frame-scrubber playhead. The other views require CPU trace "
         "enabled (see set_cpu_trace); functions/flame/callstack also need debug symbols (a "
         "sibling .elf next to the loaded program).",
         {{"view",
           {{"type", "string"},
            {"enum", json::array({"functions", "instructions", "flat", "flame", "callstack"})},
            {"description", "which profiling surface (default functions)"}}},
          {"limit", {{"type", "integer"}, {"description", "max rows (default 32, max 1000)"}}}});

    {
        auto maskArgSchema = [](const char* items, const char* range) {
            return json{
                {"type", json::array({"integer", "string", "array"})},
                {"description",
                 fmt::format("visible {}: a bit mask (integer, or hex string like \"0x0F\"), "
                             "\"all\", \"none\", or an array of the indices to keep visible "
                             "({}). Omit to leave this group unchanged.",
                             items, range)}};
        };

        const char* kMaskingBlurb =
            "Debug display masking decides which layers Denise is allowed to draw: bitplanes "
            "BPL1-8 (bit 0 = BPL1), hardware sprites SPR0-7 (bit 0 = SPR0), and playfields PF1/PF2 "
            "(bit 0 = PF1). It is purely cosmetic — the emulated machine still fetches, blits and "
            "executes everything, only the final pixel composition is filtered — so it is safe to "
            "use at any time and never changes program behaviour. In dual-playfield mode PF1 is "
            "drawn from the odd bitplanes (BPL1/3/5) and PF2 from the even ones (BPL2/4/6), so "
            "hiding a playfield also hides those planes; `effectiveBitplaneMask` is the composed "
            "mask actually handed to the renderer. Because masking happens inside the renderer, "
            "the `screenshot` tool captures the masked image — mask a layer, screenshot, and you "
            "see exactly what that layer contributes. This is the same state as the debugger's "
            "Display window and the AR console BPL/SPR/PF commands.";

        tool("get_display_masks",
             std::string("Current debug display masks (which bitplanes, sprites and playfields "
                         "are drawn), plus `hardware`: the decoded screen mode — resolution "
                         "(lores/hires/shres), interlace, display width/height in the program's "
                         "own pixels, bitplane count (BPLCON0 BPU), bitplane/sprite DMA and "
                         "dual-playfield state. ")
                 .append(kMaskingBlurb));

        tool("set_display_masks",
             std::string(
                 "Show or hide individual bitplanes, sprites and playfields to isolate what each "
                 "layer draws, then call `screenshot` to see the result (the capture reflects "
                 "these masks; it re-renders immediately, so this works while paused too). "
                 "Pass any combination of bitplanes/sprites/playfields; reset:true restores "
                 "everything first. Returns the resulting state. ")
                 .append(kMaskingBlurb),
             {{"bitplanes", maskArgSchema("bitplanes", "1-8")},
              {"sprites", maskArgSchema("sprites", "0-7")},
              {"playfields", maskArgSchema("playfields", "1-2")},
              {"reset",
               {{"type", "boolean"},
                {"description", "restore every bitplane/sprite/playfield before applying the "
                                "other arguments (default false)"}}}});
    }

    tool("get_blitter_log",
         "The current frame's blitter operations (mirrors the Blitter Operations window). Returns "
         "per-blit timing (startCycle/lRef, raster line+clock), trigger (cpu|copper), a friendly "
         "op label, size, minterm, BLTCON0/1, DMA cycle cost, and the active A/B/C/D channel "
         "addresses+modulos, plus frame totals and bus-usage percentage. Requires the emulator to "
         "be running; captured fresh each frame (no CPU trace needed). Also returns `beam` "
         "({vpos, hpos}) when a core is loaded: the log covers the current frame up to the "
         "moment you asked, so how much of the frame it represents depends on where the beam is.",
         {{"limit",
           {{"type", "integer"},
            {"description", "max blits returned, newest frame (default 64, max 2000, 0 = all)"}}}});

    // A place in the song, as every Exocet tool below takes one (core/ExocetQuery.h)
    auto exoPlace = [](json props) {
        props["playedRow"] = {{"type", "integer"},
                              {"description", "the check's played row (get_exocet_timeline)"}};
        props["frame"] = {{"type", "integer"},
                          {"description", "a song frame: the row it falls in"}};
        props["place"] = {{"type", "string"},
                          {"description", "the grid's label: \"05:10\", \"05:10.1\" (pass 1), "
                                          "\"L1:00\" (lead), \"1 05:10\" (song 1)"}};
        props["song"] = {{"type", "integer"}};
        props["pos"] = {{"type", "integer"}};
        props["row"] = {{"type", "integer"}};
        props["pass"] = {{"type", "integer"}};
        props["bookmark"] = {{"type", json::array({"integer", "string"})},
                             {"description", "a bookmark's slot (1-9) or name"}};
        return props;
    };
    const char* kExoPlaceText =
        " A place is `playedRow`, `frame` (a song frame), `place` (the grid's label: \"05:10\", "
        "\"05:10.1\" for pass 1, \"L1:00\" for the lead, \"1 05:10\" for song 1) or "
        "`song`/`pos`/`row`/`pass` (song and pass default 0), or `bookmark` (a bookmark's slot "
        "1-9 or its name: manage_exocet_bookmarks).";

    tool("get_exocet_timeline",
         std::string("The Exocet song as a timeline: fps, total rows, frames and seconds; each "
                     "song (order list, digest, module, its first and last played rows and "
                     "frames); the playhead (its place, tick, song frame and clock) when the "
                     "running demo is this pack's build; a window of played rows - place label, "
                     "pattern, ticks, song frame, seconds, `break` on a row breakpoint and `cut` "
                     "where the director is keyed; the cuts with their proofing (ready and made "
                     "frames, margin, late); and the parts shown and loaded as runs of song "
                     "frames with their phase (load, unpack, prep, run, hold). The window is "
                     "`from` (a played row, or a place label) and `count` (default 33, max "
                     "2000), else centred on a place given, else on the playhead.") +
             kExoPlaceText,
         exoPlace({{"from", {{"type", json::array({"integer", "string"})}}},
                   {"count", {{"type", "integer"}}}}));

    tool("get_exocet_grid",
         std::string("The Exocet window's grid as data: a window of played rows (`from`/`count`, "
                     "default 16 around the playhead, max 256) by tracks (`tracks` names, "
                     "`groups` names, `keyedOnly`, else all). `tracks` - each column's index, "
                     "group, type, range, default, text, named entries, key count and whether "
                     "it has a key breakpoint. Each row - its place, ticks, `values` (what the "
                     "keys give on the row's first tick, in the track's units: fix as a real, "
                     "colour as \"#RRGGBBAA\"), `names` (a value's manifest name), `keys` on the "
                     "row (tick, value, interp), `groups` (active/inactive as the demo was seen "
                     "to run them) and, with `channels`, the four tracker cells. `live` - the "
                     "running demo's own values at the playhead, beside them.") +
             kExoPlaceText,
         exoPlace({{"from", {{"type", json::array({"integer", "string"})}}},
                   {"count", {{"type", "integer"}}},
                   {"tracks", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                   {"groups", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                   {"keyedOnly", {{"type", "boolean"}}},
                   {"channels", {{"type", "boolean"}}}}));

    tool("get_exocet_keys",
         "Every key in the .exocet as edited (saved or not), by track - or `track`'s alone: song, "
         "pos, row, pass, tick, interp, value (a colour as \"#RRGGBBAA\", a named value's name "
         "too) and, once the kit's check has the rows, the played row, song frame and label "
         "(`played` false: the song no longer plays that place).",
         {{"track", {{"type", "string"}}}});

    tool("set_exocet_keys",
         std::string("Many Exocet key edits as ONE undo step, all or nothing: `keys` is a list of "
                     "{track, <place>, tick?, value?, interp?, delete?} - value as set_exocet_key "
                     "takes it, `delete` true to remove the key there instead. Every edit is "
                     "checked as it is made; if one is refused nothing is left changed and the "
                     "error names it. `save` writes the .exocet too.") +
             kExoPlaceText,
         {{"keys", {{"type", "array"}, {"items", {{"type", "object"}}}}},
          {"save", {{"type", "boolean"}}}},
         {"keys"});

    tool("undo_exocet", "Undo the last Exocet edit (the window's Ctrl+Z). Not saved.", {});
    tool("redo_exocet", "Redo the Exocet edit last undone (Ctrl+Y). Not saved.", {});
    tool("save_exocet", "Save the .exocet as edited (the window's Ctrl+S).", {});

    tool("check_exocet",
         "Run the pack's kit's check now (the Exocet window's Check): the song's played rows, "
         "the samples' report and the kit's issues. It runs on a worker thread; "
         "get_exocet_issues' `checked`, `rows` and `checkSeconds` say when it is done. A kit "
         "that has never been trusted runs only with `trust` true, which trusts it as the "
         "window's banner does (it runs Python from the kit's folder).",
         {{"trust", {{"type", "boolean"}}}});

    tool("seek_exocet",
         std::string("Rewind the frame history to a place in the song, as clicking a Frame or "
                     "Time cell in the grid does: the place's row (and `tick`), or the song "
                     "`frame` itself. Only song frames played since the boot or the last push "
                     "can be gone to - the error says which are held; run forward to one with a "
                     "row breakpoint (manage_exocet_breakpoints). Returns the song and machine "
                     "frames.") +
             kExoPlaceText,
         exoPlace({{"tick", {{"type", "integer"}}}}));

    tool("manage_exocet_breakpoints",
         std::string("Exocet breakpoints, which pause the emulator on what the demo does (only "
                     "while it runs this pack's build and song, never during a history replay). "
                     "`action`: list (default), add, remove, clear. `kind`: row - as the demo "
                     "starts the place's row (one frame early, so that frame is the one shown); "
                     "key - as it starts any row `track` has a key on; event - as event `track` "
                     "fires, `value` (a number or an entry's name) or any; cut - as director "
                     "`cut` (value or name; omitted: any) is made, or with `ready` as its part "
                     "is ready. Every call returns the list and `lastBreak` (kind, what, place, "
                     "song frame, hits): what stopped the machine last.") +
             kExoPlaceText,
         exoPlace({{"action", {{"type", "string"}}},
                   {"kind", {{"type", "string"}}},
                   {"track", {{"type", "string"}}},
                   {"value", {{"type", json::array({"integer", "string"})}}},
                   {"cut", {{"type", json::array({"integer", "string"})}}},
                   {"ready", {{"type", "boolean"}}}}));

    tool("manage_exocet_bookmarks",
         std::string("The Exocet grid's bookmarks, saved in the .exocet (the save is "
                     "save_exocet's; an edit is an undo step, as a key's): places in the song "
                     "the editor marked, each with a slot 1-9 (the grid's Ctrl+1..9; null past "
                     "the ninth) and a name. `action`: list (default), add - the place's row; "
                     "a new one takes the lowest free slot, `slot` (0 for none) and `name` "
                     "set or change it, a slot another has is taken from it - remove (the "
                     "place's) or clear. Every call returns the list in song order, each with "
                     "its place. Any tool that takes a place takes `bookmark` (slot or name), "
                     "e.g. seek_exocet to one.") +
             kExoPlaceText,
         exoPlace({{"action", {{"type", "string"}}},
                   {"slot", {{"type", "integer"}}},
                   {"name", {{"type", "string"}}}}));

    tool("get_exocet_disk_map",
         "The Exocet Disk Map: the production disk's own layout, read off the ADF by the map in "
         "the .exocet's \"disk\" section. Geometry, the first sector past every block "
         "(`usedEnd`) and the free sectors; `regions` (sector ranges, bytes, cyl.head); the "
         "directory `header` fields; `kinds` (the map's packer and the one found on the blocks, "
         "blocks, sectors, packed and unpacked bytes); `runs` of one kind; `entries` - every "
         "block's kind, sector, sectors, cyl.head, packed and unpacked size, stream offset, "
         "packer and slices, `fits` false for a block past the disk or its sectors. `kind` (a "
         "name or value) keeps one kind's runs and entries; `entries` false leaves them out. "
         "`status` says why there is no map.",
         {{"kind", {{"type", json::array({"integer", "string"})}}},
          {"entries", {{"type", "boolean"}}}});

    tool("get_exocet_music",
         "The Exocet music, as the Music window has it: the song the samples are of, the auto "
         "4-bit threshold (dB), each sample's bytes, volume, loop, whether the song plays it, the "
         ".exocet's setting (auto/8bit/4bit), its hits (rows that trigger it) and, once the kit "
         "has assessed it, its 8-bit and 4-bit packed sizes, dB, worst error, the codec chosen "
         "and why; the channels' lights (trigger, level). `sample` (1-31) keeps one, with its "
         "hit rows as places.",
         {{"sample", {{"type", "integer"}}}});

    tool(
        "get_exocet",
        "The Exocet sync tracker's state: the running program's live block and the sync pack "
        "beside the active document. A Crass-engine production's player publishes an ExoBlock "
        "- on the disk that ships, no separate build - which Peekaboo finds by its 'EXOB' magic "
        "(or as Bartman resource type 4, where a program registers it); this reads it straight "
        "out of Amiga RAM, nothing armed. `block` - the played row and 65536ths into it being "
        "evaluated, the song frame, which clock drives it (music or the fallback frame clock), "
        "the enabled groups, the push handshake (`generation` Peekaboo bumped, `seen` the "
        "runtime loaded; `settled` when equal), and the arena holding the program's exocet.bin. "
        "`pack` - the `.exocet` found beside the disk (same base name, else the one in the "
        "directory): its manifest hash, song digest and order, module and kit paths, key count. "
        "`manifestMatch` false means the disk and the `.exocet` are from different builds. "
        "`tracks` - each track with its manifest name and type and this field's live value "
        "(`fix` also as a real, `color` also as \"#RRGGBBAA\"). `activity` - which groups the "
        "demo was seen to have enabled, per played row of the checked song: `seen` and each "
        "group's rows as [first, last] spans (kept across runs). `fired` - the last 16 events "
        "delivered, oldest first; `cuts` - "
        "when each director cut was ready and when it was made (row/frame, null until then). "
        "`block.present` false: the running program has no Exocet timeline, or has not started it.",
        {});

    tool("get_exocet_memory",
         "The running disk's memory, as the Exocet window's Memory section shows it: every "
         "allocation its programs made, named, laid over exec's own free list. A Crass-engine "
         "production built with its memory map (ENGINE_MEMMAP, a diagnostic build) keeps a table "
         "of its allocations "
         "(engine/memmap.h) and points at it from its Exocet block (version 2). `table` - its "
         "capacity, entries now, the most there have been, allocations it had no room for "
         "(`dropped`), changes ever; `changing` true when the machine stopped while its program "
         "was changing the table, which is then shown as it stood. `regions` - each exec MemHeader "
         "(chip, fast/slow): free "
         "bytes, the largest free piece (the biggest AllocMem that could succeed now), bytes the "
         "table accounts for (`allocated`) and bytes in use that no entry holds (`untracked`: "
         "exec, the OS, trackdisk); `overlaps` counts entries over free memory, which means a "
         "stale table. `allocations` - by address: the program that made it (`owner` letter and "
         "`program`), its name (or source file:line), address, size. `region` \"chip\" or "
         "\"fast\" keeps one; `segments` true adds each region's run of free / allocated / "
         "untracked spans. `present` false: no table.",
         {{"region",
           {{"type", "string"}, {"description", "\"chip\" or \"fast\": only that region"}}},
          {"segments",
           {{"type", "boolean"}, {"description", "each region's spans, in address order"}}}});

    tool("set_exocet_key",
         "Key a track of the Exocet sync on a place in the song, as typing a value in the Exocet "
         "window's grid does: `track` (\"lst.flash\"), `song` (the .exocet's songs index, "
         "default 0: the intro then the main song on a disk that plays two), `pos`/`row`/`pass` "
         "(pass 0 unless the song plays that place again; a negative pos is the song's lead), "
         "optional `tick` (the tick of that row the key sits on, "
         "0 to the row's frames - 1; default 0, the row itself), `value` (in the track's own "
         "units: 1.5 on a fix track, \"#RRGGBBAA\" or \"#RRGGBB\" on a color track), "
         "optional `interp` (step, linear, smooth, ramp - the key's current one, else step), "
         "`save` to write the .exocet too. Refused with the reason the kit would give for a value "
         "out of range, and when the rows are not known yet (the kit's check). The place may "
         "also be `playedRow`, `frame` or `place` (\"05:10.1\"), as every Exocet tool takes it. "
         "Many edits at once, as one undo step: set_exocet_keys.",
         exoPlace({{"track", {{"type", "string"}}},
                   {"tick", {{"type", "integer"}}},
                   {"value", {{"type", json::array({"number", "string"})}}},
                   {"interp", {{"type", "string"}}},
                   {"save", {{"type", "boolean"}}}}),
         {"track", "value"});

    tool("delete_exocet_key",
         std::string("Remove the Exocet key on `track` at a place and `tick` (default 0); `save` "
                     "to write the .exocet.") +
             kExoPlaceText,
         exoPlace({{"track", {{"type", "string"}}},
                   {"tick", {{"type", "integer"}}},
                   {"save", {{"type", "boolean"}}}}),
         {"track"});

    tool("key_exocet_every_hit",
         "The Exocet window's Key every hit: the keys on `track` from the key at "
         "`song`/`pos`/`row`/`pass` - a row that triggers `sample` (1-31) - up to the sample's "
         "next hit "
         "are copied, at the same offsets, onto every other hit of the sample, each stopping "
         "short of the hit after it. Not saved: set_exocet_key with save, or Ctrl+S.",
         exoPlace({{"track", {{"type", "string"}}}, {"sample", {{"type", "integer"}}}}),
         {"track", "sample"});

    tool("clear_exocet_track",
         "Remove every key on an Exocet `track` (\"lst.flash\"), as Clear on the right-click of "
         "its name in the Exocet window's grid does: one undo step (undo_exocet brings them "
         "back). Returns how many were `cleared`. Refused, as every edit is, until the kit's "
         "check has the rows. `save` to write the .exocet too.",
         {{"track", {{"type", "string"}}}, {"save", {{"type", "boolean"}}}}, {"track"});

    tool("push_exocet",
         "The Exocet window's Push: packs the keys as edited with the pack's kit (export of the "
         "module, then pack), writes exocet.bin into the running demo's arena, bumps the block's "
         "generation and clears the whole frame history. The machine must be paused and the "
         "block's manifest the .exocet's. Returns as soon as the kit has started; "
         "get_exocet_proof's `lastPush` and `pushes` say how it went, and the demo takes the "
         "keys up on its next field once unpaused.",
         {});

    tool("inject_music",
         "The Exocet window's Inject: the pack's module (or `module`) exported and the keys "
         "packed against it by the kit, then written into the paused machine - music.bin into "
         "the player's music buffer, exocet.bin into the timeline's arena - with the played row "
         "to go to (the playhead's position/row/pass, found again in the new song) and "
         "`reseat` bumped; the frame history is cleared. On unpausing the player walks the new "
         "song silently to the row, unpacks the samples resident there and plays on. Returns "
         "once the kit has started; get_exocet_issues' `lastInject` has the outcome, and the "
         "reseat's (`reseat_error` from the player) once it has run. Blocked by a kit issue "
         "that blocks `inject`, and by music bigger than the running disk's buffer.",
         {{"module", {{"type", "string"}}}});

    tool("write_exocet_adf",
         "The Exocet window's Write ADF: a new disk from the inserted one, made by the pack's "
         "kit (export of the module, pack of the keys as edited, rebuild of the disk), written "
         "over `path` - by default the disk itself, in the pack. The running machine is stopped "
         "first, the old disk kept as <path>.bak, the .exocet saved with it and the document "
         "reloaded. Returns once the kit has started; get_exocet_issues' `lastWrite` has the "
         "outcome. Blocked by any kit issue that blocks `write` (the music not fitting the "
         "disk, say).",
         {{"path", {{"type", "string"}}}});

    tool("get_exocet_proof",
         "Whether the sync did what the keys say, on the machine (the Exocet window's proofing): "
         "`cuts` - each director cut's key, the song frame its part was ready and the frame it "
         "was made, the margin (frames ready before the key) or `late`; `events` - each event "
         "key on time, late (by frames) or never fired, from the block's fire ring; "
         "`divergence` - recorded frames where a value track read other than the keys give; the "
         "frames recorded, and the last push.",
         {});

    tool("get_exocet_issues",
         "The Exocet issues panel: the kit's (its check's, in the kit's own words) and "
         "Peekaboo's (the tools, a stale build, late cuts and events, divergence, a refused "
         "push), each with level, code, title, text, anchors and the actions a block stops. "
         "Also the kit's state (none, missing, untrusted, ready), the tools found, whether the "
         "check has run, and why editing or a push is blocked.",
         {});

    tool("get_crass_profile",
         "The running Amiga program's OWN profiler numbers - exact, and free. A Crass-engine "
         "production built with `#define PROFILE` counts raster lines out of CIA-B's HSYNC "
         "time-of-day counter and publishes a block of counters, which this reads straight out "
         "of Amiga RAM. Nothing is armed in the emulator and the emulated machine is not "
         "perturbed, which matters: an armed log watchpoint over the same code slows the machine "
         "enough to drop the very fields being counted. Returns `frames` - rendered/dropped/"
         "worst gap for the last completed window of `windowFields` fields (250 = five seconds) "
         "plus the live counts for the window in progress - and `sections`, each named "
         "PROFILE_START/STOP pair with its most recent measurement and the last window's "
         "min/avg/max in raster lines (a PAL field is 312 or 313 of them, so a section over ~312 "
         "cannot fit in a frame). `present` false means the program simply was not built with "
         "PROFILE. Unrelated to get_profile, which is the host-side source-level profiler over "
         "the CPU trace; this one needs neither symbols nor a trace.",
         {});

    tool("get_frame_history",
         "State of the rolling per-frame save-state timeline (the \"state buffer\") that backs "
         "rewind. Reports the recorded frame range, where the timeline is parked, memory used "
         "against its budget, the measured per-frame delta size and record cost, and how far "
         "back the budget reaches at that rate. Recording is armed only while a debugger window "
         "is open, so `recording` false usually just means nothing is watching.",
         {});

    tool("rewind",
         "Step back through recorded frames and re-run the frame you land on. The re-run is the "
         "point: because the frame is executed again, the blitter log, copper trace, DMA record "
         "and CPU trace all repopulate for a frame that has already gone past — so you can turn "
         "the CPU trace on *after* seeing a glitch, rewind, and get a trace of it. Pauses the "
         "emulator. Restore is frame-granular, not cycle-exact.",
         {{"frames",
           {{"type", "integer"}, {"description", "how many frames to step back (default 1)"}}}});

    tool("goto_frame",
         "Seek the frame-history timeline to an absolute recorded frame and re-run it, "
         "regenerating that frame's debugger telemetry. Same mechanism as `rewind`, addressed "
         "absolutely rather than relatively. Use get_frame_history for the seekable range.",
         {{"frame", {{"type", "integer"}, {"description", "recorded frame to seek to"}}}});

    tool("clear_frame_history",
         "Throw the recorded timeline away - states, scrub previews, the armed loop and any "
         "playback with it. Recording stays armed and starts filling again from the next "
         "frame, so this is how you drop everything before the run you care about instead of "
         "hunting for it in a timeline full of boot.",
         {});

    tool(
        "loop_frames",
        "Replay a recorded frame range on repeat, so an effect can be watched over and over "
        "with full per-frame telemetry. Pauses live running for the duration. Call with no "
        "arguments (or end 0) to clear the loop. The transport arguments work on their own "
        "too, on whatever is already playing: slow a running loop down, reverse it, hold it "
        "on a frame, or walk it one frame at a time. get_frame_history reports all four back.",
        {{"start", {{"type", "integer"}, {"description", "first frame of the loop"}}},
         {"end", {{"type", "integer"}, {"description", "last frame of the loop; 0 clears"}}},
         {"playing",
          {{"type", "boolean"},
           {"description", "run the replay (true) or hold it on the current frame (false)"}}},
         {"speed",
          {{"type", "number"},
           {"description", "playback rate as a share of the machine's own frame rate, "
                           "0.125-2.0 (1.0 = real time)"}}},
         {"direction",
          {{"type", "string"},
           {"enum", {"forward", "reverse", "pingpong"}},
           {"description", "pingpong plays the range forwards then backwards. Reverse costs "
                           "a state restore per frame, so it is the expensive direction."}}},
         {"step",
          {{"type", "integer"},
           {"description", "move this many frames and stop playback; negative steps backwards"}}}});

    tool("diff_frames",
         "Which emulated RAM addresses changed between two recorded frames. Answers \"what did "
         "those frames actually touch?\" without having to guess an address first — the "
         "counterpart to a watchpoint when you do not yet know what to watch. Returns contiguous "
         "changed ranges as Amiga addresses with their bank name, not save-state offsets.",
         {{"frameA", {{"type", "integer"}, {"description", "first recorded frame"}}},
          {"frameB", {{"type", "integer"}, {"description", "second recorded frame"}}},
          {"limit",
           {{"type", "integer"},
            {"description", "max changed ranges returned (default 256, max 4096)"}}}});

    tool(
        "get_register_log",
        "The current frame's custom-register writes in beam order (mirrors the Register "
        "Operations window) — who wrote a chipset register and when, which neither the copper "
        "trace (instructions, not effects) nor get_custom_registers (current value, no history) "
        "can answer. Per write: lRef (the vpos*228+hpos timestamp every other debugger view "
        "shares), raster line+clock, source (cpu|copper), register name+offset, value, whether "
        "it `changed` the previous value this frame, and the writing PC (for a copper write, "
        "the address of the MOVE itself). Covers CPU and Copper writes only: chipset activity "
        "that bypasses the register-write path — sprite DMA loading SPRxPOS/CTL/DATA, bitplane "
        "pointer auto-increment — does not appear here. Requires the emulator to be running and "
        "a core built with the register-log patch (`coreSupported` reports this). Also returns "
        "`beam` ({vpos, hpos}): the log covers the current frame up to the moment you asked.",
        {{"limit",
          {{"type", "integer"},
           {"description", "max writes returned, newest frame (default 64, max 2000, 0 = all)"}}}});

    tool("get_audio_stats",
         "Health of the emulator's audio path — the counters that say whether Peekaboo itself is "
         "corrupting the sound, as opposed to the emulated program producing bad audio. "
         "`producer` covers what the core hands over per retro_run(): a steady stream is ~882 "
         "frames/block at 44.1 kHz PAL, so a wide min/max spread means the guest delivers uneven "
         "blocks. `consumer` covers the host side: `framesHeld` counts output frames faked by "
         "repeating the last sample when the ring ran dry (audible as crunch, not as a gap), "
         "`framesDroppedResampler` counts input frames the resampler never consumed, and "
         "`framesDroppedOverrun` counts emulated audio thrown away because the ring was full. "
         "`ring.levelMin` near zero means the device callback is starving. `latencyMs` is how "
         "long after its frame was emulated a sample reaches the sound card: `ring.levelMean` "
         "plus the device's own buffer (`device.bufferFrames`), so how late the sound runs "
         "against the picture before the display's own refresh or two is subtracted. The ring is "
         "held at `ring.target`; audio queued beyond it is dropped oldest-first "
         "(`ring.framesTrimmed`), which happens after a main-thread stall. `backend` "
         "`wasapi-push` (Windows) writes each frame's audio straight into the sound card's own "
         "queue, WinUAE-style, so there `ring.*` describes that queue, `latencyMs` is all of it, "
         "`ring.target` adapts (up a period per starve, counted in `underrunCallbacks`) and "
         "`ring.pacePpm` is the +-0.6% emulation pace holding it there; `miniaudio` is the "
         "callback device with a ring in front. `producer.gapHistogram` shows how evenly the host "
         "loop delivers frames, which sets the cushion needed. All counters are session-scoped "
         "and reset when a recording starts. Requires the emulator to be running.");

    tool("get_paula_state",
         "Chipset-side state of Paula's four audio voices — the sibling of get_audio_stats, "
         "which measures the host path instead. Per channel: AUDxLC (sample start address), "
         "AUDxLEN (words), AUDxPER and the playback rate it implies, the ProTracker note that "
         "period corresponds to, AUDxVOL, whether that channel's DMA is on, whether its interrupt "
         "is pending/enabled, and the ADKCON attach bits (channel n modulating channel n+1's "
         "volume or period). `audible` is false when the Audio window has muted that voice via "
         "PuAE's audio_channel_mask; `channelMask` is the raw mask, bit n per channel. "
         "`peak`/`rms`/`sampleCount` come from the core's per-sample tap and read 0 when "
         "`traceAvailable` is false (a core built before the audio patch). `ledFilterOn` is the "
         "live low-pass state, which in 'emulated' filter mode follows the power LED rather than "
         "any setting. Requires the emulator to be running.");

    tool("start_audio_record",
         "Start capturing emulator audio to WAV at one or both debug taps. `core` captures the "
         "libretro batch payload before Peekaboo's ring buffer — what PuAE actually synthesised — "
         "and works even with no host audio device, so it is the deterministic/headless tap. "
         "`device` captures the buffer handed to the sound card — what you actually hear. "
         "Recording both writes a matched pair (the device file gets a '.device' infix) so the "
         "two can be compared directly: clean at core but corrupt at device isolates the fault to "
         "Peekaboo's host audio path. Each WAV gets a '<path>.json' sidecar with per-block ring "
         "level, held-frame and dropped-frame counts.",
         {{"tap",
           {{"type", "string"},
            {"enum", json::array({"core", "device", "both"})},
            {"description", "which tap(s) to capture (default both)"}}},
          {"path", {{"type", "string"}, {"description", "output .wav path"}}},
          {"seconds",
           {{"type", "number"},
            {"description", "stop automatically after this much audio (0 = until stopped)"}}}},
         {"path"});

    tool("stop_audio_record", "Stop audio capture at both taps and finalise the WAV files.");

    tool("get_disk_log",
         "Every disk DMA transfer this session (mirrors the Disk I/O window) — the emulated "
         "program's own floppy traffic, as opposed to read_disk_* which read the image directly. "
         "Per transfer: direction (read|write), the PC that armed it (and cpu|copper trigger), "
         "DSKPT target, DSKLEN, drive/cyl/side, byte count, a SHA-1 of the transferred bytes, and "
         "the AmigaDOS sectors decoded out of the MFM payload with per-sector hashes and checksum "
         "status.\n"
         "ORDER: `transfers` is newest-first by default; pass order:\"oldestFirst\" to read it "
         "chronologically. `limit` always keeps the most recent transfers whichever order is "
         "emitted, and the reply echoes `order`.\n"
         "SESSIONS: the log survives restarts of the emulated machine, and BOTH `seq` and "
         "`frame` restart at 0 on each one - so neither orders the log on its own, and the same "
         "seq can appear several times. Every entry therefore carries `session` (compare "
         "against the reply's `currentSession`) and `ordinal`, which is strictly increasing "
         "across the whole log. Order by `ordinal`; poll incrementally with sinceOrdinal. "
         "`sessionCount` > 1 means more than one run is present and a `note` says so.\n"
         "FIELD SCOPE, since two different ones live in the reply: `matched` (passed the "
         "filter) and `returned` (actually in `transfers`, i.e. after `limit`) are "
         "filter-scoped; `totalTransfers`, `reads`, `writes`, `bytesRead`, `bytesWritten` and "
         "`anyWrites` describe the WHOLE log and ignore the filter entirely, so 'did this "
         "program write anything back to the disk?' is answered in one call regardless of how "
         "you filtered. The reply also carries a `scope` map naming each one. "
         "Requires the emulator to be running and a core built from PuAE >= 1.19.0.",
         {{"limit",
           {{"type", "integer"},
            {"description", "max transfers returned, always the most recent ones (default 64, "
                            "max 2000, 0 = all). Whole-log totals are unaffected."}}},
          {"order",
           {{"type", "string"},
            {"enum", json::array({"newestFirst", "oldestFirst"})},
            {"default", "newestFirst"},
            {"description", "order of the `transfers` array"}}},
          {"direction",
           {{"type", "string"},
            {"enum", json::array({"any", "read", "write"})},
            {"description", "filter by transfer direction (default any)"}}},
          {"includeSectors",
           {{"type", "boolean"},
            {"description", "include per-sector decode and hashes (default true)"}}},
          {"sinceSeq",
           {{"type", "integer"},
            {"description", "only transfers with seq >= this. seq restarts at each session, so "
                            "this is scoped to sinceSession (default: the current session). "
                            "Prefer sinceOrdinal, which needs no such qualification."}}},
          {"sinceSession",
           {{"type", "integer"},
            {"description", "which session sinceSeq is measured in (default: currentSession)"}}},
          {"sinceOrdinal",
           {{"type", "integer"},
            {"description", "only transfers with ordinal > this. The session-proof way to poll "
                            "incrementally: pass the highest ordinal you have already seen."}}}});

    tool("add_disk_breakpoint",
         "Break when the emulated program starts a disk DMA transfer, so a disk write traps at "
         "the instruction that caused it. access/track/cyl/side/addr predicates are evaluated when "
         "DSKLEN is written, halting on the arming instruction; a sector predicate needs the MFM "
         "headers and so halts at transfer completion instead (the disk log still reports the "
         "arming PC). Requires a core built from PuAE >= 1.19.0.",
         {{"access",
           {{"type", "string"},
            {"enum", json::array({"any", "read", "write"})},
            {"description", "which transfers to trap on (default any)"}}},
          {"track",
           {{"type", "integer"}, {"description", "cyl*2+side, 0-159; shorthand for cyl+side"}}},
          {"cyl", {{"type", "integer"}, {"description", "cylinder 0-82 (omit for any)"}}},
          {"side", {{"type", "integer"}, {"description", "head 0 or 1 (omit for any)"}}},
          {"sector",
           {{"type", "integer"},
            {"description", "AmigaDOS sector 0-10 (omit for any); breaks at completion"}}},
          {"addrLo", addrSchema()},
          {"addrHi", addrSchema()}},
         json::array({"access"}));

    tool("remove_disk_breakpoint", "Disarm the disk DMA breakpoint set by add_disk_breakpoint.");

    AppendMcpReToolManifest(tools);
    AppendMcpUiToolManifest(tools);
    AppendMcpConsoleToolManifest(tools);

    if (!PrivateFeatures()) {
        json shown = json::array();
        for (auto& t : tools) {
            if (!IsPrivateTool(t.value("name", std::string())))
                shown.push_back(std::move(t));
        }
        tools = std::move(shown);
    }

    m_manifest = tools;
}

bool McpToolHandler::IsPrivateTool(const std::string& name)
{
    // Exocet's tools all carry its name; the crAss engine has the one
    return name.find("exocet") != std::string::npos || name == "get_crass_profile";
}

} // namespace adm::core
