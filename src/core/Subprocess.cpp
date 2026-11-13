#include "Subprocess.h"

#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <map>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>

extern char** environ; // not declared by every libc's unistd.h (macOS's is not)
#endif

namespace adm {
namespace core {

namespace {

// Split whatever has accumulated in `buffer` on '\n' and '\r' and dispatch each complete,
// non-empty line. The trailing partial line (after the last delimiter) is left in `buffer`.
void DispatchLines(std::string& buffer, const std::function<void(const std::string&)>& onLine)
{
    size_t pos;
    while ((pos = buffer.find_first_of("\r\n")) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (!line.empty())
            onLine(line);
    }
}

#ifdef _WIN32
// Quote a single argument per the CommandLineToArgvW parsing rules so that CreateProcess
// reconstructs argv exactly. See "Everyone quotes command line arguments the wrong way".
void AppendQuotedArg(std::string& cmdline, const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        cmdline += arg;
        return;
    }
    cmdline += '"';
    for (auto it = arg.begin();; ++it) {
        unsigned numBackslashes = 0;
        while (it != arg.end() && *it == '\\') {
            ++it;
            ++numBackslashes;
        }
        if (it == arg.end()) {
            // Escape all backslashes, but leave the terminating double quote unescaped.
            cmdline.append(numBackslashes * 2, '\\');
            break;
        } else if (*it == '"') {
            // Escape all backslashes and the following double quote.
            cmdline.append(numBackslashes * 2 + 1, '\\');
            cmdline += *it;
        } else {
            // Backslashes aren't special here.
            cmdline.append(numBackslashes, '\\');
            cmdline += *it;
        }
    }
    cmdline += '"';
}

// Arguments are UTF-8 throughout; the wide API is the only way a non-ASCII path reaches the child.
std::wstring WidenUtf8(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

// The parent's environment with `changes` applied, as the sorted, double-NUL-terminated block
// CreateProcessW takes with CREATE_UNICODE_ENVIRONMENT. Names compare without case, as Windows'.
std::wstring
EnvironmentBlock(const std::vector<std::pair<std::string, std::optional<std::string>>>& changes)
{
    struct NoCase
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const
        {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        }
    };
    std::map<std::wstring, std::wstring, NoCase> vars;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            const size_t eq = entry.find(L'=', 1); // "=C:=C:\..." entries start with '='
            if (eq != std::wstring::npos)
                vars[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& [name, value] : changes) {
        if (value)
            vars[WidenUtf8(name)] = WidenUtf8(*value);
        else
            vars.erase(WidenUtf8(name));
    }
    std::wstring block;
    for (const auto& [name, value] : vars) {
        block += name + L"=" + value;
        block += L'\0';
    }
    block += L'\0';
    return block;
}
#endif

} // namespace

Subprocess::~Subprocess() { Cancel(); }

int Subprocess::Run(const std::vector<std::string>& args,
                    const std::function<void(const std::string&)>& onLine)
{
    return Run(args, onLine, SubprocessOptions{});
}

#ifdef _WIN32

int Subprocess::Run(const std::vector<std::string>& args,
                    const std::function<void(const std::string&)>& onLine,
                    const SubprocessOptions& options)
{
    if (args.empty())
        return -1;

    m_cancelled.store(false);

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return -1;
    // The read end must not be inherited by the child.
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // Build the command line (CreateProcess takes a single, mutable, properly-quoted string).
    std::string cmdline;
    for (const auto& a : args) {
        if (!cmdline.empty())
            cmdline += ' ';
        AppendQuotedArg(cmdline, a);
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    std::wstring wideCmd = WidenUtf8(cmdline);
    std::wstring envBlock;
    // Suspended until it is in the job, so nothing it starts can escape it
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (!options.env.empty()) {
        envBlock = EnvironmentBlock(options.env);
        flags |= CREATE_UNICODE_ENVIRONMENT;
    }
    const std::wstring wideDir = WidenUtf8(options.workingDir);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, wideCmd.data(), nullptr, nullptr, TRUE, flags,
                             envBlock.empty() ? nullptr : envBlock.data(),
                             wideDir.empty() ? nullptr : wideDir.c_str(), &si, &pi);

    // Parent never writes to the pipe; close our copy so ReadFile sees EOF when the child exits.
    CloseHandle(writePipe);

    if (!ok) {
        CloseHandle(readPipe);
        return -1;
    }

    // The child and its children in one job, killed together on a cancel and when the job's
    // handle closes. Without one (a parent job that forbids nesting), the child alone.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits)) ||
            !AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    m_jobHandle.store(job);
    m_processHandle.store(pi.hProcess);
    ResumeThread(pi.hThread);
    // Honor a cancel that arrived between store() above and here.
    if (m_cancelled.load())
        Cancel();

    char buffer[256];
    std::string lineBuffer;
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        if (m_cancelled.load()) {
            Cancel();
            break;
        }
        lineBuffer.append(buffer, bytesRead);
        DispatchLines(lineBuffer, onLine);
    }
    if (!lineBuffer.empty())
        onLine(lineBuffer);

    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    m_processHandle.store(nullptr);
    // KILL_ON_JOB_CLOSE: anything the child left running goes with the job
    if (HANDLE j = static_cast<HANDLE>(m_jobHandle.exchange(nullptr)))
        CloseHandle(j);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return m_cancelled.load() ? 1 : static_cast<int>(exitCode);
}

