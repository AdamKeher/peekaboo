#include "ExocetPack.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <tuple>

#include "JsonRead.h"

namespace adm::core {
namespace {

using ojson = nlohmann::ordered_json;

// A scalar as Python's json.dumps writes it with ensure_ascii off: nlohmann's own dump, which
// agrees on numbers (shortest round trip, "12.0" for a whole float) and on string escapes.
std::string Scalar(const ojson& v)
{
    return v.dump(-1, ' ', false, ojson::error_handler_t::replace);
}

// json.dumps(v, separators=(", ", ": ")): everything on one line.
std::string Flat(const ojson& v)
{
    if (v.is_object()) {
        if (v.empty())
            return "{}";
        std::string s = "{";
        bool first = true;
        for (const auto& [k, x] : v.items()) {
            if (!first)
                s += ", ";
            first = false;
            s += Scalar(ojson(k)) + ": " + Flat(x);
        }
        return s + "}";
    }
    if (v.is_array()) {
        if (v.empty())
            return "[]";
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i)
                s += ", ";
            s += Flat(v[i]);
        }
        return s + "]";
    }
    return Scalar(v);
}

// tools/exocet.py:exocet_dump's emit(): objects indented a key a line; a list of scalars on one
// line; a list with objects in it a line an item, each object flat.
std::string Emit(const ojson& v, int ind)
{
    const std::string pad(2 * ind, ' ');
    if (v.is_object()) {
        if (v.empty())
            return "{}";
        std::string s = "{\n";
        bool first = true;
        for (const auto& [k, x] : v.items()) {
            if (!first)
                s += ",\n";
            first = false;
            s += pad + "  " + Scalar(ojson(k)) + ": " + Emit(x, ind + 1);
        }
        return s + "\n" + pad + "}";
    }
    if (v.is_array()) {
        const bool scalars = std::all_of(
            v.begin(), v.end(), [](const ojson& x) { return !x.is_object() && !x.is_array(); });
        if (v.empty() || scalars)
            return Flat(v);
        std::string s = "[\n";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i)
                s += ",\n";
            s += pad + "  " + (v[i].is_object() ? Flat(v[i]) : Emit(v[i], ind + 1));
        }
        return s + "\n" + pad + "]";
    }
    return Scalar(v);
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

double Num(const ojson& o, const char* key, double def = 0)
{
    auto it = o.find(key);
    return it != o.end() && it->is_number() ? it->get<double>() : def;
}

// A key's value: a number, or a colour key's "#RRGGBBAA" as its number
double KeyValue(const ojson& o, const char* key)
{
    auto it = o.find(key);
    uint32_t rgba = 0;
    if (it != o.end() && it->is_string() && ExocetParseColor(it->get<std::string>(), &rgba))
        return rgba;
    return Num(o, key);
}

std::string Str(const ojson& o, const char* key)
{
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

std::string ExocetDump(const nlohmann::ordered_json& doc) { return Emit(doc, 0) + "\n"; }

uint32_t ExocetParseHex(const std::string& text)
{
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        return 0;
    try {
        return static_cast<uint32_t>(std::stoul(text.substr(2), nullptr, 16));
    } catch (...) {
        return 0;
    }
}

bool ExocetParseColor(const std::string& text, uint32_t* rgba)
{
    const size_t b = text.find_first_not_of(" \t"), e = text.find_last_not_of(" \t");
    if (b == std::string::npos || text[b] != '#')
        return false;
    const std::string hex = text.substr(b + 1, e - b);
    if ((hex.size() != 6 && hex.size() != 8) ||
        !std::all_of(hex.begin(), hex.end(), [](unsigned char c) { return std::isxdigit(c); }))
        return false;
    const uint32_t v = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
    if (rgba)
        *rgba = hex.size() == 6 ? (v << 8) | 0xFF : v;
    return true;
}

std::string ExocetColorText(uint32_t rgba)
{
    char buf[12];
    std::snprintf(buf, sizeof buf, "#%08X", static_cast<unsigned>(rgba));
    return buf;
}

std::optional<std::string> ExocetPack::FindSibling(const std::string& diskPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path disk(diskPath);
    fs::path dir = disk.parent_path();
    if (dir.empty())
        dir = ".";
    if (!fs::is_directory(dir, ec))
        return std::nullopt;

    // 1) Same base name: cra-sneezeman.adf -> cra-sneezeman.exocet
    fs::path same = disk;
    same.replace_extension(".exocet");
    if (fs::is_regular_file(same, ec))
        return same.string();

    // 2) The one .exocet in the directory; none, or more than one, is no pack
    std::optional<std::string> only;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file(ec) || Lower(entry.path().extension().string()) != ".exocet")
            continue;
        if (only)
            return std::nullopt;
        only = entry.path().string();
    }
    return only;
}

