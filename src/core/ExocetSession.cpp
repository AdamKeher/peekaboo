#include "ExocetSession.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

#include "Logger.h"
#include "Subprocess.h"

namespace adm::core {
namespace fs = std::filesystem;
namespace {

double Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::vector<uint8_t> ReadBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

fs::file_time_type ModTime(const std::string& path)
{
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type{} : t;
}

constexpr double kCheckDebounce = 0.8; // seconds after the last edit
constexpr int kThinMargin = 25;        // frames: a cut ready less than half a second ahead

} // namespace

struct ExocetSession::Job
{
    enum Kind { Check, Push, Write, Inject, Audition } kind = Check;
    ExocetKitInfo kit;
    ExocetTools tools;
    std::vector<std::string> modules; // every song's, in play order
    std::string exocet, params;
    std::string musicSettings; // the pack's music section the export is made with
    std::string audition;      // Audition: "song" or a sample number
    int song = 0;              // Audition: which song's
    std::string title;
    std::string disk, target; // Write: the inserted ADF, and where the new one goes
    fs::path scratch;
    // The pack it was started for: the document can change while the kit runs (SetPack every
    // frame), and the result belongs to this one, not to whichever is active when it ends
    std::shared_ptr<ExocetPack> pack;
    bool needExport = false;
    uint64_t revision = 0;
    Subprocess proc; // the kit command running now, which the destructor cancels
    // out
    ExocetKitResult result;
    ExocetCheck check;
    std::vector<std::vector<uint8_t>> musics; // every song's music.bin
    std::vector<uint8_t> bin, adf;
    std::vector<ExocetIssue> issues;
};

ExocetSession::ExocetSession()
    : m_scratchName("session-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
{
    std::error_code ec;
    m_scratch = fs::path(ExocetScratchDir({})) / m_scratchName;
    fs::create_directories(m_scratch, ec);
}

void ExocetSession::MoveScratch(const std::string& exocetPath)
{
    const fs::path to = fs::path(ExocetScratchDir(exocetPath)) / m_scratchName;
    if (to == m_scratch)
        return;
    std::error_code ec;
    fs::remove_all(m_scratch, ec);
    fs::remove(m_scratch.parent_path(), ec); // exocet_temp too, when nothing else is in it
    m_scratch = to;
    fs::create_directories(m_scratch, ec);
}

ExocetSession::~ExocetSession()
{
    SaveActivity();
    // A hung kit command would hold the exit for its timeout, per command left in the job (a
    // write runs three): stop it instead. Cancelled again until the worker is through, as a
    // cancel that lands before a command starts would be lost; RunJob also checks the flag
    // between commands.
    m_cancelJobs = true;
    if (m_worker.joinable()) {
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_running)
                    break;
                m_running->proc.Cancel();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        m_worker.join();
    }
    std::error_code ec;
    fs::remove_all(m_scratch, ec);
    fs::remove(m_scratch.parent_path(), ec);
}

void ExocetSession::SetTools(const ExocetTools& tools)
{
    if (tools.python == m_tools.python && tools.toolsDir == m_tools.toolsDir && m_toolsProbed)
        return;
    m_tools = tools;
    m_toolsProbed = false;
    if (m_pack)
        LoadKit();
}

void ExocetSession::SetTrustStore(std::function<bool(const std::string&)> isTrusted,
                                  std::function<void(const std::string&)> trust)
{
    m_isTrusted = std::move(isTrusted);
    m_trust = std::move(trust);
}

void ExocetSession::SetPack(std::shared_ptr<ExocetPack> pack, const std::string& error,
                            const std::string& diskPath)
{
    m_diskPath = diskPath;
    if (pack == m_pack && error == m_packError)
        return;
    // A Write ADF reloads the document, which loads the pack again: the same pack, and what
    // was just written (and the offer to boot it) still stands.
    const bool samePack = pack && m_pack && pack->Path() == m_pack->Path();
    if (!samePack) {
        m_wroteDisk = false;
        m_lastWrite.clear();
        m_writeIssues.clear();
        SaveActivity();
        m_activity = ExocetActivity{};
        m_activityKey.clear();
        m_activitySaved = m_activity.Revision();
    }
    // the kit's work files beside the .exocet (ExocetScratchDir); a running job keeps its own
    if (!m_busy)
        MoveScratch(pack ? pack->Path() : std::string());
    m_pack = std::move(pack);
    m_packError = error;
    m_check = ExocetCheck{};
    m_pushIssues.clear();
    m_hostKeys.clear();
    m_hostKeysRevision = m_hostKeysCheck = ~0ull;
    m_checkedRevision = m_musicSeenRevision = ~0ull;
    m_musicBins.clear();
    ClearRecord();
    m_groupNames.clear();
    if (m_pack)
        for (const auto& g : m_pack->Groups())
            m_groupNames.push_back(g.name);
    LoadKit();
}

void ExocetSession::LoadKit()
{
    m_kit = ExocetKitInfo{};
    m_kitHash.clear();
    m_kitError.clear();
    m_toolIssues.clear();
    m_kitState = KitState::None;
    if (!m_pack || m_pack->KitPath().empty())
        return;
    std::string error;
    if (!m_kit.Load(m_pack->KitPath(), &error)) {
        m_kitState = KitState::Missing;
        m_kitError = error;
        return;
    }
    // the params are data the build rewrites per disk: trust is for the code
    std::vector<std::string> skip;
    if (!m_kit.paramsPath.empty())
        skip.push_back(fs::relative(m_kit.paramsPath, m_kit.dir).generic_string());
    m_kitHash = ExocetKitHash(m_kit.dir, skip);
    if (!m_toolsProbed) {
        m_toolVersions = ExocetProbeTools(m_tools);
        m_toolsProbed = true;
    }
    m_toolIssues = ExocetCheckRequires(m_kit, m_toolVersions);
    m_kitState = m_isTrusted && m_isTrusted(m_kitHash) ? KitState::Ready : KitState::Untrusted;
    m_moduleTime = ModulesTime();
    if (m_kitState == KitState::Ready)
        RequestCheck();
}

void ExocetSession::TrustKit()
{
    if (m_kitState != KitState::Untrusted || m_kitHash.empty())
        return;
    if (m_trust)
        m_trust(m_kitHash);
    Logger::Info("Exocet: trusted the kit in {} ({})", m_kit.dir, m_kitHash);
    m_kitState = KitState::Ready;
    RequestCheck();
}

void ExocetSession::RequestCheck()
{
    m_checkWanted = true;
    m_checkDue = 0;
}

std::string ExocetSession::Snapshot(const std::string& name) const
{
    const fs::path p = m_scratch / name;
    std::error_code ec;
    fs::create_directories(m_scratch, ec); // made again if something deleted it
    std::string error;
    if (!m_pack || !m_pack->WriteTo(p.string(), &error)) {
        Logger::Warn("Exocet: could not write the keys for the kit: {}", error);
        return {};
    }
    return p.string();
}

void ExocetSession::StartJob(std::unique_ptr<Job> job)
{
    if (m_worker.joinable())
        m_worker.join();
    m_busy = true;
    static const char* kWhat[] = {"check", "push", "write", "inject", "audition"};
    m_busyWhat = kWhat[job->kind];
    job->pack = m_pack;
    Job* raw = job.release();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = raw;
    }
    m_worker = std::thread([this, raw] {
        std::unique_ptr<Job> j(raw);
        RunJob(*j);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = nullptr;
        m_done = std::move(j);
    });
}

void ExocetSession::RunJob(Job& job)
{
    std::error_code ec;
    fs::create_directories(job.scratch, ec);
    const std::string root = job.scratch.string(); // each kit run's folder goes in it
    const auto run = [this, &job, &root](const char* role, const std::vector<std::string>& args) {
        if (m_cancelJobs) { // closing: no more commands
            ExocetKitResult stopped;
            stopped.issues.push_back(ExocetMakeIssue("block", "kit.cancelled",
                                                     "The kit's work was stopped",
                                                     "Peekaboo is closing.", {}));
            return stopped;
        }
        return ExocetRunKit(job.kit, job.tools, role, args, 120000, &job.proc, root);
    };
    // the modules last: the .exocet handed over is a copy in the scratch folder
    const auto withModules = [&job](std::vector<std::string> args) {
        args.insert(args.end(), job.modules.begin(), job.modules.end());
        return args;
    };
    if (job.kind == Job::Check) {
        job.result = run("check", withModules({job.exocet, job.params}));
        try {
            job.check.Parse(job.result.report);
        } catch (const std::exception& e) {
            // a backstop: on this worker thread a throw would be std::terminate
            job.check = ExocetCheck{};
            job.result.issues.push_back(ExocetMakeIssue(
                "block", "kit.report", "The kit's check report could not be read", e.what(), {}));
        }
        job.issues = job.result.issues;
        return;
    }
    if (job.kind == Job::Audition) {
        const fs::path modPath = job.scratch / "audition.mod";
        job.result = run("audition", withModules({job.exocet, job.params, std::to_string(job.song),
                                                  job.audition, modPath.string()}));
        job.issues = job.result.issues;
        if (job.result.Ok())
            job.musics = {ReadBytes(modPath)};
        return;
    }
    // a push (and an injection, a write): music.bin from the module (the kit's export, kept
    // while the module is unchanged),
    // then exocet.bin from the keys against it (the kit's pack)
    std::vector<std::string> musicPaths;
    for (size_t n = 0; n < job.modules.size(); ++n)
        musicPaths.push_back((job.scratch / ("music" + std::to_string(n) + ".bin")).string());
    if (job.needExport) {
        job.musics.clear();
        for (size_t n = 0; n < job.modules.size(); ++n) {
            const auto r = run(
                "export", withModules({job.exocet, job.params, std::to_string(n), musicPaths[n]}));
            if (!r.Ok()) {
                job.issues = r.issues;
                job.musics.clear();
                return;
            }
            job.musics.push_back(ReadBytes(musicPaths[n]));
        }
    } else {
        for (size_t n = 0; n < job.musics.size() && n < musicPaths.size(); ++n)
            std::ofstream(musicPaths[n], std::ios::binary)
                .write(reinterpret_cast<const char*>(job.musics[n].data()),
                       static_cast<std::streamsize>(job.musics[n].size()));
    }
    const fs::path binPath = job.scratch / "exocet.bin";
    std::vector<std::string> packArgs{job.exocet, binPath.string()};
    packArgs.insert(packArgs.end(), musicPaths.begin(), musicPaths.end());
    job.result = run("pack", packArgs);
    job.issues = job.result.issues;
    if (!job.result.Ok())
        return;
    job.bin = ReadBytes(binPath);
    if (job.kind != Job::Write)
        return;
    // a new disk: the inserted one, laid out again by the kit with this music and sync
    const fs::path adfPath = job.scratch / "new.adf";
    std::vector<std::string> rebuildArgs{job.disk, binPath.string(), adfPath.string()};
    rebuildArgs.insert(rebuildArgs.end(), musicPaths.begin(), musicPaths.end());
    job.result = run("rebuild", rebuildArgs);
    job.issues = job.result.issues;
    if (job.result.Ok())
        job.adf = ReadBytes(adfPath);
}

