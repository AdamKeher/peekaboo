#include "TableLayout.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <imgui_internal.h>

#include "../core/Logger.h"

namespace adm::ui {

namespace {

constexpr const char* kSectionPrefix = "[Table]";

/// Walk @p ini a line at a time, handing each line to @p sink along with whether it belongs to a
/// [Table] section. Sections run from their header to the next line that opens any section.
template <typename Sink> void ForEachLine(std::string_view ini, Sink sink)
{
    bool inTable = false;
    size_t pos = 0;
    // `pos < size`, not `<=`: a text ending in '\n' has no line after it. Walking one past would
    // emit a phantom empty line, which both invents a line for empty input and makes the walk
    // non-idempotent - extracting an already-extracted text would grow a blank line each pass.
    while (pos < ini.size()) {
        size_t eol = ini.find('\n', pos);
        if (eol == std::string_view::npos)
            eol = ini.size();
        std::string_view line = ini.substr(pos, eol - pos);
        if (!line.empty() && line.front() == '[')
            inTable = line.rfind(kSectionPrefix, 0) == 0;
        sink(line, inTable);
        pos = eol + 1;
    }
}

} // namespace

ImGuiID StableTableId(const char* key) { return ImHashStr(key); }

std::string TableLayoutStore::ExtractTables(std::string_view ini)
{
    std::string out;
    ForEachLine(ini, [&out](std::string_view line, bool inTable) {
        if (!inTable)
            return;
        out.append(line);
        out.push_back('\n');
    });
    return out;
}

std::string TableLayoutStore::StripTables(std::string_view ini)
{
    std::string out;
    ForEachLine(ini, [&out](std::string_view line, bool inTable) {
        if (inTable)
            return;
        out.append(line);
        out.push_back('\n');
    });
    return out;
}

void TableLayoutStore::Load(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        adm::core::Logger::Warn("Table layout: cannot read {}", path.string());
        return;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    // Keep only what we recognise. The file is ours, but it is plain text next to the other
    // config files and a hand-edited stray [Window] section must not reach the ImGui loader.
    m_ini = ExtractTables(buf.str());
}

void TableLayoutStore::Save(const std::filesystem::path& path) const
{
    // Nothing captured means nothing was known this run - most likely a launch that failed before
    // the first frame. Writing then would replace a good store with an empty one.
    if (m_ini.empty())
        return;
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        adm::core::Logger::Warn("Table layout: cannot write {}", path.string());
        return;
    }
    out << "; Peekaboo table column layout (order, width, visibility). Delete to reset.\n";
    out << m_ini;
}

bool TableLayoutStore::SwitchFile(const std::filesystem::path& path)
{
    Capture();
    Save();
    m_file = path;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return false;
    m_ini.clear();
    Load();
    return !m_ini.empty();
}

void TableLayoutStore::Capture()
{
    if (ImGui::GetCurrentContext() == nullptr)
        return;
    size_t size = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&size);
    if (!ini || size == 0)
        return;
    std::string captured = ExtractTables(std::string_view(ini, size));
    // A context that has drawn no tables yet reports none; that is not a reason to forget the
    // ones already loaded from the file.
    if (!captured.empty())
        m_ini = std::move(captured);
}

std::string TableLayoutStore::ApplyTo(std::string_view layout) const
{
    if (m_ini.empty())
        return std::string(layout);
    std::string merged = StripTables(layout);
    merged.append(m_ini);
    return merged;
}

void TableLayoutStore::ResetAll()
{
    m_ini.clear();
    std::error_code ec;
    std::filesystem::remove(m_file, ec);

    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx)
        return;
    // Drop the stored settings through ImGui's own handler so live tables have their binding
    // (SettingsOffset) cleared with them, then ask each table to rebuild from its defaults.
    if (ImGuiSettingsHandler* handler = ImGui::FindSettingsHandler("Table"))
        if (handler->ClearAllFn)
            handler->ClearAllFn(ctx, handler);
    for (int i = 0; i != ctx->Tables.GetMapSize(); i++)
        if (ImGuiTable* table = ctx->Tables.TryGetMapData(i))
            table->IsResetAllRequest = true;
}

TableLayoutStore& GlobalTableLayout()
{
    static TableLayoutStore store;
    return store;
}

} // namespace adm::ui