bool ExocetPack::Load(const std::string& path, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error)
            *error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    ojson doc = ojson::parse(ss.str(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        if (error)
            *error = path + " is not JSON";
        return false;
    }
    // the file arrives beside a disk: a wrong-typed version is refused, never thrown
    if (jsonread::ReadOr(doc, "exocet", 0) != 1) {
        if (error)
            *error = path + " is not an .exocet file this reads (\"exocet\": 1)";
        return false;
    }
    m_path = path;
    m_doc = std::move(doc);
    m_saved = m_doc;
    m_undo.clear();
    m_redo.clear();
    m_merge.clear();
    m_dirty = false;
    ++m_revision;
    m_savedRevision = m_revision;
    try {
        Index();
    } catch (const nlohmann::json::exception& e) {
        // a backstop: Index reads every field type-checked, so this is one it missed
        m_doc = ojson::object();
        m_saved = m_doc;
        m_path.clear();
        Index();
        if (error)
            *error = path + ": " + e.what();
        return false;
    }
    return true;
}

void ExocetPack::Index()
{
    m_groups.clear();
    m_tracks.clear();
    m_director.clear();
    auto man = m_doc.find("manifest");
    if (man == m_doc.end() || !man->is_object())
        return;
    m_director = Str(*man, "director");
    if (auto g = man->find("groups"); g != man->end() && g->is_array())
        for (const auto& x : *g)
            if (x.is_object())
                m_groups.push_back({Str(x, "name"), Str(x, "text")});
    if (auto t = man->find("tracks"); t != man->end() && t->is_array()) {
        int index = 0;
        for (const auto& x : *t) {
            if (!x.is_object())
                continue;
            ExocetTrackInfo info;
            info.index = index++;
            info.name = Str(x, "name");
            info.group = Str(x, "group");
            info.type = Str(x, "type");
            info.min = Num(x, "min");
            info.max = Num(x, "max");
            info.def = Num(x, "default");
            info.text = Str(x, "text");
            if (auto e = x.find("entries"); e != x.end() && e->is_array())
                for (const auto& y : *e)
                    if (y.is_object())
                        info.entries.push_back(
                            {static_cast<int32_t>(Num(y, "value")), Str(y, "name")});
            m_tracks.push_back(std::move(info));
        }
    }
}

bool ExocetPack::Save(std::string* error)
{
    if (m_path.empty()) {
        if (error)
            *error = "nothing loaded";
        return false;
    }
    if (!WriteTo(m_path, error))
        return false;
    m_saved = m_doc;
    m_savedRevision = m_revision;
    m_dirty = false;
    return true;
}