void ExocetSession::FinishJob(Job& job)
{
    // Another pack became active while the kit ran. A check, push, injection or audition of
    // the old one is no use to this one - its orders, its keys, its music - so it is dropped
    // (SetPack already asked for this pack's own check). A write is finished for its own pack.
    const bool stale = job.pack != m_pack;
    if (stale && job.kind != Job::Write) {
        static const char* kWhat[] = {"check", "push", "write", "inject", "audition"};
        Logger::Info("Exocet: a {} of {} finished after another pack became active; dropped",
                     kWhat[job.kind], job.pack ? job.pack->Path() : std::string("no pack"));
        return;
    }
    if (job.kind == Job::Check) {
        if (job.check.valid) {
            // the song on the machine keeps its rows even when the module has moved on: an
            // injection finds the playhead's place in it
            if (m_block.present && m_block.arena.valid && job.check.digest == m_block.arena.song)
                m_playing = job.check;
            else if (!m_playing.valid && m_check.valid && m_block.present &&
                     m_check.digest == m_block.arena.song)
                m_playing = m_check;
            m_check = std::move(job.check);
        } else {
            // keep the rows the editor is showing; the check's failure is the news
            m_check.valid = !m_check.rows.empty();
        }
        m_check.issues = job.issues;
        m_checkSeconds = job.result.seconds;
        m_checkedRevision = job.revision;
        ++m_checkGen;
        SyncActivity();
        Logger::Info("Exocet: check {} in {:.1f} s, {} rows, {} issue(s)",
                     job.result.Ok() ? "done" : "failed", job.result.seconds, m_check.rows.size(),
                     job.issues.size());
        return;
    }
    if (job.kind == Job::Audition) {
        if (job.musics.empty() || job.musics.front().empty()) {
            m_lastAudition =
                "Listening failed: " +
                (job.issues.empty() ? std::string("the kit made no module")
                                    : job.issues.front().title + " - " + job.issues.front().text);
            return;
        }
        m_lastAudition = job.title;
        if (m_hooks.listen)
            m_hooks.listen(job.musics.front(), job.title);
        return;
    }
    if (!job.musics.empty() && job.kind != Job::Inject && !stale) {
        m_musicBins = job.musics;
        m_musicBinTime = ModulesTime();
        m_musicBinSettings = job.musicSettings;
    }
    if (job.kind == Job::Inject) {
        m_injectIssues = job.issues;
        m_injectedRevision = job.revision;
        if (job.musics.empty() || job.bin.empty()) {
            m_lastInject =
                "Inject failed: " +
                (job.issues.empty() ? std::string("the kit made no music.bin")
                                    : job.issues.front().title + " - " + job.issues.front().text);
            return;
        }
        // the main song's: the one the player's music buffer holds
        ApplyInject(job.musics.back(), job.bin);
        return;
    }
    if (job.kind == Job::Write) {
        if (!stale) {
            m_writeIssues = job.issues;
            m_writtenRevision = job.revision;
        }
        if (job.adf.empty()) {
            m_lastWrite =
                "Write ADF failed: " +
                (job.issues.empty() ? std::string("the kit made no disk")
                                    : job.issues.front().title + " - " + job.issues.front().text);
            return;
        }
        // the .exocet first: the folder is a pack whose disk, keys and module belong together.
        // The job's pack, which is the one the disk was built from even if another is active.
        std::string error;
        const bool saved = stale ? job.pack && job.pack->Save(&error) : Save(&error);
        if (!saved) {
            m_lastWrite =
                "Write ADF: the keys could not be saved, so the disk was not written: " + error;
            return;
        }
        if (!m_hooks.replaceDisk || !m_hooks.replaceDisk(job.target, job.adf, error)) {
            m_lastWrite = "Write ADF failed: " + error;
            return;
        }
        m_wroteDisk = !stale;
        m_lastWrite = "Wrote " + fs::path(job.target).filename().string() + " (" +
                      std::to_string(job.adf.size() / 1024) + "K); the old disk is " +
                      fs::path(job.target).filename().string() + ".bak.";
        Logger::Info("Exocet: {}", m_lastWrite);
        return;
    }
    m_pushIssues = job.issues;
    m_pushedRevision = job.revision;
    if (job.bin.empty()) {
        m_lastPush =
            "Push failed: " + (job.issues.empty()
                                   ? std::string("the kit made no exocet.bin")
                                   : job.issues.front().title + " - " + job.issues.front().text);
        return;
    }
    ApplyPush(job.bin);
}

void ExocetSession::Update()
{
    {
        std::unique_ptr<Job> done;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            done = std::move(m_done);
        }
        if (done) {
            if (m_worker.joinable())
                m_worker.join();
            m_busy = false;
            m_busyWhat.clear();
            FinishJob(*done);
        }
    }

    const ExocetBlockState* b = m_hooks.block ? m_hooks.block() : nullptr;
    m_block = b ? *b : ExocetBlockState{};
    Record(m_block);

    // the activity record, a few seconds after it last changed
    if (m_activity.Revision() != m_activitySaved) {
        const auto now = std::chrono::steady_clock::now();
        if (m_activitySaveDue == std::chrono::steady_clock::time_point{})
            m_activitySaveDue = now + std::chrono::seconds(5);
        else if (now >= m_activitySaveDue)
            SaveActivity();
    }

    // the channel lights: a trigger is Paula pointed at a sample on a playing voice; they fade
    // by the emulated frame, so a paused machine holds them where they are
    if (m_pack && m_hooks.paula && m_block.present && m_block.frame != m_lastLightFrame) {
        m_lastLightFrame = m_block.frame;
        const PaulaStateSnapshot ps = m_hooks.paula();
        for (int c = 0; c < 4; ++c) {
            const auto& ch = ps.channels[c];
            auto& l = m_lights[c];
            l.trigger *= 0.7f;
            if (ch.dmaOn && ch.vol && ch.lc != m_lastLc[c])
                l.trigger = 1.0f;
            m_lastLc[c] = ch.lc;
            l.level = ps.traceAvailable ? std::min(1.0f, ch.rms * 4.0f)
                                        : (ch.dmaOn ? ch.vol / 64.0f : 0.0f);
        }
    }
    if (m_reseatPending && m_block.present && m_block.reseatSeen == m_reseatSent) {
        m_reseatPending = false;
        // by EXO_RESEAT_* (kExocetReseat*)
        static const char* kWhy[kExocetReseatReasons] = {
            "",
            "it is not a music.bin this replayer reads",
            "its blobs run past the buffer",
            "its schedule wants a bigger chip arena",
            "it has more loads than the table has room for",
            "its schedule would write outside the arena",
            "the song ends before the row"};
        if (m_block.reseatError == 0) {
            m_lastInject = "Reseated at played row " + std::to_string(m_block.reseatRow) + ".";
        } else {
            const auto e = m_block.reseatError;
            m_lastInject = std::string("The player refused the music: ") +
                           (e < std::size(kWhy) ? kWhy[e] : "reason " + std::to_string(e)) +
                           ". The music is off; write a new disk instead.";
            m_injectIssues.push_back(ExocetMakeIssue("block", "inject.reseat",
                                                     "The player refused the new music",
                                                     m_lastInject, {"inject"}));
        }
        Logger::Info("Exocet: {}", m_lastInject);
    }

    const double now = Now();
    if (now - m_proofAt > 0.5) {
        m_proofAt = now;
        m_proofIssues = ProofIssues();
    }
    if (!m_pack || m_kitState != KitState::Ready)
        return;
    // the module the musician saves from their tracker, watched
    if (now - m_lastPoll > 1.0) {
        m_lastPoll = now;
        const auto t = ModulesTime();
        if (t != m_moduleTime) {
            m_moduleTime = t;
            m_musicBins.clear();
            Logger::Info("Exocet: a module changed - checking it");
            RequestCheck();
        }
    }
    // A key edit is not checked here: what packs the keys (a push, a write) resolves them and
    // reports their issues. The check exports the music, so it waits for the music section to
    // change from the one the last check started with (or a module, above).
    if (m_pack->Revision() != m_musicSeenRevision) {
        m_musicSeenRevision = m_pack->Revision();
        if (!m_checkWanted && m_pack->MusicText() != m_checkMusic) {
            m_checkWanted = true;
            m_checkDue = now + kCheckDebounce;
        }
    }
    if (m_checkWanted && !m_busy && now >= m_checkDue) {
        m_checkWanted = false;
        auto job = std::make_unique<Job>();
        job->kind = Job::Check;
        job->kit = m_kit;
        job->tools = m_tools;
        job->modules = m_pack->ModulePaths();
        job->params = m_kit.paramsPath;
        job->revision = m_pack->Revision();
        job->exocet = Snapshot("check.exocet");
        if (!job->exocet.empty()) {
            m_checkMusic = m_pack->MusicText();
            StartJob(std::move(job));
        }
    }
}

