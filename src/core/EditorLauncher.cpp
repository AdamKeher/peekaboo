#include "EditorLauncher.h"

#include "Logger.h"
#include "Subprocess.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace adm::core {

namespace {

#ifdef _WIN32
constexpr char kPathSep = ';';
constexpr bool kWindows = true;
#else
constexpr char kPathSep = ':';
constexpr bool kWindows = false;
#endif

bool EndsWithNoCase(std::string_view s, std::string_view suffix)
{
    if (s.size() < suffix.size())
        return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

std::vector<std::string> SplitPath(std::string_view pathEnv)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= pathEnv.size()) {
        size_t sep = pathEnv.find(kPathSep, start);
        std::string_view piece = (sep == std::string_view::npos)
                                     ? pathEnv.substr(start)
                                     : pathEnv.substr(start, sep - start);
        // A quoted PATH entry is legal on Windows and must not become part of the filename.
        while (!piece.empty() && (piece.front() == '"' || piece.front() == ' '))
            piece.remove_prefix(1);
        while (!piece.empty() && (piece.back() == '"' || piece.back() == ' '))
            piece.remove_suffix(1);
        if (!piece.empty())
            out.emplace_back(piece);
        if (sep == std::string_view::npos)
            break;
        start = sep + 1;
    }
    return out;
}

// Forward slashes throughout, matching SourceFileCache: VS Code accepts either, and a
// single convention keeps the logged command line and the resolver's output comparable.
std::string Join(const std::string& dir, const std::string& leaf)
{
    std::string out = (fs::path(dir) / leaf).lexically_normal().string();
    for (char& c : out)
        if (c == '\\')
            c = '/';
    return out;
}

std::string Env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
}

} // namespace

std::string EditorCommand::Describe() const
{
    std::string out;
    for (const auto& a : argv) {
        if (!out.empty())
            out.push_back(' ');
        out += a;
    }
    return out;
}

EditorCommand MakeVsCodeGoto(std::string_view launcher, std::string_view file, uint32_t line)
{
    EditorCommand cmd;
    if (launcher.empty() || file.empty())
        return cmd;

    std::string target(file);
    target += ':';
    target += std::to_string(line == 0 ? 1u : line);

    // A batch shim cannot be started directly by CreateProcess, which resolves .exe only and
    // ignores PATHEXT. Route those through cmd.exe; everything else is executed directly.
    if (EndsWithNoCase(launcher, ".cmd") || EndsWithNoCase(launcher, ".bat"))
        cmd.argv = {"cmd.exe", "/c", std::string(launcher), "-g", target};
    else
        cmd.argv = {std::string(launcher), "-g", target};
    return cmd;
}

std::optional<std::string> FindVsCodeLauncher(std::string_view pathEnv, const PathExistsFn& exists,
                                              const std::vector<std::string>& homeDirs)
{
    if (!exists)
        return std::nullopt;

    // 1. PATH. On Windows the entry is code.cmd; prefer the sibling Code.exe one level up so
    //    the command line is not parsed a second time by cmd.exe.
    for (const auto& dir : SplitPath(pathEnv)) {
        if (kWindows) {
            const std::string cmdShim = Join(dir, "code.cmd");
            if (exists(cmdShim)) {
                const std::string real = Join(dir, "../Code.exe");
                return exists(real) ? real : cmdShim;
            }
            const std::string exe = Join(dir, "code.exe");
            if (exists(exe))
                return exe;
        } else {
            const std::string bin = Join(dir, "code");
            if (exists(bin))
                return bin;
        }
    }

    // 2. Well-known install roots, for the common case of VS Code installed but never added
    //    to PATH (its installer makes that a checkbox).
    for (const auto& root : homeDirs) {
        if (root.empty())
            continue;
        const std::string candidate = kWindows ? Join(root, "Code.exe") : Join(root, "code");
        if (exists(candidate))
            return candidate;
    }

    return std::nullopt;
}

std::optional<std::string> FindVsCodeLauncher()
{
    std::vector<std::string> roots;
#ifdef _WIN32
    const std::string localAppData = Env("LOCALAPPDATA");
    if (!localAppData.empty())
        roots.push_back(Join(localAppData, "Programs/Microsoft VS Code"));
    roots.push_back("C:/Program Files/Microsoft VS Code");
    roots.push_back("C:/Program Files (x86)/Microsoft VS Code");
#else
    roots.push_back("/usr/share/code/bin");
    roots.push_back("/usr/local/bin");
    roots.push_back("/Applications/Visual Studio Code.app/Contents/Resources/app/bin");
#endif

    auto exists = [](const std::string& p) {
        std::error_code ec;
        return fs::is_regular_file(fs::path(p), ec);
    };
    return FindVsCodeLauncher(Env("PATH"), exists, roots);
}

bool OpenInVsCode(const std::string& file, uint32_t line, std::string& err)
{
    auto launcher = FindVsCodeLauncher();
    if (!launcher) {
        err = "VS Code was not found on PATH or in the usual install directories. Install the "
              "'code' command (VS Code: Shell Command: Install 'code' command in PATH).";
        return false;
    }

    const EditorCommand cmd = MakeVsCodeGoto(*launcher, file, line);
    if (!cmd.Valid()) {
        err = "could not build a VS Code command for " + file;
        return false;
    }

    // Detached: the CLI hands off to the running instance over its own IPC, but even that
    // round trip is far longer than a UI frame.
    std::thread([argv = cmd.argv] {
        Subprocess proc;
        const int rc = proc.Run(
            argv, [](const std::string& outLine) { Logger::Info("VS Code: {}", outLine); });
        if (rc == -1)
            Logger::Error("Could not launch VS Code ({}).", argv.empty() ? "" : argv[0]);
    }).detach();

    Logger::Info("Opening in VS Code: {}", cmd.Describe());
    return true;
}

} // namespace adm::core
