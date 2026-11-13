#include "BootBlockLibrary.h"

#include <algorithm>
#include <fstream>

#include "Logger.h"

namespace fs = std::filesystem;

namespace adm::core {

std::string BootBlockLibrary::SanitizeName(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        // Strip path separators and characters that are invalid on common filesystems.
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            out += '_';
        } else if (static_cast<unsigned char>(c) < 0x20) {
            // Skip control characters.
            continue;
        } else {
            out += c;
        }
    }

    // Trim leading/trailing whitespace and dots (dots are problematic on Windows).
    auto first = out.find_first_not_of(" .");
    auto last = out.find_last_not_of(" .");
    if (first == std::string::npos)
        return "bootblock";
    return out.substr(first, last - first + 1);
}

bool BootBlockLibrary::Load()
{
    m_dir = fs::current_path() / "bootblocks";
    if (!fs::exists(m_dir)) {
        fs::path parent = fs::current_path() / ".." / "bootblocks";
        if (fs::exists(parent))
            m_dir = parent;
    }

    Refresh();
    return true;
}

void BootBlockLibrary::Refresh()
{
    m_entries.clear();

    std::error_code ec;
    if (!fs::exists(m_dir, ec) || !fs::is_directory(m_dir, ec))
        return;

    for (const auto& entry : fs::directory_iterator(m_dir, ec)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (ext != ".bin")
            continue;
        m_entries.push_back({entry.path().stem().string(), entry.path().string()});
    }

    std::sort(m_entries.begin(), m_entries.end(),
              [](const BootBlockEntry& a, const BootBlockEntry& b) { return a.name < b.name; });
}

fs::path BootBlockLibrary::EnsureDir()
{
    if (m_dir.empty())
        m_dir = fs::current_path() / "bootblocks";

    std::error_code ec;
    if (!fs::exists(m_dir, ec))
        fs::create_directories(m_dir, ec);
    return m_dir;
}

bool BootBlockLibrary::Save(const std::string& name, const std::vector<uint8_t>& sector0,
                            const std::vector<uint8_t>& sector1)
{
    std::string stem = SanitizeName(name);
    if (stem.empty()) {
        Logger::Error("BootBlockLibrary: invalid boot block name '{}'", name);
        return false;
    }

    fs::path dir = EnsureDir();
    fs::path target = dir / (stem + ".bin");

    // Assemble exactly 1024 bytes (block 0 followed by block 1), padding short sectors.
    std::vector<uint8_t> data(1024, 0);
    std::copy_n(sector0.begin(), std::min<size_t>(512, sector0.size()), data.begin());
    std::copy_n(sector1.begin(), std::min<size_t>(512, sector1.size()), data.begin() + 512);

    std::ofstream f(target, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        Logger::Error("BootBlockLibrary: could not write '{}'", target.string());
        return false;
    }
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    f.close();

    Logger::Info("BootBlockLibrary: saved '{}'", target.string());
    Refresh();
    return true;
}

bool BootBlockLibrary::Rename(const std::string& oldName, const std::string& newName)
{
    std::string newStem = SanitizeName(newName);
    if (newStem.empty())
        return false;

    fs::path src = m_dir / (SanitizeName(oldName) + ".bin");
    fs::path dst = m_dir / (newStem + ".bin");
    if (src == dst)
        return true;

    std::error_code ec;
    // Don't silently overwrite an unrelated existing entry of the same name.
    if (fs::exists(dst, ec)) {
        Logger::Error("BootBlockLibrary: rename target '{}' already exists", dst.string());
        return false;
    }
    fs::rename(src, dst, ec);
    if (ec) {
        Logger::Error("BootBlockLibrary: rename failed: {}", ec.message());
        return false;
    }

    Refresh();
    return true;
}

bool BootBlockLibrary::Delete(const std::string& name)
{
    fs::path target = m_dir / (SanitizeName(name) + ".bin");
    std::error_code ec;
    if (!fs::remove(target, ec)) {
        Logger::Error("BootBlockLibrary: delete failed for '{}'", target.string());
        return false;
    }

    Refresh();
    return true;
}

std::vector<uint8_t> BootBlockLibrary::Read(const std::string& name) const
{
    fs::path target = m_dir / (SanitizeName(name) + ".bin");
    std::ifstream f(target, std::ios::binary);
    if (!f.is_open())
        return {};

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    return data;
}

} // namespace adm::core