void ExocetSession::OnFrame(uint64_t emuFrame)
{
    if (!m_pack || !m_hooks.block)
        return;
    if (const ExocetBlockState* b = m_hooks.block()) {
        Record(*b);
        // after Record: a new boot clears the record, this with it
        if (b->present)
            m_emuFrames[b->frame] = emuFrame;
        CheckBreaks(*b);
    }
}

bool ExocetSession::RowBreak(int playedRow) const
{
    if (playedRow < 0 || playedRow >= static_cast<int>(m_check.rows.size()))
        return false;
    const auto& r = m_check.rows[playedRow];
    return m_rowBreaks.count({r.song, r.pos, r.row, r.pass}) != 0;
}

void ExocetSession::ToggleRowBreak(int playedRow) { SetRowBreak(playedRow, !RowBreak(playedRow)); }

void ExocetSession::SetRowBreak(int playedRow, bool on)
{
    if (playedRow < 0 || playedRow >= static_cast<int>(m_check.rows.size()))
        return;
    const auto& r = m_check.rows[playedRow];
    const std::tuple<int, int, int, int> place{r.song, r.pos, r.row, r.pass};
    if (on)
        m_rowBreaks.insert(place);
    else
        m_rowBreaks.erase(place);
}

int ExocetSession::KeyBreakTrack(int playedRow)
{
    for (const int track : KeyBreaks())
        for (const auto& k : HostKeys(track))
            if (k.row == playedRow)
                return track;
    return -1;
}

std::set<int> ExocetSession::KeyBreaks() const
{
    std::set<int> out;
    for (const auto& name : m_keyBreaks)
        if (const int t = FindTrackIndex(name); t >= 0)
            out.insert(t);
    return out;
}

std::set<std::pair<int, std::optional<int32_t>>> ExocetSession::EventBreaks() const
{
    std::set<std::pair<int, std::optional<int32_t>>> out;
    for (const auto& [name, value] : m_eventBreaks)
        if (const int t = FindTrackIndex(name); t >= 0)
            out.insert({t, value});
    return out;
}

void ExocetSession::SetKeyBreak(int track, bool on)
{
    if (!m_pack || track < 0 || track >= static_cast<int>(m_pack->Tracks().size()))
        return;
    const std::string& name = m_pack->Tracks()[static_cast<size_t>(track)].name;
    if (on)
        m_keyBreaks.insert(name);
    else
        m_keyBreaks.erase(name);
}

void ExocetSession::SetEventBreak(int track, std::optional<int32_t> value, bool on)
{
    if (!m_pack || track < 0 || track >= static_cast<int>(m_pack->Tracks().size()))
        return;
    const std::string& name = m_pack->Tracks()[static_cast<size_t>(track)].name;
    if (on)
        m_eventBreaks.insert({name, value});
    else
        m_eventBreaks.erase({name, value});
}

void ExocetSession::SetCutBreak(int cut, bool ready, bool on)
{
    if (on)
        m_cutBreaks.insert({cut, ready});
    else
        m_cutBreaks.erase({cut, ready});
}

void ExocetSession::ClearBreaks()
{
    m_rowBreaks.clear();
    m_keyBreaks.clear();
    m_eventBreaks.clear();
    m_cutBreaks.clear();
}

bool ExocetSession::EventBreakMatches(const ExocetFire& f)
{
    for (const auto& [track, value] : EventBreaks()) {
        if (track != f.track)
            continue;
        if (!value)
            return true;
        // the fire names its key's row, not its value: the track's keys on that row say it
        for (const auto& k : HostKeys(track))
            if (k.row == f.row && k.value == *value)
                return true;
    }
    return false;
}

bool ExocetSession::CutBreakMatches(int cut, bool ready) const
{
    return m_cutBreaks.count({cut, ready}) != 0 || m_cutBreaks.count({-1, ready}) != 0;
}

std::string ExocetSession::CutName(int cut) const
{
    const int director = DirectorIndex();
    if (director >= 0)
        for (const auto& e : m_pack->Tracks()[director].entries)
            if (e.value == cut)
                return e.name;
    return "cut " + std::to_string(cut);
}

void ExocetSession::CheckBreaks(const ExocetBlockState& b)
{
    // The row it is on, whatever happens below, so a breakpoint set on the row the demo is
    // already in (or resumed from) waits for the next time the row starts
    const int last = m_breakLastRow;
    m_breakLastRow = b.present ? b.row : -1;
    // And what it had fired and cut: an event or cut break takes only what is new since the
    // last frame of the same run, never what a breakpoint finds already there
    const bool sameRun = b.present && b.blockAddress == m_breakBlockAddr;
    const uint16_t lastFired = m_breakFired;
    const std::vector<ExocetCut> lastCuts = std::move(m_breakCuts);
    m_breakBlockAddr = b.present ? b.blockAddress : 0;
    m_breakFired = b.firedCount;
    m_breakCuts = b.cuts;
    if (BreakCount() == 0 || !b.present || !m_check.valid || !m_hooks.breakAt)
        return;
    // only against the song the rows are, from a build of this pack - as the activity is
    if (!m_pack || b.manifest != m_pack->ManifestHash() || !b.arena.valid ||
        b.arena.song != m_check.digest)
        return;
    auto hit = [&](const char* kind, std::string what, int row) {
        m_lastBreak.kind = kind;
        m_lastBreak.what = std::move(what);
        m_lastBreak.row = row;
        m_lastBreak.frame = b.frame;
        ++m_lastBreak.hits;
        m_hooks.breakAt(0);
    };
    const int rows = static_cast<int>(m_check.rows.size());
    const int row = b.row;
    const bool songs = m_check.orders.size() > 1;
    if (row != last && row < rows) {
        if (RowBreak(row)) {
            hit("row", ExocetPlaceLabel(m_check.rows[row], songs), row);
            return;
        }
        if (const int track = KeyBreakTrack(row); track >= 0) {
            hit("key", m_pack->Tracks()[track].name, row);
            return;
        }
    }
    if (sameRun) {
        const int fresh = static_cast<uint16_t>(b.firedCount - lastFired);
        const int n = std::min<int>(fresh, static_cast<int>(b.fired.size()));
        for (int i = static_cast<int>(b.fired.size()) - n; i < static_cast<int>(b.fired.size());
             ++i) {
            const ExocetFire& f = b.fired[static_cast<size_t>(i)];
            if (f.track < m_pack->Tracks().size() && EventBreakMatches(f)) {
                hit("event", m_pack->Tracks()[f.track].name, f.row);
                return;
            }
        }
        for (const auto& c : b.cuts) {
            const ExocetCut* before = nullptr;
            for (const auto& l : lastCuts)
                if (l.cut == c.cut)
                    before = &l;
            const uint16_t wasReady = before ? before->readyFrame : 0xFFFF;
            const uint16_t wasCut = before ? before->cutFrame : 0xFFFF;
            if (c.cutFrame != 0xFFFF && c.cutFrame != wasCut && CutBreakMatches(c.cut, false)) {
                hit("cut", CutName(c.cut), c.cutRow);
                return;
            }
            if (c.readyFrame != 0xFFFF && c.readyFrame != wasReady &&
                CutBreakMatches(c.cut, true)) {
                hit("cut ready", CutName(c.cut), c.readyRow);
                return;
            }
        }
    }
    // One frame of warning: when the next frame starts a breakpoint's row, the emulator's batch
    // stops on it, so that frame is the one shown (and the debugger's logs are of it)
    const int next = row + 1;
    if (next < rows && m_check.rows[next].frame == static_cast<uint32_t>(b.frame) + 1 &&
        (RowBreak(next) || KeyBreakTrack(next) >= 0))
        m_hooks.breakAt(1);
}

std::vector<ExocetIssue> ExocetSession::Issues() const { return IssuesBut(nullptr); }

