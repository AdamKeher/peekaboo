#include "DebuggerTrainerView.h"
#include "../TableLayout.h"

#include <cstdio>
#include <cstring>

#include <fmt/format.h>
#include <imgui.h>
#include <nfd.h>

#include "../IconsFontAwesome6.h"

namespace adm::ui {

using core::AnchorKind;
using core::LockEntry;
using core::LockMode;
using core::ResolveState;
using core::TrainerDoc;
using core::TrainerFile;
using core::TrainerMatch;

namespace {

ImVec4 StateColour(ResolveState s)
{
    switch (s) {
        case ResolveState::Resolved:
            return ImVec4(0.45f, 0.90f, 0.45f, 1.0f);
        case ResolveState::VerifyMismatch:
            return ImVec4(1.00f, 0.75f, 0.30f, 1.0f);
        case ResolveState::Ambiguous:
            return ImVec4(1.00f, 0.85f, 0.40f, 1.0f);
        case ResolveState::NotFound:
            return ImVec4(0.95f, 0.45f, 0.40f, 1.0f);
        case ResolveState::Invalid:
            return ImVec4(0.95f, 0.45f, 0.40f, 1.0f);
        case ResolveState::Unresolved:
            return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

const char* StateGlyph(ResolveState s)
{
    switch (s) {
        case ResolveState::Resolved:
            return ICON_FA_CHECK;
        case ResolveState::VerifyMismatch:
            return "!";
        case ResolveState::Ambiguous:
            return "?";
        case ResolveState::NotFound:
            return ICON_FA_XMARK;
        case ResolveState::Invalid:
            return ICON_FA_XMARK;
        case ResolveState::Unresolved:
            return "-";
    }
    return "";
}

// Hex by default, matching the console's number rules; a leading '!' means decimal.
uint32_t ParseNumberField(const char* text, bool& ok)
{
    ok = false;
    if (!text || !*text)
        return 0;
    try {
        const bool decimal = text[0] == '!';
        const uint32_t v = static_cast<uint32_t>(
            std::stoul(decimal ? text + 1 : text, nullptr, decimal ? 10 : 16));
        ok = true;
        return v;
    } catch (...) {
        return 0;
    }
}

} // namespace

bool DebuggerTrainerView::LoadTrainerFile(const std::string& path, TrainerMatch match,
                                          std::string& err)
{
    if (!m_locks)
        return false;
    auto doc = TrainerFile::Load(path, err);
    if (!doc)
        return false;

    // Nothing arrives enabled. The whole point of the anchors is that a file authored against
    // one release has to prove itself against this one first.
    for (auto& e : doc->entries) {
        e.enabled = false;
        e.applied = false;
        e.id.clear();
    }
    m_locks->ReplaceAll(std::move(doc->entries));
    if (m_makeContext) {
        auto ctx = m_makeContext();
        if (ctx.mem)
            m_locks->ResolveAll(ctx);
    }
    SetLoadedTrainer(path, match);
    return true;
}

void DebuggerTrainerView::Draw()
{
    if (!m_open)
        return;

    ImGui::SetNextWindowSize(ImVec2(860, 640), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (m_noFocus)
        flags |= ImGuiWindowFlags_NoFocusOnAppearing;

    if (ImGui::Begin(ICON_FA_LOCK " Trainer###Trainer##debugger", &m_open, flags)) {
        if (!m_locks) {
            ImGui::TextDisabled("Lock store unavailable.");
        } else {
            DrawToolbar();

            // A core without the protection hooks will happily accept locks and enforce none
            // of them; say so once, loudly, rather than letting the user debug a ghost. Only
            // when we have actually asked a loaded core, though - warning before anything has
            // booted accuses a perfectly good build of being broken.
            if (m_supported && m_supported() == ProtectionSupport::Unsupported) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.30f, 1.0f));
                ImGui::TextWrapped(
                    "This PuAE core has no protection hooks, so Block and Set locks will have "
                    "no effect. Rebuild it with scripts/build_puae. (Patches still work — they "
                    "are written directly to memory.)");
                ImGui::PopStyleColor();
                ImGui::Separator();
            }

            if (m_trainerMatch == TrainerMatch::TitleGroup) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.40f, 1.0f));
                ImGui::TextWrapped("Matched by TOSEC title group, not an exact MD5 — this "
                                   "trainer was authored for a different release. Check each "
                                   "entry resolves before trusting it.");
                ImGui::PopStyleColor();
                ImGui::Separator();
            }

