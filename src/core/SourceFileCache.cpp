#include "SourceFileCache.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace adm::core {

namespace {

// Normalizes separators without touching the meaning of a leading "../": that is
// relative to the .elf directory and collapsing it early would lose probe 2.
std::string NormalizeSeparators(std::string_view path)
{
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        char n = (c == '\\') ? '/' : c;
        // Collapse repeated separators, but keep a leading "//" (UNC).
        if (n == '/' && !out.empty() && out.back() == '/' && out.size() > 1)
            continue;
        out.push_back(n);
    }
    return out;
}

std::vector<std::string> SplitComponents(std::string_view path)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string_view piece = (slash == std::string_view::npos)
                                     ? path.substr(start)
                                     : path.substr(start, slash - start);
        if (!piece.empty() && piece != ".")
            parts.emplace_back(piece);
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return parts;
}

std::string JoinSuffix(const std::vector<std::string>& parts, size_t count)
{
    std::string out;
    for (size_t i = parts.size() - count; i < parts.size(); ++i) {
        if (!out.empty())
            out.push_back('/');
        out += parts[i];
    }
    return out;
}

bool IsExistingFile(const std::string& path)
{
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(fs::path(path), ec);
}

void AppendUnique(std::vector<std::string>& out, std::string candidate)
{
    if (candidate.empty())
        return;
    if (std::find(out.begin(), out.end(), candidate) == out.end())
        out.push_back(std::move(candidate));
}

std::string Combine(const std::string& dir, const std::string& rel)
{
    if (dir.empty())
        return rel;
    std::error_code ec;
    fs::path p = fs::path(dir) / fs::path(rel);
    fs::path normalized = p.lexically_normal();
    return NormalizeSeparators(normalized.string());
}

// Advances one UTF-8 sequence starting at `i`. Returns the sequence length, or 0 when
// the bytes at `i` are not a well-formed sequence.
int Utf8SequenceLength(std::string_view s, size_t i)
{
    unsigned char c = static_cast<unsigned char>(s[i]);
    int want = 0;
    if (c < 0x80)
        return 1;
    else if ((c & 0xE0) == 0xC0)
        want = 2;
    else if ((c & 0xF0) == 0xE0)
        want = 3;
    else if ((c & 0xF8) == 0xF0)
        want = 4;
    else
        return 0;

    if (i + want > s.size())
        return 0;
    for (int k = 1; k < want; ++k)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
            return 0;
    return want;
}

} // namespace

bool SourceFileCache::IsPseudoPath(std::string_view dwarfPath)
{
    if (dwarfPath.empty())
        return true;
    const size_t slash = dwarfPath.find_last_of("/\\");
    const std::string_view leaf =
        (slash == std::string_view::npos) ? dwarfPath : dwarfPath.substr(slash + 1);
    return leaf.size() >= 2 && leaf.front() == '<' && leaf.back() == '>';
}

std::string_view SourceFile::Line(uint32_t oneBased) const
{
    if (oneBased == 0 || oneBased > lines.size())
        return {};
    return lines[oneBased - 1];
}

void SourceFileCache::SetElfPath(std::string elfPath)
{
    std::string normalized = NormalizeSeparators(elfPath);
    if (normalized == m_elfPath)
        return;
    m_elfPath = std::move(normalized);
    m_elfDir = m_elfPath.empty() ? std::string{}
                                 : NormalizeSeparators(fs::path(m_elfPath).parent_path().string());
    Clear();
}

void SourceFileCache::SetSearchRoots(std::vector<std::string> roots)
{
    std::vector<std::string> normalized;
    normalized.reserve(roots.size());
    for (auto& r : roots)
        normalized.push_back(NormalizeSeparators(r));
    if (normalized == m_searchRoots)
        return;
    m_searchRoots = std::move(normalized);
    RetryFailed();
}

void SourceFileCache::SetTabWidth(int width)
{
    width = std::clamp(width, 1, 16);
    if (width == m_tabWidth)
        return;
    m_tabWidth = width;
    Clear();
}

