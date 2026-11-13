#include "IdentifyResultPanel.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <imgui.h>
#include <stb_image.h>

// nanosvg - headers only (implementation in nanosvg static lib)
#include <nanosvg.h>
#include <nanosvgrast.h>

#include "../../core/Logger.h"

namespace adm::ui {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers (file-local)
// ─────────────────────────────────────────────────────────────────────────────

static GLuint Panel_DecodeAndUpload(TextureManager* mgr, const std::vector<uint8_t>& data,
                                    int& outW, int& outH)
{
    if (!mgr || data.empty())
        return 0;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels =
        stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &channels, 4);
    if (!pixels)
        return 0;

    GLuint tex = mgr->CreateTexture(pixels, w, h);
    stbi_image_free(pixels);

    if (tex) {
        outW = w;
        outH = h;
    }
    return tex;
}

static GLuint Panel_LoadSVGTexture(TextureManager* mgr, const char* filename, int size)
{
    if (!mgr)
        return 0;
    NSVGimage* image = nsvgParseFromFile(filename, "px", 96.0f);
    if (!image) {
        adm::core::Logger::Error("IdentifyResultPanel: failed to parse SVG: {}", filename);
        return 0;
    }

    float scale = static_cast<float>(size) / image->width;
    int w = size;
    int h = static_cast<int>(image->height * scale + 0.5f);

    std::vector<uint8_t> pixels(static_cast<size_t>(w * h * 4), 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (rast) {
        nsvgRasterize(rast, image, 0.0f, 0.0f, scale, pixels.data(), w, h, w * 4);
        nsvgDeleteRasterizer(rast);
    }
    GLuint tex = mgr->CreateTexture(pixels.data(), w, h);
    nsvgDelete(image);
    return tex;
}

static void Panel_DrawScaledImage(GLuint tex, int imgW, int imgH, float maxWidth)
{
    if (!tex || imgW <= 0 || imgH <= 0)
        return;

    float scale = 1.0f;
    if (static_cast<float>(imgW) > maxWidth) {
        scale = maxWidth / static_cast<float>(imgW);
    }

    float displayW = static_cast<float>(imgW) * scale;
    float displayH = static_cast<float>(imgH) * scale;

    ImGui::Image(TextureManager::ToImTextureID(tex), ImVec2(displayW, displayH));
}

static void Panel_DrawInfoRow(const char* label, const std::string& value)
{
    if (value.empty())
        return;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

static void Panel_DrawLinkRow(const char* label, const std::string& url)
{
    if (url.empty())
        return;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::TextLinkOpenURL("Link", url.c_str());
}

static void Panel_DrawLinkRowWithLabel(const char* label, const char* text, const char* url)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    if (url && url[0] != '\0') {
        ImGui::TextLinkOpenURL(text, url);
    } else {
        ImGui::TextUnformatted(text);
    }
}

static void Panel_DrawInfoRowWithLogo(const char* label, const std::string& value, GLuint logoTex,
                                      int logoW, int logoH)
{
    if (value.empty())
        return;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();

    if (logoTex && logoW > 0 && logoH > 0) {
        float logoMaxH = ImGui::GetTextLineHeight() * 1.5f;
        float scale = logoMaxH / static_cast<float>(logoH);
        float displayW = static_cast<float>(logoW) * scale;
        float displayH = logoMaxH;
        ImGui::Image(TextureManager::ToImTextureID(logoTex), ImVec2(displayW, displayH));
        ImGui::SameLine();
    }

    ImGui::TextWrapped("%s", value.c_str());
}

static void Panel_DrawFilledStar(ImDrawList* drawList, ImVec2 center, float outerR, float innerR,
                                 ImU32 color)
{
    constexpr float PI = 3.14159265f;
    ImVec2 points[10];
    for (int i = 0; i < 10; i++) {
        float angle = (static_cast<float>(i) * 36.0f - 90.0f) * (PI / 180.0f);
        float r = (i % 2 == 0) ? outerR : innerR;
        points[i] = ImVec2(center.x + r * cosf(angle), center.y + r * sinf(angle));
    }
    for (int i = 0; i < 10; i++) {
        drawList->AddTriangleFilled(center, points[i], points[(i + 1) % 10], color);
    }
}

static void Panel_DrawStarOutline(ImDrawList* drawList, ImVec2 center, float outerR, float innerR,
                                  ImU32 color, float thickness = 1.0f)
{
    constexpr float PI = 3.14159265f;
    ImVec2 points[10];
    for (int i = 0; i < 10; i++) {
        float angle = (static_cast<float>(i) * 36.0f - 90.0f) * (PI / 180.0f);
        float r = (i % 2 == 0) ? outerR : innerR;
        points[i] = ImVec2(center.x + r * cosf(angle), center.y + r * sinf(angle));
    }
    drawList->AddPolyline(points, 10, color, ImDrawFlags_Closed, thickness);
}

static void Panel_DrawStarRating(float ratingOutOf5)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    float starSize = ImGui::GetTextLineHeight();
    float outerR = starSize * 0.5f;
    float innerR = outerR * 0.38f;
    float spacing = starSize + 3.0f;

    ImU32 goldColor = IM_COL32(255, 193, 7, 255);
    ImU32 emptyColor = IM_COL32(80, 80, 80, 180);
    ImU32 outlineColor = IM_COL32(200, 160, 0, 255);

    for (int i = 0; i < 5; i++) {
        ImVec2 center(pos.x + outerR + static_cast<float>(i) * spacing, pos.y + outerR);
        float fill = std::clamp(ratingOutOf5 - static_cast<float>(i), 0.0f, 1.0f);

        if (fill <= 0.0f) {
            Panel_DrawFilledStar(drawList, center, outerR, innerR, emptyColor);
        } else if (fill >= 1.0f) {
            Panel_DrawFilledStar(drawList, center, outerR, innerR, goldColor);
        } else {
            Panel_DrawFilledStar(drawList, center, outerR, innerR, emptyColor);

            float leftEdge = center.x - outerR;
            float clipRight = leftEdge + fill * starSize;
            drawList->PushClipRect(ImVec2(leftEdge, pos.y), ImVec2(clipRight, pos.y + starSize),
                                   true);
            Panel_DrawFilledStar(drawList, center, outerR, innerR, goldColor);
            drawList->PopClipRect();
        }

        Panel_DrawStarOutline(drawList, center, outerR, innerR, outlineColor, 1.0f);
    }

    ImGui::Dummy(ImVec2(5.0f * spacing, starSize));
}

