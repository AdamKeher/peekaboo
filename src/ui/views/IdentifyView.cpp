#include "IdentifyView.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include <imgui.h>

#include "../../core/Logger.h"
#include "../../core/TosecManager.h" // For filename parsing
#include "../IconsFontAwesome6.h"
#include "IdentifyResultPanel.h"

namespace adm::ui {

IdentifyView::IdentifyView(TextureManager* textureMgr)
    : m_textureManager(textureMgr), m_panel(std::make_unique<IdentifyResultPanel>(textureMgr))
{
}

IdentifyView::~IdentifyView() = default;

void IdentifyView::Open() { m_isOpen = true; }

void IdentifyView::SetDocument(const std::string& filePath, core::DiskIdentifier* identifier)
{
    bool fileChanged = (filePath != m_currentFilePath);
    m_currentFilePath = filePath;
    m_identifier = identifier;

    // Reset the shared panel when switching to a different document.
    if (fileChanged) {
        m_panel->ClearResult();
        m_panelSig.clear();
    }
}

void IdentifyView::Clear()
{
    m_identifier = nullptr;
    m_currentFilePath.clear();
    m_panel->ClearResult();
    m_panelSig.clear();
}

void IdentifyView::Close() { m_isOpen = false; }

// ─────────────────────────────────────────────────────────────────────────────
// Small table helpers used by the TOSEC / filename / links sections.
// (The ScreenScraper and Pouet sections are rendered by IdentifyResultPanel.)
// ─────────────────────────────────────────────────────────────────────────────

static void DrawInfoRow(const char* label, const std::string& value)
{
    if (value.empty())
        return;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

static void DrawLinkRow(const char* label, const std::string& url)
{
    if (url.empty())
        return;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextLinkOpenURL("Link", url.c_str());
}

static std::string JoinVector(const std::vector<std::string>& vec, const std::string& sep = ", ")
{
    std::string res;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i > 0)
            res += sep;
        res += vec[i];
    }
    return res;
}

static std::string UrlEncode(const std::string& str)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(str.size() * 2);
    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else if (c == ' ') {
            result += '+';
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 0xF];
        }
    }
    return result;
}

// Draw a coloured badge-style label (no table row needed)
static void DrawBadge(const char* text, ImVec4 color)
{
    ImGui::TextColored(color, "%s", text);
    ImGui::SameLine(0, 6);
}

// ─────────────────────────────────────────────────────────────────────────────
// Panel delegation
// ─────────────────────────────────────────────────────────────────────────────

std::string IdentifyView::ResultSignature(const core::IdentifyResult& r)
{
    // Cheap signature covering everything the panel renders: identity, the found/
    // pouet flags, and the image-data sizes (so freshly downloaded art triggers a
    // texture reload).
    std::string sig = r.md5;
    sig += '|';
    sig += r.found ? '1' : '0';
    sig += r.hasPouetInfo ? '1' : '0';
    sig += '|';
    sig += r.title;
    sig += '|';
    sig += std::to_string(r.screenshotData.size());
    sig += ',';
    sig += std::to_string(r.boxArtData.size());
    sig += ',';
    sig += std::to_string(r.wheelData.size());
    sig += ',';
    sig += std::to_string(r.developerLogoData.size());
    sig += ',';
    sig += std::to_string(r.publisherLogoData.size());
    return sig;
}

