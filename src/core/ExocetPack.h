#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm::core {

// A production's Exocet sync file - the `.exocet` in the sync pack beside the disk (the crass
// repo's docs/exocet.md, "The sync pack").
// JSON: the keys, which Peekaboo edits; the manifest - the tracks the demo declares - which
// the build writes and Peekaboo only reads; the song's order list and digest the keys were placed
// against; and the module and the kit, as paths relative to the file, so a pack works wherever it
// is unzipped.
//
// The parsed document is the source of truth, kept in the order it was read, so anything this
// does not know about is carried through a save untouched; the save writes exactly the layout
// tools/exocet.py does (exocet_dump), so a save is a readable `git diff` of the keys that moved.

struct ExocetGroupInfo
{
    std::string name;
    std::string text;
};

struct ExocetEntry
{
    int32_t value = 0;
    std::string name;
};

struct ExocetTrackInfo
{
    int index = 0;    // EXO_* order: the manifest's
    std::string name; // "group.name"
    std::string group;
    std::string type; // fix, int, index, ptr, event, color
    double min = 0, max = 0, def = 0;
    std::string text;
    std::vector<ExocetEntry> entries; // an event's named values, a pointer's table slots
};

struct ExocetKeyInfo
{
    int song = 0; // the .exocet's "songs" index: written first when it has more than one
    int pos = 0, row = 0, pass = 0;
    double value = 0; // a colour's 0xRRGGBBAA as a number
    std::string interp = "step";
    int tick = 0; // the tick of the row it sits on; written only when not 0, after "interp"
};

struct ExocetBookmarkInfo
{
    int song = 0; // written only when the .exocet has more than one, as a key's
    int pos = 0, row = 0, pass = 0;
    int slot = 0;     // 1-9, 0 for none
    std::string name; // empty for none
};

class ExocetPack
{
public:
    // The `.exocet` beside a disk, by the rules a program's `.elf` is found by
    // (DebugSymbols::FindSiblingElf): the same base name, else the one `.exocet` in the directory.
    static std::optional<std::string> FindSibling(const std::string& diskPath);

    bool Load(const std::string& path, std::string* error = nullptr);
    bool Save(std::string* error = nullptr);
    // The document as the file would hold it, for a push's pack without saving over the file.
    bool WriteTo(const std::string& path, std::string* error = nullptr) const;
    // The text Save writes.
    std::string Dump() const;

    const std::string& Path() const { return m_path; }
    std::string Production() const;
    // The pack's files, resolved against the `.exocet`'s directory; empty when not named. The
    // modules are the songs', in play order (the .exocet's "songs": an intro and a main song on
    // the Sneezeman disk, one for most).
    int SongCount() const;
    std::string ModulePath(int song) const;
    std::vector<std::string> ModulePaths() const;
    std::string KitPath() const;

    // The manifest as the build wrote it: EXO_MANIFEST, the groups and the tracks.
    uint32_t ManifestHash() const;
    const std::vector<ExocetGroupInfo>& Groups() const { return m_groups; }
    const std::vector<ExocetTrackInfo>& Tracks() const { return m_tracks; }
    const ExocetTrackInfo* FindTrack(const std::string& name) const;
    // The director: the track the demo declared with EXO_EVENT_DIRECTOR, whose values are the
    // cuts (the manifest's "director"); empty when it has none. And its group.
    const std::string& Director() const { return m_director; }
    std::string DirectorGroup() const
    {
        const auto* t = FindTrack(m_director);
        return t ? t->group : std::string();
    }

    // Each song's order list and digest the keys were placed against.
    std::vector<int> SongOrder(int song) const;
    uint32_t SongDigest(int song) const;

    // A track's keys, in the file's order. Tracks the manifest no longer has keep theirs.
    std::vector<ExocetKeyInfo> Keys(const std::string& track) const;
    // And as the file was last loaded or saved: what an edit is told apart from.
    std::vector<ExocetKeyInfo> SavedKeys(const std::string& track) const;
    std::vector<std::string> KeyedTracks() const;
    size_t KeyCount() const;

