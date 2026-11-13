#include "ExocetKit.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "JsonRead.h"
#include "MD5.h"
#include "Subprocess.h"

namespace adm::core {
namespace fs = std::filesystem;
namespace {

std::string ReadText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string Str(const nlohmann::json& o, const char* key)
{
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// "3.12.10" -> {3, 12, 10}
std::vector<int> Version(const std::string& s)
{
    std::vector<int> out;
    int n = 0;
    bool any = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            n = n * 10 + (c - '0');
            any = true;
        } else if (c == '.') {
            out.push_back(n);
            n = 0;
            any = false;
        } else {
            break;
        }
    }
    if (any)
        out.push_back(n);
    return out;
}

std::string Tail(const std::string& s, size_t n)
{
    return s.size() <= n ? s : "..." + s.substr(s.size() - n);
}

} // namespace

bool ExocetIssue::Blocks(const std::string& action) const
{
    return level == "block" && std::find(blocks.begin(), blocks.end(), action) != blocks.end();
}

nlohmann::json ExocetIssue::ToJson() const
{
    nlohmann::json j = {{"level", level}, {"code", code}, {"title", title}, {"text", text}};
    if (!at.is_null())
        j["at"] = at;
    if (!subject.is_null())
        j["subject"] = subject;
    if (level == "block")
        j["blocks"] = blocks;
    if (peekaboo)
        j["source"] = "peekaboo";
    return j;
}

std::vector<ExocetIssue> ExocetParseIssues(const nlohmann::json& report)
{
    std::vector<ExocetIssue> out;
    auto it = report.find("issues");
    if (it == report.end() || !it->is_array())
        return out;
    for (const auto& i : *it) {
        if (!i.is_object())
            continue;
        ExocetIssue x;
        x.level = Str(i, "level");
        x.code = Str(i, "code");
        x.title = Str(i, "title");
        x.text = Str(i, "text");
        if (auto a = i.find("at"); a != i.end())
            x.at = *a;
        if (auto s = i.find("subject"); s != i.end())
            x.subject = *s;
        if (auto b = i.find("blocks"); b != i.end() && b->is_array())
            for (const auto& v : *b)
                if (v.is_string())
                    x.blocks.push_back(v.get<std::string>());
        out.push_back(std::move(x));
    }
    return out;
}

ExocetIssue ExocetMakeIssue(const std::string& level, const std::string& code,
                            const std::string& title, const std::string& text,
                            std::vector<std::string> blocks)
{
    ExocetIssue i;
    i.level = level;
    i.code = code;
    i.title = title;
    i.text = text;
    i.blocks = std::move(blocks);
    i.peekaboo = true;
    return i;
}

bool ExocetKitInfo::Load(const std::string& kitJson, std::string* error)
{
    *this = ExocetKitInfo{};
    const auto doc = nlohmann::json::parse(ReadText(kitJson), nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || jsonread::ReadOr(doc, "kit", 0) != 1) {
        if (error)
            *error = kitJson + " is not a kit.json this reads (\"kit\": 1)";
        return false;
    }
    path = kitJson;
    dir = fs::path(kitJson).parent_path().string();
    production = Str(doc, "production");
    if (const std::string p = Str(doc, "params"); !p.empty())
        paramsPath = (fs::path(dir) / p).lexically_normal().string();
    if (auto r = doc.find("requires"); r != doc.end() && r->is_object())
        for (const auto& [k, v] : r->items())
            if (v.is_string())
                requirements[k] = v.get<std::string>();
    if (auto c = doc.find("commands"); c != doc.end() && c->is_object()) {
        for (const auto& [role, cmd] : c->items()) {
            if (!cmd.is_array() || cmd.empty())
                continue;
            std::vector<std::string> parts;
            for (const auto& a : cmd)
                if (a.is_string())
                    parts.push_back(a.get<std::string>());
            if (!parts.empty())
                commands[role] = std::move(parts);
        }
    }
    return true;
}