void Subprocess::Cancel()
{
    m_cancelled.store(true);
    // the whole job when there is one: a grandchild holding the output pipe ends too
    if (HANDLE j = static_cast<HANDLE>(m_jobHandle.load()))
        TerminateJobObject(j, 1);
    HANDLE h = static_cast<HANDLE>(m_processHandle.load());
    if (h)
        TerminateProcess(h, 1);
}

PipeProcess::~PipeProcess()
{
    if (m_running)
        Kill();
}

bool PipeProcess::Start(const std::vector<std::string>& args, const std::filesystem::path& logFile,
                        std::string& outError)
{
    outError.clear();
    if (m_running) {
        outError = "already running";
        return false;
    }
    if (args.empty()) {
        outError = "nothing to run";
        return false;
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    // A large buffer lets the encoder fall a few frames behind without blocking the writer.
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 8 * 1024 * 1024)) {
        outError = "could not create a pipe";
        return false;
    }
    SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);

    HANDLE log =
        CreateFileW(logFile.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        outError = "could not create the log file " + logFile.string();
        return false;
    }

    std::string cmdline;
    for (const auto& a : args) {
        if (!cmdline.empty())
            cmdline += ' ';
        AppendQuotedArg(cmdline, a);
    }
    std::wstring wideCmd = WidenUtf8(cmdline);

    // Only these two handles go to the child. Without the list it inherits every inheritable
    // handle in the process, including other children's pipes, which then never see EOF until
    // this one exits.
    HANDLE inherited[2] = {readEnd, log};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuffer(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    const bool haveList = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                    inherited, sizeof(inherited), nullptr, nullptr);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = readEnd;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    si.lpAttributeList = haveList ? attrs : nullptr;

    PROCESS_INFORMATION pi = {};
    const DWORD flags = CREATE_NO_WINDOW | (haveList ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL ok = CreateProcessW(nullptr, wideCmd.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                   nullptr, &si.StartupInfo, &pi);
    const DWORD launchError = ok ? 0 : GetLastError();
    if (haveList)
        DeleteProcThreadAttributeList(attrs);
    CloseHandle(readEnd);
    CloseHandle(log);
    if (!ok) {
        CloseHandle(writeEnd);
        outError =
            "could not start " + args[0] + " (Windows error " + std::to_string(launchError) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    m_process = pi.hProcess;
    m_stdinWrite = writeEnd;
    m_running = true;
    return true;
}

bool PipeProcess::Write(const uint8_t* data, size_t size)
{
    if (!m_running || !m_stdinWrite)
        return false;
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, size_t{1} << 30));
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(m_stdinWrite), data, chunk, &written, nullptr) ||
            written == 0)
            return false;
        data += written;
        size -= written;
    }
    return true;
}

