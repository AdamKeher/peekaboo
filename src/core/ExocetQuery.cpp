#include "ExocetQuery.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>

#include <fmt/format.h>

#include "ExocetDiskMap.h"
#include "ExocetEval.h"
#include "ExocetKit.h"
#include "ExocetPack.h"
#include "ExocetSession.h"

namespace adm::core {

using nlohmann::json;

namespace {

json Err(const std::string& msg) { return json{{"error", msg}}; }

std::string ToHex(uint32_t v, int width) { return fmt::format("{:0{}X}", v, width); }

ExocetType TypeOf(const ExocetTrackInfo& t)
{
    return static_cast<ExocetType>(ExocetTypeFromName(t.type));
}

// A value as exocet.bin holds it, in the track's own units: a fix track's as a real, a colour's
// as "#RRGGBBAA", the number itself for the rest
json ValueJson(const ExocetTrackInfo& t, int32_t bin)
{
    const ExocetType type = TypeOf(t);
    if (type == ExocetType::Color)
        return ExocetColorText(static_cast<uint32_t>(bin));
    if (type == ExocetType::Fix)
        return bin / 65536.0;
    return bin;
}

// The name an event's or a pointer's value has in the manifest, or empty
std::string EntryName(const ExocetTrackInfo& t, int32_t value)
{
    for (const auto& e : t.entries)
        if (e.value == value)
            return e.name;
    return {};
}

const char* StateName(ExocetActivity::State st)
{
    switch (st) {
        case ExocetActivity::State::Active:
            return "active";
        case ExocetActivity::State::Inactive:
            return "inactive";
        default:
            return "unknown";
    }
}

const char* PhaseName(int phase)
{
    static const char* kNames[] = {"-", "load", "unpack", "prep", "run", "hold"};
    return kNames[phase >= 0 && phase <= 5 ? phase : 0];
}

// The rows a window covers: `from` (a played row or any place) and `count`, else `around` rows
// either side of the playhead, else the song's start
std::pair<int, int> RowWindow(ExocetSession& s, const json& args, int defCount, int maxCount,
                              std::string* error)
{
    const int rows = static_cast<int>(s.Check().rows.size());
    int count = std::clamp(args.value("count", defCount), 1, maxCount);
    int from = -1;
    if (args.contains("from")) {
        if (args["from"].is_number_integer())
            from = args["from"].get<int>();
        else
            from = ExocetResolvePlace(s, json{{"place", args["from"]}}, error);
        if (from < 0) {
            if (error->empty())
                *error = "from: not a played row";
            return {0, 0};
        }
    } else {
        const int at = ExocetResolvePlace(s, args, error, false);
        if (at == -1)
            return {0, 0};
        if (at >= 0)
            from = at;
        else if (s.Block().present && s.ManifestMatches())
            from = std::max(0, static_cast<int>(s.Block().row) - count / 2);
        else
            from = 0;
    }
    from = std::clamp(from, 0, std::max(0, rows - 1));
    return {from, std::min(rows, from + count)};
}

std::string RowsUnknown(const ExocetSession& s)
{
    const std::string why = s.EditBlockedReason();
    return "the song's rows are not known yet" + (why.empty() ? std::string() : ": " + why);
}

std::atomic<uint64_t> g_bulkEdits{0};

std::string PlaceTextOf(const ExocetCheck& c, int row)
{
    return row >= 0 && row < static_cast<int>(c.rows.size())
               ? ExocetPlaceLabel(c.rows[static_cast<size_t>(row)], c.orders.size() > 1)
               : std::string("?");
}

} // namespace

int ExocetResolvePlace(const ExocetSession& s, const json& args, std::string* error, bool required)
{
    const ExocetCheck& c = s.Check();
    const int rows = static_cast<int>(c.rows.size());
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return -1;
    };
    const bool named = args.contains("playedRow") || args.contains("frame") ||
                       args.contains("place") || args.contains("pos") || args.contains("row") ||
                       args.contains("bookmark");
    if (!named)
        return required ? fail("no place: give playedRow, frame, place, pos/row or bookmark") : -2;
    if (!c.valid || rows == 0)
        return fail(RowsUnknown(s));
    if (args.contains("bookmark")) {
        // a bookmark's slot (1-9) or its name
        const json& want = args["bookmark"];
        if (s.Pack())
            for (const auto& b : s.Pack()->Bookmarks())
                if ((want.is_number_integer() && b.slot && want.get<int>() == b.slot) ||
                    (want.is_string() && !b.name.empty() && want.get<std::string>() == b.name)) {
                    const int r = c.FindRow(b.song, b.pos, b.row, b.pass);
                    if (r < 0)
                        return fail("the bookmark is on a place the song does not play");
                    return r;
                }
        return fail("no bookmark " + want.dump() + ": a slot 1-9 or a bookmark's name");
    }
    if (args.contains("playedRow")) {
        const int r = args["playedRow"].is_number_integer() ? args["playedRow"].get<int>() : -1;
        if (r < 0 || r >= rows)
            return fail("playedRow: the song plays rows 0 to " + std::to_string(rows - 1));
        return r;
    }
    if (args.contains("frame")) {
        if (!args["frame"].is_number_integer() || args["frame"].get<int64_t>() < 0)
            return fail("frame: a song frame, 0 or more");
        const auto f = args["frame"].get<uint64_t>();
        if (f >= c.frames)
            return fail("frame: the song is " + std::to_string(c.frames) + " frames");
        return c.RowAtFrame(static_cast<uint32_t>(f));
    }
    int song = args.value("song", 0), pos = 0, row = 0, pass = args.value("pass", 0);
    if (args.contains("place")) {
        const std::string text = args["place"].is_string() ? args["place"].get<std::string>() : "";
        if (!ExocetParsePlace(text, &song, &pos, &row, &pass))
            return fail("place \"" + text + "\": pp:rr, pp:rr.pass, L1:rr or \"song pp:rr\"");
    } else {
        pos = args.value("pos", -1000);
        row = args.value("row", -1);
    }
    const int r = c.FindRow(song, pos, row, pass);
    if (r < 0)
        return fail("song " + std::to_string(song) + " does not play position " +
                    std::to_string(pos) + " row " + std::to_string(row) + " pass " +
                    std::to_string(pass));
    return r;
}

json ExocetPlaceJson(const ExocetCheck& check, int playedRow)
{
    if (playedRow < 0 || playedRow >= static_cast<int>(check.rows.size()))
        return nullptr;
    const ExocetRow& r = check.rows[static_cast<size_t>(playedRow)];
    return json{{"song", r.song},
                {"pos", r.pos},
                {"row", r.row},
                {"pass", r.pass},
                {"playedRow", playedRow},
                {"frame", r.frame},
                {"label", ExocetPlaceLabel(r, check.orders.size() > 1)}};
}

// ---------------------------------------------------------------------------------------------
// reading