std::vector<ExocetIssue> ExocetSession::IssuesBut(const std::vector<ExocetIssue>* skip) const
{
    std::vector<ExocetIssue> out = m_toolIssues;
    // the keys' issues (the kit's exocet.*) from a run that saw keys edited since are of keys
    // that are not there now: the next push or write reports the keys as they are
    const auto add = [&](const std::vector<ExocetIssue>& from, uint64_t revision) {
        if (&from == skip)
            return;
        const bool stale = m_pack && m_pack->Revision() != revision;
        for (const auto& i : from)
            if (!stale || i.code.rfind("exocet.", 0) != 0)
                out.push_back(i);
    };
    add(m_check.issues, m_checkedRevision);
    add(m_pushIssues, m_pushedRevision);
    add(m_writeIssues, m_writtenRevision);
    add(m_injectIssues, m_injectedRevision);
    out.insert(out.end(), m_proofIssues.begin(), m_proofIssues.end());
    return out;
}

std::vector<ExocetIssue> ExocetSession::ProofIssues()
{
    std::vector<ExocetIssue> out;
    auto* self = this;
    if (m_block.present && m_pack && !ManifestMatches()) {
        char text[200];
        std::snprintf(text, sizeof text,
                      "The running disk was built from other declarations (manifest 0x%08X) than "
                      "the .exocet (0x%08X): rebuild the disk, or open the one built with it.",
                      m_block.manifest, m_pack->ManifestHash());
        out.push_back(ExocetMakeIssue("block", "proof.stale",
                                      "The disk and the keys are from "
                                      "different builds",
                                      text, {"push"}));
    }
    if (!m_check.valid || !m_pack)
        return out;
    for (const auto& e : self->Events()) {
        if (e.state != EventProof::Late && e.state != EventProof::Missed)
            continue;
        const auto& t = m_pack->Tracks()[e.track];
        const auto& r = m_check.rows[e.keyRow];
        ExocetIssue i = ExocetMakeIssue(
            "warn", e.state == EventProof::Late ? "proof.late" : "proof.missed",
            t.name + (e.state == EventProof::Late ? " fired late" : " never fired"),
            e.state == EventProof::Late
                ? "The key at position " + std::to_string(r.pos) + " row " + std::to_string(r.row) +
                      " fired " + std::to_string(e.lateFrames) + " frame(s) after its row began."
                : "Its row came while its group was not enabled, so the event was never "
                  "delivered.");
        i.at = {{"song", r.song}, {"pos", r.pos}, {"row", r.row}, {"pass", r.pass}};
        i.subject = {{"track", t.name}};
        out.push_back(std::move(i));
    }
    for (const auto& c : self->Cuts()) {
        if (c.keyRow < 0 || c.readyFrame < 0)
            continue;
        const auto& r = m_check.rows[c.keyRow];
        ExocetIssue i;
        if (c.late > 0) {
            i = ExocetMakeIssue("warn", "proof.cut_late", "The " + c.name + " cut was late",
                                "Its part was ready " + std::to_string(-c.margin) +
                                    " frame(s) after the key, and the cut came " +
                                    std::to_string(c.late) +
                                    " frame(s) late. Move the key later, or make the load faster.");
        } else if (c.margin < kThinMargin) {
            i = ExocetMakeIssue("warn", "proof.cut_margin",
                                "The " + c.name + " cut has a thin margin",
                                "Its part was ready only " + std::to_string(c.margin) +
                                    " frame(s) before the key. A real drive loads at another "
                                    "speed; a thin margin is how a cut becomes late.");
        } else {
            continue;
        }
        i.at = {{"song", r.song}, {"pos", r.pos}, {"row", r.row}, {"pass", r.pass}};
        i.subject = {{"track", m_pack->Director()}};
        out.push_back(std::move(i));
    }
    for (const auto& [t, frames] : self->Divergence()) {
        ExocetIssue i = ExocetMakeIssue(
            "warn", "proof.divergence", m_pack->Tracks()[t].name + " read other than its keys",
            std::to_string(frames) + " recorded frame(s) where the demo's value was not the one "
                                     "the keys give. Pushed keys not taken up, or the effect "
                                     "writes the value itself.");
        i.subject = {{"track", m_pack->Tracks()[t].name}};
        out.push_back(std::move(i));
    }
    return out;
}

std::string ExocetSession::BlockedBy(const std::string& action) const
{
    // what the action's own last run reported does not stop it: running it again is how that
    // is found out again, and a failed push would otherwise keep the Push button off
    const std::vector<ExocetIssue>* own = action == "push"     ? &m_pushIssues
                                          : action == "write"  ? &m_writeIssues
                                          : action == "inject" ? &m_injectIssues
                                                               : nullptr;
    for (const auto& i : IssuesBut(own))
        if (i.Blocks(action))
            return i.title;
    return {};
}

std::string ExocetSession::EditBlockedReason() const
{
    if (!m_pack)
        return m_packError.empty() ? "No .exocet beside the disk." : m_packError;
    if (!m_check.valid) {
        switch (m_kitState) {
            case KitState::None:
                return "The .exocet names no kit, and the rows come from the kit's check.";
            case KitState::Missing:
                return "The kit is missing: " + m_kitError;
            case KitState::Untrusted:
                return "Trust the kit to see the song's rows.";
            default:
                return m_busy ? "Checking..." : "The kit's check has not given the rows yet.";
        }
    }
    for (int n = 0; n < m_pack->SongCount() && n < static_cast<int>(m_check.orders.size()); ++n) {
        const auto order = m_pack->SongOrder(n);
        if (!order.empty() && order != m_check.orders[static_cast<size_t>(n)])
            return "A module's order list is not the one the keys were placed against; the kit "
                   "has moved them (see its issues), but the file still holds the old places.";
    }
    return {};
}

int ExocetSession::FindTrackIndex(const std::string& name) const
{
    if (!m_pack)
        return -1;
    const auto* t = m_pack->FindTrack(name);
    return t ? t->index : -1;
}

int ExocetSession::Fps() const
{
    if (m_block.present)
        return m_block.fps;
    if (m_fpsParams != m_kit.paramsPath) {
        m_fpsParams = m_kit.paramsPath;
        m_fpsKit = 0;
        std::ifstream f(m_kit.paramsPath);
        if (f) {
            const auto j = nlohmann::json::parse(f, nullptr, false);
            if (j.is_object() && j.contains("fps") && j["fps"].is_number_integer())
                m_fpsKit = j["fps"].get<int>();
        }
    }
    return m_fpsKit == 50 || m_fpsKit == 60 ? m_fpsKit : 50;
}

int ExocetSession::DirectorIndex() const
{
    return m_pack ? FindTrackIndex(m_pack->Director()) : -1;
}

int ExocetSession::RowTicks(int playedRow) const
{
    if (playedRow < 0 || playedRow >= static_cast<int>(m_check.rows.size()))
        return 1;
    return std::max<int>(1, std::min<int>(256, m_check.rows[playedRow].frames));
}

int ExocetSession::KeyTick(const ExocetKey& k) const
{
    return static_cast<int>((uint32_t(k.sub) * RowTicks(k.row) + 255) >> 8);
}

const ExocetKey* ExocetSession::HostKeyAt(int track, int playedRow, int tick)
{
    const uint8_t sub = ExocetSub(static_cast<uint32_t>(tick), RowTicks(playedRow));
    for (const auto& k : HostKeys(track))
        if (k.row == playedRow && k.sub == sub)
            return &k;
    return nullptr;
}

bool ExocetSession::SetKey(const std::string& track, int playedRow, double value,
                           const std::string& interp, std::string* error, const std::string& merge,
                           int tick)
{
    if (const std::string why = EditBlockedReason(); !why.empty()) {
        if (error)
            *error = why;
        return false;
    }
    if (playedRow < 0 || playedRow >= static_cast<int>(m_check.rows.size())) {
        if (error)
            *error = "not a played row";
        return false;
    }
    const auto& r = m_check.rows[playedRow];
    if (tick < 0 || tick >= RowTicks(playedRow)) {
        if (error)
            *error = "the row has " + std::to_string(RowTicks(playedRow)) + " ticks";
        return false;
    }
    if (!m_pack->SetKey(track, r.song, r.pos, r.row, r.pass, value, interp, error, merge, tick))
        return false;
    m_pack->SetSongs(m_check.orders, m_check.digests);
    return true;
}

bool ExocetSession::DeleteKey(const std::string& track, int playedRow, int tick,
                              const std::string& merge, std::string* error)
{
    // the rules SetKey keeps: a delete that could not be undone is not made
    if (const std::string why = EditBlockedReason(); !why.empty()) {
        if (error)
            *error = why;
        return false;
    }
    if (!m_pack || playedRow < 0 || playedRow >= static_cast<int>(m_check.rows.size()))
        return false;
    const auto& r = m_check.rows[playedRow];
    return m_pack->DeleteKey(track, r.song, r.pos, r.row, r.pass, tick, merge);
}

const std::map<int, ExocetBookmarkInfo>& ExocetSession::BookmarkRows()
{
    if (!m_pack || m_bookmarksRevision != m_pack->Revision() || m_bookmarksCheck != m_checkGen) {
        m_bookmarkRows.clear();
        m_bookmarksRevision = m_pack ? m_pack->Revision() : ~0ull;
        m_bookmarksCheck = m_checkGen;
        if (m_pack && m_check.valid)
            for (auto& b : m_pack->Bookmarks())
                if (const int row = m_check.FindRow(b.song, b.pos, b.row, b.pass); row >= 0)
                    m_bookmarkRows[row] = std::move(b);
    }
    return m_bookmarkRows;
}