bool ExocetPack::WriteTo(const std::string& path, std::string* error) const
{
    // written beside and renamed over, so a failed write never leaves half a file
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error)
                *error = "cannot write " + tmp;
            return false;
        }
        out << Dump();
        if (!out) {
            if (error)
                *error = "cannot write " + tmp;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (error)
            *error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

std::string ExocetPack::Dump() const { return ExocetDump(m_doc); }

std::string ExocetPack::Production() const { return Str(m_doc, "production"); }

static std::string Resolve(const std::string& base, const std::string& rel)
{
    if (rel.empty())
        return {};
    const std::filesystem::path p = std::filesystem::path(base).parent_path() / rel;
    return p.lexically_normal().string();
}

static const ojson* SongAt(const ojson& doc, int song)
{
    auto s = doc.find("songs");
    if (s == doc.end() || !s->is_array() || song < 0 || song >= static_cast<int>(s->size()))
        return nullptr;
    const ojson& one = (*s)[static_cast<size_t>(song)];
    return one.is_object() ? &one : nullptr;
}

int ExocetPack::SongCount() const
{
    auto s = m_doc.find("songs");
    return s != m_doc.end() && s->is_array() ? static_cast<int>(s->size()) : 0;
}

std::string ExocetPack::ModulePath(int song) const
{
    const ojson* s = SongAt(m_doc, song);
    return s ? Resolve(m_path, Str(*s, "module")) : std::string();
}

std::vector<std::string> ExocetPack::ModulePaths() const
{
    std::vector<std::string> out;
    for (int i = 0; i < SongCount(); ++i)
        out.push_back(ModulePath(i));
    return out;
}

std::string ExocetPack::KitPath() const { return Resolve(m_path, Str(m_doc, "kit")); }

uint32_t ExocetPack::ManifestHash() const
{
    auto man = m_doc.find("manifest");
    return man != m_doc.end() && man->is_object() ? ExocetParseHex(Str(*man, "hash")) : 0;
}

const ExocetTrackInfo* ExocetPack::FindTrack(const std::string& name) const
{
    for (const auto& t : m_tracks)
        if (t.name == name)
            return &t;
    return nullptr;
}

std::vector<int> ExocetPack::SongOrder(int song) const
{
    std::vector<int> out;
    const ojson* s = SongAt(m_doc, song);
    if (!s)
        return out;
    if (auto o = s->find("order"); o != s->end() && o->is_array())
        for (const auto& x : *o)
            if (x.is_number_integer())
                out.push_back(x.get<int>());
    return out;
}

uint32_t ExocetPack::SongDigest(int song) const
{
    const ojson* s = SongAt(m_doc, song);
    return s ? ExocetParseHex(Str(*s, "digest")) : 0;
}

std::vector<ExocetKeyInfo> ExocetPack::Keys(const std::string& track) const
{
    return KeysIn(m_doc, track);
}

std::vector<ExocetKeyInfo> ExocetPack::SavedKeys(const std::string& track) const
{
    return KeysIn(m_saved, track);
}

std::vector<ExocetKeyInfo> ExocetPack::KeysIn(const ojson& doc, const std::string& track)
{
    std::vector<ExocetKeyInfo> out;
    auto keys = doc.find("keys");
    if (keys == doc.end() || !keys->is_object())
        return out;
    auto list = keys->find(track);
    if (list == keys->end() || !list->is_array())
        return out;
    for (const auto& k : *list) {
        if (!k.is_object())
            continue;
        ExocetKeyInfo info;
        info.song = static_cast<int>(Num(k, "song"));
        info.pos = static_cast<int>(Num(k, "pos"));
        info.row = static_cast<int>(Num(k, "row"));
        info.pass = static_cast<int>(Num(k, "pass"));
        info.value = KeyValue(k, "value");
        const std::string interp = Str(k, "interp");
        if (!interp.empty())
            info.interp = interp;
        info.tick = static_cast<int>(Num(k, "tick"));
        out.push_back(info);
    }
    return out;
}

std::vector<std::string> ExocetPack::KeyedTracks() const
{
    std::vector<std::string> out;
    auto keys = m_doc.find("keys");
    if (keys != m_doc.end() && keys->is_object())
        for (const auto& [name, list] : keys->items())
            out.push_back(name);
    return out;
}

size_t ExocetPack::KeyCount() const
{
    size_t n = 0;
    auto keys = m_doc.find("keys");
    if (keys != m_doc.end() && keys->is_object())
        for (const auto& [name, list] : keys->items())
            n += list.is_array() ? list.size() : 0;
    return n;
}

static bool Refuse(std::string* error, const std::string& why)
{
    if (error)
        *error = why;
    return false;
}

bool ExocetPack::SetKey(const std::string& track, int song, int pos, int row, int pass,
                        double value, const std::string& interp, std::string* error,
                        const std::string& merge, int tick)
{
    const ExocetTrackInfo* t = FindTrack(track);
    if (!t)
        return Refuse(error, track + " is not a track of this production");
    // a negative position is a song's lead, the kit's rows before the musician's
    if (song < 0 || (song && song >= SongCount()) || pos < -128 || row < 0 || row > 63 ||
        pass < 0 || tick < 0 || tick > 255)
        return Refuse(error, "not a place in the song");
    std::string how = interp.empty() ? "step" : interp;
    if (how != "step" && how != "linear" && how != "smooth" && how != "ramp")
        return Refuse(error, "\"" + how + "\" is not step, linear, smooth or ramp");
    if (t->type == "index" || t->type == "ptr" || t->type == "event")
        how = "step";
    const bool color = t->type == "color";
    if (color && !(value >= 0 && value <= 4294967295.0))
        return Refuse(error, track + " takes a colour, \"#RRGGBBAA\"");
    if (!color && !(value >= t->min && value <= t->max))
        return Refuse(error,
                      track + " takes " + ojson(t->min).dump() + ".." + ojson(t->max).dump());
    const bool fix = t->type == "fix";
    if (!fix && value != std::floor(value))
        return Refuse(error, track + " takes whole numbers");
    if (t->type == "event" && value == 0)
        return Refuse(error, "0 is no event");
    if (t->type == "event" && !t->entries.empty() &&
        std::none_of(t->entries.begin(), t->entries.end(),
                     [&](const ExocetEntry& e) { return e.value == value; }))
        return Refuse(error, track + " has no entry " + ojson(value).dump());

    ojson key = ojson::object();
    if (SongCount() > 1)
        key["song"] = song;
    key["pos"] = pos;
    key["row"] = row;
    key["pass"] = pass;
    if (fix)
        key["value"] = value;
    else if (color)
        key["value"] = ExocetColorText(static_cast<uint32_t>(value));
    else
        key["value"] = static_cast<int64_t>(value);
    key["interp"] = how;
    if (tick)
        key["tick"] = tick;

    ojson before = m_doc;
    if (!m_doc.contains("keys") || !m_doc["keys"].is_object())
        m_doc["keys"] = ojson::object();
    ojson& list = m_doc["keys"][track];
    if (!list.is_array())
        list = ojson::array();
    // an entry that is not a key object sorts first and matches nothing
    const auto place = [](const ojson& k) {
        if (!k.is_object()) {
            constexpr int kNone = std::numeric_limits<int>::min();
            return std::make_tuple(kNone, kNone, kNone, kNone, kNone);
        }
        return std::make_tuple(jsonread::ReadOr(k, "song", 0), jsonread::ReadOr(k, "pos", 0),
                               jsonread::ReadOr(k, "row", 0), jsonread::ReadOr(k, "pass", 0),
                               jsonread::ReadOr(k, "tick", 0));
    };
    const auto want = std::make_tuple(song, pos, row, pass, tick);
    size_t at = 0;
    while (at < list.size() && place(list[at]) < want)
        ++at;
    if (at < list.size() && place(list[at]) == want)
        list[at] = key;
    else
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), key);
    if (m_doc == before)
        return true; // the key it already was: no step
    Remember(std::move(before), merge);
    m_dirty = true;
    ++m_revision;
    return true;
}