void SourceFileCache::SetMaxFileBytes(uint64_t bytes)
{
    if (bytes == m_maxFileBytes)
        return;
    m_maxFileBytes = bytes;
    Clear();
}

std::vector<std::string> SourceFileCache::ProbeCandidates(const std::string& dwarfPath) const
{
    std::vector<std::string> out;
    if (dwarfPath.empty())
        return out;

    const std::string norm = NormalizeSeparators(dwarfPath);

    // 1. The recorded path verbatim — the build happened on this machine.
    AppendUnique(out, norm);

    // 2. Relative to the .elf directory: "../src/main.c" and "src/main.c" are the two
    //    shapes the bartman toolchain emits most.
    if (!m_elfDir.empty())
        AppendUnique(out, Combine(m_elfDir, norm));

    const std::vector<std::string> parts = SplitComponents(norm);
    if (parts.empty())
        return out;
    const size_t maxSuffix =
        std::min<size_t>(parts.size(), static_cast<size_t>(kMaxSuffixComponents));

    // 3. Suffix walk-up from the .elf directory, suffix-major: a longer, more specific
    //    trailing match always beats a shallower accidental basename hit. This is what
    //    DebugLines::PathScore encodes, applied to the filesystem rather than to the
    //    line table.
    if (!m_elfDir.empty()) {
        for (size_t k = maxSuffix; k >= 1; --k) {
            const std::string suffix = JoinSuffix(parts, k);
            fs::path dir(m_elfDir);
            for (int level = 0; level <= kMaxWalkUpLevels; ++level) {
                AppendUnique(out, Combine(NormalizeSeparators(dir.string()), suffix));
                fs::path parent = dir.parent_path();
                if (parent.empty() || parent == dir)
                    break;
                dir = parent;
            }
            if (k == 1)
                break;
        }
    }

    // 4. User search roots. No ancestor walk — the user named that directory deliberately.
    for (const auto& root : m_searchRoots) {
        if (root.empty())
            continue;
        for (size_t k = maxSuffix; k >= 1; --k) {
            AppendUnique(out, Combine(root, JoinSuffix(parts, k)));
            if (k == 1)
                break;
        }
    }

    return out;
}

std::optional<std::string> SourceFileCache::ResolvePath(const std::string& dwarfPath) const
{
    for (const auto& candidate : ProbeCandidates(dwarfPath))
        if (IsExistingFile(candidate))
            return candidate;
    return std::nullopt;
}

std::vector<std::string> SourceFileCache::SplitText(std::string_view raw) const
{
    std::vector<std::string> lines;
    if (raw.empty())
        return lines; // an empty file has no lines, not one empty one

    // Classic-Mac line endings: without this the whole file arrives as one enormous line
    // and immediately hits the length cap.
    const bool crOnly =
        raw.find('\n') == std::string_view::npos && raw.find('\r') != std::string_view::npos;
    const char sep = crOnly ? '\r' : '\n';

    size_t start = 0;
    while (start <= raw.size()) {
        size_t end = raw.find(sep, start);
        std::string_view piece =
            (end == std::string_view::npos) ? raw.substr(start) : raw.substr(start, end - start);

        // Strip the CR of a CRLF pair.
        if (!crOnly && !piece.empty() && piece.back() == '\r')
            piece.remove_suffix(1);

        std::string outLine;
        outLine.reserve(piece.size());
        int column = 0;
        for (size_t i = 0; i < piece.size();) {
            if (column >= kMaxLineLength)
                break;
            const char c = piece[i];
            if (c == '\t') {
                const int spaces = m_tabWidth - (column % m_tabWidth);
                outLine.append(static_cast<size_t>(spaces), ' ');
                column += spaces;
                ++i;
                continue;
            }
            unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F) {
                // Any other control character becomes a space so it cannot disturb layout.
                outLine.push_back(' ');
                ++column;
                ++i;
                continue;
            }
            if (u < 0x80) {
                outLine.push_back(c);
                ++column;
                ++i;
                continue;
            }
            // Multi-byte: keep well-formed UTF-8 (comments in source are legitimately
            // non-ASCII), replace only invalid sequences.
            const int len = Utf8SequenceLength(piece, i);
            if (len == 0) {
                outLine.push_back('?');
                ++column;
                ++i;
            } else {
                outLine.append(piece.substr(i, len));
                ++column;
                i += static_cast<size_t>(len);
            }
        }
        if (column >= kMaxLineLength)
            outLine.append("...");
        lines.push_back(std::move(outLine));

        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }

    // A file ending in a newline must not produce a phantom trailing empty line.
    if (!lines.empty() && lines.back().empty() && !raw.empty() && raw.back() == sep)
        lines.pop_back();

    return lines;
}