const ExocetBookmarkInfo* ExocetSession::BookmarkAt(int playedRow)
{
    const auto& rows = BookmarkRows();
    auto it = rows.find(playedRow);
    return it == rows.end() ? nullptr : &it->second;
}

int ExocetSession::FreeBookmarkSlot()
{
    std::set<int> taken;
    if (m_pack)
        for (const auto& b : m_pack->Bookmarks())
            taken.insert(b.slot);
    for (int slot = 1; slot <= 9; ++slot)
        if (!taken.count(slot))
            return slot;
    return 0;
}

int ExocetSession::BookmarkSlotRow(int slot)
{
    if (slot < 1 || slot > 9)
        return -1;
    for (const auto& [row, b] : BookmarkRows())
        if (b.slot == slot)
            return row;
    return -1;
}

bool ExocetSession::SetBookmark(int playedRow, int slot, const std::string& name,
                                std::string* error)
{
    if (const std::string why = EditBlockedReason(); !why.empty()) {
        if (error)
            *error = why;
        return false;
    }
    if (playedRow < 0 || playedRow >= static_cast<int>(m_check.rows.size())) {
        if (error)
            *error = "not a played row";
        return false;
    }
    const auto& r = m_check.rows[playedRow];
    return m_pack->SetBookmark({r.song, r.pos, r.row, r.pass, slot, name}, error);
}

bool ExocetSession::RemoveBookmark(int playedRow)
{
    if (!m_pack || playedRow < 0 || playedRow >= static_cast<int>(m_check.rows.size()))
        return false;
    const auto& r = m_check.rows[playedRow];
    return m_pack->DeleteBookmark(r.song, r.pos, r.row, r.pass);
}

bool ExocetSession::ToggleBookmark(int playedRow, std::string* error)
{
    if (BookmarkAt(playedRow))
        return RemoveBookmark(playedRow);
    return SetBookmark(playedRow, FreeBookmarkSlot(), {}, error);
}

int ExocetSession::ClearTrack(const std::string& track, std::string* error)
{
    std::string why = EditBlockedReason();
    if (why.empty() && !m_pack->FindTrack(track))
        why = "no track \"" + track + "\" in the manifest";
    if (!why.empty()) {
        if (error)
            *error = why;
        return -1;
    }
    return static_cast<int>(m_pack->ClearTrack(track));
}

void ExocetSession::AdoptCheckForTest(ExocetCheck check)
{
    m_check = std::move(check);
    m_check.valid = true;
    ++m_checkGen;
    // as if the kit had checked the pack as it is now
    m_checkedRevision = m_musicSeenRevision = m_pack ? m_pack->Revision() : 0;
    m_checkMusic = m_pack ? m_pack->MusicText() : std::string();
}

bool ExocetSession::CanUndo() const
{
    return m_pack && m_pack->CanUndo() && EditBlockedReason().empty();
}

bool ExocetSession::CanRedo() const
{
    return m_pack && m_pack->CanRedo() && EditBlockedReason().empty();
}

bool ExocetSession::Undo() { return CanUndo() && m_pack->Undo(); }

bool ExocetSession::Redo() { return CanRedo() && m_pack->Redo(); }

bool ExocetSession::Save(std::string* error)
{
    m_saveError.clear();
    if (!m_pack) {
        m_saveError = "no .exocet";
        if (error)
            *error = m_saveError;
        return false;
    }
    if (!m_pack->Save(&m_saveError)) {
        if (error)
            *error = m_saveError;
        return false;
    }
    Logger::Info("Exocet: saved {}", m_pack->Path());
    return true;
}

const std::vector<ExocetKey>& ExocetSession::HostKeys(int track)
{
    if (!m_pack || m_hostKeysRevision != m_pack->Revision() || m_hostKeysCheck != m_checkGen) {
        m_hostKeys.clear();
        m_hostKeysRevision = m_pack ? m_pack->Revision() : ~0ull;
        m_hostKeysCheck = m_checkGen;
    }
    auto it = m_hostKeys.find(track);
    if (it != m_hostKeys.end())
        return it->second;
    std::vector<ExocetKey> keys;
    if (m_pack && track >= 0 && track < static_cast<int>(m_pack->Tracks().size())) {
        const auto& t = m_pack->Tracks()[track];
        const auto type = static_cast<ExocetType>(ExocetTypeFromName(t.type));
        for (const auto& k : m_pack->Keys(t.name)) {
            const int row = m_check.FindRow(k.song, k.pos, k.row, k.pass);
            if (row < 0)
                continue;
            const int interp = ExocetInterpFromName(k.interp);
            keys.push_back({static_cast<uint16_t>(row), static_cast<uint8_t>(std::max(0, interp)),
                            ExocetToBin(type, k.value),
                            ExocetSub(static_cast<uint32_t>(std::max(0, k.tick)), RowTicks(row))});
        }
        std::stable_sort(keys.begin(), keys.end(),
                         [](const ExocetKey& a, const ExocetKey& b) { return a.At() < b.At(); });
    }
    return m_hostKeys[track] = std::move(keys);
}

const std::map<std::pair<int, int>, ExocetSession::KeyChange>& ExocetSession::Changes(int track)
{
    if (!m_pack || m_changesRevision != m_pack->Revision() ||
        m_changesSaved != m_pack->SavedRevision() || m_changesCheck != m_checkGen) {
        m_changes.clear();
        m_changesRevision = m_pack ? m_pack->Revision() : ~0ull;
        m_changesSaved = m_pack ? m_pack->SavedRevision() : ~0ull;
        m_changesCheck = m_checkGen;
    }
    auto it = m_changes.find(track);
    if (it != m_changes.end())
        return it->second;
    std::map<std::pair<int, int>, KeyChange> out;
    if (m_pack && m_check.valid && track >= 0 &&
        track < static_cast<int>(m_pack->Tracks().size())) {
        const std::string& name = m_pack->Tracks()[track].name;
        const auto now = m_pack->Keys(name);
        const auto saved = m_pack->SavedKeys(name);
        const auto same = [](const ExocetKeyInfo& a, const ExocetKeyInfo& b) {
            return a.song == b.song && a.pos == b.pos && a.row == b.row && a.pass == b.pass &&
                   a.tick == b.tick;
        };
        // a key's cell, as HostKeys places it; none off the song
        const auto cell = [this](const ExocetKeyInfo& k) {
            const int row = m_check.FindRow(k.song, k.pos, k.row, k.pass);
            return std::make_pair(
                row, row < 0 ? 0
                             : static_cast<int>(ExocetSub(
                                   static_cast<uint32_t>(std::max(0, k.tick)), RowTicks(row))));
        };
        for (const auto& k : now) {
            const auto s = std::find_if(saved.begin(), saved.end(),
                                        [&](const ExocetKeyInfo& x) { return same(k, x); });
            if (s != saved.end() && s->value == k.value && s->interp == k.interp)
                continue;
            const auto at = cell(k);
            if (at.first < 0)
                continue;
            KeyChange c;
            if (s != saved.end()) {
                c.kind = KeyChange::Changed;
                c.was = s->value;
                c.wasInterp = s->interp;
            }
            out[at] = c;
        }
        for (const auto& s : saved) {
            if (std::any_of(now.begin(), now.end(),
                            [&](const ExocetKeyInfo& x) { return same(s, x); }))
                continue;
            const auto at = cell(s);
            if (at.first >= 0)
                out.emplace(at, KeyChange{KeyChange::Removed, s.value, s.interp});
        }
    }
    return m_changes[track] = std::move(out);
}

std::array<int, 3> ExocetSession::ChangeCounts()
{
    std::array<int, 3> n{};
    if (m_pack)
        for (const auto& t : m_pack->Tracks())
            for (const auto& [at, c] : Changes(t.index))
                ++n[c.kind];
    return n;
}

int32_t ExocetSession::HostValue(int track, uint32_t row, uint16_t frac)
{
    if (!m_pack || track < 0 || track >= static_cast<int>(m_pack->Tracks().size()))
        return 0;
    const auto& t = m_pack->Tracks()[track];
    const auto type = static_cast<ExocetType>(ExocetTypeFromName(t.type));
    const auto& keys = HostKeys(track);
    if (type == ExocetType::Event) {
        const uint32_t now = (row << 16) | frac;
        for (const auto& k : keys)
            if (k.At() == now)
                return k.value;
        return 0;
    }
    return ExocetValueAt(keys, ExocetToBin(type, t.def), row, frac, type);
}

bool ExocetSession::ManifestMatches() const
{
    return m_pack && m_block.present && m_block.manifest == m_pack->ManifestHash();
}

std::string ExocetSession::PushBlockedReason() const
{
    if (!m_pack)
        return "No .exocet beside the disk.";
    if (!m_block.present)
        return "No Exocet block: the running program has no Exocet timeline, or has not "
               "started it yet.";
    if (!ManifestMatches())
        return "The running disk and the .exocet are from different builds.";
    if (m_kitState != KitState::Ready)
        return m_kitState == KitState::Untrusted ? "Trust the kit first: a push packs with it."
                                                 : "No kit to pack with.";
    if (m_busy)
        return "The kit is busy (" + m_busyWhat + ").";
    if (m_hooks.paused && !m_hooks.paused())
        return "Pause the machine first.";
    if (const std::string b = BlockedBy("push"); !b.empty())
        return b;
    return {};
}

