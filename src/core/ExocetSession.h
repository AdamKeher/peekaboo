#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "CrassMemMap.h"
#include "ExocetActivity.h"
#include "ExocetBlock.h"
#include "ExocetEval.h"
#include "ExocetKit.h"
#include "ExocetPack.h"
#include "LaunchOptions.h"
#include "PaulaStateSnapshot.h"

namespace adm::core {

// Everything the Exocet window and the MCP tools share: the active
// document's sync pack, its kit and the last `check`, the edits, the push, and the proofing record.
// Kit commands run on a worker thread against a snapshot of the keys; what they produce is taken up
// by Update() on the main thread, which is also the only place the machine is touched.
class ExocetSession
{
public:
    struct Hooks
    {
        std::function<const ExocetBlockState*()> block; // null when no core is running
        // the disk's memory map over exec's memory (core/CrassMemMap.h); null: none
        std::function<const ExocetMemoryState*()> memory;
        std::function<bool()> paused;
        std::function<bool(uint32_t addr, const uint8_t* data, size_t size)> writeRam;
        std::function<void()> releaseTimeline; // Application::ReleaseTimelineMachine
        std::function<void()> clearTimeline;   // Application::ClearFrameTimeline
        std::function<void()> stateChanged;
        // Write ADF: the new disk's bytes over `path` - eject it first if it is running, keep
        // the old one as <path>.bak, reload the document. False with `error` set.
        std::function<bool(const std::string& path, const std::vector<uint8_t>& disk,
                           std::string& error)>
            replaceDisk;
        std::function<void()> bootDisk;            // start the emulator on the active document
        std::function<PaulaStateSnapshot()> paula; // the live channels, for their lights
        // A row breakpoint: stop the emulator after `framesAhead` more frames - 0, after the
        // one that just completed (Application: RequestHostBreak / SetFrameBreakTarget)
        std::function<void(int framesAhead)> breakAt;
        // A module to listen to, in Peekaboo's player (the Music window's listening)
        std::function<void(const std::vector<uint8_t>& module, const std::string& title)> listen;
    };

    ExocetSession();
    ~ExocetSession();

    void SetHooks(Hooks hooks) { m_hooks = std::move(hooks); }
    void SetTools(const ExocetTools& tools);
    void SetTrustStore(std::function<bool(const std::string&)> isTrusted,
                       std::function<void(const std::string&)> trust);

    // The active document's pack (null for none) and why a pack that was found did not load.
    void SetPack(std::shared_ptr<ExocetPack> pack, const std::string& error,
                 const std::string& diskPath = {});
    const std::string& DiskPath() const { return m_diskPath; }
    ExocetPack* Pack() const { return m_pack.get(); }
    const std::string& PackError() const { return m_packError; }

    // Main thread, every UI frame: kit results, file watching, the proofing record.
    void Update();
    // Every emulated frame, from AmigaEmulator's frame-complete hook: the proofing record takes
    // each one (a UI frame can cover a batch of them, and turbo many), with a pack loaded.
    // `emuFrame` is the machine's frame number, the frame history's.
    void OnFrame(uint64_t emuFrame);