bool ExocetPack::DeleteKey(const std::string& track, int song, int pos, int row, int pass, int tick,
                           const std::string& merge)
{
    auto keys = m_doc.find("keys");
    if (keys == m_doc.end() || !keys->is_object())
        return false;
    auto list = keys->find(track);
    if (list == keys->end() || !list->is_array())
        return false;
    for (size_t i = 0; i < list->size(); ++i) {
        const ojson& k = (*list)[i];
        if (!k.is_object())
            continue;
        if (jsonread::ReadOr(k, "song", 0) == song && jsonread::ReadOr(k, "pos", -1000) == pos &&
            jsonread::ReadOr(k, "row", -1) == row && jsonread::ReadOr(k, "pass", 0) == pass &&
            jsonread::ReadOr(k, "tick", 0) == tick) {
            ojson before = m_doc;
            // `list` is into m_doc, which the copy left alone
            list->erase(list->begin() + static_cast<std::ptrdiff_t>(i));
            Remember(std::move(before), merge);
            m_dirty = true;
            ++m_revision;
            return true;
        }
    }
    return false;
}

size_t ExocetPack::ClearTrack(const std::string& track)
{
    auto keys = m_doc.find("keys");
    if (keys == m_doc.end() || !keys->is_object())
        return 0;
    auto list = keys->find(track);
    if (list == keys->end() || !list->is_array() || list->empty())
        return 0;
    const size_t n = list->size();
    ojson before = m_doc;
    // `keys` and `list` are into m_doc, which the copy left alone
    keys->erase(list);
    Remember(std::move(before), {});
    m_dirty = true;
    ++m_revision;
    return n;
}

const ExocetKeyInfo* ExocetPack::FindKey(const std::string& track, int song, int pos, int row,
                                         int pass, int tick) const
{
    m_found = Keys(track);
    for (const auto& k : m_found)
        if (k.song == song && k.pos == pos && k.row == row && k.pass == pass && k.tick == tick)
            return &k;
    return nullptr;
}

void ExocetPack::Remember(ojson before, const std::string& merge)
{
    constexpr size_t kLimit = 500;
    m_redo.clear();
    if (!merge.empty() && merge == m_merge && !m_undo.empty())
        return; // the step before it already holds the document from before them both
    m_merge = merge;
    m_undo.push_back(std::move(before));
    ++m_steps;
    if (m_undo.size() > kLimit)
        m_undo.erase(m_undo.begin());
}