            const float avail = ImGui::GetContentRegionAvail().y;

            // Height of the detail panel, derived from the text metrics rather than a fixed
            // pixel count. A hardcoded height only holds for one font size and one exact set
            // of rows, and starts scrolling the moment either changes - which is what adding
            // the "Details" header did to it.
            //
            // Worst realistic case is nine rows: the header, the description, then Id, Mode,
            // Anchor, Verify, State, a wrapped note, and the mode-specific line. A Patch
            // entry can add a tenth ("Was"), which is rare enough to let scroll handle.
            const ImGuiStyle& style = ImGui::GetStyle();
            float detail = ImGui::GetTextLineHeightWithSpacing() * 9.0f +
                           style.WindowPadding.y * 2.0f + style.ItemSpacing.y;
            // ...but never at the table's expense: it is the primary content.
            if (detail > avail * 0.55f)
                detail = avail * 0.55f;
            if (detail > avail - 80.0f)
                detail = avail - 80.0f;
            if (detail < 60.0f)
                detail = 60.0f;

            if (ImGui::BeginChild("##locktable", ImVec2(0, avail - detail), true)) {
                DrawTable();
            }
            ImGui::EndChild();

            if (ImGui::BeginChild("##lockdetail", ImVec2(0, 0), true)) {
                DrawDetail();
            }
            ImGui::EndChild();

            DrawAddDialog();
        }
    }
    ImGui::End();
    m_noFocus = false;
}

void DebuggerTrainerView::DrawToolbar()
{
    if (ImGui::Button(ICON_FA_PLUS " Add")) {
        m_addOpen = true;
        std::memset(m_addDesc, 0, sizeof(m_addDesc));
        std::memset(m_addAddr, 0, sizeof(m_addAddr));
        std::memset(m_addValue, 0, sizeof(m_addValue));
        std::memset(m_addBytes, 0, sizeof(m_addBytes));
        std::memset(m_addPattern, 0, sizeof(m_addPattern));
        std::memset(m_addVerify, 0, sizeof(m_addVerify));
    }
    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open...")) {
        nfdchar_t* outPath = nullptr;
        nfdfilteritem_t filters[2] = {{"Peekaboo trainer", "json"}, {"RetroArch cheats", "cht"}};
        const nfdresult_t r = NFD_OpenDialog(&outPath, filters, 2, nullptr);
        ImGui::GetIO().ClearInputKeys(); // the dialog blocks and can swallow key-up events
        if (r == NFD_OKAY && outPath) {
            const std::string path(outPath);
            NFD_FreePath(outPath);
            std::string err;
            const bool isCht = path.size() > 4 && path.substr(path.size() - 4) == ".cht";
            if (isCht) {
                int skipped = 0;
                auto doc = TrainerFile::ImportCht(path, err, &skipped);
                if (doc) {
                    for (auto& e : doc->entries)
                        e.id.clear();
                    const size_t n = doc->entries.size();
                    m_locks->ReplaceAll(std::move(doc->entries));
                    if (m_makeContext) {
                        auto ctx = m_makeContext();
                        if (ctx.mem)
                            m_locks->ResolveAll(ctx);
                    }
                    SetStatus(skipped ? fmt::format("Imported {} cheat(s); {} skipped (no Amiga "
                                                    "equivalent).",
                                                    n, skipped)
                                      : fmt::format("Imported {} cheat(s).", n));
                    SetLoadedTrainer(path, TrainerMatch::None);
                } else {
                    SetStatus(err, true);
                }
            } else if (LoadTrainerFile(path, TrainerMatch::None, err)) {
                SetStatus(
                    fmt::format("Loaded {} — all entries disabled; enable what verifies.", path));
            } else {
                SetStatus(err, true);
            }
        }
    }
    ImGui::SameLine();

    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save As...")) {
        nfdchar_t* outPath = nullptr;
        nfdfilteritem_t filters[1] = {{"Peekaboo trainer", "json"}};
        if (NFD_SaveDialog(&outPath, filters, 1, nullptr, "trainer.json") == NFD_OKAY && outPath) {
            const std::string path(outPath);
            NFD_FreePath(outPath);
            TrainerDoc doc;
            doc.entries = m_locks->All();
            std::string err;
            if (TrainerFile::Save(path, doc, err))
                SetStatus(fmt::format("Saved {} entr{} to {}.", doc.entries.size(),
                                      doc.entries.size() == 1 ? "y" : "ies", path));
            else
                SetStatus(err, true);
        }
        ImGui::GetIO().ClearInputKeys();
    }
    ImGui::SameLine();

    const bool haveCtx = m_makeContext != nullptr;
    ImGui::BeginDisabled(!haveCtx);
    if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT " Rescan")) {
        auto ctx = m_makeContext();
        if (ctx.mem) {
            m_locks->ResolveAll(ctx);
            size_t ok = 0;
            for (const auto& e : m_locks->All()) {
                if (e.state == ResolveState::Resolved)
                    ++ok;
            }
            SetStatus(fmt::format("Re-resolved {} entr{}: {} found.", m_locks->Size(),
                                  m_locks->Size() == 1 ? "y" : "ies", ok));
        } else {
            SetStatus("No memory — is the emulator running?", true);
        }
    }
    ImGui::EndDisabled();
    if (!haveCtx && ImGui::IsItemHovered())
        ImGui::SetTooltip("Anchors resolve against the running machine.");

    ImGui::SameLine();
    ImGui::BeginDisabled(m_locks->Empty());
    if (ImGui::Button(ICON_FA_TRASH " Clear")) {
        // Revert applied patches first, or their bytes stay in guest memory with nothing
        // left that remembers the originals.
        if (m_makeContext) {
            auto ctx = m_makeContext();
            for (const auto& e : m_locks->All()) {
                if (e.mode == LockMode::Patch && e.applied) {
                    std::string err;
                    m_locks->SetEnabled(e.id, false, ctx, err);
                }
            }
        }
        m_locks->Clear();
        m_selected.clear();
        m_trainerPath.clear();
        m_trainerMatch = TrainerMatch::None;
        SetStatus("All entries removed.");
    }
    ImGui::EndDisabled();

    if (!m_status.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(m_statusIsError ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f)
                                           : ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
                           "%s", m_status.c_str());
    }

    if (!m_trainerPath.empty()) {
        ImGui::TextDisabled("%s  (%s)", m_trainerPath.c_str(), core::ToString(m_trainerMatch));
    }
    ImGui::Separator();
}