json ExocetStatusJson(const ExocetSnapshot& s)
{
    const ExocetBlockState& b = s.block;
    const ExocetPack* pack = s.pack.get();
    json r;

    // The pack beside the document: what the editor has, whether or not a program is running
    json jp;
    jp["present"] = pack != nullptr;
    if (pack) {
        jp["path"] = pack->Path();
        jp["production"] = pack->Production();
        jp["manifest"] = "0x" + ToHex(pack->ManifestHash(), 8);
        json songs = json::array();
        for (int n = 0; n < pack->SongCount(); ++n)
            songs.push_back({{"module", pack->ModulePath(n)},
                             {"digest", "0x" + ToHex(pack->SongDigest(n), 8)},
                             {"order", pack->SongOrder(n)}});
        jp["songs"] = songs;
        jp["kit"] = pack->KitPath();
        jp["keys"] = pack->KeyCount();
    } else if (!s.packError.empty()) {
        jp["error"] = s.packError;
    } else {
        jp["reason"] =
            "no .exocet beside " +
            (s.documentPath.empty() ? std::string("the active document") : s.documentPath);
    }
    r["pack"] = jp;

    json jb;
    jb["present"] = b.present;
    if (!b.present) {
        // No timeline, not booted that far, or a registry entry the running program no
        // longer backs.
        jb["reason"] = "no valid Exocet block in the running program's memory";
    } else {
        jb["blockAddress"] = ToHex(b.blockAddress, 8);
        jb["version"] = b.version;
        jb["manifest"] = "0x" + ToHex(b.manifest, 8);
        jb["tracks"] = b.tracks;
        jb["row"] = b.row;
        jb["frac"] = b.frac;
        jb["frame"] = b.frame;
        jb["clock"] = b.clock == 0 ? "music" : "frames";
        jb["enabledGroups"] = b.enabled;
        jb["generation"] = b.generation;
        jb["seen"] = b.seen;
        jb["settled"] = b.Settled();
        jb["firedCount"] = b.firedCount;
        json arena;
        arena["address"] = ToHex(b.arenaAddress, 8);
        arena["bytes"] = b.arenaBytes;
        arena["valid"] = b.arena.valid;
        if (b.arena.valid) {
            arena["manifest"] = "0x" + ToHex(b.arena.manifest, 8);
            arena["songDigest"] = "0x" + ToHex(b.arena.song, 8);
            if (b.arena.main)
                arena["mainDigest"] = "0x" + ToHex(b.arena.main, 8);
            arena["tracks"] = b.arena.tracks;
            arena["rows"] = b.arena.rows;
            arena["keys"] = b.arena.keys;
            arena["spans"] = b.arena.spans;
            arena["frames"] = b.arena.frames;
        }
        jb["arena"] = arena;
        if (b.memmapAddress != 0)
            jb["memmapAddress"] = ToHex(b.memmapAddress, 8);
        if (b.musicAddress != 0 || b.reseat != 0 || b.reseatSeen != 0) {
            jb["music"] = {{"address", ToHex(b.musicAddress, 8)},
                           {"bytes", b.musicBytes},
                           {"reseat", b.reseat},
                           {"reseatSeen", b.reseatSeen},
                           {"reseatRow", b.reseatRow},
                           {"reseatError", b.reseatError}};
        }
    }
    r["block"] = jb;

    // Same build? A push needs both, and their manifests to agree.
    if (pack && b.present) {
        r["manifestMatch"] = pack->ManifestHash() == b.manifest;
        if (b.arena.valid)
            r["songMatch"] = pack->SongDigest(0) == b.arena.song &&
                             (pack->SongCount() < 2 || pack->SongDigest(1) == b.arena.main);
    }

    // The track list: the manifest's when there is a pack, else bare indices off the block
    auto trackName = [&](int i) -> std::string {
        if (pack && (!b.present || pack->ManifestHash() == b.manifest) &&
            i < static_cast<int>(pack->Tracks().size()))
            return pack->Tracks()[i].name;
        return {};
    };
    json tracks = json::array();
    const int count = b.present ? b.tracks : (pack ? static_cast<int>(pack->Tracks().size()) : 0);
    for (int i = 0; i < count; ++i) {
        json jt;
        jt["index"] = i;
        const std::string tn = trackName(i);
        if (!tn.empty())
            jt["name"] = tn;
        if (pack && i < static_cast<int>(pack->Tracks().size()) && !tn.empty()) {
            const auto& t = pack->Tracks()[i];
            jt["type"] = t.type;
            jt["keys"] = pack->Keys(t.name).size();
        }
        if (b.present && i < static_cast<int>(b.values.size())) {
            const int32_t v = b.values[i];
            jt["value"] = v;
            if (pack && !tn.empty() && pack->Tracks()[i].type == "fix")
                jt["fix"] = v / 65536.0;
            if (pack && !tn.empty() && pack->Tracks()[i].type == "color")
                jt["color"] = ExocetColorText(static_cast<uint32_t>(v));
        }
        tracks.push_back(jt);
    }
    r["tracks"] = tracks;
    if (!s.activity.is_null())
        r["activity"] = s.activity;

    if (b.present) {
        json fired = json::array();
        for (const auto& f : b.fired) {
            json jf = {{"track", f.track}, {"row", f.row}, {"frame", f.frame}};
            if (const std::string tn = trackName(f.track); !tn.empty())
                jf["name"] = tn;
            fired.push_back(jf);
        }
        r["fired"] = fired;

        auto opt = [](uint16_t v) -> json { return v == 0xFFFF ? json(nullptr) : json(v); };
        json cuts = json::array();
        for (const auto& c : b.cuts)
            cuts.push_back({{"cut", c.cut},
                            {"readyRow", opt(c.readyRow)},
                            {"readyFrame", opt(c.readyFrame)},
                            {"cutRow", opt(c.cutRow)},
                            {"cutFrame", opt(c.cutFrame)}});
        r["cuts"] = cuts;
    }
    return r;
}