int PipeProcess::Finish()
{
    if (!m_process)
        return -1;
    if (m_stdinWrite) {
        CloseHandle(static_cast<HANDLE>(m_stdinWrite)); // EOF: the child flushes and exits
        m_stdinWrite = nullptr;
    }
    WaitForSingleObject(static_cast<HANDLE>(m_process), INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(static_cast<HANDLE>(m_process), &exitCode);
    CloseHandle(static_cast<HANDLE>(m_process));
    m_process = nullptr;
    m_running = false;
    return static_cast<int>(exitCode);
}

void PipeProcess::Kill()
{
    if (m_process)
        TerminateProcess(static_cast<HANDLE>(m_process), 1);
    Finish();
}

#else // POSIX

int Subprocess::Run(const std::vector<std::string>& args,
                    const std::function<void(const std::string&)>& onLine,
                    const SubprocessOptions& options)
{
    if (args.empty())
        return -1;

    m_cancelled.store(false);

    int pipeFds[2];
    if (pipe(pipeFds) != 0)
        return -1;

    // Built BEFORE fork(): this process runs several threads (GDB/MCP/HTTP servers, download
    // workers), so the child may inherit a malloc lock held mid-allocation — anything between
    // fork() and exec must be async-signal-safe, which heap allocation is not.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    // The child's environment, likewise before fork(): the parent's with the changes applied.
    std::vector<std::string> envStrings;
    std::vector<char*> envp;
    if (!options.env.empty()) {
        for (char** e = environ; *e; ++e) {
            const std::string entry(*e);
            const std::string name = entry.substr(0, entry.find('='));
            if (std::none_of(options.env.begin(), options.env.end(),
                             [&](const auto& c) { return c.first == name; }))
                envStrings.push_back(entry);
        }
        for (const auto& [name, value] : options.env)
            if (value)
                envStrings.push_back(name + "=" + *value);
        for (auto& e : envStrings)
            envp.push_back(e.data());
        envp.push_back(nullptr);
    }
    const char* dir = options.workingDir.empty() ? nullptr : options.workingDir.c_str();

    pid_t pid = fork();
    if (pid < 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        return -1;
    }

    if (pid == 0) {
        // Child: become our own process-group leader so the parent can kill the whole group,
        // redirect stdout+stderr into the pipe, then exec the target.
        setpgid(0, 0);
        if (dup2(pipeFds[1], STDOUT_FILENO) < 0 || dup2(pipeFds[1], STDERR_FILENO) < 0)
            _exit(126); // no pipe = the parent would block forever on a silent child
        close(pipeFds[0]);
        close(pipeFds[1]);
        if (dir && chdir(dir) != 0)
            _exit(126);
        if (!envp.empty())
            environ = envp.data(); // execvp searches this environment's PATH

        execvp(argv[0], argv.data());
        _exit(127); // exec failed
    }

    // Parent.
    close(pipeFds[1]);
    // Mirror the child's setpgid to avoid a race where we signal the group before the child has
    // set it; harmless if the child already ran.
    setpgid(pid, pid);
    m_childPid.store(pid);
    // Honor a cancel that arrived between store() above and here.
    if (m_cancelled.load())
        kill(-pid, SIGTERM);

    FILE* pipe = fdopen(pipeFds[0], "r");
    if (!pipe) {
        close(pipeFds[0]);
        kill(-pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        m_childPid.store(0);
        return -1;
    }

    char buffer[256];
    std::string lineBuffer;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        if (m_cancelled.load()) {
            kill(-pid, SIGTERM);
            break;
        }
        lineBuffer += buffer;
        DispatchLines(lineBuffer, onLine);
    }
    if (!lineBuffer.empty())
        onLine(lineBuffer);

    fclose(pipe); // also closes pipeFds[0]

    int status = 0;
    waitpid(pid, &status, 0);
    m_childPid.store(0);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void Subprocess::Cancel()
{
    m_cancelled.store(true);
    pid_t pid = m_childPid.load();
    if (pid > 0)
        kill(-pid, SIGTERM);
}

PipeProcess::~PipeProcess()
{
    if (m_running)
        Kill();
}

bool PipeProcess::Start(const std::vector<std::string>& args, const std::filesystem::path& logFile,
                        std::string& outError)
{
    outError.clear();
    if (m_running) {
        outError = "already running";
        return false;
    }
    if (args.empty()) {
        outError = "nothing to run";
        return false;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        outError = "could not create a pipe";
        return false;
    }
    // Close-on-exec, so a child some other thread starts does not keep this pipe open.
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    const int logFd = open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (logFd < 0) {
        close(fds[0]);
        close(fds[1]);
        outError = "could not create the log file " + logFile.string();
        return false;
    }

    // Built before fork(): heap allocation is not async-signal-safe, and this process has threads.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        close(logFd);
        outError = "could not fork";
        return false;
    }
    if (pid == 0) {
        // dup2 clears close-on-exec on the new descriptors.
        if (dup2(fds[0], STDIN_FILENO) < 0 || dup2(logFd, STDOUT_FILENO) < 0 ||
            dup2(logFd, STDERR_FILENO) < 0)
            _exit(126);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(fds[0]);
    close(logFd);
    m_pid = pid;
    m_stdinWrite = fds[1];
    m_running = true;
    return true;
}

bool PipeProcess::Write(const uint8_t* data, size_t size)
{
    if (!m_running || m_stdinWrite < 0)
        return false;
    while (size > 0) {
        const ssize_t n = ::write(m_stdinWrite, data, size);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false; // EPIPE once the child has gone
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

int PipeProcess::Finish()
{
    if (m_pid <= 0)
        return -1;
    if (m_stdinWrite >= 0) {
        close(m_stdinWrite);
        m_stdinWrite = -1;
    }
    int status = 0;
    while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {
    }
    m_pid = 0;
    m_running = false;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void PipeProcess::Kill()
{
    if (m_pid > 0)
        kill(m_pid, SIGTERM);
    Finish();
}

#endif

} // namespace core
} // namespace adm