    // ---- breakpoints: pause the emulator on what the demo does ----
    // Row: as the demo starts a row - by song place (position, row, pass), so they stay on their
    // rows across a check; set and looked up by played row. Key: as it starts any row a track has
    // a key on. Event: as an event track fires (any value, or one). Cut: as a director cut is made
    // (or, `ready`, its part is ready); cut -1 is any. The session's only - not saved.
    bool RowBreak(int playedRow) const;
    void ToggleRowBreak(int playedRow);
    void SetRowBreak(int playedRow, bool on);
    // ---- bookmarks: the pack's (ExocetPack::Bookmarks), by played row ----
    // The played rows bookmarked, in song order, each with its bookmark; one the song does not
    // play is left out. Kept until the pack or the check changes.
    const std::map<int, ExocetBookmarkInfo>& BookmarkRows();
    const ExocetBookmarkInfo* BookmarkAt(int playedRow);
    // A bookmark on the row, or off it: on, it takes the lowest slot no other has (0 past
    // the ninth). False with `error` set when editing is refused (EditBlockedReason).
    bool ToggleBookmark(int playedRow, std::string* error = nullptr);
    // The row's bookmark set as given - its slot taken from any other that has it
    bool SetBookmark(int playedRow, int slot, const std::string& name,
                     std::string* error = nullptr);
    bool RemoveBookmark(int playedRow);
    // The lowest slot 1-9 no bookmark has, 0 for none
    int FreeBookmarkSlot();
    // The played row of the bookmark in `slot`, -1 for none
    int BookmarkSlotRow(int slot);

    // The first key-break track with a key on the played row, -1 for none.
    int KeyBreakTrack(int playedRow);
    void SetKeyBreak(int track, bool on);
    void SetEventBreak(int track, std::optional<int32_t> value, bool on);
    void SetCutBreak(int cut, bool ready, bool on);
    void ClearBreaks();
    size_t BreakCount() const
    {
        return m_rowBreaks.size() + m_keyBreaks.size() + m_eventBreaks.size() + m_cutBreaks.size();
    }
    const std::set<std::tuple<int, int, int, int>>& RowBreakPlaces() const { return m_rowBreaks; }
    // By the current pack's track index. The breaks are kept by track name, so they follow a
    // track across packs; one whose track this pack lacks is left out (and comes back with it).
    std::set<int> KeyBreaks() const;
    std::set<std::pair<int, std::optional<int32_t>>> EventBreaks() const;
    const std::set<std::pair<int, bool>>& CutBreaks() const { return m_cutBreaks; }
    // The last breakpoint that stopped the machine: its kind ("row", "key", "event", "cut") and
    // what (the place, track or cut), the played row and the song frame. `hits` counts them all.
    struct BreakHit
    {
        std::string kind, what;
        int row = -1;
        uint32_t frame = 0;
        uint64_t hits = 0;
    };
    const BreakHit& LastBreak() const { return m_lastBreak; }

    // ---- the kit ----
    enum class KitState {
        None,      // no pack, or the pack names no kit
        Missing,   // kit.json not there or not readable
        Untrusted, // never run until the user says so
        Ready
    };
    KitState GetKitState() const { return m_kitState; }
    const ExocetKitInfo& Kit() const { return m_kit; }
    // The song's frames a second: the running demo's (its block's fps), else the kit's
    // (params.json "fps"), else 50 - a song frame is a field on a PAL Amiga
    int Fps() const;
    const std::string& KitHash() const { return m_kitHash; }
    const std::string& KitError() const { return m_kitError; }
    void TrustKit();
    void RequestCheck();
    bool Busy() const { return m_busy; }
    const std::string& BusyWhat() const { return m_busyWhat; }
    const ExocetCheck& Check() const { return m_check; }
    // Tests only: take `check` as if the kit had produced it.
    void AdoptCheckForTest(ExocetCheck check);
    uint64_t CheckGeneration() const { return m_checkGen; }
    double LastCheckSeconds() const { return m_checkSeconds; }
    const ExocetToolVersions& Tools() const { return m_toolVersions; }

    // Every issue now: the tools', the last check's, the last push's and the proofing's - but
    // not a run's issues about the keys (exocet.*) once the keys have been edited since it.
    std::vector<ExocetIssue> Issues() const;
    // The first block issue stopping `action`, or empty; the action's own last run's issues
    // never do (running it again is how they are found out again).
    std::string BlockedBy(const std::string& action) const;