json ExocetMemoryJson(const ExocetMemoryState& m, const json& args)
{
    json r;
    r["present"] = m.present;
    if (!m.present) {
        r["reason"] = "no memory map: the running program's Exocet block (get_exocet) is "
                      "missing, older than version 2, or its build keeps no map";
        return r;
    }
    const std::string only = args.value("region", std::string());
    const bool withSegments = args.value("segments", false);
    const auto& t = m.table;
    r["table"] = {{"address", ToHex(t.address, 8)},
                  {"capacity", t.capacity},
                  {"count", t.count},
                  {"peak", t.peak},
                  {"dropped", t.dropped},
                  {"generation", t.generation},
                  {"changes", t.changes},
                  {"changing", t.changing}};
    json regions = json::array();
    for (const auto& reg : m.regions) {
        const std::string kind = reg.chip ? "chip" : "fast";
        if (!only.empty() && only != kind)
            continue;
        json jr = {{"name", reg.name},
                   {"kind", kind},
                   {"lower", ToHex(reg.lower, 8)},
                   {"upper", ToHex(reg.upper, 8)},
                   {"size", reg.upper - reg.lower},
                   {"free", reg.free},
                   {"largestFree", reg.largestFree},
                   {"freeChunks", reg.freeChunks},
                   {"allocated", reg.allocated},
                   {"untracked", reg.untracked},
                   {"overlaps", reg.overlaps}};
        if (withSegments) {
            json segs = json::array();
            for (const auto& seg : reg.segments) {
                json js = {{"start", ToHex(seg.start, 8)},
                           {"bytes", seg.end - seg.start},
                           {"kind", seg.kind == CrassMemSegment::Kind::Free        ? "free"
                                    : seg.kind == CrassMemSegment::Kind::Allocated ? "allocated"
                                                                                   : "untracked"}};
                if (seg.entry >= 0)
                    js["label"] = t.entries[static_cast<size_t>(seg.entry)].Label();
                segs.push_back(js);
            }
            jr["segments"] = segs;
        }
        regions.push_back(jr);
    }
    r["regions"] = regions;
    json allocs = json::array();
    for (const auto& e : t.entries) {
        bool inRegion = only.empty();
        for (const auto& reg : m.regions)
            if ((reg.chip ? "chip" : "fast") == only && e.addr >= reg.lower && e.addr < reg.upper)
                inRegion = true;
        if (!inRegion)
            continue;
        allocs.push_back({{"owner", std::string(1, e.owner)},
                          {"program", m.OwnerName(e.owner)},
                          {"name", e.Label()},
                          {"address", ToHex(e.addr, 8)},
                          {"size", e.size},
                          {"chip", e.Chip()},
                          {"named", e.Named()},
                          {"noted", (e.flags & kCrassMemMapNoted) != 0}});
    }
    r["allocations"] = allocs;
    return r;
}

json ExocetTimelineJson(ExocetSession& s, const json& args)
{
    const ExocetCheck& c = s.Check();
    if (!s.Pack())
        return Err(s.PackError().empty() ? "no .exocet beside the active document" : s.PackError());
    if (!c.valid)
        return Err(RowsUnknown(s));
    std::string error;
    const auto [from, to] = RowWindow(s, args, 33, 2000, &error);
    if (!error.empty())
        return Err(error);
    const int fps = s.Fps();
    json r;
    r["fps"] = fps;
    r["rows"] = c.rows.size();
    r["frames"] = c.frames;
    r["seconds"] = c.frames / static_cast<double>(fps);

    // the songs, as the rows run through them
    json songs = json::array();
    for (size_t n = 0; n < c.orders.size(); ++n) {
        json js = {{"song", n}, {"order", c.orders[n]}};
        if (n < c.digests.size())
            js["digest"] = "0x" + ToHex(c.digests[n], 8);
        int first = -1, last = -1;
        for (int i = 0; i < static_cast<int>(c.rows.size()); ++i)
            if (c.rows[static_cast<size_t>(i)].song == n) {
                if (first < 0)
                    first = i;
                last = i;
            }
        if (first >= 0) {
            js["firstRow"] = first;
            js["lastRow"] = last;
            js["firstFrame"] = c.rows[static_cast<size_t>(first)].frame;
            js["endFrame"] = last + 1 < static_cast<int>(c.rows.size())
                                 ? c.rows[static_cast<size_t>(last + 1)].frame
                                 : c.frames;
        }
        if (s.Pack() && static_cast<int>(n) < s.Pack()->SongCount())
            js["module"] = s.Pack()->ModulePath(static_cast<int>(n));
        songs.push_back(js);
    }
    r["songs"] = songs;

    const ExocetBlockState& b = s.Block();
    if (b.present && s.ManifestMatches() && b.row < c.rows.size()) {
        json ph = ExocetPlaceJson(c, b.row);
        const int ticks = s.RowTicks(b.row);
        ph["frac"] = b.frac;
        // the song frame's tick of its row (the fraction is floored, so it can read a tick early)
        const int64_t into = static_cast<int64_t>(b.frame) - c.rows[b.row].frame;
        ph["tick"] = std::clamp<int64_t>(into, 0, ticks - 1);
        ph["songFrame"] = b.frame;
        ph["clock"] = b.clock == 0 ? "music" : "frames";
        r["playhead"] = ph;
    } else {
        r["playhead"] = nullptr;
    }

    // the director's keys, by row, for the rows' `cut`
    const ExocetTrackInfo* director = s.Pack()->FindTrack(s.Pack()->Director());
    json rows = json::array();
    for (int i = from; i < to; ++i) {
        const ExocetRow& row = c.rows[static_cast<size_t>(i)];
        json jr = ExocetPlaceJson(c, i);
        jr["pattern"] = row.pattern;
        jr["ticks"] = row.frames;
        jr["seconds"] = row.frame / static_cast<double>(fps);
        if (s.RowBreak(i))
            jr["break"] = true;
        if (const auto* b = s.BookmarkAt(i)) {
            json m = json::object();
            if (b->slot)
                m["slot"] = b->slot;
            if (!b->name.empty())
                m["name"] = b->name;
            jr["bookmark"] = m;
        }
        if (director)
            for (const auto& k : s.HostKeys(director->index))
                if (k.row == i) {
                    const std::string name = EntryName(*director, k.value);
                    jr["cut"] = name.empty() ? json(k.value) : json(name);
                }
        rows.push_back(jr);
    }
    r["window"] = {{"from", from}, {"to", to}};
    r["rowsShown"] = rows;

    json cuts = json::array();
    for (const auto& cp : s.Cuts()) {
        json j = {{"cut", cp.cut}, {"name", cp.name}, {"key", ExocetPlaceJson(c, cp.keyRow)}};
        j["readyFrame"] = cp.readyFrame >= 0 ? json(cp.readyFrame) : json(nullptr);
        j["cutFrame"] = cp.cutFrame >= 0 ? json(cp.cutFrame) : json(nullptr);
        if (cp.keyRow >= 0 && cp.readyFrame >= 0) {
            j["margin"] = cp.margin;
            j["late"] = cp.late;
        }
        cuts.push_back(j);
    }
    r["cuts"] = cuts;

    const auto& names = s.PartNames();
    auto parts = [&](const std::vector<ExocetSession::PartRun>& runs) {
        json out = json::array();
        for (const auto& p : runs) {
            json j = {
                {"from", p.from}, {"to", p.to}, {"part", p.part}, {"phase", PhaseName(p.phase)}};
            if (p.part >= 1 && p.part <= names.size())
                j["name"] = names[p.part - 1];
            out.push_back(j);
        }
        return out;
    };
    r["partsShown"] = parts(s.PartsShown());
    r["partsLoaded"] = parts(s.PartsLoaded());
    return r;
}