bool ExocetSession::Push()
{
    if (!PushBlockedReason().empty())
        return false;
    auto job = std::make_unique<Job>();
    job->kind = Job::Push;
    job->kit = m_kit;
    job->tools = m_tools;
    job->modules = m_pack->ModulePaths();
    job->params = m_kit.paramsPath;
    job->scratch = m_scratch;
    job->revision = m_pack->Revision();
    job->exocet = Snapshot("push.exocet");
    job->musicSettings = m_pack->MusicText();
    job->needExport = NeedExport();
    if (!job->needExport)
        job->musics = m_musicBins;
    if (job->exocet.empty())
        return false;
    m_lastPush = "Packing...";
    StartJob(std::move(job));
    return true;
}

// The music.bins are the modules' and the music section's: either changing makes them stale
bool ExocetSession::NeedExport() const
{
    return !m_pack || m_musicBins.size() != static_cast<size_t>(m_pack->SongCount()) ||
           m_musicBinTime != ModulesTime() || m_musicBinSettings != m_pack->MusicText();
}

// The newest of the songs' modules' times: any one saved makes the music stale
std::filesystem::file_time_type ExocetSession::ModulesTime() const
{
    std::filesystem::file_time_type t{};
    if (m_pack)
        for (const auto& m : m_pack->ModulePaths())
            t = std::max(t, ModTime(m));
    return t;
}

std::string ExocetSession::WriteBlockedReason() const
{
    if (!m_pack)
        return "No .exocet beside the disk.";
    if (m_diskPath.empty())
        return "No disk to write from.";
    if (m_kitState != KitState::Ready)
        return m_kitState == KitState::Untrusted ? "Trust the kit first: it writes the disk."
                                                 : "No kit to write the disk with.";
    if (!m_kit.HasRole("rebuild"))
        return "The kit has no rebuild command.";
    if (m_busy)
        return "The kit is busy (" + m_busyWhat + ").";
    if (const std::string b = BlockedBy("write"); !b.empty())
        return b;
    return {};
}

bool ExocetSession::WriteAdf(const std::string& path)
{
    if (!WriteBlockedReason().empty())
        return false;
    auto job = std::make_unique<Job>();
    job->kind = Job::Write;
    job->kit = m_kit;
    job->tools = m_tools;
    job->modules = m_pack->ModulePaths();
    job->params = m_kit.paramsPath;
    job->scratch = m_scratch;
    job->revision = m_pack->Revision();
    job->disk = m_diskPath;
    job->target = path.empty() ? m_diskPath : path;
    job->exocet = Snapshot("write.exocet");
    job->musicSettings = m_pack->MusicText();
    job->needExport = NeedExport();
    if (!job->needExport)
        job->musics = m_musicBins;
    if (job->exocet.empty())
        return false;
    m_wroteDisk = false;
    m_lastWrite = "Writing the disk...";
    StartJob(std::move(job));
    return true;
}

std::string ExocetSession::InjectBlockedReason() const
{
    if (!m_pack)
        return "No .exocet beside the disk.";
    if (!m_block.present)
        return "No Exocet block: the running program has no Exocet timeline, or has not "
               "started it yet.";
    if (!ManifestMatches())
        return "The running disk and the .exocet are from different builds.";
    if (!m_block.musicAddress || !m_block.musicBytes)
        return "The running disk cannot take new music (its player has no music buffer in the "
               "block): write a new disk instead.";
    if (m_kitState != KitState::Ready)
        return m_kitState == KitState::Untrusted ? "Trust the kit first: it exports the music."
                                                 : "No kit to export the music with.";
    if (!m_kit.HasRole("export"))
        return "The kit has no export command.";
    if (m_busy)
        return "The kit is busy (" + m_busyWhat + ").";
    if (m_reseatPending)
        return "The last injection has not been taken up yet: unpause.";
    if (m_hooks.paused && !m_hooks.paused())
        return "Pause the machine first.";
    if (const std::string b = BlockedBy("inject"); !b.empty())
        return b;
    return {};
}

bool ExocetSession::Inject(const std::string& module)
{
    if (!InjectBlockedReason().empty())
        return false;
    auto job = std::make_unique<Job>();
    job->kind = Job::Inject;
    job->kit = m_kit;
    job->tools = m_tools;
    // `module`, if given, in place of the main song's: the one the music buffer holds
    job->modules = m_pack->ModulePaths();
    if (!module.empty() && !job->modules.empty())
        job->modules.back() = module;
    job->params = m_kit.paramsPath;
    job->scratch = m_scratch;
    job->revision = m_pack->Revision();
    job->exocet = Snapshot("inject.exocet");
    job->musicSettings = m_pack->MusicText();
    job->needExport = true;
    if (job->exocet.empty())
        return false;
    m_injectIssues.clear();
    m_lastInject = "Exporting the music...";
    StartJob(std::move(job));
    return true;
}

void ExocetSession::ApplyInject(const std::vector<uint8_t>& music, const std::vector<uint8_t>& bin)
{
    auto fail = [&](const std::string& code, const std::string& title, const std::string& text) {
        m_injectIssues.push_back(ExocetMakeIssue("block", code, title, text, {"inject"}));
        m_lastInject = "Inject refused: " + title;
    };
    ExocetBin parsed;
    std::string error;
    if (!parsed.Parse(bin.data(), bin.size(), &error))
        return fail("inject.refused", "The kit's exocet.bin does not read", error);
    if (music.size() < 56 || !(music[0] == 'S' && music[1] == 'N' && music[2] == 'M'))
        return fail("inject.refused", "The kit's music.bin does not read", "");
    const uint32_t digest = (uint32_t(music[52]) << 24) | (uint32_t(music[53]) << 16) |
                            (uint32_t(music[54]) << 8) | music[55];
    if ((parsed.main ? parsed.main : parsed.song) != digest)
        return fail("inject.refused", "The keys were not packed against this music", "");
    if (!m_block.present || !ManifestMatches() || !m_block.musicAddress)
        return fail("inject.refused", "The block changed while exporting", "Inject again.");
    if (music.size() > m_block.musicBytes)
        return fail("music.buffer", "The music is bigger than the running disk's buffer",
                    "music.bin is " + std::to_string(music.size()) +
                        " bytes; the running disk "
                        "keeps " +
                        std::to_string(m_block.musicBytes) +
                        " for it. Write a new disk instead, or shorten or trim samples.");
    if (bin.size() > m_block.arenaBytes)
        return fail("inject.refused", "The keys do not fit the arena",
                    std::to_string(bin.size()) + " bytes; the running disk has room for " +
                        std::to_string(m_block.arenaBytes) + ".");
    if (m_hooks.paused && !m_hooks.paused())
        return fail("inject.refused", "The machine was unpaused while exporting",
                    "Pause and inject again.");

    // The playhead's place in the song the machine is playing, found again in the new one; the
    // nearest place when the new song does not play it
    uint32_t target = m_block.row;
    std::string where = "played row " + std::to_string(target);
    if (m_playing.valid && m_block.row < m_playing.rows.size() && m_check.valid) {
        const auto& p = m_playing.rows[m_block.row];
        int row = m_check.FindRow(p.song, p.pos, p.row, p.pass);
        if (row < 0)
            row = m_check.FindRow(p.song, p.pos, p.row, 0);
        if (row < 0)
            for (size_t i = 0; i < m_check.rows.size() && row < 0; ++i)
                if (m_check.rows[i].song == p.song && m_check.rows[i].pos >= p.pos)
                    row = static_cast<int>(i);
        if (row >= 0) {
            target = static_cast<uint32_t>(row);
            where = "position " + std::to_string(p.pos) + " row " + std::to_string(p.row) +
                    " (played row " + std::to_string(row) + ")";
        }
    }
    if (m_check.valid && target >= m_check.rows.size())
        target = m_check.rows.empty() ? 0 : static_cast<uint32_t>(m_check.rows.size() - 1);

    if (m_hooks.releaseTimeline)
        m_hooks.releaseTimeline();
    const uint16_t reseat = static_cast<uint16_t>(m_block.reseat + 1);
    const uint8_t row[2] = {static_cast<uint8_t>(target >> 8), static_cast<uint8_t>(target)};
    const uint8_t bump[2] = {static_cast<uint8_t>(reseat >> 8), static_cast<uint8_t>(reseat)};
    // the music and the keys first, the row, and `reseat` last: the player acts on that
    if (!m_hooks.writeRam || !m_hooks.writeRam(m_block.musicAddress, music.data(), music.size()) ||
        !m_hooks.writeRam(m_block.arenaAddress, bin.data(), bin.size()) ||
        !m_hooks.writeRam(m_block.blockAddress + kExocetBlockReseatRowOffset, row, 2) ||
        !m_hooks.writeRam(m_block.blockAddress + kExocetBlockReseatOffset, bump, 2))
        return fail("inject.refused", "The music could not be written",
                    "The block's buffers are not writable RAM.");
    if (m_hooks.clearTimeline)
        m_hooks.clearTimeline();
    ClearRecord();
    m_playing = m_check; // the machine's song, once it has reseated
    m_reseatSent = reseat;
    m_reseatPending = true;
    ++m_injections;
    m_lastInject = "Injected " + std::to_string(music.size()) + " bytes of music at " + where +
                   ": the player reseats when it runs.";
    Logger::Info("Exocet: {}", m_lastInject);
    if (m_hooks.stateChanged)
        m_hooks.stateChanged();
}