    // Editing, by the rules tools/exocet.py's resolve applies, so the kit never refuses what was
    // allowed here: the track must be the manifest's, the value in its range (a whole number but
    // for a fix track, never 0 on an event track, one of an event's entries when it has them),
    // the interpolation one of the four - and "step" for index, pointer and event tracks, which
    // have no other. A key at the same (position, row, pass, tick) is replaced; the list stays in
    // song order. `tick` is the tick of the row the key sits on (0..255 here; the session holds
    // it to the row's length). False with `error` set when refused.
    // Each edit is one undo step, but for one with the `merge` of the step before it (a colour
    // dragged through a hundred values is one step), until BreakMerge.
    bool SetKey(const std::string& track, int song, int pos, int row, int pass, double value,
                const std::string& interp, std::string* error = nullptr,
                const std::string& merge = {}, int tick = 0);
    bool DeleteKey(const std::string& track, int song, int pos, int row, int pass, int tick = 0,
                   const std::string& merge = {});
    // Every key on `track` removed, the track's list with them (the file lists only keyed
    // tracks), as one undo step. The keys removed; 0, and no step, when it had none.
    size_t ClearTrack(const std::string& track);
    const ExocetKeyInfo* FindKey(const std::string& track, int song, int pos, int row, int pass,
                                 int tick = 0) const;
    // The bookmarks: song places the editor marked, each with
    // a slot 1-9 that Ctrl+1..9 jumps to (0: none, past the ninth) and a name. The file's
    // "bookmarks" list, in song order; an edit is an undo step and makes the file dirty, as a
    // key's does. SetBookmark replaces the one at the place, and a slot taken by another
    // bookmark is taken from it. False with `error` set when refused.
    std::vector<ExocetBookmarkInfo> Bookmarks() const;
    bool SetBookmark(const ExocetBookmarkInfo& mark, std::string* error = nullptr);
    bool DeleteBookmark(int song, int pos, int row, int pass);
    // Every bookmark removed, one undo step; how many
    size_t ClearBookmarks();
    // The songs the keys are placed against, as saved: each song's order and digest.
    void SetSongs(const std::vector<std::vector<int>>& orders,
                  const std::vector<uint32_t>& digests);
    // Edits since the last Load or Save; undoing back to what was saved is none.
    bool Dirty() const { return m_dirty; }

    // Undo and redo of the key edits, as whole documents (a pack is a few hundred keys). A Load
    // forgets them; a Save does not. SetSong is part of the edit it follows, not a step.
    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    size_t UndoSteps() const { return m_undo.size(); }
    bool Undo();
    bool Redo();
    void BreakMerge() { m_merge.clear(); }
    // A point a run of edits can be taken back to whole: the document, and how many undo steps
    // had been made. Rollback restores the document and drops the steps made since, without
    // the redo an Undo would leave - a refused bulk edit is not a step to redo. Unlike comparing
    // UndoSteps(), it holds at the undo history's cap, where the count no longer grows.
    struct Checkpoint
    {
        nlohmann::ordered_json doc;
        uint64_t steps = 0;
    };
    Checkpoint MakeCheckpoint() const { return {m_doc, m_steps}; }
    void Rollback(const Checkpoint& checkpoint);
    // Bumped by every edit, so a view can tell its cached keys are stale.
    uint64_t Revision() const { return m_revision; }
    // The revision the file was last loaded or saved at.
    uint64_t SavedRevision() const { return m_savedRevision; }

    // The music section (the crass repo's docs/exocet.md, "The music"): how each sample may be
    // packed for the disk - "auto" (the kit decides by what the 4-bit sample measures), "8bit"
    // or "4bit" - and the bar, in dB, auto takes 4-bit over. A sample the section does not name
    // is auto; setting one back to auto takes it out. Each change is an undo step and, like a
    // key edit, bumps the revision, so the kit checks again.
    static constexpr double kAutoDb = 30.0; // the kit's music_kit.AUTO_DB
    std::string SampleSetting(int sample) const;
    bool SetSampleSetting(int sample, const std::string& setting, std::string* error = nullptr);
    double AutoDb() const;
    // `merge`: one undo step for a drag of the slider
    bool SetAutoDb(double db, const std::string& merge = {});
    // The section as the file holds it, "" for none: an export is stale when it changes.
    std::string MusicText() const;

    const nlohmann::ordered_json& Document() const { return m_doc; }

private:
    void Index();
    static std::vector<ExocetKeyInfo> KeysIn(const nlohmann::ordered_json& doc,
                                             const std::string& track);
    // An edit made from `before`: an undo step, unless it merges with the last.
    void Remember(nlohmann::ordered_json before, const std::string& merge);
    // The document swapped for an undo step's.
    void Restored();

    std::string m_path;
    std::string m_director; // the manifest's "director"
    nlohmann::ordered_json m_doc;
    bool m_dirty = false;
    uint64_t m_revision = 0;
    uint64_t m_savedRevision = 0;
    mutable std::vector<ExocetKeyInfo> m_found; // FindKey's answer
    nlohmann::ordered_json m_saved;             // the document as last loaded or saved
    std::vector<nlohmann::ordered_json> m_undo, m_redo;
    std::string m_merge;  // the last step's merge, empty: none
    uint64_t m_steps = 0; // undo steps ever made, for Rollback
    std::vector<ExocetGroupInfo> m_groups;
    std::vector<ExocetTrackInfo> m_tracks;
};

// The JSON layout tools/exocet.py writes (exocet_dump): two-space indentation, a list of scalars
// and every object inside a list on one line, `", "` and `": "` between items.
std::string ExocetDump(const nlohmann::ordered_json& doc);

// "0x1c2d3e4f" -> 0x1c2d3e4f; 0 for anything else.
uint32_t ExocetParseHex(const std::string& text);

// A colour key as the file writes it: "#RRGGBBAA", or "#RRGGBB" with alpha FF (tools/exocet.py's
// color_value). False for anything else.
bool ExocetParseColor(const std::string& text, uint32_t* rgba);
// 0xRRGGBBAA -> "#RRGGBBAA", as tools/exocet.py's color_text writes it.
std::string ExocetColorText(uint32_t rgba);

} // namespace adm::core