void ExocetPack::Rollback(const Checkpoint& checkpoint)
{
    for (uint64_t n = m_steps; n > checkpoint.steps && !m_undo.empty(); --n)
        m_undo.pop_back();
    m_steps = std::min(m_steps, checkpoint.steps);
    m_merge.clear();
    if (m_doc == checkpoint.doc)
        return;
    m_doc = checkpoint.doc;
    Restored();
}

bool ExocetPack::Undo()
{
    if (m_undo.empty())
        return false;
    m_redo.push_back(std::move(m_doc));
    m_doc = std::move(m_undo.back());
    m_undo.pop_back();
    Restored();
    return true;
}

bool ExocetPack::Redo()
{
    if (m_redo.empty())
        return false;
    m_undo.push_back(std::move(m_doc));
    m_doc = std::move(m_redo.back());
    m_redo.pop_back();
    Restored();
    return true;
}

void ExocetPack::Restored()
{
    m_merge.clear();
    m_dirty = m_doc != m_saved;
    ++m_revision;
    Index();
}

void ExocetPack::SetSongs(const std::vector<std::vector<int>>& orders,
                          const std::vector<uint32_t>& digests)
{
    auto s = m_doc.find("songs");
    if (s == m_doc.end() || !s->is_array())
        return;
    ojson songs = *s;
    for (size_t i = 0; i < songs.size() && i < orders.size() && i < digests.size(); ++i) {
        if (!songs[i].is_object())
            continue;
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08x", digests[i]);
        songs[i]["order"] = orders[i];
        songs[i]["digest"] = hex;
    }
    if (songs == *s)
        return;
    *s = songs;
    m_dirty = true;
    ++m_revision;
}

// ---- the bookmarks ----

std::vector<ExocetBookmarkInfo> ExocetPack::Bookmarks() const
{
    std::vector<ExocetBookmarkInfo> out;
    auto list = m_doc.find("bookmarks");
    if (list == m_doc.end() || !list->is_array())
        return out;
    for (const auto& b : *list) {
        if (!b.is_object())
            continue;
        ExocetBookmarkInfo info;
        info.song = static_cast<int>(Num(b, "song"));
        info.pos = static_cast<int>(Num(b, "pos"));
        info.row = static_cast<int>(Num(b, "row"));
        info.pass = static_cast<int>(Num(b, "pass"));
        info.slot = static_cast<int>(Num(b, "slot"));
        info.name = Str(b, "name");
        out.push_back(std::move(info));
    }
    return out;
}

bool ExocetPack::SetBookmark(const ExocetBookmarkInfo& mark, std::string* error)
{
    if (mark.song < 0 || (mark.song && mark.song >= SongCount()) || mark.pos < -128 ||
        mark.row < 0 || mark.row > 63 || mark.pass < 0)
        return Refuse(error, "not a place in the song");
    if (mark.slot < 0 || mark.slot > 9)
        return Refuse(error, "a bookmark's slot is 1 to 9, or 0 for none");
    ojson entry = ojson::object();
    if (SongCount() > 1)
        entry["song"] = mark.song;
    entry["pos"] = mark.pos;
    entry["row"] = mark.row;
    entry["pass"] = mark.pass;
    if (mark.slot)
        entry["slot"] = mark.slot;
    if (!mark.name.empty())
        entry["name"] = mark.name;

    ojson before = m_doc;
    ojson& list = m_doc["bookmarks"];
    if (!list.is_array())
        list = ojson::array();
    const auto place = [](const ojson& b) {
        return std::make_tuple(b.value("song", 0), b.value("pos", 0), b.value("row", 0),
                               b.value("pass", 0));
    };
    const auto want = std::make_tuple(mark.song, mark.pos, mark.row, mark.pass);
    // the slot is this one's alone now
    if (mark.slot)
        for (auto& b : list)
            if (b.is_object() && place(b) != want && b.value("slot", 0) == mark.slot)
                b.erase("slot");
    size_t at = 0;
    while (at < list.size() && place(list[at]) < want)
        ++at;
    if (at < list.size() && place(list[at]) == want)
        list[at] = entry;
    else
        list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), entry);
    if (m_doc == before)
        return true;
    Remember(std::move(before), {});
    m_dirty = m_doc != m_saved;
    ++m_revision;
    return true;
}