void DebuggerTrainerView::DrawTable()
{
    if (m_locks->Empty()) {
        ImGui::TextDisabled("No locks.");
        return;
    }

    constexpr ImGuiTableFlags kFlags = kAdjustableColumns | ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                                       ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("##locks", 8, kFlags))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30.0f);
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 78.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 74.0f);
    ImGui::TableSetupColumn("Anchor", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide,
                            24.0f);
    ImGui::TableHeadersRow();

    std::string toRemove;
    for (const auto& e : m_locks->All()) {
        ImGui::TableNextRow();
        ImGui::PushID(e.id.c_str());

        ImGui::TableSetColumnIndex(0);
        bool on = e.enabled;
        if (ImGui::Checkbox("##on", &on)) {
            if (!m_makeContext) {
                SetStatus("No memory — is the emulator running?", true);
            } else {
                auto ctx = m_makeContext();
                std::string err;
                if (!m_locks->SetEnabled(e.id, on, ctx, err))
                    SetStatus(err, true);
                else
                    SetStatus(on ? fmt::format("{} enabled.", e.desc)
                                 : fmt::format("{} disabled.", e.desc));
            }
        }

        ImGui::TableSetColumnIndex(1);
        const bool selected = m_selected == e.id;
        if (ImGui::Selectable(e.desc.empty() ? "(no description)" : e.desc.c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowOverlap)) {
            m_selected = e.id;
        }

        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(e.group.c_str());

        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(core::ToString(e.mode));

        ImGui::TableSetColumnIndex(4);
        if (e.resolved)
            ImGui::Text("$%08X", *e.resolved);
        else
            ImGui::TextDisabled("-");

        ImGui::TableSetColumnIndex(5);
        switch (e.mode) {
            case LockMode::Block:
                ImGui::Text("%u byte%s", e.len, e.len == 1 ? "" : "s");
                break;
            case LockMode::Set:
                ImGui::Text("$%0*X", e.size * 2, e.value);
                break;
            case LockMode::Patch:
                ImGui::Text("%zu byte%s", e.bytes.size(), e.bytes.size() == 1 ? "" : "s");
                break;
        }

        ImGui::TableSetColumnIndex(6);
        ImGui::PushStyleColor(ImGuiCol_Text, StateColour(e.state));
        ImGui::Text("%s %s", StateGlyph(e.state), core::ToString(e.state));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !e.note.empty())
            ImGui::SetTooltip("%s", e.note.c_str());

        ImGui::TableSetColumnIndex(7);
        if (ImGui::SmallButton(ICON_FA_XMARK))
            toRemove = e.id;

        ImGui::PopID();
    }
    ImGui::EndTable();

    if (!toRemove.empty()) {
        if (m_makeContext) {
            auto ctx = m_makeContext();
            std::string err;
            m_locks->SetEnabled(toRemove, false, ctx, err); // reverts a patch if applied
        }
        m_locks->Remove(toRemove);
        if (m_selected == toRemove)
            m_selected.clear();
    }
}

