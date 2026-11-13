#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adm::core {

/// A resolved command line, ready to hand to Subprocess.
struct EditorCommand
{
    std::vector<std::string> argv; // argv[0] is the program, looked up on PATH
    [[nodiscard]] bool Valid() const { return !argv.empty(); }
    /// Space-joined, for logging and tests. Not shell-quoted — never feed this to a shell.
    [[nodiscard]] std::string Describe() const;
};

/**
 * @brief argv that opens `file` at `line` in VS Code, given an already-resolved launcher.
 *
 * Pure and platform-independent so the argument shape can be tested without VS Code
 * installed. `-g file:line` is VS Code's "goto" form; the CLI hands the request to an
 * already-running instance over its own IPC, so this doubles as the "tell the running
 * editor to jump" path and needs no channel of our own.
 *
 * A .cmd/.bat launcher cannot be started by CreateProcess directly (it resolves .exe only,
 * not PATHEXT), so those are wrapped in `cmd.exe /c`. Prefer a real executable where one
 * exists -- see FindVsCodeLauncher.
 */
[[nodiscard]] EditorCommand MakeVsCodeGoto(std::string_view launcher, std::string_view file,
                                           uint32_t line);

/// Injectable existence predicate so the search order can be unit-tested.
using PathExistsFn = std::function<bool(const std::string&)>;

/**
 * @brief Locate a VS Code launcher: PATH first, then the well-known install directories.
 *
 * On Windows the thing on PATH is `code.cmd`, a batch shim. When one is found, the real
 * `Code.exe` one directory up is preferred: it takes the same arguments and skips cmd.exe's
 * second round of command-line parsing, which would otherwise mangle a path containing
 * `&` or `^`.
 *
 * @param pathEnv  the PATH variable's contents (separator-split internally)
 * @param exists   returns true when the given absolute path is an existing file
 * @param homeDirs extra roots to probe (on Windows, LOCALAPPDATA and the Program Files pair)
 */
[[nodiscard]] std::optional<std::string>
FindVsCodeLauncher(std::string_view pathEnv, const PathExistsFn& exists,
                   const std::vector<std::string>& homeDirs);

/// Same, reading PATH and the install roots from the real environment and filesystem.
[[nodiscard]] std::optional<std::string> FindVsCodeLauncher();

/**
 * @brief Open `file` at `line` in VS Code, reusing a running instance when there is one.
 *
 * Spawns on a detached thread: even against a live instance the CLI takes long enough that
 * doing it inline would visibly hitch the UI frame.
 *
 * @param err set to a human-readable reason when this returns false
 * @return false only when no launcher could be found; a launch failure after that is logged.
 */
bool OpenInVsCode(const std::string& file, uint32_t line, std::string& err);

} // namespace adm::core