json ExocetGridJson(ExocetSession& s, const json& args)
{
    ExocetPack* pack = s.Pack();
    const ExocetCheck& c = s.Check();
    if (!pack)
        return Err(s.PackError().empty() ? "no .exocet beside the active document" : s.PackError());
    if (!c.valid)
        return Err(RowsUnknown(s));
    std::string error;
    const auto [from, to] = RowWindow(s, args, 16, 256, &error);
    if (!error.empty())
        return Err(error);

    // the columns: the tracks named, the groups named, or every track
    std::vector<const ExocetTrackInfo*> cols;
    auto wanted = [&](const char* key, const std::string& value) {
        if (!args.contains(key))
            return false;
        const json& list = args[key];
        if (list.is_string())
            return list.get<std::string>() == value;
        for (const auto& v : list)
            if (v.is_string() && v.get<std::string>() == value)
                return true;
        return false;
    };
    const bool filtered = args.contains("tracks") || args.contains("groups");
    const bool keyedOnly = args.value("keyedOnly", false);
    for (const auto& t : pack->Tracks()) {
        if (filtered && !wanted("tracks", t.name) && !wanted("groups", t.group))
            continue;
        if (keyedOnly && s.HostKeys(t.index).empty())
            continue;
        cols.push_back(&t);
    }
    if (filtered && cols.empty())
        return Err("no track or group by those names (get_exocet lists them)");

    json r;
    json tracks = json::array();
    for (const auto* t : cols) {
        json jt = {{"index", t->index},
                   {"name", t->name},
                   {"group", t->group},
                   {"type", t->type},
                   {"min", t->min},
                   {"max", t->max},
                   {"def", t->def},
                   {"text", t->text},
                   {"keys", s.HostKeys(t->index).size()},
                   {"keyBreak", s.KeyBreaks().count(t->index) != 0}};
        if (!t->entries.empty()) {
            json e = json::array();
            for (const auto& en : t->entries)
                e.push_back({{"value", en.value}, {"name", en.name}});
            jt["entries"] = e;
        }
        tracks.push_back(jt);
    }
    r["tracks"] = tracks;

    const bool channels = args.value("channels", false);
    std::vector<std::string> groups;
    for (const auto* t : cols)
        if (std::find(groups.begin(), groups.end(), t->group) == groups.end())
            groups.push_back(t->group);
    json rows = json::array();
    for (int i = from; i < to; ++i) {
        const ExocetRow& row = c.rows[static_cast<size_t>(i)];
        json jr = ExocetPlaceJson(c, i);
        jr["ticks"] = row.frames;
        if (s.RowBreak(i))
            jr["break"] = true;
        if (const auto* b = s.BookmarkAt(i)) {
            json m = json::object();
            if (b->slot)
                m["slot"] = b->slot;
            if (!b->name.empty())
                m["name"] = b->name;
            jr["bookmark"] = m;
        }
        json values = json::object(), names = json::object(), keys = json::object();
        for (const auto* t : cols) {
            const int32_t v = s.HostValue(t->index, static_cast<uint32_t>(i), 0);
            values[t->name] = ValueJson(*t, v);
            if (const std::string n = EntryName(*t, v); !n.empty())
                names[t->name] = n;
            json ks = json::array();
            for (const auto& k : s.HostKeys(t->index))
                if (k.row == i)
                    ks.push_back({{"tick", s.KeyTick(k)},
                                  {"value", ValueJson(*t, k.value)},
                                  {"interp", ExocetInterpName(k.interp)}});
            if (!ks.empty())
                keys[t->name] = ks;
        }
        jr["values"] = values;
        if (!names.empty())
            jr["names"] = names;
        if (!keys.empty())
            jr["keys"] = keys;
        if (s.Activity().Seen(i)) {
            json act = json::object();
            for (const auto& g : groups)
                act[g] = StateName(s.Activity().At(g, i));
            jr["groups"] = act;
        }
        if (channels) {
            json ch = json::array();
            for (const auto& cell : row.cells)
                ch.push_back({{"note", ExocetNoteName(cell[1])},
                              {"sample", cell[0]},
                              {"effect", fmt::format("{:X}{:02X}", cell[2], cell[3])}});
            jr["channels"] = ch;
        }
        rows.push_back(jr);
    }
    r["window"] = {{"from", from}, {"to", to}};
    r["rows"] = rows;

    // the demo's own values on the playhead's row, beside what the keys say there
    const ExocetBlockState& b = s.Block();
    if (b.present && s.ManifestMatches()) {
        json live = json::object();
        for (const auto* t : cols)
            if (t->index < static_cast<int>(b.values.size()))
                live[t->name] = ValueJson(*t, b.values[static_cast<size_t>(t->index)]);
        r["playhead"] = ExocetPlaceJson(c, b.row);
        r["live"] = live;
    }
    return r;
}

json ExocetKeysJson(ExocetSession& s, const json& args)
{
    ExocetPack* pack = s.Pack();
    if (!pack)
        return Err("no .exocet beside the active document");
    const std::string only = args.value("track", std::string());
    if (!only.empty() && !pack->FindTrack(only))
        return Err("no track " + only + " in the manifest");
    const ExocetCheck& c = s.Check();
    json r;
    json out = json::object();
    size_t total = 0;
    for (const auto& name : pack->KeyedTracks()) {
        if (!only.empty() && name != only)
            continue;
        const ExocetTrackInfo* t = pack->FindTrack(name);
        json list = json::array();
        for (const auto& k : pack->Keys(name)) {
            json jk = {{"song", k.song}, {"pos", k.pos},       {"row", k.row},    {"pass", k.pass},
                       {"tick", k.tick}, {"interp", k.interp}, {"value", k.value}};
            if (t && TypeOf(*t) == ExocetType::Color)
                jk["value"] = ExocetColorText(static_cast<uint32_t>(k.value));
            if (t)
                if (const std::string n = EntryName(*t, static_cast<int32_t>(k.value)); !n.empty())
                    jk["name"] = n;
            const int played = c.valid ? c.FindRow(k.song, k.pos, k.row, k.pass) : -1;
            if (played >= 0) {
                jk["playedRow"] = played;
                jk["frame"] = c.rows[static_cast<size_t>(played)].frame + k.tick;
                jk["label"] =
                    ExocetPlaceLabel(c.rows[static_cast<size_t>(played)], c.orders.size() > 1);
            } else if (c.valid) {
                jk["played"] = false; // the song does not play that place
            }
            list.push_back(jk);
        }
        total += list.size();
        out[name] = list;
    }
    if (!only.empty() && !out.contains(only))
        out[only] = json::array();
    r["keys"] = out;
    r["total"] = total;
    r["dirty"] = pack->Dirty();
    if (!c.valid)
        r["note"] = "no played rows or frames until the kit's check: " + RowsUnknown(s);
    return r;
}