void DebuggerTrainerView::DrawDetail()
{
    ImGui::SeparatorText("Details");

    const LockEntry* e = m_selected.empty() ? nullptr : m_locks->Find(m_selected);
    if (!e)
        return;

    ImGui::TextUnformatted(e->desc.c_str());
    ImGui::Spacing();

    ImGui::Text("Id       %s", e->id.c_str());
    ImGui::Text("Mode     %s", core::ToString(e->mode));
    ImGui::Text("Anchor   %s", e->anchor.Describe(e->resolved).c_str());
    if (!e->verify.Empty())
        ImGui::Text("Verify   %s", e->verify.ToString().c_str());

    ImGui::PushStyleColor(ImGuiCol_Text, StateColour(e->state));
    ImGui::Text("State    %s", core::ToString(e->state));
    ImGui::PopStyleColor();
    if (!e->note.empty())
        ImGui::TextWrapped("         %s", e->note.c_str());

    switch (e->mode) {
        case LockMode::Block:
            ImGui::Text("Blocks   %u byte%s of guest writes", e->len, e->len == 1 ? "" : "s");
            break;
        case LockMode::Set:
            ImGui::Text("Forces   $%0*X (%u-bit)", e->size * 2, e->value, e->size * 8);
            break;
        case LockMode::Patch: {
            std::string hex;
            for (size_t i = 0; i < e->bytes.size(); ++i)
                hex += fmt::format("{}{:02X}", i ? " " : "", e->bytes[i]);
            ImGui::Text("Writes   %s", hex.c_str());
            if (!e->original.empty() && e->applied) {
                std::string orig;
                for (size_t i = 0; i < e->original.size(); ++i)
                    orig += fmt::format("{}{:02X}", i ? " " : "", e->original[i]);
                ImGui::Text("Was      %s", orig.c_str());
            }
            break;
        }
    }
}