void IdentifyView::UpdatePanel(const core::IdentifyResult& result)
{
    std::string sig = ResultSignature(result);
    if (sig != m_panelSig) {
        m_panelSig = sig;
        m_panel->SetResult(result);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TOSEC section
// ─────────────────────────────────────────────────────────────────────────────

void IdentifyView::DrawTosecIdentification(const core::IdentifyResult& result)
{
    ImGui::SeparatorText("TOSEC Identification");

    if (ImGui::BeginTable("tosec_table", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        // ── MD5 ──────────────────────────────────────────────────────────────
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("MD5 Hash");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(result.md5.empty() ? "Not computed" : result.md5.c_str());

        // ── Database status ───────────────────────────────────────────────────
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Status");
        ImGui::TableNextColumn();
        if (!core::TosecManager::Instance().IsLoaded()) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), "Loading Database...");
        } else if (result.md5.empty()) {
            // Nothing has been hashed yet - saying "Unknown / Not in Database" here would read
            // as a verdict on the disk rather than as "no lookup has happened".
            ImGui::TextUnformatted("Not identified");
        } else if (result.isTosecKnown) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Known Dump");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Unknown / Not in Database");
        }

        if (result.isTosecKnown) {
            // ── Identity ─────────────────────────────────────────────────────
            DrawInfoRow("TOSEC Name", result.tosecRomName);
            DrawInfoRow("Title", result.tosecTitle);
            DrawInfoRow("Version", result.tosecVersion);
            DrawInfoRow("Demo", result.tosecDemo);
            DrawInfoRow("Year", result.tosecYear);
            DrawInfoRow("Publisher", result.tosecPublisher);
            DrawInfoRow("Release Group", result.tosecReleaseGroup);
            DrawInfoRow("Category", result.tosecCategory);
            DrawInfoRow("Language", result.tosecLanguage);
            DrawInfoRow("System", result.tosecSystem);
            DrawInfoRow("Video", result.tosecVideo);
            DrawInfoRow("Region", result.tosecRegion);
            DrawInfoRow("Media", JoinVector(result.tosecMedia));

            // ── Dump integrity ────────────────────────────────────────────────
            {
                bool anyIntegrity = result.tosecVerified || result.tosecAlternateDump ||
                                    result.tosecBadDump || result.tosecOverdump ||
                                    result.tosecVirusInfected || result.tosecFixed;
                if (anyIntegrity) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted("Dump Status");
                    ImGui::TableNextColumn();

                    if (result.tosecVerified)
                        DrawBadge("[!] Verified", ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                    if (result.tosecAlternateDump)
                        DrawBadge("[a] Alternate", ImVec4(0.8f, 0.8f, 0.4f, 1.0f));
                    if (result.tosecBadDump)
                        DrawBadge("[b] Bad Dump", ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    if (result.tosecOverdump)
                        DrawBadge("[o] Overdump", ImVec4(0.8f, 0.6f, 0.4f, 1.0f));
                    if (result.tosecVirusInfected) {
                        std::string virusLabel =
                            result.tosecVirusName.empty()
                                ? "[v] Virus"
                                : ("[v] Virus (" + result.tosecVirusName + ")");
                        DrawBadge(virusLabel.c_str(), ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                    }
                    if (result.tosecFixed)
                        DrawBadge("[f] Fixed", ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
                    ImGui::NewLine();
                }
            }

            // ── Scene modifications ───────────────────────────────────────────
            if (result.tosecCracked) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "[cr] Cracked");
                ImGui::TableNextColumn();
                if (!result.tosecCrackGroup.empty())
                    ImGui::TextUnformatted(result.tosecCrackGroup.c_str());
            }
            if (result.tosecTrained) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(0.8f, 0.5f, 1.0f, 1.0f), "[t] Trained");
                ImGui::TableNextColumn();
                {
                    std::string detail;
                    if (result.tosecTrainerCount > 0)
                        detail += "+" + std::to_string(result.tosecTrainerCount);
                    if (!result.tosecTrainerLanguage.empty()) {
                        if (!detail.empty())
                            detail += " ";
                        detail += "(" + result.tosecTrainerLanguage + ")";
                    }
                    if (!result.tosecTrainerGroup.empty()) {
                        if (!detail.empty())
                            detail += " ";
                        detail += "by " + result.tosecTrainerGroup;
                    }
                    if (!detail.empty())
                        ImGui::TextUnformatted(detail.c_str());
                }
            }
            if (result.tosecHacked) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.6f, 1.0f), "[h] Hacked");
                ImGui::TableNextColumn();
                {
                    std::string detail;
                    if (!result.tosecHackType.empty())
                        detail += result.tosecHackType;
                    if (!result.tosecHackGroup.empty()) {
                        if (!detail.empty())
                            detail += " ";
                        detail += "by " + result.tosecHackGroup;
                    }
                    if (!detail.empty())
                        ImGui::TextUnformatted(detail.c_str());
                }
            }
            if (result.tosecModified) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "[m] Modified");
                ImGui::TableNextColumn();
                {
                    std::string detail;
                    if (!result.tosecModType.empty())
                        detail += result.tosecModType;
                    if (!result.tosecModGroup.empty()) {
                        if (!detail.empty())
                            detail += " ";
                        detail += "by " + result.tosecModGroup;
                    }
                    if (!detail.empty())
                        ImGui::TextUnformatted(detail.c_str());
                }
            }

            // ── Download link ─────────────────────────────────────────────────
            DrawLinkRow("TOSEC Download", result.tosecDownloadUrl);

            // ── Filename match ────────────────────────────────────────────────
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Filename Match");
            ImGui::TableNextColumn();
            if (result.effectiveFilename == result.tosecRomName) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Match");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Renamed");
                if (!result.effectiveFilename.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", result.effectiveFilename.c_str());
                }
            }
        }

        ImGui::EndTable();

        // ── Find in TOSEC Browser ─────────────────────────────────────────
        if (m_onFindInTosec) {
            bool tosecLoaded = core::TosecManager::Instance().IsLoaded();
            // Nothing to look up until the disk has been hashed.
            ImGui::BeginDisabled(!tosecLoaded || result.md5.empty());
            if (ImGui::Button("Find in TOSEC Browser"))
                m_onFindInTosec(result.md5);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!tosecLoaded)
                    ImGui::SetTooltip("TOSEC database is loading...");
                else if (result.md5.empty())
                    ImGui::SetTooltip("Press Identify first - this searches by the disk's MD5.");
            }
        }
    }
}