json ExocetMusicJson(ExocetSession& s, const json& args)
{
    ExocetPack* pack = s.Pack();
    const ExocetCheck& c = s.Check();
    if (!pack)
        return Err("no .exocet beside the active document");
    if (!c.valid)
        return Err(RowsUnknown(s));
    json r;
    r["musicSong"] = s.MusicSong();
    r["autoDb"] = pack->AutoDb();
    r["channels"] = c.channels;
    const int only = args.value("sample", 0);
    json samples = json::array();
    for (const auto& si : c.samples) {
        if (only && si.index != only)
            continue;
        const auto hits = s.HitRows(si.index);
        json j = {{"index", si.index},
                  {"name", si.name},
                  {"bytes", si.bytes},
                  {"volume", si.volume},
                  {"loop", si.loop},
                  {"used", si.used},
                  {"setting", pack->SampleSetting(si.index)},
                  {"hits", hits.size()}};
        if (si.assessed) {
            j["packed8"] = si.packed;
            j["packed4"] = si.delta4;
            j["db"] = si.db;
            j["worst"] = si.worst;
            j["codec"] = si.codec;
            j["why"] = si.why;
        }
        if (only) {
            json rows = json::array();
            for (const int h : hits)
                rows.push_back(ExocetPlaceJson(c, h));
            j["hitRows"] = rows;
        }
        samples.push_back(j);
    }
    if (only && samples.empty())
        return Err("no sample " + std::to_string(only) + " in the song");
    r["samples"] = samples;
    json lights = json::array();
    for (const auto& l : s.Lights())
        lights.push_back({{"trigger", std::round(l.trigger * 1000) / 1000},
                          {"level", std::round(l.level * 1000) / 1000}});
    r["lights"] = lights;
    if (!s.LastAudition().empty())
        r["lastAudition"] = s.LastAudition();
    return r;
}

json ExocetDiskMapJson(ExocetSession& s, const json& args)
{
    const ExocetDiskMapLoad load = LoadExocetDiskMap(s.Pack(), s.PackError(), s.DiskPath());
    json r;
    r["disk"] = s.DiskPath();
    r["ok"] = load.map.ok;
    if (!load.status.empty())
        r["status"] = load.status;
    if (!load.haveSpec)
        return r;
    const ExocetDiskSpec& spec = load.spec;
    const ExocetDiskMap& m = load.map;
    auto place = [&](uint32_t sector) {
        const uint32_t track = sector / std::max<uint32_t>(1, spec.sectorsPerTrack);
        return fmt::format("{}.{}", track / 2, track % 2);
    };
    r["name"] = spec.name;
    r["text"] = spec.text;
    r["sectorBytes"] = spec.sectorBytes;
    r["sectors"] = spec.sectors;
    r["sectorsPerTrack"] = spec.sectorsPerTrack;
    if (!m.ok)
        return r;
    r["usedEnd"] = m.usedEnd;
    r["freeSectors"] = spec.sectors > m.usedEnd ? spec.sectors - m.usedEnd : 0;
    r["warnings"] = m.warnings;
    json regions = json::array();
    for (const auto& g : m.regions)
        regions.push_back({{"name", g.name},
                           {"text", g.text},
                           {"first", g.first},
                           {"count", g.count},
                           {"bytes", static_cast<uint64_t>(g.count) * spec.sectorBytes},
                           {"cylHead", place(g.first)},
                           {"fromEntries", g.fromEntries}});
    r["regions"] = regions;
    json header = json::array();
    for (const auto& h : m.header)
        header.push_back({{"name", h.field ? h.field->name : std::string()},
                          {"value", h.value},
                          {"text", h.text}});
    r["header"] = header;
    json kinds = json::array();
    for (const auto& k : m.kinds)
        kinds.push_back({{"kind", k.kind},
                         {"name", k.name},
                         {"text", k.text},
                         {"packer", k.packer},
                         {"found", k.found},
                         {"blocks", k.blocks},
                         {"sectors", k.sectors},
                         {"packed", k.packed},
                         {"unpacked", k.unpacked}});
    r["kinds"] = kinds;

    // `kind` keeps one kind's runs and entries, by its name or value
    auto keep = [&](int kind, const std::string& name) {
        if (!args.contains("kind"))
            return true;
        const json& k = args["kind"];
        return k.is_number_integer() ? k.get<int>() == kind
                                     : k.is_string() && k.get<std::string>() == name;
    };
    json runs = json::array();
    for (const auto& u : m.runs)
        if (keep(u.kind, u.kindName))
            runs.push_back({{"kind", u.kindName},
                            {"firstEntry", u.firstEntry},
                            {"lastEntry", u.lastEntry},
                            {"firstSector", u.firstSector},
                            {"endSector", u.endSector},
                            {"cylHead", place(u.firstSector)},
                            {"blocks", u.blocks},
                            {"packed", u.packed},
                            {"unpacked", u.unpacked},
                            {"packer", u.packer},
                            {"packerOk", u.packerOk}});
    r["runs"] = runs;
    if (args.value("entries", true)) {
        json entries = json::array();
        for (const auto& e : m.entries)
            if (keep(e.kind, e.kindName))
                entries.push_back({{"index", e.index},
                                   {"kind", e.kindName},
                                   {"sector", e.sector},
                                   {"sectors", e.sectors},
                                   {"cylHead", place(e.sector)},
                                   {"packed", e.packed},
                                   {"unpacked", e.unpacked},
                                   {"streamOffset", e.streamOffset},
                                   {"packer", e.packer},
                                   {"slices", e.slices},
                                   {"fits", e.fits},
                                   {"packerOk", e.packerOk}});
        r["entries"] = entries;
    }
    return r;
}

json ExocetProofJson(ExocetSession& s)
{
    json r;
    r["busy"] = s.Busy() ? s.BusyWhat() : std::string();
    r["lastPush"] = s.LastPush();
    r["pushes"] = s.Pushes();
    r["recordedFrames"] = s.Recorded().size();
    if (!s.Recorded().empty()) {
        r["firstFrame"] = s.Recorded().begin()->first;
        r["lastFrame"] = s.Recorded().rbegin()->first;
    }
    const ExocetCheck& c = s.Check();
    json cuts = json::array();
    for (const auto& cp : s.Cuts()) {
        json j = {{"cut", cp.cut}, {"name", cp.name}, {"key", ExocetPlaceJson(c, cp.keyRow)}};
        j["readyFrame"] = cp.readyFrame >= 0 ? json(cp.readyFrame) : json(nullptr);
        j["cutFrame"] = cp.cutFrame >= 0 ? json(cp.cutFrame) : json(nullptr);
        if (cp.keyRow >= 0 && cp.readyFrame >= 0) {
            j["margin"] = cp.margin;
            j["late"] = cp.late;
        }
        cuts.push_back(j);
    }
    r["cuts"] = cuts;
    static const char* kStates[] = {"pending", "on time", "late", "never fired"};
    json events = json::array();
    for (const auto& e : s.Events()) {
        json j = {{"track", s.Pack() ? s.Pack()->Tracks()[e.track].name : std::string()},
                  {"value", e.value},
                  {"key", ExocetPlaceJson(c, e.keyRow)},
                  {"state", kStates[e.state]}};
        if (e.firedRow >= 0) {
            j["firedRow"] = e.firedRow;
            j["firedFrame"] = e.firedFrame;
            j["lateFrames"] = e.lateFrames;
        }
        events.push_back(j);
    }
    r["events"] = events;
    json div = json::object();
    for (const auto& [t, n] : s.Divergence())
        div[s.Pack()->Tracks()[t].name] = n;
    r["divergence"] = div;
    return r;
}