    // ---- editing, by the check's played rows ----
    std::string EditBlockedReason() const;
    // `tick` is the tick of the row the key sits on, 0..the row's frames - 1.
    bool SetKey(const std::string& track, int playedRow, double value, const std::string& interp,
                std::string* error = nullptr, const std::string& merge = {}, int tick = 0);
    // `merge`: as SetKey's, so a run of edits can be one undo step
    bool DeleteKey(const std::string& track, int playedRow, int tick = 0,
                   const std::string& merge = {}, std::string* error = nullptr);
    // Every key on `track` removed, one undo step: the keys removed, or -1 with `error` set when
    // editing is refused (EditBlockedReason) or the manifest has no such track.
    int ClearTrack(const std::string& track, std::string* error = nullptr);
    // Ticks in a played row (its frames), 1 for a row the check does not have.
    int RowTicks(int playedRow) const;
    // The tick of its row a key's sub puts it on (the first whose fraction reaches it).
    int KeyTick(const ExocetKey& k) const;
    // The pack's undo and redo, refused while editing is (EditBlockedReason).
    bool CanUndo() const;
    bool CanRedo() const;
    bool Undo();
    bool Redo();
    bool Save(std::string* error = nullptr);
    // Why the last save failed; empty after one that did not.
    const std::string& SaveError() const { return m_saveError; }
    // A track's keys as played rows and exocet.bin values, from the pack as edited.
    const std::vector<ExocetKey>& HostKeys(int track);
    int32_t HostValue(int track, uint32_t row, uint16_t frac);
    // A track's host key at (row, tick), or null.
    const ExocetKey* HostKeyAt(int track, int playedRow, int tick);

    // ---- the edit: the keys that differ from the .exocet as last saved (or loaded) ----
    struct KeyChange
    {
        enum Kind : uint8_t { Added, Changed, Removed } kind = Added;
        double was = 0;        // Changed and Removed: the saved key's value
        std::string wasInterp; // and its interpolation
    };
    // A track's, by (played row, sub) as HostKeys places a key: added, changed (value or
    // interpolation) or removed since the save. A saved key on a place the song does not play
    // has no cell, and is left out.
    const std::map<std::pair<int, int>, KeyChange>& Changes(int track);
    // Every track's, counted by kind: added, changed, removed
    std::array<int, 3> ChangeCounts();

    // ---- the live block ----
    const ExocetBlockState& Block() const { return m_block; }
    // The block as the machine holds it now, not as the last UI frame took it: what the Exocet
    // video reads at each emulated frame. Null without a running machine.
    const ExocetBlockState* LiveBlock() const { return m_hooks.block ? m_hooks.block() : nullptr; }
    // The disk's memory map, read when asked (the emulator caches it per frame); null without a
    // running machine. Main thread.
    const ExocetMemoryState* Memory() const { return m_hooks.memory ? m_hooks.memory() : nullptr; }
    bool ManifestMatches() const;
    // The emulator is paused (false without the hook)
    bool Paused() const { return m_hooks.paused && m_hooks.paused(); }

    // ---- the push ----
    std::string PushBlockedReason() const;
    bool Push(); // starts it; false (and PushBlockedReason) when it cannot
    const std::string& LastPush() const { return m_lastPush; }
    uint64_t Pushes() const { return m_pushes; }

    // ---- Write ADF ----
    std::string WriteBlockedReason() const;
    // The kit's export, pack and rebuild of the inserted disk, written over `path` (empty: the
    // disk itself, in the pack), the .exocet saved with it.
    bool WriteAdf(const std::string& path = {});
    const std::string& LastWrite() const { return m_lastWrite; }
    bool WroteDisk() const { return m_wroteDisk; }
    void BootWrittenDisk();

    // ---- music injection ----
    std::string InjectBlockedReason() const;
    // The module (or `module`, when given) exported and the keys packed against it by the kit,
    // then written into the paused machine for the player to reseat itself on: the new song
    // at the place the playhead is on, the frame history cleared.
    bool Inject(const std::string& module = {});
    const std::string& LastInject() const { return m_lastInject; }
    uint64_t Injections() const { return m_injections; }