void IdentifyView::DrawIdentifyButton(bool busy)
{
    ImGui::BeginDisabled(!m_documentLoaded || busy);
    if (ImGui::Button("Identify")) {
        if (m_onIdentify) {
            m_onIdentify();
        }
    }
    ImGui::EndDisabled();
    // A disabled item is not "hovered" by default, so the tooltip explaining why needs the
    // AllowWhenDisabled flag - without it the greyed button says nothing at all.
    if (!m_documentLoaded && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Load a disk image to identify it.");
    }
}

void IdentifyView::DrawSpinnerState(const std::string& status)
{
    const char* spinChars = "|/-\\";
    int frame = static_cast<int>(ImGui::GetTime() * 8.0f) % 4;
    ImGui::Text("%c %s", spinChars[frame], status.c_str());
}

void IdentifyView::DrawContent(const core::IdentifyResult& result, core::IdentifyState state,
                               const std::string& status)
{
    UpdatePanel(result);

    bool isSearchingSS = (state == core::IdentifyState::SearchingByHash ||
                          state == core::IdentifyState::SearchingByName);
    bool isSearchingPouet = (state == core::IdentifyState::SearchingPouet);
    bool dbLoaded = core::TosecManager::Instance().IsLoaded();
    bool isAnySearching =
        (isSearchingSS || isSearchingPouet || state == core::IdentifyState::Hashing);
    // Null before this document has ever been identified - the per-source buttons below drive
    // it directly, so they stay disabled until Identify has created it.
    bool haveIdentifier = (m_identifier != nullptr);

    std::string txtSsSearch = "Search ScreenScraper";
    std::string txtSsRefresh = "Refresh ScreenScraper";
    std::string txtPouetSearch = "Search Pouet";
    std::string txtPouetRefresh = "Refresh Pouet Info";

    // Calculate common width for buttons
    float maxBtnWidth = 0.0f;
    maxBtnWidth = std::max(maxBtnWidth, ImGui::CalcTextSize(txtSsSearch.c_str()).x);
    maxBtnWidth = std::max(maxBtnWidth, ImGui::CalcTextSize(txtSsRefresh.c_str()).x);
    maxBtnWidth = std::max(maxBtnWidth, ImGui::CalcTextSize(txtPouetSearch.c_str()).x);
    maxBtnWidth = std::max(maxBtnWidth, ImGui::CalcTextSize(txtPouetRefresh.c_str()).x);
    maxBtnWidth += ImGui::GetStyle().FramePadding.x * 2.0f;

    if (state == core::IdentifyState::Error)
        DrawErrorBanner(result);

    // 0. Whole-disk identify: hashes the disk, matches it against TOSEC, then searches
    // ScreenScraper. The per-source buttons further down re-run only their own section.
    DrawIdentifyButton(isAnySearching);
    ImGui::Spacing();

    // 1. TOSEC Section
    DrawTosecIdentification(result);
    if (state == core::IdentifyState::Hashing) {
        DrawSpinnerState(status);
    }

    // 2. ScreenScraper Section
    ImGui::SeparatorText("ScreenScraper (Games Database)");
    {
        // While a search runs, show the spinner; otherwise let the shared panel render
        // the data (or its own "No ScreenScraper metadata found." placeholder).
        if (isSearchingSS)
            DrawSpinnerState(status);
        else
            m_panel->DrawScreenScraper();

        ImGui::Spacing();
        const char* ssBtnText = result.found ? txtSsRefresh.c_str() : txtSsSearch.c_str();
        ImGui::BeginDisabled(!haveIdentifier || !dbLoaded || isAnySearching);
        if (ImGui::Button(ssBtnText, ImVec2(maxBtnWidth, 0))) {
            m_identifier->StartIdentify(m_currentFilePath);
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
    }

    // 3. Pouet Section
    ImGui::SeparatorText("Pouet Info (Demo Scene)");
    {
        // Existing data takes priority over the spinner (so a refresh keeps showing it).
        if (!result.hasPouetInfo && isSearchingPouet)
            DrawSpinnerState(status);
        else
            m_panel->DrawPouet();

        ImGui::Spacing();
        const char* pouetBtnText =
            result.hasPouetInfo ? txtPouetRefresh.c_str() : txtPouetSearch.c_str();
        ImGui::BeginDisabled(!haveIdentifier || !dbLoaded || isAnySearching);
        if (ImGui::Button(pouetBtnText, ImVec2(maxBtnWidth, 0))) {
            std::string filename = std::filesystem::path(m_currentFilePath).filename().string();
            auto fileInfo = core::TosecManager::ParseFilename(filename);
            std::string searchTitle = result.isTosecKnown ? result.tosecTitle : fileInfo.title;
            if (searchTitle.empty())
                searchTitle = filename;
            m_identifier->StartPouetIdentify(m_currentFilePath, searchTitle);
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
    }

    // 4. Filename Analysis
    DrawFilenameAnalysis();

    // 5. External Links
    if (!result.gameId.empty() || !result.title.empty()) {
        ImGui::SeparatorText("External Links");

        if (ImGui::BeginTable("identify_links", 2,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Site", ImGuiTableColumnFlags_WidthFixed, 220.0f);
            ImGui::TableSetupColumn("Link", ImGuiTableColumnFlags_WidthStretch);

            if (!result.gameId.empty()) {
                std::string ssUrl =
                    "https://www.screenscraper.fr/gameinfos.php?gameid=" + result.gameId;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("ScreenScraper");
                ImGui::TableNextColumn();
                ImGui::TextLinkOpenURL(ssUrl.c_str(), ssUrl.c_str());
            }

            if (!result.title.empty()) {
                std::string encoded = UrlEncode(result.title);

                std::string holUrl = "https://amiga.abime.net/games/list/?gamename=" + encoded;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Hall of Light");
                ImGui::TableNextColumn();
                ImGui::TextLinkOpenURL(holUrl.c_str(), holUrl.c_str());

                std::string lemonUrl =
                    "https://www.lemonamiga.com/games/list.php?list_title=" + encoded;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Lemon Amiga");
                ImGui::TableNextColumn();
                ImGui::TextLinkOpenURL(lemonUrl.c_str(), lemonUrl.c_str());
            }

            ImGui::EndTable();
        }
    }
}

// A failed identify used to replace the whole window with this text, leaving no way
// to retry and nothing to look at. It is a banner above the normal content instead:
// every control stays where it was, and Identify is live again the moment the run
// ends, so the retry is the same button that started it.
void IdentifyView::DrawErrorBanner(const core::IdentifyResult& result)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.12f, 0.12f, 0.55f));
    ImGui::BeginChild("##identify_error", ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s",
                       result.errorMessage.empty() ? "Identification failed"
                                                   : result.errorMessage.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void IdentifyView::DrawFilenameAnalysis()
{
    ImGui::SeparatorText("Filename Analysis");

    std::string filename = std::filesystem::path(m_currentFilePath).filename().string();
    auto fileInfo = core::TosecManager::ParseFilename(filename);

    if (ImGui::BeginTable("filename_table", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        DrawInfoRow("Filename", filename);
        if (fileInfo.valid) {
            DrawInfoRow("Detected Title", fileInfo.title);
            DrawInfoRow("Detected Year", fileInfo.year);
            DrawInfoRow("Detected Publisher", fileInfo.publisher);

            if (!fileInfo.flags.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Flags / Tags");
                ImGui::TableNextColumn();
                for (const auto& flag : fileInfo.flags) {
                    ImGui::TextUnformatted(flag.c_str());
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }
        } else {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Analysis");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Non-standard naming");
        }

        ImGui::EndTable();
    }
}

void IdentifyView::Draw()
{
    if (!m_isOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(600, 700), ImGuiCond_FirstUseEver);

    // The "###" tail is the window's stable identity - saved dock layouts still name it by the
    // old title, so only the visible half was renamed.
    if (!ImGui::Begin(ICON_FA_FINGERPRINT " Disk Identifier###Identify Disk", &m_isOpen)) {
        ImGui::End();
        return;
    }

    // Before this document has ever been identified there is no identifier to read, so the
    // window draws its normal sections from an empty result with everything but Identify
    // disabled - the user sees what the window offers instead of a wall of explanation.
    if (!m_identifier) {
        DrawContent(core::IdentifyResult{}, core::IdentifyState::Idle, {});
        ImGui::End();
        return;
    }

    // Snapshot all identifier state once per frame for consistent rendering
    auto result = m_identifier->GetResult();
    auto state = m_identifier->GetState();
    auto status = m_identifier->GetStatusMessage();

    DrawContent(result, state, status);

    ImGui::End();
}

} // namespace adm::ui