std::string ExocetKitHash(const std::string& dir, const std::vector<std::string>& skip)
{
    std::error_code ec;
    std::vector<std::pair<std::string, fs::path>> files;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && it->path().filename() == "__pycache__") {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;
        const std::string rel = fs::relative(it->path(), dir, ec).generic_string();
        if (std::find(skip.begin(), skip.end(), rel) != skip.end())
            continue;
        files.emplace_back(rel, it->path());
    }
    if (ec || files.empty())
        return {};
    std::sort(files.begin(), files.end());
    MD5 md5;
    for (const auto& [rel, p] : files) {
        md5.Update(rel.data(), rel.size());
        md5.Update("", 1);
        const std::string bytes = ReadText(p);
        md5.Update(bytes.data(), bytes.size());
    }
    md5.Final();
    return md5.HexDigest();
}

ExocetToolVersions ExocetProbeTools(const ExocetTools& tools)
{
    ExocetToolVersions v;
    {
        Subprocess proc;
        std::vector<std::string> lines;
        const int rc = proc.Run({tools.python.empty() ? "python" : tools.python, "-c",
                                 "import sys\nprint(sys.version.split()[0])\ntry:\n import lz4\n "
                                 "print(lz4.__version__)\n"
                                 "except Exception:\n print('')"},
                                [&](const std::string& l) { lines.push_back(l); });
        if (rc == 0 && !lines.empty()) {
            v.pythonRuns = true;
            v.python = lines[0];
            if (lines.size() > 1)
                v.lz4 = lines[1];
        } else {
            v.error = "could not run " + (tools.python.empty() ? "python" : tools.python);
        }
    }
    if (!tools.toolsDir.empty()) {
        for (const char* name : {"salvador.exe", "salvador"}) {
            const fs::path exe = fs::path(tools.toolsDir) / name;
            std::error_code ec;
            if (!fs::is_regular_file(exe, ec))
                continue;
            Subprocess proc;
            std::string out;
            proc.Run({exe.string()}, [&](const std::string& l) { out += l + "\n"; });
            // "salvador command-line tool v1.4.1 by Emmanuel Marty"
            const size_t at = out.find(" v");
            if (at != std::string::npos) {
                size_t end = at + 2;
                while (end < out.size() &&
                       (std::isdigit(static_cast<unsigned char>(out[end])) || out[end] == '.'))
                    ++end;
                v.salvador = out.substr(at + 2, end - at - 2);
            }
            break;
        }
    }
    return v;
}

std::vector<ExocetIssue> ExocetCheckRequires(const ExocetKitInfo& kit, const ExocetToolVersions& v)
{
    std::vector<ExocetIssue> out;
    const std::vector<std::string> all = {"push", "inject", "write"};
    if (!v.pythonRuns) {
        out.push_back(ExocetMakeIssue(
            "block", "tool.python", "Python could not be run",
            "The kit is Python; " + v.error + ". Set Exocet's Python in Preferences.", all));
        return out;
    }
    for (const auto& [tool, want] : kit.requirements) {
        if (tool == "python") {
            if (want.rfind(">=", 0) == 0 && Version(v.python) < Version(want.substr(2)))
                out.push_back(ExocetMakeIssue(
                    "block", "tool.python", "The kit needs a newer Python",
                    "The kit needs Python " + want + "; Peekaboo runs " + v.python + ".", all));
            continue;
        }
        const std::string have = tool == "python-lz4" ? v.lz4
                                 : tool == "salvador" ? v.salvador
                                                      : "?";
        if (have == want)
            continue;
        out.push_back(ExocetMakeIssue(
            "block", tool == "python-lz4" ? "tool.lz4" : "tool." + tool,
            "The kit's " + tool + " is not the one Peekaboo has",
            "The kit packs with " + tool + " " + want + "; Peekaboo has " +
                (have.empty() ? std::string("none") : have) +
                ". A disk packed with another version would not be the one the build writes.",
            all));
    }
    return out;
}

bool ExocetKitResult::Ok() const
{
    return launched && !timedOut && exitCode == 0 &&
           std::none_of(issues.begin(), issues.end(),
                        [](const ExocetIssue& i) { return i.level == "block"; });
}