    // ---- the samples: 8-bit or 4-bit (the crass repo's docs/exocet.md, "The music") ----
    // How a sample may be packed, as the pack says: "auto", "8bit" or "4bit". An edit, by the
    // rules key edits keep (EditBlockedReason), and checked again by the kit.
    bool SetSampleSetting(int sample, const std::string& setting, std::string* error = nullptr);
    bool SetAutoDb(double db, const std::string& merge = {});
    // Listening, in Peekaboo's player: the module as it is; the song as the Amiga will play it
    // (`sample` 0: every 4-bit sample as it decodes); one sample at C-3, 8-bit then 4-bit. The
    // last two are the kit's audition, so they run on the worker like a check.
    std::string AuditionBlockedReason() const;
    // A song's module (-1: the one the samples' report is of, MusicSong)
    bool ListenOriginal(int song = -1);
    bool Audition(int sample, int song = -1);
    int MusicSong() const;
    const std::string& LastAudition() const { return m_lastAudition; }

    // ---- the music, live ----
    // Per channel, 0..1: 1 when Paula was pointed at a sample (AUDxLC moved on a playing
    // voice), fading over a few frames; and the level measured off the audio tap.
    struct ChannelLight
    {
        float trigger = 0;
        float level = 0;
    };
    const std::array<ChannelLight, 4>& Lights() const { return m_lights; }

    // Key a track on every hit of a sample: the keys from `templateRow` (itself a row that
    // triggers the sample) up to the sample's next hit are copied, at the same offsets, onto
    // every other hit, each stopping short of the hit after it. Returns the keys set.
    int KeyEveryHit(const std::string& track, int sample, int templateRow,
                    std::string* error = nullptr);
    std::vector<int> HitRows(int sample) const;

    // ---- proofing ----
    struct Sample
    {
        uint16_t row = 0, frac = 0;
        uint32_t enabled = 0;
        std::vector<int32_t> values;
    };
    const std::map<uint32_t, Sample>& Recorded() const { return m_recorded; }
    // The machine frame the demo evaluated song frame `songFrame` on, as recorded since the boot
    // or the last push: what the grid seeks the frame history to. False when it was not seen.
    bool EmuFrameOf(uint32_t songFrame, uint64_t& out) const
    {
        const auto it = m_emuFrames.find(songFrame);
        if (it == m_emuFrames.end())
            return false;
        out = it->second;
        return true;
    }
    const std::map<uint32_t, uint64_t>& EmuFrames() const { return m_emuFrames; }

    // Which groups the demo had enabled on each played row of the checked song.
    // Not cleared by a push or a new boot - it is
    // the demo's, not the keys' - and kept across runs in the activity file, by the .exocet's path.
    const ExocetActivity& Activity() const { return m_activity; }
    void ForgetActivity();
    // Where the record is kept: exocet_activity.json in the working directory unless set.
    void SetActivityFile(const std::filesystem::path& file) { m_activityFile = file; }

    struct EventProof
    {
        int track = 0;
        int keyRow = 0; // played row
        int32_t value = 0;
        enum State {
            Pending, // its row has not come while recording
            OnTime,
            Late,
            Missed // its row came, it never fired (its group was off)
        } state = Pending;
        int firedRow = -1;
        int firedFrame = -1;
        int lateFrames = 0;
    };
    std::vector<EventProof> Events();

    struct CutProof
    {
        int cut = 0;
        std::string name;
        int keyRow = -1; // -1: no key for it
        uint32_t keyFrame = 0;
        int readyFrame = -1, cutFrame = -1; // -1: not yet
        int margin = 0;                     // frames ready before the key (negative: after it)
        int late = 0;                       // frames the cut came after its key
    };
    std::vector<CutProof> Cuts();

    // Frames recorded where a keyed value track read other than its keys say, by track.
    std::map<int, int> Divergence();
    void ClearRecord();