SourceFile SourceFileCache::Load(const std::string& dwarfPath) const
{
    SourceFile file;
    file.dwarfPath = dwarfPath;

    if (IsPseudoPath(dwarfPath)) {
        file.status = SourceFileStatus::Pseudo;
        return file;
    }

    auto resolved = ResolvePath(dwarfPath);
    if (!resolved) {
        file.status = SourceFileStatus::NotFound;
        return file;
    }
    file.resolvedPath = *resolved;

    std::error_code ec;
    const uintmax_t size = fs::file_size(fs::path(*resolved), ec);
    if (ec) {
        file.status = SourceFileStatus::ReadFailed;
        return file;
    }
    file.sizeBytes = static_cast<uint64_t>(size);
    if (file.sizeBytes > m_maxFileBytes) {
        file.status = SourceFileStatus::TooLarge;
        return file;
    }

    std::ifstream in(*resolved, std::ios::binary);
    if (!in) {
        file.status = SourceFileStatus::ReadFailed;
        return file;
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // A UTF-8 BOM would otherwise show up as stray glyphs on line 1.
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF)
        raw.erase(0, 3);

    const size_t sniff = std::min<size_t>(raw.size(), 4096);
    if (raw.find('\0', 0) != std::string::npos && raw.find('\0', 0) < sniff) {
        file.status = SourceFileStatus::Binary;
        return file;
    }

    file.lines = SplitText(raw);
    file.status = SourceFileStatus::Ok;
    return file;
}

const SourceFile& SourceFileCache::Get(const std::string& dwarfPath)
{
    auto it = m_files.find(dwarfPath);
    if (it != m_files.end())
        return it->second;

    SourceFile loaded = Load(dwarfPath);
    loaded.version = m_nextVersion++;
    return m_files.emplace(dwarfPath, std::move(loaded)).first->second;
}

std::optional<std::string_view> SourceFileCache::GetLine(const std::string& dwarfPath,
                                                         uint32_t line)
{
    const SourceFile& file = Get(dwarfPath);
    if (!file.Ok())
        return std::nullopt;
    std::string_view text = file.Line(line);
    if (line == 0 || line > file.LineCount())
        return std::nullopt;
    return text;
}

void SourceFileCache::Clear() { m_files.clear(); }

void SourceFileCache::Forget(const std::string& dwarfPath) { m_files.erase(dwarfPath); }

void SourceFileCache::RetryFailed()
{
    for (auto it = m_files.begin(); it != m_files.end();) {
        if (it->second.Ok())
            ++it;
        else
            it = m_files.erase(it);
    }
}

SourceFileCache::Stats SourceFileCache::GetStats() const
{
    Stats s;
    for (const auto& [path, file] : m_files) {
        if (file.status == SourceFileStatus::Pseudo)
            continue; // not a real file; counting it would make the ratio a lie
        ++s.entries;
        if (!file.Ok())
            continue;
        ++s.resolved;
        for (const auto& line : file.lines)
            s.bytes += line.size();
    }
    return s;
}

} // namespace adm::core