std::string ExocetScratchDir(const std::string& exocetPath)
{
    std::error_code ec;
    if (exocetPath.empty())
        return (fs::temp_directory_path(ec) / "peekaboo-exocet").string();
    return (fs::path(exocetPath).parent_path() / "exocet_temp").string();
}

ExocetKitResult ExocetRunKit(const ExocetKitInfo& kit, const ExocetTools& tools,
                             const std::string& role, const std::vector<std::string>& args,
                             int timeoutMs, Subprocess* proc, const std::string& scratchRoot)
{
    ExocetKitResult r;
    auto cmd = kit.commands.find(role);
    if (cmd == kit.commands.end()) {
        r.issues.push_back(ExocetMakeIssue(
            "block", "kit.role", "The kit has no " + role + " command",
            kit.path + " names no \"" + role + "\" command.", {"push", "inject", "write"}));
        return r;
    }
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    const fs::path scratch =
        (scratchRoot.empty() ? fs::path(ExocetScratchDir({})) : fs::path(scratchRoot)) /
        (role + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(counter++));
    fs::create_directories(scratch, ec);
    const fs::path report = scratch / "report.json";

    std::vector<std::string> argv = {tools.python.empty() ? "python" : tools.python,
                                     (fs::path(kit.dir) / cmd->second[0]).string()};
    argv.insert(argv.end(), cmd->second.begin() + 1, cmd->second.end());
    argv.insert(argv.end(), args.begin(), args.end());
    argv.push_back(report.string());

    SubprocessOptions opts;
    opts.workingDir = scratch.string();
    opts.env = {{"PYTHONPATH", std::nullopt},
                {"PYTHONDONTWRITEBYTECODE", std::string("1")},
                {"PYTHONIOENCODING", std::string("utf-8")},
                {"EXOCET_TOOLS", tools.toolsDir.empty()
                                     ? std::nullopt
                                     : std::optional<std::string>(tools.toolsDir)}};

    Subprocess local;
    Subprocess& p = proc ? *proc : local;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::atomic<bool> timedOut{false};
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(m);
        if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return done; })) {
            timedOut = true;
            p.Cancel();
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    r.exitCode = p.Run(argv, [&](const std::string& l) { r.output += l + "\n"; }, opts);
    {
        std::lock_guard<std::mutex> lock(m);
        done = true;
    }
    cv.notify_all();
    watchdog.join();
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.launched = r.exitCode != -1;
    r.timedOut = timedOut;

    if (fs::is_regular_file(report, ec)) {
        r.report = nlohmann::json::parse(ReadText(report), nullptr, false);
        if (r.report.is_discarded())
            r.report = nlohmann::json();
    }
    fs::remove_all(scratch, ec);
    r.issues = ExocetParseIssues(r.report);

    if (!r.launched) {
        r.issues.push_back(ExocetMakeIssue("block", "tool.python", "The kit could not be run",
                                           "Could not start " + argv[0] + ".",
                                           {"push", "inject", "write"}));
    } else if (r.timedOut) {
        r.issues.push_back(
            ExocetMakeIssue("block", "kit.timeout", "The kit's " + role + " took too long",
                            "Stopped after " + std::to_string(timeoutMs / 1000) + " s.",
                            {"push", "inject", "write"}));
    } else if (r.exitCode != 0 && r.issues.empty()) {
        r.issues.push_back(
            ExocetMakeIssue("block", "kit." + role, "The kit's " + role + " failed",
                            "Exit " + std::to_string(r.exitCode) + ": " + Tail(r.output, 600),
                            {"push", "inject", "write"}));
    }
    return r;
}