    // ---- the parts ----
    // What the demo was doing, by song frame, as its block said: the part on the screen and its
    // phase, and the part the disk read or unpacked for - runs of frames alike, in order. Kept
    // for one boot (a song frame far behind the last starts again), not cleared by a push.
    struct PartRun
    {
        uint32_t from = 0, to = 0; // song frames, `to` past the last
        uint8_t part = 0, phase = 0;
    };
    const std::vector<PartRun>& PartsShown() const { return m_partsShown; }
    const std::vector<PartRun>& PartsLoaded() const { return m_partsLoaded; }
    const std::vector<std::string>& PartNames() const { return m_partNames; }
    void ForgetParts();

private:
    struct Job;
    void StartJob(std::unique_ptr<Job> job);
    void RunJob(Job& job);
    void FinishJob(Job& job);
    void LoadKit();
    std::string Snapshot(const std::string& name) const; // the edited .exocet, in the scratch dir
    void ApplyPush(const std::vector<uint8_t>& bin);
    void Record(const ExocetBlockState& b);
    int FindTrackIndex(const std::string& name) const;
    std::vector<ExocetIssue> ProofIssues(); // Update() keeps m_proofIssues from it
    int DirectorIndex() const;

    Hooks m_hooks;
    ExocetTools m_tools;
    ExocetToolVersions m_toolVersions;
    bool m_toolsProbed = false;
    std::function<bool(const std::string&)> m_isTrusted;
    std::function<void(const std::string&)> m_trust;

    std::shared_ptr<ExocetPack> m_pack;
    std::string m_packError;
    std::string m_saveError;

    KitState m_kitState = KitState::None;
    ExocetKitInfo m_kit;
    mutable std::string m_fpsParams; // the params.json m_fpsKit was read from
    mutable int m_fpsKit = 0;
    std::string m_kitHash, m_kitError;
    std::vector<ExocetIssue> m_toolIssues;

    ExocetCheck m_check;
    uint64_t m_checkGen = 0;
    double m_checkSeconds = 0;
    bool m_checkWanted = false;
    double m_checkDue = 0; // debounce: steady seconds
    uint64_t m_checkedRevision = ~0ull;
    std::string m_checkMusic;             // the music section the last check started with
    uint64_t m_musicSeenRevision = ~0ull; // the pack revision it was last compared at
    uint64_t m_pushedRevision = 0;        // the pack revision each run's issues are of
    uint64_t m_writtenRevision = 0;
    uint64_t m_injectedRevision = 0;
    std::vector<ExocetIssue> IssuesBut(const std::vector<ExocetIssue>* skip) const;
    std::filesystem::file_time_type m_moduleTime{};

    std::vector<ExocetIssue> m_pushIssues;
    std::vector<ExocetIssue> m_writeIssues;
    std::array<ChannelLight, 4> m_lights{};
    std::array<uint32_t, 4> m_lastLc{};
    uint32_t m_lastLightFrame = ~0u;
    std::vector<ExocetIssue> m_injectIssues;
    std::string m_lastInject;
    uint64_t m_injections = 0;
    uint16_t m_reseatSent = 0; // the `reseat` last written; 0: none outstanding
    bool m_reseatPending = false;
    ExocetCheck m_playing; // the check of the song on the machine (by digest)
    void ApplyInject(const std::vector<uint8_t>& music, const std::vector<uint8_t>& bin);
    std::string m_lastWrite;
    bool m_wroteDisk = false;
    std::string m_diskPath;
    std::vector<ExocetIssue> m_proofIssues;
    double m_proofAt = 0;
    double m_lastPoll = 0;
    std::string m_lastPush;
    uint64_t m_pushes = 0;