json ExocetIssuesJson(ExocetSession& s)
{
    using KS = ExocetSession::KitState;
    static const char* kKit[] = {"none", "missing", "untrusted", "ready"};
    json r;
    r["kit"] = kKit[static_cast<int>(s.GetKitState())];
    r["kitDir"] = s.Kit().dir;
    r["kitHash"] = s.KitHash();
    if (s.GetKitState() == KS::Missing)
        r["kitError"] = s.KitError();
    const auto& v = s.Tools();
    r["tools"] = {{"python", v.python}, {"lz4", v.lz4}, {"salvador", v.salvador}};
    r["checked"] = s.Check().valid;
    r["checkSeconds"] = s.LastCheckSeconds();
    r["busy"] = s.Busy() ? s.BusyWhat() : std::string();
    r["rows"] = s.Check().rows.size();
    r["editBlocked"] = s.EditBlockedReason();
    r["pushBlocked"] = s.PushBlockedReason();
    r["writeBlocked"] = s.WriteBlockedReason();
    r["lastWrite"] = s.LastWrite();
    r["injectBlocked"] = s.InjectBlockedReason();
    r["lastInject"] = s.LastInject();
    json issues = json::array();
    for (const auto& i : s.Issues())
        issues.push_back(i.ToJson());
    r["issues"] = issues;
    return r;
}

// ---------------------------------------------------------------------------------------------
// editing

namespace {

// A key's value from a tool's argument: a number, or a colour "#RRGGBBAA" / "#RRGGBB"
bool KeyValue(const json& args, double* value, std::string* error)
{
    uint32_t rgba = 0;
    if (!args.contains("value")) {
        *error = "value is required";
        return false;
    }
    const json& v = args["value"];
    if (v.is_number()) {
        *value = v.get<double>();
        return true;
    }
    if (v.is_string() && ExocetParseColor(v.get<std::string>(), &rgba)) {
        *value = rgba;
        return true;
    }
    *error = "value: a number, or a colour \"#RRGGBBAA\" / \"#RRGGBB\"";
    return false;
}

// One key edit, `merge` naming the undo step it joins. Empty on success.
std::string EditKey(ExocetSession& s, const json& args, bool remove, const std::string& merge,
                    bool* changed)
{
    const std::string track = args.value("track", std::string{});
    if (!s.Pack()->FindTrack(track))
        return "no track \"" + track + "\" in the manifest";
    std::string error;
    const int played = ExocetResolvePlace(s, args, &error);
    if (played < 0)
        return error;
    const int tick = args.value("tick", 0);
    if (tick < 0 || tick >= s.RowTicks(played))
        return "tick " + std::to_string(tick) + ": that row has " +
               std::to_string(s.RowTicks(played)) + " ticks";
    if (remove) {
        *changed = s.DeleteKey(track, played, tick, merge, &error);
        return error;
    }
    double value = 0;
    if (!KeyValue(args, &value, &error))
        return error;
    std::string interp = args.value("interp", std::string{});
    if (interp.empty()) {
        const ExocetRow& r = s.Check().rows[static_cast<size_t>(played)];
        const auto* k = s.Pack()->FindKey(track, r.song, r.pos, r.row, r.pass, tick);
        interp = k ? k->interp : "step";
    }
    if (!s.SetKey(track, played, value, interp, &error, merge, tick))
        return error;
    *changed = true;
    return {};
}

} // namespace

json ExocetSetKeyJson(ExocetSession& s, const json& args, bool remove)
{
    if (!s.Pack())
        return Err("no .exocet beside the active document");
    if (const std::string why = s.EditBlockedReason(); !why.empty())
        return Err(why);
    bool changed = false;
    if (const std::string error = EditKey(s, args, remove, {}, &changed); !error.empty())
        return Err(error);
    std::string error;
    const int played = ExocetResolvePlace(s, args, &error);
    json r;
    r[remove ? "deleted" : "set"] = changed;
    r["playedRow"] = played;
    r["tick"] = args.value("tick", 0);
    if (args.value("save", false)) {
        if (!s.Save(&error))
            return Err("the key was changed but not saved: " + error);
        r["saved"] = true;
    }
    r["dirty"] = s.Pack()->Dirty();
    r["keys"] = s.Pack()->Keys(args.value("track", std::string{})).size();
    return r;
}

json ExocetSetKeysJson(ExocetSession& s, const json& args)
{
    ExocetPack* pack = s.Pack();
    if (!pack)
        return Err("no .exocet beside the active document");
    if (const std::string why = s.EditBlockedReason(); !why.empty())
        return Err(why);
    if (!args.contains("keys") || !args["keys"].is_array() || args["keys"].empty())
        return Err("keys: a list of {track, place, value | delete}");
    // All of them or none: each edit joins one undo step, which is undone if any is refused
    const std::string merge = "bulk-" + std::to_string(++g_bulkEdits);
    pack->BreakMerge();
    const size_t stepsBefore = pack->UndoSteps();
    const ExocetPack::Checkpoint checkpoint = pack->MakeCheckpoint();
    int set = 0, deleted = 0, missing = 0;
    for (size_t i = 0; i < args["keys"].size(); ++i) {
        const json& k = args["keys"][i];
        std::string error;
        bool remove = false, changed = false;
        try {
            if (!k.is_object())
                error = "not a {track, place, value | delete}";
            else {
                remove = k.value("delete", false);
                error = EditKey(s, k, remove, merge, &changed);
            }
        } catch (const std::exception& e) {
            error = e.what(); // a wrong-typed field: refused like any other, and taken back
        }
        if (!error.empty()) {
            pack->Rollback(checkpoint);
            return Err("keys[" + std::to_string(i) + "]: " + error + " - nothing was changed");
        }
        if (!remove)
            ++set;
        else if (changed)
            ++deleted;
        else
            ++missing;
    }
    pack->BreakMerge();
    json r = {{"set", set}, {"deleted", deleted}};
    if (missing)
        r["notFound"] = missing; // deletes of keys that were not there
    r["undoSteps"] = pack->UndoSteps() - stepsBefore;
    if (args.value("save", false)) {
        std::string error;
        if (!s.Save(&error))
            return Err("the keys were changed but not saved: " + error);
        r["saved"] = true;
    }
    r["dirty"] = pack->Dirty();
    r["keys"] = pack->KeyCount();
    return r;
}

json ExocetEveryHitJson(ExocetSession& s, const json& args)
{
    if (!s.Pack())
        return Err("no .exocet beside the active document");
    const std::string track = args.value("track", std::string{});
    const int sample = args.value("sample", 0);
    std::string error;
    const int played = ExocetResolvePlace(s, args, &error);
    if (played < 0)
        return Err(error);
    const int set = s.KeyEveryHit(track, sample, played, &error);
    if (!error.empty())
        return Err(error);
    return json{{"set", set},
                {"hits", s.HitRows(sample).size()},
                {"keys", s.Pack()->Keys(track).size()},
                {"dirty", s.Pack()->Dirty()}};
}