// ─────────────────────────────────────────────────────────────────────────────
// IdentifyResultPanel implementation
// ─────────────────────────────────────────────────────────────────────────────

IdentifyResultPanel::IdentifyResultPanel(TextureManager* textureMgr) : m_textureManager(textureMgr)
{
}

IdentifyResultPanel::~IdentifyResultPanel() { ReleaseTextures(); }

void IdentifyResultPanel::SetResult(const core::IdentifyResult& result)
{
    // If the result changes, release existing textures so they get re-decoded
    ReleaseTextures();
    m_texturesLoaded = false;
    m_result = result;
    m_hasResult = true;
}

void IdentifyResultPanel::ClearResult()
{
    ReleaseTextures();
    m_texturesLoaded = false;
    m_result = {};
    m_hasResult = false;
}

void IdentifyResultPanel::ReleaseTextures()
{
    if (m_textureManager) {
        if (m_screenshotTex) {
            m_textureManager->ReleaseTexture(m_screenshotTex);
            m_screenshotTex = 0;
        }
        if (m_boxArtTex) {
            m_textureManager->ReleaseTexture(m_boxArtTex);
            m_boxArtTex = 0;
        }
        if (m_wheelTex) {
            m_textureManager->ReleaseTexture(m_wheelTex);
            m_wheelTex = 0;
        }
        if (m_devLogoTex) {
            m_textureManager->ReleaseTexture(m_devLogoTex);
            m_devLogoTex = 0;
        }
        if (m_pubLogoTex) {
            m_textureManager->ReleaseTexture(m_pubLogoTex);
            m_pubLogoTex = 0;
        }
        if (m_rulezTex) {
            m_textureManager->ReleaseTexture(m_rulezTex);
            m_rulezTex = 0;
        }
        if (m_isokTex) {
            m_textureManager->ReleaseTexture(m_isokTex);
            m_isokTex = 0;
        }
        if (m_sucksTex) {
            m_textureManager->ReleaseTexture(m_sucksTex);
            m_sucksTex = 0;
        }
    }
    m_screenshotW = m_screenshotH = 0;
    m_boxArtW = m_boxArtH = 0;
    m_wheelW = m_wheelH = 0;
    m_devLogoW = m_devLogoH = 0;
    m_pubLogoW = m_pubLogoH = 0;
}