void DebuggerTrainerView::DrawAddDialog()
{
    if (!m_addOpen)
        return;

    ImGui::OpenPopup("Add lock##trainer");
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Add lock##trainer", &m_addOpen,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::InputText("Description", m_addDesc, sizeof(m_addDesc));
    ImGui::Combo("Mode", &m_addMode, "Block writes\0Force value\0Patch bytes\0");
    ImGui::Combo("Anchor", &m_addAnchor, "Absolute\0Seglist\0Signature\0Operand\0");
    ImGui::Separator();

    switch (m_addAnchor) {
        case 0:
            ImGui::InputText("Address (hex)", m_addAddr, sizeof(m_addAddr),
                             ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::TextDisabled("Fixed address. Fine for a game that takes over the machine;");
            ImGui::TextDisabled("breaks on a different release or memory configuration.");
            break;
        case 1:
            ImGui::InputInt("Segment", &m_addSeg);
            ImGui::InputInt("Offset", &m_addSegOffset);
            ImGui::TextDisabled("Survives relocation — use for AmigaDOS-loaded programs.");
            break;
        case 2:
            ImGui::InputText("Pattern", m_addPattern, sizeof(m_addPattern));
            ImGui::InputInt("Target offset", &m_addTargetOffset);
            ImGui::TextDisabled("e.g. 4EB9 ?? ?? ?? ?? 6600   (?? = any byte)");
            break;
        case 3:
            ImGui::InputText("Pattern", m_addPattern, sizeof(m_addPattern));
            ImGui::InputInt("Operand at", &m_addOperandAt);
            ImGui::InputInt("Operand size", &m_addOperandSize);
            ImGui::TextDisabled("Matches the instruction that writes the variable, then takes");
            ImGui::TextDisabled("the variable's address from its operand. e.g. 5339 ?? ?? ?? ??");
            break;
        default:
            break;
    }

    ImGui::Separator();
    if (m_addMode == 0) {
        ImGui::InputInt("Length (bytes)", &m_addLen);
    } else if (m_addMode == 1) {
        ImGui::Combo("Width", &m_addSizeIdx,
                     "8-bit\0"
                     "16-bit\0"
                     "32-bit\0");
        ImGui::InputText("Value (hex)", m_addValue, sizeof(m_addValue),
                         ImGuiInputTextFlags_CharsHexadecimal);
    } else {
        ImGui::InputText("Bytes (hex)", m_addBytes, sizeof(m_addBytes),
                         ImGuiInputTextFlags_CharsHexadecimal);
    }
    ImGui::InputText("Verify (hex, optional)", m_addVerify, sizeof(m_addVerify));
    ImGui::TextDisabled("Guard bytes expected at the anchor. Without them nothing checks that");
    ImGui::TextDisabled("this address still means what it did when the trainer was written.");

    ImGui::Separator();
    if (ImGui::Button("Add", ImVec2(120, 0))) {
        LockEntry e;
        e.desc = m_addDesc[0] ? m_addDesc : "unnamed lock";
        e.mode =
            m_addMode == 0 ? LockMode::Block : (m_addMode == 1 ? LockMode::Set : LockMode::Patch);
        std::string err;
        bool ok = true;

        switch (m_addAnchor) {
            case 0: {
                bool parsed = false;
                e.anchor.kind = AnchorKind::Absolute;
                e.anchor.addr = ParseNumberField(m_addAddr, parsed);
                if (!parsed) {
                    err = "address is not a hex number";
                    ok = false;
                }
                break;
            }
            case 1:
                e.anchor.kind = AnchorKind::Seglist;
                e.anchor.seg = m_addSeg;
                e.anchor.offset = static_cast<uint32_t>(m_addSegOffset);
                break;
            case 2:
            case 3: {
                e.anchor.kind = m_addAnchor == 2 ? AnchorKind::Signature : AnchorKind::Operand;
                auto p = core::BytePattern::Parse(m_addPattern, err);
                if (!p) {
                    ok = false;
                } else {
                    e.anchor.pattern = *p;
                    e.anchor.targetOffset = m_addTargetOffset;
                    e.anchor.operandAt = m_addOperandAt;
                    e.anchor.operandSize = m_addOperandSize;
                }
                break;
            }
            default:
                break;
        }

        if (ok && m_addVerify[0]) {
            auto v = core::BytePattern::Parse(m_addVerify, err);
            if (!v)
                ok = false;
            else
                e.verify = *v;
        }

        if (ok) {
            if (e.mode == LockMode::Block) {
                e.len = m_addLen > 0 ? static_cast<uint32_t>(m_addLen) : 1;
            } else if (e.mode == LockMode::Set) {
                e.size = m_addSizeIdx == 0 ? 1 : (m_addSizeIdx == 1 ? 2 : 4);
                e.len = e.size;
                bool parsed = false;
                e.value = ParseNumberField(m_addValue, parsed);
                if (!parsed) {
                    err = "value is not a hex number";
                    ok = false;
                }
            } else {
                auto parsed = core::MemoryLocks::ParseHexBytes(m_addBytes, err);
                if (!parsed || parsed->empty()) {
                    if (err.empty())
                        err = "patch has no bytes";
                    ok = false;
                } else {
                    e.bytes = *parsed;
                }
            }
        }

        if (!ok) {
            SetStatus(err, true);
        } else {
            const std::string id = m_locks->Add(e);
            m_selected = id;
            if (m_makeContext) {
                auto ctx = m_makeContext();
                if (ctx.mem)
                    m_locks->ResolveAll(ctx);
            }
            SetStatus(fmt::format("Added {} — tick it to enable.", id));
            m_addOpen = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
        m_addOpen = false;

    if (!m_addOpen)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace adm::ui