json ExocetClearTrackJson(ExocetSession& s, const json& args)
{
    if (!s.Pack())
        return Err("no .exocet beside the active document");
    const std::string track = args.value("track", std::string{});
    std::string error;
    const int cleared = s.ClearTrack(track, &error);
    if (cleared < 0)
        return Err(error);
    json r = {{"track", track}, {"cleared", cleared}};
    if (args.value("save", false)) {
        if (!s.Save(&error))
            return Err("the track was cleared but not saved: " + error);
        r["saved"] = true;
    }
    r["dirty"] = s.Pack()->Dirty();
    r["keys"] = s.Pack()->KeyCount();
    return r;
}

json ExocetEditJson(ExocetSession& s, const std::string& action)
{
    ExocetPack* pack = s.Pack();
    if (!pack)
        return Err("no .exocet beside the active document");
    json r;
    if (action == "save") {
        std::string error;
        if (!s.Save(&error))
            return Err("not saved: " + error);
        r["saved"] = pack->Path();
    } else if (action == "undo" || action == "redo") {
        const bool undo = action == "undo";
        if (const std::string why = s.EditBlockedReason(); !why.empty())
            return Err(why);
        if (undo ? !pack->CanUndo() : !pack->CanRedo())
            return Err(undo ? "nothing to undo" : "nothing to redo");
        if (!(undo ? s.Undo() : s.Redo()))
            return Err(action + " failed");
        r[undo ? "undone" : "redone"] = true;
    } else {
        return Err("action: undo, redo or save");
    }
    r["dirty"] = pack->Dirty();
    r["canUndo"] = pack->CanUndo();
    r["canRedo"] = pack->CanRedo();
    r["keys"] = pack->KeyCount();
    return r;
}

json ExocetCheckJson(ExocetSession& s, const json& args)
{
    using KS = ExocetSession::KitState;
    if (!s.Pack())
        return Err("no .exocet beside the active document");
    json r;
    if (s.GetKitState() == KS::Untrusted) {
        if (!args.value("trust", false))
            return Err("the kit is not trusted: it runs Python from " + s.Kit().dir +
                       ". Trust it in the Exocet window, or pass trust: true");
        s.TrustKit();
        r["trusted"] = s.KitHash();
    }
    if (s.GetKitState() == KS::None)
        return Err("the .exocet names no kit");
    if (s.GetKitState() == KS::Missing)
        return Err("the kit is missing: " + s.KitError());
    s.RequestCheck();
    r["requested"] = true;
    r["busy"] = s.Busy() ? s.BusyWhat() : std::string();
    r["note"] = "the check runs on a worker thread after a short debounce; get_exocet_issues' "
                "`checked`, `rows` and `checkSeconds` say when it is done";
    return r;
}

// ---------------------------------------------------------------------------------------------
// the machine

json ExocetPushJson(ExocetSession& s)
{
    if (const std::string why = s.PushBlockedReason(); !why.empty())
        return Err(why);
    if (!s.Push())
        return Err("the push could not start");
    // The kit packs on a worker thread and the arena is written on the main thread once it
    // is done, so this returns at once: get_exocet_proof's `lastPush` says how it went.
    return json{{"started", true}, {"pushesBefore", s.Pushes()}};
}

json ExocetInjectJson(ExocetSession& s, const json& args)
{
    if (const std::string why = s.InjectBlockedReason(); !why.empty())
        return Err(why);
    const std::string module = args.value("module", std::string{});
    if (!s.Inject(module))
        return Err("the injection could not start");
    return json{{"started", true},
                {"note", "the kit exports and packs on a worker thread, Peekaboo writes the "
                         "paused machine, and the player reseats when it runs: "
                         "get_exocet_issues' `lastInject` follows it"}};
}

json ExocetWriteJson(ExocetSession& s, const json& args)
{
    if (const std::string why = s.WriteBlockedReason(); !why.empty())
        return Err(why);
    const std::string path = args.value("path", std::string{});
    if (!s.WriteAdf(path))
        return Err("the write could not start");
    return json{{"started", true},
                {"target", path.empty() ? s.DiskPath() : path},
                {"note", "the kit exports, packs and rebuilds on a worker thread; "
                         "get_exocet_issues' `lastWrite` says when the disk is written"}};
}

json ExocetSeekJson(ExocetSession& s, const json& args,
                    const std::function<std::string(uint64_t)>& seek)
{
    if (!seek)
        return Err("frame history unavailable");
    const ExocetCheck& c = s.Check();
    std::string error;
    const int played = ExocetResolvePlace(s, args, &error);
    if (played < 0)
        return Err(error);
    // a song frame asked for is that frame; a place is its row's first, or the tick's
    uint32_t songFrame = 0;
    if (args.contains("frame")) {
        songFrame = args["frame"].get<uint32_t>();
    } else {
        const int tick = args.value("tick", 0);
        if (tick < 0 || tick >= s.RowTicks(played))
            return Err("tick " + std::to_string(tick) + ": that row has " +
                       std::to_string(s.RowTicks(played)) + " ticks");
        songFrame = c.rows[static_cast<size_t>(played)].frame + static_cast<uint32_t>(tick);
    }
    uint64_t emu = 0;
    if (!s.EmuFrameOf(songFrame, emu)) {
        const auto& rec = s.Recorded();
        if (rec.empty())
            return Err(fmt::format("song frame {} was not played since the boot or the last push: "
                                   "nothing is recorded. Set a row breakpoint on it and run to "
                                   "it instead",
                                   songFrame));
        // the demo did not evaluate every song frame (it loads, its clock jumps): the nearest
        // one it did is the useful answer
        auto near = rec.lower_bound(songFrame);
        if (near == rec.end())
            near = std::prev(near);
        else if (near != rec.begin() &&
                 songFrame - std::prev(near)->first < near->first - songFrame)
            near = std::prev(near);
        const bool inside = songFrame >= rec.begin()->first && songFrame <= rec.rbegin()->first;
        return Err(fmt::format(
            "song frame {} {}; {} of song frames {} to {} were seen since the boot or the last "
            "push. The nearest is song frame {} ({}). Ahead of what was played, set a row "
            "breakpoint and run to it instead",
            songFrame,
            inside ? "is a frame the demo did not evaluate (it was loading, or its clock jumped)"
                   : "was not played since the boot or the last push",
            rec.size(), rec.begin()->first, rec.rbegin()->first, near->first,
            PlaceTextOf(c, c.RowAtFrame(near->first))));
    }
    if (const std::string why = seek(emu); !why.empty())
        return Err(why);
    json r = {{"seeked", true}, {"songFrame", songFrame}, {"machineFrame", emu}};
    r["place"] = ExocetPlaceJson(c, played);
    return r;
}