void IdentifyResultPanel::EnsureTexturesLoaded()
{
    if (m_texturesLoaded || !m_hasResult || !m_result.found)
        return;
    m_texturesLoaded = true;
    m_screenshotTex = Panel_DecodeAndUpload(m_textureManager, m_result.screenshotData,
                                            m_screenshotW, m_screenshotH);
    m_boxArtTex =
        Panel_DecodeAndUpload(m_textureManager, m_result.boxArtData, m_boxArtW, m_boxArtH);
    m_wheelTex = Panel_DecodeAndUpload(m_textureManager, m_result.wheelData, m_wheelW, m_wheelH);
    m_devLogoTex =
        Panel_DecodeAndUpload(m_textureManager, m_result.developerLogoData, m_devLogoW, m_devLogoH);
    m_pubLogoTex =
        Panel_DecodeAndUpload(m_textureManager, m_result.publisherLogoData, m_pubLogoW, m_pubLogoH);
}

void IdentifyResultPanel::DrawScreenScraper()
{
    if (!m_hasResult || !m_result.found) {
        ImGui::TextDisabled("No ScreenScraper metadata found.");
        return;
    }

    EnsureTexturesLoaded();

    if (m_wheelTex) {
        float avail = ImGui::GetContentRegionAvail().x;
        float maxW = std::min(avail, 300.0f);
        float scale = maxW / static_cast<float>(m_wheelW);
        if (scale > 1.0f)
            scale = 1.0f;
        float w = static_cast<float>(m_wheelW) * scale;
        float h = static_cast<float>(m_wheelH) * scale;
        float indent = (avail - w) * 0.5f;
        if (indent > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        ImGui::Image(TextureManager::ToImTextureID(m_wheelTex), ImVec2(w, h));
        ImGui::Spacing();
    }

    if (ImGui::BeginTable("##panel_ss_results", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        Panel_DrawInfoRow("Title", m_result.title);
        Panel_DrawInfoRowWithLogo("Developer", m_result.developer, m_devLogoTex, m_devLogoW,
                                  m_devLogoH);
        Panel_DrawInfoRowWithLogo("Publisher", m_result.publisher, m_pubLogoTex, m_pubLogoW,
                                  m_pubLogoH);
        Panel_DrawInfoRow("Genre", m_result.genre);
        Panel_DrawInfoRow("Release Date", m_result.releaseDate);
        Panel_DrawInfoRow("Players", m_result.players);
        Panel_DrawInfoRow("System", m_result.system);
        Panel_DrawInfoRow("Family", m_result.family);

        if (!m_result.rating.empty()) {
            int ratingVal = 0;
            try {
                ratingVal = std::stoi(m_result.rating);
            } catch (...) {
                // Non-numeric rating from the API — render as 0 stars.
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Rating");
            ImGui::TableNextColumn();

            float starsOutOf5 = static_cast<float>(ratingVal) / 4.0f;
            Panel_DrawStarRating(starsOutOf5);
            ImGui::SameLine();
            ImGui::Text("(%s/20)", m_result.rating.c_str());
        }

        ImGui::EndTable();
    }

    if (!m_result.synopsis.empty()) {
        ImGui::SeparatorText("Description");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("%s", m_result.synopsis.c_str());
        ImGui::PopTextWrapPos();
    }

    if (m_screenshotTex || m_boxArtTex) {
        float contentWidth = ImGui::GetContentRegionAvail().x;

        if (m_screenshotTex && m_boxArtTex) {
            float ssWidth = contentWidth * 0.58f;
            float boxWidth = contentWidth * 0.35f;

            ImGui::BeginGroup();
            ImGui::SeparatorText("Screenshot");
            Panel_DrawScaledImage(m_screenshotTex, m_screenshotW, m_screenshotH, ssWidth);
            ImGui::EndGroup();

            ImGui::SameLine();

            ImGui::BeginGroup();
            ImGui::SeparatorText("Box Art");
            Panel_DrawScaledImage(m_boxArtTex, m_boxArtW, m_boxArtH, boxWidth);
            ImGui::EndGroup();
        } else if (m_screenshotTex) {
            ImGui::SeparatorText("Screenshot");
            Panel_DrawScaledImage(m_screenshotTex, m_screenshotW, m_screenshotH,
                                  contentWidth * 0.9f);
        } else if (m_boxArtTex) {
            ImGui::SeparatorText("Box Art");
            Panel_DrawScaledImage(m_boxArtTex, m_boxArtW, m_boxArtH, contentWidth * 0.5f);
        }
    }
}

void IdentifyResultPanel::DrawPouet()
{
    if (!m_hasResult || !m_result.hasPouetInfo) {
        ImGui::TextDisabled("No Pouet metadata found.");
        return;
    }

    // Load Pouet vote icons on demand
    if (!m_rulezTex) {
        m_rulezTex = Panel_LoadSVGTexture(m_textureManager, "assets/pouet/rulez.svg", 21);
        m_isokTex = Panel_LoadSVGTexture(m_textureManager, "assets/pouet/isok.svg", 21);
        m_sucksTex = Panel_LoadSVGTexture(m_textureManager, "assets/pouet/sucks.svg", 21);
    }

    if (ImGui::BeginTable("##panel_pouet_table", 2,
                          ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        Panel_DrawInfoRow("Prod Name", m_result.pouetName);

        if (!m_result.pouetGroupId.empty()) {
            std::string groupUrl =
                "https://www.pouet.net/groups.php?which=" + m_result.pouetGroupId;
            Panel_DrawLinkRowWithLabel("Group(s)", m_result.pouetGroups.c_str(), groupUrl.c_str());
        } else {
            Panel_DrawInfoRow("Group(s)", m_result.pouetGroups);
        }

        Panel_DrawInfoRow("Type(s)", m_result.pouetTypes);
        Panel_DrawInfoRow("Release Date", m_result.pouetDate);

        if (!m_result.pouetVoteAvg.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Votes");
            ImGui::TableNextColumn();

            auto DrawVote = [&](GLuint tex, const std::string& count) {
                if (tex) {
                    float iconSize = 21.0f;
                    float textHeight = ImGui::GetTextLineHeight();
                    float offsetY = (iconSize - textHeight) * 0.5f;

                    ImVec2 pos = ImGui::GetCursorPos();
                    ImGui::SetCursorPosY(pos.y - offsetY);
                    ImGui::Image(TextureManager::ToImTextureID(tex), ImVec2(iconSize, iconSize));
                    ImGui::SetCursorPos(ImVec2(pos.x + iconSize + 4.0f, pos.y));
                }
                ImGui::TextUnformatted(count.empty() ? "0" : count.c_str());
                ImGui::SameLine(0, 12);
            };

            DrawVote(m_rulezTex, m_result.pouetVoteUp);
            DrawVote(m_isokTex, m_result.pouetVotePig);
            DrawVote(m_sucksTex, m_result.pouetVoteDown);

            ImGui::Text("Avg: %s", m_result.pouetVoteAvg.c_str());
        }

        if (!m_result.pouetPartyName.empty()) {
            std::string partyStr = m_result.pouetPartyName;
            if (!m_result.pouetPartyYear.empty()) {
                partyStr += ", " + m_result.pouetPartyYear;
            }
            Panel_DrawInfoRow("Party", partyStr);

            if (!m_result.pouetPartyCompo.empty()) {
                Panel_DrawInfoRow("Competition", m_result.pouetPartyCompo);
            }
            if (!m_result.pouetPartyPlace.empty()) {
                std::string placeStr = m_result.pouetPartyPlace;
                if (placeStr == "1")
                    placeStr += "st";
                else if (placeStr == "2")
                    placeStr += "nd";
                else if (placeStr == "3")
                    placeStr += "rd";
                else
                    placeStr += "th";
                Panel_DrawInfoRow("Place", placeStr);
            }
        }

        Panel_DrawLinkRow("Pouet Link", m_result.pouetDownloadUrl);

        ImGui::EndTable();
    }
}

} // namespace adm::ui