std::vector<int> ExocetSession::HitRows(int sample) const
{
    std::vector<int> out;
    for (size_t i = 0; i < m_check.rows.size(); ++i)
        for (const auto& c : m_check.rows[i].cells)
            if (c[0] == sample) {
                out.push_back(static_cast<int>(i));
                break;
            }
    return out;
}

int ExocetSession::KeyEveryHit(const std::string& track, int sample, int templateRow,
                               std::string* error)
{
    auto refuse = [&](const std::string& why) {
        if (error)
            *error = why;
        return 0;
    };
    if (const std::string why = EditBlockedReason(); !why.empty())
        return refuse(why);
    const ExocetTrackInfo* t = m_pack->FindTrack(track);
    if (!t)
        return refuse(track + " is not a track");
    const auto hits = HitRows(sample);
    const auto at = std::find(hits.begin(), hits.end(), templateRow);
    if (at == hits.end())
        return refuse("Select a key on a row that triggers the sample: it is the pattern.");
    const int next = at + 1 == hits.end() ? static_cast<int>(m_check.rows.size()) : *(at + 1);
    // the pattern: this track's keys from the template hit to the next one
    struct PatternKey
    {
        int offset;
        double value;
        std::string interp;
        int tick;
    };
    std::vector<PatternKey> pattern;
    for (const auto& k : m_pack->Keys(track)) {
        const int row = m_check.FindRow(k.song, k.pos, k.row, k.pass);
        if (row >= templateRow && row < next)
            pattern.push_back({row - templateRow, k.value, k.interp, k.tick});
    }
    if (pattern.empty() || pattern.front().offset != 0)
        return refuse("There is no key on the selected hit to copy.");
    int set = 0;
    for (size_t h = 0; h < hits.size(); ++h) {
        if (hits[h] == templateRow)
            continue;
        const int stop = h + 1 < hits.size() ? hits[h + 1] : static_cast<int>(m_check.rows.size());
        for (const auto& k : pattern) {
            const int row = hits[h] + k.offset;
            if (row >= stop)
                break;
            std::string e;
            if (SetKey(track, row, k.value, k.interp, &e, {}, std::min(k.tick, RowTicks(row) - 1)))
                ++set;
        }
    }
    Logger::Info("Exocet: {} keys on {}, on every hit of sample {}", set, track, sample);
    return set;
}

void ExocetSession::BootWrittenDisk()
{
    if (m_wroteDisk && m_hooks.bootDisk) {
        m_wroteDisk = false;
        m_hooks.bootDisk();
    }
}

void ExocetSession::ApplyPush(const std::vector<uint8_t>& bin)
{
    ExocetBin parsed;
    std::string error;
    auto fail = [&](const std::string& title, const std::string& text) {
        m_pushIssues.push_back(ExocetMakeIssue("block", "push.refused", title, text, {"push"}));
        m_lastPush = "Push refused: " + title;
    };
    if (!parsed.Parse(bin.data(), bin.size(), &error))
        return fail("The kit's exocet.bin does not read", error);
    if (!m_block.present || !ManifestMatches() || parsed.manifest != m_block.manifest)
        return fail("The block changed while packing", "Push again.");
    if (m_block.arena.valid && parsed.song != m_block.arena.song) {
        char text[240];
        std::snprintf(text, sizeof text,
                      "The keys were packed against the module as it is now (song $%08X); the "
                      "running disk plays another ($%08X). Inject the music or write a new disk "
                      "first.",
                      parsed.song, m_block.arena.song);
        m_pushIssues.push_back(ExocetMakeIssue(
            "block", "push.song", "The module is not the one on the disk", text, {"push"}));
        m_lastPush = "Push refused: the module is not the one on the disk";
        return;
    }
    if (bin.size() > m_block.arenaBytes)
        return fail("The keys do not fit the arena", std::to_string(bin.size()) +
                                                         " bytes; the running disk has room for " +
                                                         std::to_string(m_block.arenaBytes) + ".");
    if (m_hooks.paused && !m_hooks.paused())
        return fail("The machine was unpaused while packing", "Pause and push again.");

    // The machine, from the timeline, first: what is written lands in the live machine, and the
    // recorded frames before this one hold the old sync - they all go.
    if (m_hooks.releaseTimeline)
        m_hooks.releaseTimeline();
    const uint16_t gen = static_cast<uint16_t>(m_block.generation + 1);
    const uint8_t genBytes[2] = {static_cast<uint8_t>(gen >> 8), static_cast<uint8_t>(gen)};
    if (!m_hooks.writeRam || !m_hooks.writeRam(m_block.arenaAddress, bin.data(), bin.size()) ||
        !m_hooks.writeRam(m_block.blockAddress + kExocetBlockGenerationOffset, genBytes, 2))
        return fail("The arena could not be written", "The block's memory is not writable RAM.");
    if (m_hooks.clearTimeline)
        m_hooks.clearTimeline();
    ClearRecord();
    ++m_pushes;
    m_lastPush = "Pushed " + std::to_string(bin.size()) + " bytes, generation " +
                 std::to_string(gen) + ": the demo takes it up on its next field.";
    Logger::Info("Exocet: {}", m_lastPush);
    if (m_hooks.stateChanged)
        m_hooks.stateChanged();
}

void ExocetSession::SyncActivity()
{
    if (!m_pack || !m_check.valid)
        return;
    const int rows = static_cast<int>(m_check.rows.size());
    if (m_activityKey == m_pack->Path() && m_activity.Song() == m_check.digest &&
        m_activity.Rows() == rows)
        return;
    // another song under the same .exocet: what was recorded was for the old one's rows
    if (m_activityKey == m_pack->Path())
        SaveActivity();
    m_activityKey = m_pack->Path();
    m_activity = ExocetActivity{};
    m_activity.SetSong(m_check.digest, rows);
    std::error_code ec;
    if (fs::exists(m_activityFile, ec)) {
        std::ifstream in(m_activityFile, std::ios::binary);
        const auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object() && j.contains(m_activityKey) &&
            m_activity.FromJson(j[m_activityKey], m_check.digest, rows))
            Logger::Info("Exocet: activity for {} rows of {} from {}", m_activity.SeenRows(), rows,
                         m_activityFile.string());
    }
    m_activitySaved = m_activity.Revision();
    m_activitySaveDue = {};
}

void ExocetSession::SaveActivity()
{
    m_activitySaveDue = {};
    if (m_activityKey.empty() || m_activity.Revision() == m_activitySaved)
        return;
    m_activitySaved = m_activity.Revision();
    std::error_code ec;
    nlohmann::json all = nlohmann::json::object();
    if (fs::exists(m_activityFile, ec)) {
        std::ifstream in(m_activityFile, std::ios::binary);
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object())
            all = std::move(j);
    }
    all[m_activityKey] = m_activity.ToJson();
    const fs::path tmp = m_activityFile.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        // one line an .exocet, so the file reads as a list of them
        out << "{";
        bool first = true;
        for (const auto& [key, entry] : all.items()) {
            out << (first ? "\n  " : ",\n  ") << nlohmann::json(key).dump() << ": " << entry.dump();
            first = false;
        }
        out << "\n}\n";
        if (!out) {
            Logger::Warn("Exocet: could not write {}", tmp.string());
            return;
        }
    }
    fs::rename(tmp, m_activityFile, ec);
    if (ec)
        Logger::Warn("Exocet: could not write {}: {}", m_activityFile.string(), ec.message());
}

void ExocetSession::ForgetActivity()
{
    m_activity.Clear();
    SaveActivity();
}

void ExocetSession::ClearRecord()
{
    m_recorded.clear();
    m_divValid = false;
    m_emuFrames.clear();
    m_fired.clear();
}

void ExocetSession::ForgetParts()
{
    m_partsShown.clear();
    m_partsLoaded.clear();
    m_partsLastFrame = 0;
}

namespace {

// Frame `frame` doing `word` (part << 8 | phase, 0 nothing): the last run carried on, or a new one
void AddPart(std::vector<ExocetSession::PartRun>& runs, uint32_t frame, uint16_t word)
{
    if (!word)
        return;
    const uint8_t part = static_cast<uint8_t>(word >> 8), phase = static_cast<uint8_t>(word);
    if (!runs.empty()) {
        auto& last = runs.back();
        // a frame or two the recorder missed does not break a run
        if (last.part == part && last.phase == phase && frame >= last.to && frame <= last.to + 3) {
            last.to = frame + 1;
            return;
        }
        if (frame >= last.from && frame < last.to)
            return; // a frame seen again (a step back in the history)
    }
    runs.push_back({frame, frame + 1, part, phase});
}

} // namespace

void ExocetSession::RecordParts(const ExocetBlockState& b)
{
    if (b.version < 3)
        return;
    if (!b.partNames.empty())
        m_partNames = b.partNames;
    // a new boot: its song frames start again
    if (b.frame + 200u < m_partsLastFrame)
        ForgetParts();
    m_partsLastFrame = b.frame;
    AddPart(m_partsShown, b.frame, b.partRun);
    AddPart(m_partsLoaded, b.frame, b.partLoad);
}