    std::map<int, std::vector<ExocetKey>> m_hostKeys;
    uint64_t m_hostKeysRevision = ~0ull, m_hostKeysCheck = ~0ull;
    std::map<int, ExocetBookmarkInfo> m_bookmarkRows; // BookmarkRows'
    uint64_t m_bookmarksRevision = ~0ull, m_bookmarksCheck = ~0ull;
    std::map<int, std::map<std::pair<int, int>, KeyChange>> m_changes;
    uint64_t m_changesRevision = ~0ull, m_changesSaved = ~0ull, m_changesCheck = ~0ull;

    ExocetBlockState m_block;
    uint32_t m_lastBlockAddr = 0;
    uint16_t m_lastGeneration = 0;
    std::map<uint32_t, Sample> m_recorded;
    // Divergence's counts, kept as frames are recorded; counted again from m_recorded only when
    // the keys (the pack revision) or the check change. The keyed tracks it counts, with their
    // group's enable bit.
    void CountDivergence(const Sample& s, int sign);
    std::map<int, int> m_divCounts;
    std::vector<std::pair<int, uint32_t>> m_divTracks;
    uint64_t m_divRevision = ~0ull, m_divCheckGen = ~0ull;
    bool m_divValid = false;
    std::map<uint32_t, uint64_t> m_emuFrames; // song frame -> machine frame, from OnFrame only
    std::vector<PartRun> m_partsShown, m_partsLoaded;
    std::vector<std::string> m_partNames;
    uint32_t m_partsLastFrame = 0;
    void RecordParts(const ExocetBlockState& b);
    ExocetActivity m_activity;
    std::string m_activityKey; // the .exocet the record is for; empty: none yet
    std::vector<std::string> m_groupNames;
    std::filesystem::path m_activityFile = ProfilePath("exocet_activity.json");
    uint64_t m_activitySaved = 0;
    std::chrono::steady_clock::time_point m_activitySaveDue{};
    void SyncActivity();
    void SaveActivity();
    std::set<std::tuple<uint16_t, uint16_t, uint16_t>> m_fired; // track, row, frame
    std::set<std::tuple<int, int, int, int>> m_rowBreaks;       // song, pos, row, pass
    std::set<std::string> m_keyBreaks;                          // track names
    std::set<std::pair<std::string, std::optional<int32_t>>>
        m_eventBreaks;                          // name, value (none: any)
    std::set<std::pair<int, bool>> m_cutBreaks; // cut (-1 any), ready
    int m_breakLastRow = -1;                    // the played row the last frame was on, -1: none
    // What the last frame's block had fired and cut, so a break only sees what is new
    uint32_t m_breakBlockAddr = 0;
    uint16_t m_breakFired = 0;
    std::vector<ExocetCut> m_breakCuts;
    BreakHit m_lastBreak;
    void CheckBreaks(const ExocetBlockState& b);
    bool EventBreakMatches(const ExocetFire& f);
    bool CutBreakMatches(int cut, bool ready) const;
    std::string CutName(int cut) const;

    std::filesystem::path m_scratch; // this session's folder in the pack's exocet_temp
    std::string m_scratchName;       // its name: "session-<time>"
    void MoveScratch(const std::string& exocetPath);
    std::vector<std::vector<uint8_t>> m_musicBins; // the kit's exports of the songs, for a pack
    std::filesystem::file_time_type ModulesTime() const;
    std::filesystem::file_time_type m_musicBinTime{};
    std::string m_musicBinSettings; // the pack's music section it was exported with
    bool NeedExport() const;
    std::string m_lastAudition;

    std::thread m_worker;
    std::mutex m_mutex;
    std::unique_ptr<Job> m_done; // finished, not yet taken up
    // The job the worker is running (under m_mutex), so the destructor can stop its kit command
    // - and whatever that started - rather than wait out a timeout per remaining command
    Job* m_running = nullptr;
    std::atomic<bool> m_cancelJobs{false};
    std::atomic<bool> m_busy{false};
    std::string m_busyWhat;
};

} // namespace adm::core