bool ExocetPack::DeleteBookmark(int song, int pos, int row, int pass)
{
    auto list = m_doc.find("bookmarks");
    if (list == m_doc.end() || !list->is_array())
        return false;
    for (size_t i = 0; i < list->size(); ++i) {
        const ojson& b = (*list)[i];
        if (b.value("song", 0) == song && b.value("pos", -1000) == pos &&
            b.value("row", -1) == row && b.value("pass", 0) == pass) {
            ojson before = m_doc;
            // `list` is into m_doc, which the copy left alone
            list->erase(list->begin() + static_cast<std::ptrdiff_t>(i));
            if (list->empty())
                m_doc.erase("bookmarks");
            Remember(std::move(before), {});
            m_dirty = m_doc != m_saved;
            ++m_revision;
            return true;
        }
    }
    return false;
}

size_t ExocetPack::ClearBookmarks()
{
    auto list = m_doc.find("bookmarks");
    if (list == m_doc.end() || !list->is_array() || list->empty())
        return 0;
    const size_t n = list->size();
    ojson before = m_doc;
    m_doc.erase("bookmarks");
    Remember(std::move(before), {});
    m_dirty = m_doc != m_saved;
    ++m_revision;
    return n;
}

// ---- the music section ----

std::string ExocetPack::SampleSetting(int sample) const
{
    auto m = m_doc.find("music");
    if (m == m_doc.end() || !m->is_object())
        return "auto";
    auto s = m->find("samples");
    if (s == m->end() || !s->is_object())
        return "auto";
    auto v = s->find(std::to_string(sample));
    return v != s->end() && v->is_string() ? v->get<std::string>() : "auto";
}

bool ExocetPack::SetSampleSetting(int sample, const std::string& setting, std::string* error)
{
    if (sample < 1 || sample > 31) {
        if (error)
            *error = "a module's samples are 1 to 31";
        return false;
    }
    if (setting != "auto" && setting != "8bit" && setting != "4bit") {
        if (error)
            *error = "a sample is auto, 8bit or 4bit";
        return false;
    }
    if (SampleSetting(sample) == setting)
        return true;
    ojson before = m_doc;
    ojson& music = m_doc["music"];
    if (!music.is_object())
        music = ojson::object();
    ojson& samples = music["samples"];
    if (!samples.is_object())
        samples = ojson::object();
    const std::string key = std::to_string(sample);
    if (setting == "auto")
        samples.erase(key);
    else
        samples[key] = setting;
    // in sample order, so a change is one line of diff wherever it lands
    std::vector<std::pair<int, std::string>> sorted;
    for (const auto& [k, v] : samples.items())
        if (v.is_string()) // anything else reads as "auto": dropped
            sorted.emplace_back(std::atoi(k.c_str()), v.get<std::string>());
    std::sort(sorted.begin(), sorted.end());
    samples = ojson::object();
    for (const auto& [k, v] : sorted)
        samples[std::to_string(k)] = v;
    if (samples.empty())
        music.erase("samples");
    if (music.empty())
        m_doc.erase("music");
    Remember(std::move(before), {});
    m_dirty = m_doc != m_saved; // a sample set back to how it was saved is no change
    ++m_revision;
    return true;
}

double ExocetPack::AutoDb() const
{
    auto m = m_doc.find("music");
    if (m == m_doc.end() || !m->is_object())
        return kAutoDb;
    auto a = m->find("auto_db");
    return a != m->end() && a->is_number() ? a->get<double>() : kAutoDb;
}

bool ExocetPack::SetAutoDb(double db, const std::string& merge)
{
    db = std::round(std::clamp(db, 0.0, 60.0) * 2.0) / 2.0; // half a dB is as fine as it matters
    if (db == AutoDb())
        return true;
    ojson before = m_doc;
    ojson& music = m_doc["music"];
    if (!music.is_object())
        music = ojson::object();
    if (db == kAutoDb) {
        music.erase("auto_db");
        if (music.empty())
            m_doc.erase("music");
    } else if (db == std::floor(db)) {
        music["auto_db"] = static_cast<int>(db); // 28, as a person writes it, not 28.0
    } else {
        music["auto_db"] = db;
    }
    Remember(std::move(before), merge);
    m_dirty = m_doc != m_saved;
    ++m_revision;
    return true;
}

std::string ExocetPack::MusicText() const
{
    auto m = m_doc.find("music");
    return m == m_doc.end() ? std::string() : m->dump();
}

} // namespace adm::core
