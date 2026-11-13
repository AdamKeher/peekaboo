// The console's EXO command: the Exocet sync tracker, as the Exocet MCP tools see it. Every
// subcommand builds the arguments the matching tool takes and prints the JSON core/ExocetQuery.h
// answers with, so the console and MCP never disagree.

#include <algorithm>
#include <cctype>
#include <memory>

#include <fmt/format.h>

#include "ArCommandInterpreter.h"
#include "ExocetActivity.h"
#include "ExocetPack.h"
#include "ExocetQuery.h"
#include "ExocetSession.h"

namespace adm::core {

using nlohmann::json;

namespace {

std::string Upper(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool IsDecimal(const std::string& s)
{
    return !s.empty() && s.size() < 10 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

// A place token into a tool's place arguments: #row (a played row), F<frame> (a song frame),
// else the grid's label (05:10, 05:10.1, L1:00, 1:05:10). False when it is none of them.
bool PlaceArg(const std::string& token, json& args)
{
    // @3: bookmark 3; @drop: the bookmark named drop
    if (token.size() > 1 && token[0] == '@') {
        const std::string b = token.substr(1);
        args["bookmark"] = IsDecimal(b) ? json(std::stoi(b)) : json(b);
        return true;
    }
    if (token.size() > 1 && token[0] == '#' && IsDecimal(token.substr(1))) {
        args["playedRow"] = std::stoi(token.substr(1));
        return true;
    }
    if (token.size() > 1 && (token[0] == 'F' || token[0] == 'f') && IsDecimal(token.substr(1))) {
        args["frame"] = std::stoi(token.substr(1));
        return true;
    }
    if (token.find(':') == std::string::npos)
        return false;
    args["place"] = token;
    return true;
}

// T<tick>
bool TickArg(const std::string& token, json& args)
{
    if (token.size() > 1 && (token[0] == 'T' || token[0] == 't') && IsDecimal(token.substr(1))) {
        args["tick"] = std::stoi(token.substr(1));
        return true;
    }
    return false;
}

// A value from the console: a colour (#RRGGBB[AA]), a number, else the word itself
json ValueArg(const std::string& token)
{
    if (!token.empty() && token[0] == '#')
        return token;
    try {
        size_t used = 0;
        const double v = std::stod(token, &used);
        if (used == token.size())
            return v;
    } catch (...) {
    }
    return token;
}

std::string Text(const json& v)
{
    if (v.is_null())
        return "-";
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_float()) {
        std::string s = fmt::format("{:.4f}", v.get<double>());
        while (s.size() > 1 && s.back() == '0')
            s.pop_back();
        if (s.back() == '.')
            s.pop_back();
        return s;
    }
    return v.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string PlaceText(const json& p)
{
    return p.is_object() ? p.value("label", std::string("?")) : std::string("-");
}

} // namespace

// Anything without a table of its own: its fields, one a line, nested ones indented
void ArCommandInterpreter::EmitExocetJson(const json& r)
{
    // replace: a string from guest memory that is not UTF-8 must not throw out of the console
    std::string text = r.dump(2, ' ', false, json::error_handler_t::replace);
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        Emit(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos)
            break;
        at = nl + 1;
    }
}

void ArCommandInterpreter::CmdExocet(const std::string& argLine)
{
    ExocetSession* s = m_host.exocetSession ? m_host.exocetSession() : nullptr;
    if (!s) {
        Error("Error: Exocet unavailable");
        return;
    }
    const auto toks = Tokenize(argLine);
    const std::string sub = toks.empty() ? std::string() : Upper(toks[0]);
    auto failed = [&](const json& r) {
        if (!r.contains("error"))
            return false;
        Error("Error: " + r["error"].get<std::string>());
        return true;
    };
    auto usage = [&](const char* text) { Error(std::string("Error: usage ") + text); };

    // ---- the live block and the pack ----
    if (sub.empty() || sub == "STATUS") {
        ExocetSnapshot snap;
        if (const ExocetBlockState* b = s->LiveBlock())
            snap.block = *b;
        if (s->Pack())
            snap.pack = std::shared_ptr<const ExocetPack>(s->Pack(), [](const ExocetPack*) {});
        snap.packError = s->PackError();
        snap.documentPath = s->DiskPath();
        const json r = ExocetStatusJson(snap);
        const json& p = r["pack"];
        if (p.value("present", false))
            Emit(fmt::format("Pack    {}  manifest {}  {} keys", p.value("path", ""),
                             p.value("manifest", ""), Text(p["keys"])));
        else
            Info("Pack    " + p.value("error", p.value("reason", std::string("none"))));
        const json& b = r["block"];
        if (b.value("present", false))
            Emit(fmt::format("Block   ${}  row {} frac {} frame {}  clock {}  groups ${:X}  {}",
                             b.value("blockAddress", ""), Text(b["row"]), Text(b["frac"]),
                             Text(b["frame"]), b.value("clock", ""), b.value("enabledGroups", 0u),
                             b.value("settled", false) ? "settled" : "push pending"));
        else
            Info("Block   " + b.value("reason", std::string()));
        if (r.contains("manifestMatch"))
            Emit(r["manifestMatch"].get<bool>() ? "Same build: the block's manifest is the pack's"
                                                : "DIFFERENT builds: the disk and the .exocet "
                                                  "disagree",
                 r["manifestMatch"].get<bool>() ? ArColor::Info : ArColor::Error);
        Emit(fmt::format("Rows    {}  breakpoints {}  {}", s->Check().rows.size(), s->BreakCount(),
                         s->Pack() && s->Pack()->Dirty() ? "unsaved edits" : "saved"));
        return;
    }

    // ---- timeline ----
    if (sub == "TL" || sub == "TIMELINE") {
        json args = json::object();
        for (size_t i = 1; i < toks.size(); ++i) {
            if (PlaceArg(toks[i], args))
                continue;
            if (IsDecimal(toks[i]))
                args["count"] = std::stoi(toks[i]);
            else
                return usage("EXO TL (<place>) (<count>)");
        }
        const json r = ExocetTimelineJson(*s, args);
        if (failed(r))
            return;
        Emit(fmt::format("{} rows, {} frames, {:.1f} s at {} fps", Text(r["rows"]),
                         Text(r["frames"]), r.value("seconds", 0.0), Text(r["fps"])));
        for (const auto& g : r["songs"])
            Emit(fmt::format("  song {}  rows {}-{}  frames {}-{}  {}", Text(g["song"]),
                             Text(g.value("firstRow", json())), Text(g.value("lastRow", json())),
                             Text(g.value("firstFrame", json())), Text(g.value("endFrame", json())),
                             g.value("module", "")));
        if (!r["playhead"].is_null())
            Emit(fmt::format("Playhead {} tick {}  song frame {}  ({} clock)",
                             PlaceText(r["playhead"]), Text(r["playhead"]["tick"]),
                             Text(r["playhead"]["songFrame"]), Text(r["playhead"]["clock"])),
                 ArColor::Highlight);
        EmitTableHead("  Row  Place      Pat Ticks  Frame   Time   Cut");
        for (const auto& row : r["rowsShown"])
            Emit(fmt::format("{}{:>4}  {:<10} {:>3} {:>5} {:>6} {:>6.2f}  {}",
                             row.value("break", false) ? "*" : " ", Text(row["playedRow"]),
                             Text(row["label"]), Text(row["pattern"]), Text(row["ticks"]),
                             Text(row["frame"]), row.value("seconds", 0.0),
                             row.contains("cut") ? Text(row["cut"]) : std::string()));
        if (!r["cuts"].empty()) {
            EmitTableHead("Cut  Name             Key         Ready   Made   Margin");
            for (const auto& c : r["cuts"])
                Emit(fmt::format("{:>3}  {:<16} {:<11} {:>6} {:>6} {:>6}{}", Text(c["cut"]),
                                 Text(c["name"]), PlaceText(c["key"]), Text(c["readyFrame"]),
                                 Text(c["cutFrame"]), Text(c.value("margin", json())),
                                 c.value("late", 0) > 0 ? "  LATE" : ""));
        }
        for (const char* lane : {"partsShown", "partsLoaded"})
            if (!r[lane].empty()) {
                EmitTableHead(std::string(lane == std::string("partsShown") ? "Shown" : "Loaded") +
                              "  frames         part / phase");
                for (const auto& p : r[lane])
                    Emit(fmt::format("  {:>6}-{:<6}  {} {}", Text(p["from"]), Text(p["to"]),
                                     p.value("name", Text(p["part"])), Text(p["phase"])));
            }
        return;
    }

    // ---- the grid ----
    if (sub == "GRID" || sub == "G") {
        json args = json::object();
        json names = json::array();
        for (size_t i = 1; i < toks.size(); ++i) {
            if (PlaceArg(toks[i], args))
                continue;
            if (IsDecimal(toks[i]))
                args["count"] = std::stoi(toks[i]);
            else if (Upper(toks[i]) == "CH")
                args["channels"] = true;
            else
                names.push_back(toks[i]);
        }
        if (!names.empty()) {
            // a name may be a track or a group: ask for both
            args["tracks"] = names;
            args["groups"] = names;
        }
        const json r = ExocetGridJson(*s, args);
        if (failed(r))
            return;
        std::vector<std::string> cols;
        std::string head = "  Row  Place     ";
        for (const auto& t : r["tracks"]) {
            cols.push_back(t["name"].get<std::string>());
            head += fmt::format(" {:>12.12}", cols.back());
        }
        if (cols.size() > 8)
            Info(fmt::format("{} tracks: name tracks or groups to narrow it (EXO GRID 05:00 lst)",
                             cols.size()));
        EmitTableHead(head);
        const int playRow = r.contains("playhead") && r["playhead"].is_object()
                                ? r["playhead"].value("playedRow", -1)
                                : -1;
        for (const auto& row : r["rows"]) {
            const int pr = row.value("playedRow", -1);
            std::string line = fmt::format("{}{:>4}  {:<10}", row.value("break", false) ? "*" : " ",
                                           pr, Text(row["label"]));
            for (const auto& c : cols) {
                std::string cell = Text(row["values"][c]);
                if (row.contains("names") && row["names"].contains(c))
                    cell = row["names"][c].get<std::string>();
                const bool keyed = row.contains("keys") && row["keys"].contains(c);
                line += fmt::format(" {:>12.12}", (keyed ? "\x07" : "") + cell);
            }
            // \x07 marks a keyed cell: show it as the grid's bold, here a leading '>'
            std::replace(line.begin(), line.end(), '\x07', '>');
            if (row.contains("channels"))
                for (const auto& ch : row["channels"])
                    line += fmt::format("  {} {:02} {}", Text(ch["note"]), ch.value("sample", 0),
                                        Text(ch["effect"]));
            Emit(line, pr == playRow ? ArColor::Highlight : ArColor::Default);
        }
        if (r.contains("live")) {
            std::string line = "  live           ";
            for (const auto& c : cols)
                line += fmt::format(" {:>12.12}", Text(r["live"].value(c, json())));
            Emit(line, ArColor::Info);
        }
        Info("'>' a key on the row, '*' a row breakpoint; values are each row's first tick");
        return;
    }

    if (sub == "KEYS") {
        json args = json::object();
        if (toks.size() > 1)
            args["track"] = toks[1];
        const json r = ExocetKeysJson(*s, args);
        if (failed(r))
            return;
        for (const auto& [track, keys] : r["keys"].items()) {
            EmitTableHead(fmt::format("{} - {} key(s)", track, keys.size()));
            for (const auto& k : keys)
                Emit(fmt::format("  {:<10} T{:<3} {:>12} {:<7} {}",
                                 k.value("label", fmt::format("{}:{}.{}", Text(k["pos"]),
                                                              Text(k["row"]), Text(k["pass"]))),
                                 Text(k["tick"]), Text(k["value"]), Text(k["interp"]),
                                 k.value("name", k.value("played", true) ? "" : "(not played)")));
        }
        Info(fmt::format("{} key(s){}", Text(r["total"]),
                         r.value("dirty", false) ? ", unsaved edits" : ""));
        return;
    }

    // ---- editing ----
    if (sub == "KEY" || sub == "DELKEY") {
        const bool remove = sub == "DELKEY";
        json args = json::object();
        if (toks.size() < (remove ? 3u : 4u) || !PlaceArg(toks[2], args))
            return usage(remove ? "EXO DELKEY <track> <place> (T<tick>)"
                                : "EXO KEY <track> <place> <value> (<interp>) (T<tick>)");
        args["track"] = toks[1];
        size_t i = 3;
        if (!remove)
            args["value"] = ValueArg(toks[i++]);
        for (; i < toks.size(); ++i)
            if (!TickArg(toks[i], args))
                args["interp"] = toks[i];
        const json r = ExocetSetKeyJson(*s, args, remove);
        if (failed(r))
            return;
        Info(fmt::format("{} {} at played row {} tick {} - {} key(s) on the track{}",
                         remove ? (r.value("deleted", false) ? "Deleted" : "No key to delete")
                                : "Keyed",
                         toks[1], Text(r["playedRow"]), Text(r["tick"]), Text(r["keys"]),
                         r.value("dirty", false) ? ", unsaved" : ""));
        return;
    }

    if (sub == "EVERYHIT") {
        json args = json::object();
        if (toks.size() < 4 || !IsDecimal(toks[2]) || !PlaceArg(toks[3], args))
            return usage("EXO EVERYHIT <track> <sample> <place>");
        args["track"] = toks[1];
        args["sample"] = std::stoi(toks[2]);
        const json r = ExocetEveryHitJson(*s, args);
        if (failed(r))
            return;
        Info(fmt::format("Set {} key(s) over {} hit(s); {} on the track now, not saved",
                         Text(r["set"]), Text(r["hits"]), Text(r["keys"])));
        return;
    }

    if (sub == "CLEAR") {
        if (toks.size() < 2)
            return usage("EXO CLEAR <track>");
        const json r = ExocetClearTrackJson(*s, {{"track", toks[1]}});
        if (failed(r))
            return;
        Info(fmt::format("Cleared {} key(s) from {}{}", Text(r["cleared"]), toks[1],
                         r.value("dirty", false) ? ", unsaved" : ""));
        return;
    }

    if (sub == "UNDO" || sub == "REDO" || sub == "SAVE") {
        std::string action = sub;
        std::transform(action.begin(), action.end(), action.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const json r = ExocetEditJson(*s, action);
        if (failed(r))
            return;
        if (r.contains("saved"))
            Info("Saved " + r["saved"].get<std::string>());
        else
            Info(fmt::format("{} - {} key(s){}", sub == "UNDO" ? "Undone" : "Redone",
                             Text(r["keys"]), r.value("dirty", false) ? ", unsaved" : ""));
        return;
    }

    if (sub == "CHECK") {
        const json r =
            ExocetCheckJson(*s, json{{"trust", toks.size() > 1 && Upper(toks[1]) == "TRUST"}});
        if (failed(r))
            return;
        if (r.contains("trusted"))
            Info("Trusted the kit " + r["trusted"].get<std::string>());
        Info("Check requested - EXO ISSUES shows when it is done");
        return;
    }

    // ---- the machine ----
    if (sub == "SEEK") {
        json args = json::object();
        for (size_t i = 1; i < toks.size(); ++i)
            if (!PlaceArg(toks[i], args) && !TickArg(toks[i], args))
                return usage("EXO SEEK <place> (T<tick>)");
        const json r = ExocetSeekJson(*s, args, m_host.frameHistorySeek);
        if (failed(r))
            return;
        Info(fmt::format("At {} - song frame {}, machine frame {}", PlaceText(r["place"]),
                         Text(r["songFrame"]), Text(r["machineFrame"])));
        StateChanged();
        return;
    }

    if (sub == "BR" || sub == "BD") {
        json args = {{"action", "list"}};
        if (toks.size() > 1) {
            const std::string kind = Upper(toks[1]);
            if (kind == "CLEAR" || kind == "CLR") {
                args["action"] = "clear";
            } else {
                args["action"] = sub == "BR" ? "add" : "remove";
                bool ok = true;
                if (kind == "ROW") {
                    args["kind"] = "row";
                    ok = toks.size() > 2 && PlaceArg(toks[2], args);
                } else if (kind == "KEY") {
                    args["kind"] = "key";
                    ok = toks.size() > 2;
                    if (ok)
                        args["track"] = toks[2];
                } else if (kind == "EVENT") {
                    args["kind"] = "event";
                    ok = toks.size() > 2;
                    if (ok)
                        args["track"] = toks[2];
                    if (toks.size() > 3)
                        args["value"] =
                            IsDecimal(toks[3]) ? json(std::stoi(toks[3])) : json(toks[3]);
                } else if (kind == "CUT") {
                    args["kind"] = "cut";
                    for (size_t i = 2; i < toks.size(); ++i) {
                        if (Upper(toks[i]) == "READY")
                            args["ready"] = true;
                        else
                            args["cut"] =
                                IsDecimal(toks[i]) ? json(std::stoi(toks[i])) : json(toks[i]);
                    }
                } else {
                    ok = false;
                }
                if (!ok)
                    return usage("EXO BR|BD ROW <place> | KEY <track> | EVENT <track> (<value>) | "
                                 "CUT (<cut>) (READY) | EXO BR CLEAR");
            }
        }
        const json r = ExocetBreakpointsJson(*s, args);
        if (failed(r))
            return;
        if (r.value("count", 0) == 0)
            Info("No Exocet breakpoints");
        for (const auto& p : r["rows"])
            Emit("  row    " +
                 (p.contains("label")
                      ? PlaceText(p)
                      : fmt::format("song {} {}:{}.{} (not played)", Text(p["song"]),
                                    Text(p["pos"]), Text(p["row"]), Text(p["pass"]))));
        for (const auto& k : r["keys"])
            Emit("  key    " + k.get<std::string>());
        for (const auto& e : r["events"])
            Emit(fmt::format("  event  {} {}", Text(e["track"]),
                             e.contains("name")     ? Text(e["name"])
                             : e["value"].is_null() ? std::string("(any value)")
                                                    : Text(e["value"])));
        for (const auto& c : r["cuts"])
            Emit(fmt::format("  cut    {} {}",
                             c.contains("name") ? Text(c["name"]) : Text(c["cut"]),
                             c.value("ready", false) ? "ready" : "made"));
        if (!r["lastBreak"].is_null()) {
            const json& l = r["lastBreak"];
            Emit(fmt::format("Last: {} {} at {} (song frame {}), {} hit(s)", Text(l["kind"]),
                             Text(l["what"]), PlaceText(l["place"]), Text(l["songFrame"]),
                             Text(l["hits"])),
                 ArColor::Info);
        }
        return;
    }

    if (sub == "BM" || sub == "BMD") {
        json args = {{"action", "list"}};
        if (toks.size() > 1 && Upper(toks[1]) == "CLEAR" && sub == "BM") {
            args["action"] = "clear";
        } else if (toks.size() > 1) {
            if (!PlaceArg(toks[1], args))
                return usage("EXO BM (<place>|@<n>) (S<slot>) (<name>) | EXO BMD <place>|@<n|name> "
                             "| EXO BM CLEAR");
            args["action"] = sub == "BM" ? "add" : "remove";
            std::string name;
            for (size_t i = 2; i < toks.size(); ++i) {
                const std::string& t = toks[i];
                if (name.empty() && t.size() > 1 && (t[0] == 'S' || t[0] == 's') &&
                    IsDecimal(t.substr(1)))
                    args["slot"] = std::stoi(t.substr(1));
                else
                    name += (name.empty() ? "" : " ") + t;
            }
            if (!name.empty())
                args["name"] = name;
        }
        const json r = ExocetBookmarksJson(*s, args);
        if (failed(r))
            return;
        if (r["bookmarks"].empty())
            Info("No bookmarks");
        for (const auto& b : r["bookmarks"])
            Emit(fmt::format(
                "  {}  {:<12} {}", b["slot"].is_null() ? std::string("-") : Text(b["slot"]),
                b.contains("label") ? PlaceText(b)
                                    : fmt::format("{}:{}.{} (not played)", Text(b["pos"]),
                                                  Text(b["row"]), Text(b["pass"])),
                b.value("name", std::string())));
        if (args["action"] != "list")
            StateChanged();
        return;
    }

    if (sub == "MUSIC") {
        json args = json::object();
        if (toks.size() > 1 && IsDecimal(toks[1]))
            args["sample"] = std::stoi(toks[1]);
        const json r = ExocetMusicJson(*s, args);
        if (failed(r))
            return;
        Emit(fmt::format("Song {}  auto 4-bit above {} dB", Text(r["musicSong"]),
                         Text(r["autoDb"])));
        EmitTableHead(" #  Name                    Bytes  8-bit  4-bit     dB  Setting Codec Hits");
        for (const auto& m : r["samples"])
            Emit(fmt::format("{:>2}  {:<22.22} {:>6} {:>6} {:>6} {:>6}  {:<7} {:<5} {:>4}{}",
                             Text(m["index"]), Text(m["name"]), Text(m["bytes"]),
                             Text(m.value("packed8", json())), Text(m.value("packed4", json())),
                             Text(m.value("db", json())), Text(m["setting"]),
                             Text(m.value("codec", json())), Text(m["hits"]),
                             m.value("used", false) ? "" : "  unused"));
        if (r["samples"].size() == 1 && r["samples"][0].contains("hitRows")) {
            std::string line = "Hits:";
            for (const auto& h : r["samples"][0]["hitRows"])
                line += " " + PlaceText(h);
            Emit(line);
        }
        return;
    }

    if (sub == "DISK") {
        json args = {{"entries", false}};
        for (size_t i = 1; i < toks.size(); ++i) {
            if (Upper(toks[i]) == "ENTRIES")
                args["entries"] = true;
            else
                args["kind"] = IsDecimal(toks[i]) ? json(std::stoi(toks[i])) : json(toks[i]);
        }
        const json r = ExocetDiskMapJson(*s, args);
        if (failed(r))
            return;
        if (r.contains("status"))
            Info(r["status"].get<std::string>());
        if (!r.value("ok", false))
            return;
        Emit(fmt::format("{}  {} sectors of {}, used to {}, {} free", Text(r["name"]),
                         Text(r["sectors"]), Text(r["sectorBytes"]), Text(r["usedEnd"]),
                         Text(r["freeSectors"])));
        for (const auto& w : r["warnings"])
            Error("  " + w.get<std::string>());
        EmitTableHead("Region            First  Count  Cyl.Hd");
        for (const auto& g : r["regions"])
            Emit(fmt::format("{:<16} {:>6} {:>6}  {}", Text(g["name"]), Text(g["first"]),
                             Text(g["count"]), Text(g["cylHead"])));
        EmitTableHead("Kind              Packer Found  Blocks Sectors   Packed  Unpacked");
        for (const auto& k : r["kinds"])
            Emit(fmt::format("{:<16} {:<6} {:<6} {:>6} {:>7} {:>8} {:>9}", Text(k["name"]),
                             Text(k["packer"]), Text(k["found"]), Text(k["blocks"]),
                             Text(k["sectors"]), Text(k["packed"]), Text(k["unpacked"])));
        EmitTableHead("Run kind          Entries   Sectors      Cyl.Hd  Packed  Packer");
        for (const auto& u : r["runs"])
            Emit(fmt::format("{:<16} {:>3}-{:<4} {:>5}-{:<5}  {:<6} {:>7}  {}{}", Text(u["kind"]),
                             Text(u["firstEntry"]), Text(u["lastEntry"]), Text(u["firstSector"]),
                             Text(u["endSector"]), Text(u["cylHead"]), Text(u["packed"]),
                             Text(u["packer"]),
                             u.value("packerOk", true) ? "" : " (not the map's)"));
        if (r.contains("entries")) {
            EmitTableHead("  #  Kind            Sector Sectors Cyl.Hd  Packed Unpacked Packer");
            for (const auto& e : r["entries"])
                Emit(fmt::format("{:>3}  {:<15} {:>6} {:>7} {:<6} {:>7} {:>8} {}{}",
                                 Text(e["index"]), Text(e["kind"]), Text(e["sector"]),
                                 Text(e["sectors"]), Text(e["cylHead"]), Text(e["packed"]),
                                 Text(e["unpacked"]), Text(e["packer"]),
                                 e.value("fits", true) ? "" : "  DOES NOT FIT"),
                     e.value("fits", true) ? ArColor::Default : ArColor::Error);
        }
        return;
    }

    if (sub == "MEM") {
        const ExocetMemoryState* m = s->Memory();
        json args = json::object();
        if (toks.size() > 1)
            args["region"] = Upper(toks[1]) == "CHIP" ? "chip" : "fast";
        const json r = ExocetMemoryJson(m ? *m : ExocetMemoryState{}, args);
        if (!r.value("present", false)) {
            Info(r.value("reason", std::string("no memory map")));
            return;
        }
        const json& t = r["table"];
        Emit(fmt::format("Table ${}  {}/{} entries, peak {}, dropped {}, {} changes{}",
                         Text(t["address"]), Text(t["count"]), Text(t["capacity"]), Text(t["peak"]),
                         Text(t["dropped"]), Text(t["changes"]),
                         t.value("changing", false) ? "  (mid-change)" : ""));
        EmitTableHead("Region  Range               Free  Largest  Allocated  Untracked");
        for (const auto& g : r["regions"])
            Emit(fmt::format("{:<6}  ${}-${} {:>7} {:>8} {:>10} {:>10}{}", Text(g["kind"]),
                             Text(g["lower"]), Text(g["upper"]), Text(g["free"]),
                             Text(g["largestFree"]), Text(g["allocated"]), Text(g["untracked"]),
                             g.value("overlaps", 0) ? "  STALE (overlaps)" : ""));
        EmitTableHead("Program       Name                         Address      Size");
        for (const auto& a : r["allocations"])
            Emit(fmt::format("{:<12}  {:<28.28} ${}  {:>8}{}", Text(a["program"]), Text(a["name"]),
                             Text(a["address"]), Text(a["size"]),
                             a.value("chip", false) ? "  chip" : ""));
        return;
    }

    if (sub == "PROOF") {
        const json r = ExocetProofJson(*s);
        Emit(fmt::format("{} recorded frames, {} push(es)  {}", Text(r["recordedFrames"]),
                         Text(r["pushes"]), Text(r["lastPush"])));
        if (!r["cuts"].empty()) {
            EmitTableHead("Cut  Name             Key         Ready   Made   Margin");
            for (const auto& c : r["cuts"])
                Emit(fmt::format("{:>3}  {:<16} {:<11} {:>6} {:>6} {:>6}{}", Text(c["cut"]),
                                 Text(c["name"]), PlaceText(c["key"]), Text(c["readyFrame"]),
                                 Text(c["cutFrame"]), Text(c.value("margin", json())),
                                 c.value("late", 0) > 0 ? "  LATE" : ""));
        }
        if (!r["events"].empty()) {
            EmitTableHead("Event track         Value  Key         State");
            for (const auto& e : r["events"])
                Emit(fmt::format("{:<18} {:>6}  {:<11} {}{}", Text(e["track"]), Text(e["value"]),
                                 PlaceText(e["key"]), Text(e["state"]),
                                 e.contains("lateFrames") && e["lateFrames"].get<int>() > 0
                                     ? fmt::format(" by {} frames", Text(e["lateFrames"]))
                                     : std::string()));
        }
        for (const auto& [track, n] : r["divergence"].items())
            Error(fmt::format("  {} read other than its keys on {} frame(s)", track, Text(n)));
        return;
    }

    if (sub == "ISSUES") {
        const json r = ExocetIssuesJson(*s);
        Emit(fmt::format("Kit {}  {} rows{}", Text(r["kit"]), Text(r["rows"]),
                         r.value("busy", std::string()).empty()
                             ? std::string()
                             : "  busy: " + r["busy"].get<std::string>()));
        for (const char* k : {"editBlocked", "pushBlocked", "writeBlocked", "injectBlocked"})
            if (!r.value(k, std::string()).empty())
                Emit(fmt::format("  {}: {}", k, r[k].get<std::string>()));
        if (r["issues"].empty())
            Info("No issues");
        for (const auto& i : r["issues"])
            Emit(fmt::format("  [{}] {} - {}", Text(i.value("level", json())),
                             Text(i.value("title", json())), Text(i.value("text", json()))),
                 i.value("level", std::string()) == "block" ? ArColor::Error : ArColor::Default);
        return;
    }

    if (sub == "PUSH" || sub == "INJECT" || sub == "WRITE") {
        json args = json::object();
        if (toks.size() > 1)
            args[sub == "INJECT" ? "module" : "path"] = toks[1];
        const json r = sub == "PUSH"     ? ExocetPushJson(*s)
                       : sub == "INJECT" ? ExocetInjectJson(*s, args)
                                         : ExocetWriteJson(*s, args);
        if (failed(r))
            return;
        Info(sub == "PUSH" ? "Push started - EXO PROOF shows how it went"
                           : r.value("note", std::string("started")));
        return;
    }

    if (sub == "JSON" && toks.size() > 1) {
        // the raw answer of a reading subcommand, for when a table leaves something out
        const std::string what = Upper(toks[1]);
        const json r = what == "TL"      ? ExocetTimelineJson(*s, json::object())
                       : what == "GRID"  ? ExocetGridJson(*s, json::object())
                       : what == "MUSIC" ? ExocetMusicJson(*s, json::object())
                       : what == "DISK"  ? ExocetDiskMapJson(*s, json::object())
                                         : json{{"error", "EXO JSON TL|GRID|MUSIC|DISK"}};
        if (!failed(r))
            EmitExocetJson(r);
        return;
    }

    Error("Error: unknown EXO subcommand (HELP lists them under EXOCET)");
}

} // namespace adm::core