void ExocetSession::Record(const ExocetBlockState& b)
{
    if (!b.present) {
        m_lastBlockAddr = 0;
        return;
    }
    RecordParts(b);
    if (b.blockAddress != m_lastBlockAddr) {
        // a new boot, or another program: what was recorded belongs to the last one
        ClearRecord();
        m_lastBlockAddr = b.blockAddress;
    }
    m_lastGeneration = b.generation;
    // which parts are running: only against the song the rows are, from a build of this pack
    if (m_check.valid && !m_activityKey.empty() && m_pack && b.manifest == m_pack->ManifestHash() &&
        b.arena.valid && b.arena.song == m_check.digest)
        m_activity.Mark(b.row, b.enabled, m_groupNames);
    Sample s;
    s.row = b.row;
    s.frac = b.frac;
    s.enabled = b.enabled;
    s.values = b.values;
    // the divergence counts follow the record: a frame recorded again replaces its count
    const bool keep = m_divValid && m_pack && m_check.valid &&
                      m_divRevision == m_pack->Revision() && m_divCheckGen == m_checkGen;
    if (keep) {
        if (auto it = m_recorded.find(b.frame); it != m_recorded.end())
            CountDivergence(it->second, -1);
        CountDivergence(s, 1);
    } else {
        m_divValid = false;
    }
    m_recorded[b.frame] = std::move(s);
    for (const auto& f : b.fired)
        m_fired.insert({f.track, f.row, f.frame});
}

std::vector<ExocetSession::EventProof> ExocetSession::Events()
{
    std::vector<EventProof> out;
    if (!m_pack || !m_check.valid)
        return out;
    const uint32_t first = m_recorded.empty() ? 0 : m_recorded.begin()->first;
    const uint32_t last = m_recorded.empty() ? 0 : m_recorded.rbegin()->first;
    for (const auto& t : m_pack->Tracks()) {
        if (t.type != "event")
            continue;
        const auto& keys = HostKeys(t.index);
        for (size_t k = 0; k < keys.size(); ++k) {
            EventProof e;
            e.track = t.index;
            e.keyRow = keys[k].row;
            e.value = keys[k].value;
            const uint32_t next = k + 1 < keys.size() ? keys[k + 1].row : 0xFFFFFFFFu;
            const uint32_t keyFrame = m_check.rows[e.keyRow].frame + KeyTick(keys[k]);
            for (const auto& [tr, row, frame] : m_fired) {
                if (tr != t.index || row < e.keyRow || row >= next)
                    continue;
                if (e.firedRow < 0 || row < e.firedRow) {
                    e.firedRow = row;
                    e.firedFrame = frame;
                }
            }
            if (e.firedRow >= 0) {
                e.state = e.firedRow == e.keyRow ? EventProof::OnTime : EventProof::Late;
                e.lateFrames = e.firedFrame - static_cast<int>(keyFrame);
            } else if (!m_recorded.empty() && first <= keyFrame &&
                       last >= keyFrame + m_check.rows[e.keyRow].frames) {
                e.state = EventProof::Missed;
            }
            out.push_back(e);
        }
    }
    return out;
}

std::vector<ExocetSession::CutProof> ExocetSession::Cuts()
{
    std::vector<CutProof> out;
    const int director = DirectorIndex();
    if (!m_pack || !m_check.valid || director < 0)
        return out;
    const auto& t = m_pack->Tracks()[director];
    const auto& keys = HostKeys(director);
    auto proof = [&](int cut) -> CutProof& {
        for (auto& c : out)
            if (c.cut == cut)
                return c;
        CutProof c;
        c.cut = cut;
        c.name = "cut " + std::to_string(cut);
        for (const auto& e : t.entries)
            if (e.value == cut)
                c.name = e.name;
        out.push_back(c);
        return out.back();
    };
    for (const auto& k : keys) {
        CutProof& c = proof(k.value);
        c.keyRow = k.row;
        c.keyFrame = m_check.rows[k.row].frame + KeyTick(k);
    }
    if (m_block.present && ManifestMatches()) {
        for (const auto& bc : m_block.cuts) {
            CutProof& c = proof(bc.cut);
            c.readyFrame = bc.readyFrame == 0xFFFF ? -1 : bc.readyFrame;
            c.cutFrame = bc.cutFrame == 0xFFFF ? -1 : bc.cutFrame;
        }
    }
    for (auto& c : out) {
        if (c.keyRow < 0)
            continue;
        if (c.readyFrame >= 0)
            c.margin = static_cast<int>(c.keyFrame) - c.readyFrame;
        if (c.cutFrame >= 0)
            c.late = std::max(0, c.cutFrame - static_cast<int>(c.keyFrame));
    }
    std::sort(out.begin(), out.end(),
              [](const CutProof& a, const CutProof& b) { return a.cut < b.cut; });
    return out;
}

std::map<int, int> ExocetSession::Divergence()
{
    std::map<int, int> out;
    if (!m_pack || !m_check.valid || !ManifestMatches())
        return out;
    // A count is a key evaluation per keyed track per recorded frame - minutes of recording
    // make that millions - so it is made once, and kept by Record as frames come in, until the
    // keys or the check change.
    if (!m_divValid || m_divRevision != m_pack->Revision() || m_divCheckGen != m_checkGen) {
        m_divTracks.clear();
        for (const auto& t : m_pack->Tracks()) {
            if (t.type == "event" || HostKeys(t.index).empty())
                continue;
            int g = 0;
            for (size_t i = 0; i < m_pack->Groups().size(); ++i)
                if (m_pack->Groups()[i].name == t.group)
                    g = static_cast<int>(i);
            m_divTracks.push_back({t.index, 1u << g});
        }
        m_divCounts.clear();
        m_divRevision = m_pack->Revision();
        m_divCheckGen = m_checkGen;
        m_divValid = true;
        for (const auto& [frame, s] : m_recorded)
            CountDivergence(s, 1);
    }
    for (const auto& [track, n] : m_divCounts)
        if (n)
            out[track] = n;
    return out;
}

void ExocetSession::CountDivergence(const Sample& s, int sign)
{
    for (const auto& [track, bit] : m_divTracks) {
        if (!(s.enabled & bit) || track >= static_cast<int>(s.values.size()))
            continue;
        if (HostValue(track, s.row, s.frac) != s.values[static_cast<size_t>(track)])
            m_divCounts[track] += sign;
    }
}

// ---- the samples ----

bool ExocetSession::SetSampleSetting(int sample, const std::string& setting, std::string* error)
{
    if (!m_pack) {
        if (error)
            *error = "No .exocet beside the disk.";
        return false;
    }
    m_pack->BreakMerge();
    return m_pack->SetSampleSetting(sample, setting, error);
}

bool ExocetSession::SetAutoDb(double db, const std::string& merge)
{
    return m_pack && m_pack->SetAutoDb(db, merge);
}

std::string ExocetSession::AuditionBlockedReason() const
{
    if (!m_pack)
        return "No .exocet beside the disk.";
    if (m_pack->ModulePath(MusicSong()).empty())
        return "The .exocet names no module.";
    if (m_kitState != KitState::Ready)
        return m_kitState == KitState::Untrusted ? "Trust the kit first: it makes what you hear."
                                                 : "No kit to make it with.";
    if (!m_kit.HasRole("audition"))
        return "The kit has no audition command: it is older than 4-bit samples.";
    if (m_busy)
        return "The kit is busy (" + m_busyWhat + ").";
    if (!m_hooks.listen)
        return "Nothing to play it in.";
    return {};
}

// The song the samples' report is of: the paged one (its samples have codecs), else the last
int ExocetSession::MusicSong() const
{
    if (m_check.musicSong >= 0)
        return m_check.musicSong;
    return m_pack ? std::max(0, m_pack->SongCount() - 1) : 0;
}

bool ExocetSession::ListenOriginal(int song)
{
    if (song < 0)
        song = MusicSong();
    if (!m_pack || m_pack->ModulePath(song).empty() || !m_hooks.listen)
        return false;
    const auto bytes = ReadBytes(m_pack->ModulePath(song));
    if (bytes.empty()) {
        m_lastAudition = "Could not read the module.";
        return false;
    }
    m_lastAudition = fs::path(m_pack->ModulePath(song)).filename().string() + ", as it is";
    m_hooks.listen(bytes, m_lastAudition);
    return true;
}

bool ExocetSession::Audition(int sample, int song)
{
    if (!AuditionBlockedReason().empty())
        return false;
    if (song < 0)
        song = MusicSong();
    auto job = std::make_unique<Job>();
    job->kind = Job::Audition;
    job->kit = m_kit;
    job->tools = m_tools;
    job->modules = m_pack->ModulePaths();
    job->song = song;
    job->params = m_kit.paramsPath;
    job->scratch = m_scratch;
    job->revision = m_pack->Revision();
    job->exocet = Snapshot("audition.exocet");
    job->audition = sample > 0 ? std::to_string(sample) : std::string("song");
    const std::string mod = fs::path(m_pack->ModulePath(song)).filename().string();
    if (sample > 0) {
        std::string name;
        for (const auto& s : m_check.samples)
            if (s.index == sample)
                name = s.name;
        job->title = fmt::format("Sample {} \"{}\": 8-bit, then 4-bit", sample, name);
    } else {
        job->title = mod + ", as the Amiga will play it";
    }
    if (job->exocet.empty())
        return false;
    m_lastAudition = "Making it...";
    StartJob(std::move(job));
    return true;
}

} // namespace adm::core