bool ExocetCheck::Parse(const nlohmann::json& report)
{
    *this = ExocetCheck{};
    issues = ExocetParseIssues(report);
    auto rs = report.find("rows");
    if (rs == report.end() || !rs->is_array())
        return false;
    if (auto m = report.find("music"); m != report.end() && m->is_object()) {
        music = *m;
        jsonread::ReadIf(*m, "channels", channels);
        jsonread::ReadIf(*m, "frames", frames);
        const std::string d = Str(*m, "digest");
        if (d.size() > 2)
            digest = static_cast<uint32_t>(std::strtoul(d.c_str() + 2, nullptr, 16));
        if (auto o = m->find("order"); o != m->end() && o->is_array())
            for (const auto& x : *o)
                if (x.is_number_integer())
                    order.push_back(x.get<int>());
        if (auto ss = m->find("songs"); ss != m->end() && ss->is_array())
            for (const auto& s : *ss) {
                if (!jsonread::ReadOr(s, "resident", false))
                    musicSong = static_cast<int>(digests.size());
                const std::string sd = Str(s, "digest");
                digests.push_back(
                    sd.size() > 2 ? static_cast<uint32_t>(std::strtoul(sd.c_str() + 2, nullptr, 16))
                                  : 0);
            }
        if (auto os = m->find("orders"); os != m->end() && os->is_array())
            for (const auto& o : *os) {
                std::vector<int> one;
                if (o.is_array())
                    for (const auto& x : o)
                        if (x.is_number_integer())
                            one.push_back(x.get<int>());
                orders.push_back(std::move(one));
            }
        if (digests.empty())
            digests.push_back(digest);
        if (orders.empty())
            orders.push_back(order);
        digest = digests.front();
    }
    if (auto e = report.find("exocet"); e != report.end())
        exocet = *e;
    // The report comes from a kit, maybe one under development: every read is type-checked, an
    // entry of the wrong shape is skipped (and said), and a cell value that is not a 16-bit
    // unsigned reads as 0. Parse runs on the kit worker, where a throw is std::terminate.
    int skipped = 0;
    rows.reserve(rs->size());
    for (const auto& r : *rs) {
        if (!r.is_object()) {
            ++skipped;
            continue;
        }
        ExocetRow row;
        jsonread::ReadIf(r, "song", row.song);
        jsonread::ReadIf(r, "pos", row.pos);
        jsonread::ReadIf(r, "pattern", row.pattern);
        jsonread::ReadIf(r, "row", row.row);
        jsonread::ReadIf(r, "pass", row.pass);
        jsonread::ReadIf(r, "frame", row.frame);
        jsonread::ReadIf(r, "frames", row.frames);
        if (auto c = r.find("cells"); c != r.end() && c->is_array())
            for (const auto& cell : *c) {
                std::array<uint16_t, 4> v{};
                if (cell.is_array()) {
                    for (size_t k = 0; k < 4 && k < cell.size(); ++k)
                        if (!jsonread::Get(cell[k], v[k]))
                            ++skipped;
                } else {
                    ++skipped; // an empty cell keeps the channels in place
                }
                row.cells.push_back(v);
            }
        m_index[Place(row.song, row.pos, row.row, row.pass)] = static_cast<int>(rows.size());
        rows.push_back(std::move(row));
    }
    if (auto ss = report.find("samples"); ss != report.end() && ss->is_array())
        for (const auto& s : *ss) {
            if (!s.is_object()) {
                ++skipped;
                continue;
            }
            ExocetSampleInfo si;
            jsonread::ReadIf(s, "index", si.index);
            si.name = Str(s, "name");
            jsonread::ReadIf(s, "bytes", si.bytes);
            jsonread::ReadIf(s, "volume", si.volume);
            jsonread::ReadIf(s, "loop", si.loop);
            if (s.contains("codec")) {
                si.assessed = true;
                jsonread::ReadIf(s, "packed", si.packed);
                jsonread::ReadIf(s, "delta4", si.delta4);
                jsonread::ReadIf(s, "db", si.db);
                jsonread::ReadIf(s, "worst", si.worst);
                si.setting = Str(s, "setting");
                si.codec = Str(s, "codec");
                si.why = Str(s, "why");
                jsonread::ReadIf(s, "used", si.used);
            }
            samples.push_back(std::move(si));
        }
    if (auto ks = report.find("keys"); ks != report.end() && ks->is_object())
        for (const auto& [track, list] : ks->items()) {
            auto& out = keys[track];
            if (!list.is_array()) {
                ++skipped;
                continue;
            }
            for (const auto& k : list) {
                if (!k.is_object()) {
                    ++skipped;
                    continue;
                }
                ExocetKey key;
                jsonread::ReadIf(k, "row", key.row);
                jsonread::ReadIf(k, "interp", key.interp);
                jsonread::ReadIf(k, "value", key.value);
                jsonread::ReadIf(k, "sub", key.sub);
                out.push_back(key);
            }
        }
    if (skipped)
        issues.push_back(ExocetMakeIssue(
            "warn", "kit.report", "The kit's report has malformed entries",
            std::to_string(skipped) + " row, cell, sample or key entries were the wrong type and "
                                      "were skipped.",
            {}));
    valid = true;
    return true;
}