json ExocetBookmarksJson(ExocetSession& s, const json& args)
{
    ExocetPack* pack = s.Pack();
    if (!pack)
        return Err("no .exocet beside the active document");
    const std::string action = args.value("action", std::string("list"));
    const ExocetCheck& c = s.Check();
    json r = json::object();
    if (action == "clear") {
        if (const std::string why = s.EditBlockedReason(); !why.empty())
            return Err(why);
        r["removed"] = pack->ClearBookmarks();
    } else if (action == "add" || action == "remove") {
        std::string error;
        const int row = ExocetResolvePlace(s, args, &error);
        if (row < 0)
            return Err(error);
        const ExocetBookmarkInfo* had = s.BookmarkAt(row);
        if (action == "remove") {
            if (!had)
                return Err("no bookmark at " + PlaceTextOf(c, row));
            s.RemoveBookmark(row);
        } else {
            // a new one takes the lowest free slot; one there keeps its slot and name
            int slot = had ? had->slot : s.FreeBookmarkSlot();
            std::string name = had ? had->name : std::string();
            if (args.contains("slot")) {
                if (!args["slot"].is_number_integer())
                    return Err("slot: 1 to 9, or 0 for none");
                slot = args["slot"].get<int>();
            }
            if (args.contains("name"))
                name = args["name"].is_string() ? args["name"].get<std::string>() : "";
            if (!s.SetBookmark(row, slot, name, &error))
                return Err(error);
        }
    } else if (action != "list") {
        return Err("action: list, add, remove or clear");
    }
    // the list, in song order: a bookmark on a place the song does not play by its place
    json marks = json::array();
    for (const auto& b : pack->Bookmarks()) {
        const int played = c.valid ? c.FindRow(b.song, b.pos, b.row, b.pass) : -1;
        json m = played >= 0 ? ExocetPlaceJson(c, played)
                             : json{{"song", b.song},
                                    {"pos", b.pos},
                                    {"row", b.row},
                                    {"pass", b.pass},
                                    {"played", false}};
        m["slot"] = b.slot ? json(b.slot) : json(nullptr);
        m["name"] = b.name;
        marks.push_back(m);
    }
    r["bookmarks"] = marks;
    r["count"] = marks.size();
    r["dirty"] = pack->Dirty();
    return r;
}

json ExocetBreakpointsJson(ExocetSession& s, const json& args)
{
    ExocetPack* pack = s.Pack();
    if (!pack)
        return Err("no .exocet beside the active document");
    const std::string action = args.value("action", std::string("list"));
    const std::string kind = args.value("kind", std::string());
    const ExocetCheck& c = s.Check();

    auto trackArg = [&](std::string* error) -> const ExocetTrackInfo* {
        const std::string name = args.value("track", std::string());
        const ExocetTrackInfo* t = pack->FindTrack(name);
        if (!t)
            *error = name.empty() ? "track is required" : "no track \"" + name + "\"";
        return t;
    };

    if (action == "clear") {
        s.ClearBreaks();
    } else if (action == "add" || action == "remove") {
        const bool on = action == "add";
        std::string error;
        if (kind == "row") {
            const int row = ExocetResolvePlace(s, args, &error);
            if (row < 0)
                return Err(error);
            s.SetRowBreak(row, on);
        } else if (kind == "key") {
            const ExocetTrackInfo* t = trackArg(&error);
            if (!t)
                return Err(error);
            s.SetKeyBreak(t->index, on);
        } else if (kind == "event") {
            const ExocetTrackInfo* t = trackArg(&error);
            if (!t)
                return Err(error);
            if (t->type != "event")
                return Err(t->name + " is a " + t->type +
                           " track; an event break is on an event "
                           "track (a key break stops on any keyed "
                           "row)");
            std::optional<int32_t> value;
            if (args.contains("value") && args["value"].is_number_integer()) {
                value = args["value"].get<int32_t>();
            } else if (args.contains("value") && args["value"].is_string()) {
                const std::string want = args["value"].get<std::string>();
                for (const auto& e : t->entries)
                    if (e.name == want)
                        value = e.value;
                if (!value)
                    return Err("no value named \"" + want + "\" on " + t->name);
            }
            s.SetEventBreak(t->index, value, on);
        } else if (kind == "cut") {
            int cut = -1;
            if (args.contains("cut") && args["cut"].is_number_integer()) {
                cut = args["cut"].get<int>();
            } else if (args.contains("cut") && args["cut"].is_string()) {
                const std::string want = args["cut"].get<std::string>();
                const ExocetTrackInfo* d = pack->FindTrack(pack->Director());
                if (d)
                    for (const auto& e : d->entries)
                        if (e.name == want)
                            cut = e.value;
                if (cut < 0)
                    return Err("no cut named \"" + want + "\"");
            }
            s.SetCutBreak(cut, args.value("ready", false), on);
        } else {
            return Err("kind: row, key, event or cut");
        }
    } else if (action != "list") {
        return Err("action: list, add, remove or clear");
    }

    json r;
    json rows = json::array();
    for (const auto& [song, pos, row, pass] : s.RowBreakPlaces()) {
        const int played = c.valid ? c.FindRow(song, pos, row, pass) : -1;
        rows.push_back(played >= 0
                           ? ExocetPlaceJson(c, played)
                           : json{{"song", song}, {"pos", pos}, {"row", row}, {"pass", pass}});
    }
    r["rows"] = rows;
    json keys = json::array();
    for (const int t : s.KeyBreaks())
        if (t >= 0 && static_cast<size_t>(t) < pack->Tracks().size())
            keys.push_back(pack->Tracks()[static_cast<size_t>(t)].name);
    r["keys"] = keys;
    json events = json::array();
    for (const auto& [t, value] : s.EventBreaks()) {
        if (t < 0 || static_cast<size_t>(t) >= pack->Tracks().size())
            continue;
        const auto& info = pack->Tracks()[static_cast<size_t>(t)];
        json e = {{"track", info.name}, {"value", value ? json(*value) : json(nullptr)}};
        if (value)
            if (const std::string n = EntryName(info, *value); !n.empty())
                e["name"] = n;
        events.push_back(e);
    }
    r["events"] = events;
    json cuts = json::array();
    const ExocetTrackInfo* d = pack->FindTrack(pack->Director());
    for (const auto& [cut, ready] : s.CutBreaks()) {
        json e = {{"cut", cut < 0 ? json("any") : json(cut)}, {"ready", ready}};
        if (d && cut >= 0)
            if (const std::string n = EntryName(*d, cut); !n.empty())
                e["name"] = n;
        cuts.push_back(e);
    }
    r["cuts"] = cuts;
    r["count"] = s.BreakCount();
    const auto& last = s.LastBreak();
    if (last.hits)
        r["lastBreak"] = {{"kind", last.kind},
                          {"what", last.what},
                          {"place", ExocetPlaceJson(c, last.row)},
                          {"songFrame", last.frame},
                          {"hits", last.hits}};
    else
        r["lastBreak"] = nullptr;
    return r;
}

} // namespace adm::core