uint64_t ExocetCheck::Place(int song, int pos, int row, int pass)
{
    return (uint64_t(song & 0xFF) << 48) | (uint64_t(uint16_t(pos)) << 32) |
           (uint64_t(row & 0xFFFF) << 16) | uint64_t(pass & 0xFFFF);
}

int ExocetCheck::FindRow(int song, int pos, int row, int pass) const
{
    auto it = m_index.find(Place(song, pos, row, pass));
    return it == m_index.end() ? -1 : it->second;
}

int ExocetCheck::RowAtFrame(uint32_t frame) const
{
    if (rows.empty())
        return -1;
    auto it = std::upper_bound(rows.begin(), rows.end(), frame,
                               [](uint32_t f, const ExocetRow& r) { return f < r.frame; });
    return it == rows.begin() ? 0 : static_cast<int>(it - rows.begin()) - 1;
}

std::string ExocetPlaceLabel(const ExocetRow& r, bool songs)
{
    std::string out = songs ? std::to_string(r.song) + " " : std::string();
    char buf[32];
    if (r.pos < 0)
        std::snprintf(buf, sizeof buf, "L%d:%02u", -r.pos, r.row);
    else
        std::snprintf(buf, sizeof buf, "%02d:%02u", r.pos, r.row);
    out += buf;
    if (r.pass)
        out += "." + std::to_string(r.pass);
    return out;
}

bool ExocetParsePlace(const std::string& text, int* song, int* pos, int* row, int* pass)
{
    auto number = [](const std::string& s, int* out) {
        if (s.empty() || s.size() > 6 ||
            !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }))
            return false;
        *out = std::stoi(s);
        return true;
    };
    std::string t = text;
    std::replace(t.begin(), t.end(), ' ', ':');
    std::vector<std::string> parts;
    for (size_t at = 0;;) {
        const size_t colon = t.find(':', at);
        parts.push_back(t.substr(at, colon == std::string::npos ? colon : colon - at));
        if (colon == std::string::npos)
            break;
        at = colon + 1;
    }
    if (parts.size() < 2 || parts.size() > 3)
        return false;
    int s = song ? *song : 0, p = 0, r = 0, n = 0;
    if (parts.size() == 3 && !number(parts[0], &s))
        return false;
    std::string ps = parts[parts.size() - 2], rs = parts.back();
    if (const size_t dot = rs.find('.'); dot != std::string::npos) {
        if (!number(rs.substr(dot + 1), &n))
            return false;
        rs.resize(dot);
    }
    const bool lead = !ps.empty() && (ps[0] == 'L' || ps[0] == 'l');
    if (!number(lead ? ps.substr(1) : ps, &p) || !number(rs, &r) || (lead && p == 0))
        return false;
    if (song)
        *song = s;
    *pos = lead ? -p : p;
    *row = r;
    *pass = n;
    return true;
}

std::string ExocetNoteName(uint16_t period)
{
    if (!period)
        return "---";
    // ProTracker's finetune-0 periods, C-1 to B-3
    static const uint16_t kPeriods[36] = {
        856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320,
        302, 285, 269, 254, 240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113};
    static const char* kNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-",
                                     "F#", "G-", "G#", "A-", "A#", "B-"};
    int best = 0;
    for (int i = 1; i < 36; ++i)
        if (std::abs(kPeriods[i] - period) < std::abs(kPeriods[best] - period))
            best = i;
    return std::string(kNames[best % 12]) + std::to_string(1 + best / 12);
}

} // namespace adm::core
