#include "ArColorPalette.h"

#include "ColorManager.h"

namespace adm::ui {

namespace {

// Reads an Asm.* colour from ColorManager, falling back to a built-in value when
// the JSON key is absent so highlighting still works without colors_syntax.json.
ImU32 SyntaxColor(const char* key, ImU32 fallback)
{
    const auto& cm = ColorManager::Instance();
    return cm.HasColor(key) ? cm.GetColor(key) : fallback;
}

ImU32 Rgb(float r, float g, float b)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

} // namespace

ImU32 ResolveArColor(core::ArColor color)
{
    const ImU32 text = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_Text));

    switch (color) {
        case core::ArColor::Address:
            return Rgb(0.55f, 0.75f, 1.00f);
        case core::ArColor::HexData:
            return text;
        case core::ArColor::Ascii:
            return Rgb(0.65f, 0.85f, 0.65f);
        case core::ArColor::Mnemonic:
            return Rgb(0.95f, 0.85f, 0.55f);
        case core::ArColor::Error:
            return Rgb(1.00f, 0.45f, 0.45f);
        case core::ArColor::Info:
            return Rgb(0.60f, 0.80f, 0.95f);
        case core::ArColor::Highlight:
            return Rgb(1.00f, 0.70f, 0.30f);
        case core::ArColor::Prompt:
            return Rgb(0.80f, 0.70f, 1.00f);
        case core::ArColor::Register:
            return Rgb(0.55f, 0.95f, 0.70f);

        // 68k syntax-highlighting token colours (customisable via colors_syntax.json).
        case core::ArColor::AsmOpcode:
            return SyntaxColor("Asm.Opcode", Rgb(0.95f, 0.85f, 0.55f));
        case core::ArColor::AsmSize:
            return SyntaxColor("Asm.Size", Rgb(0.56f, 0.56f, 0.56f));
        case core::ArColor::AsmRegister:
            return SyntaxColor("Asm.Register", Rgb(0.55f, 0.95f, 0.70f));
        case core::ArColor::AsmImmediate:
            return SyntaxColor("Asm.Immediate", Rgb(1.00f, 0.69f, 0.38f));
        case core::ArColor::AsmAddress:
            return SyntaxColor("Asm.Address", Rgb(0.55f, 0.75f, 1.00f));
        case core::ArColor::AsmNumber:
            return SyntaxColor("Asm.Number", Rgb(0.50f, 0.82f, 0.82f));
        case core::ArColor::AsmPunct:
            return SyntaxColor("Asm.Punctuation", Rgb(0.69f, 0.69f, 0.69f));
        case core::ArColor::Comment:
            return SyntaxColor("Asm.Comment", Rgb(0.50f, 0.50f, 0.50f));
        case core::ArColor::Label:
            return SyntaxColor("Asm.Label", Rgb(0.95f, 0.85f, 0.45f));

        // C/C++ source token colours. Deliberately distinct from their asm counterparts
        // (Src.Comment green vs Asm.Comment grey) so an interleaved listing reads as two
        // languages rather than one muddled one, and pitched slightly below the
        // disassembly in weight — the machine code is still the primary content.
        case core::ArColor::SrcKeyword:
            return SyntaxColor("Src.Keyword", Rgb(0.773f, 0.525f, 0.753f));
        case core::ArColor::SrcType:
            return SyntaxColor("Src.Type", Rgb(0.306f, 0.788f, 0.690f));
        case core::ArColor::SrcPreproc:
            return SyntaxColor("Src.Preproc", Rgb(0.608f, 0.608f, 0.416f));
        case core::ArColor::SrcString:
            return SyntaxColor("Src.String", Rgb(0.808f, 0.569f, 0.471f));
        case core::ArColor::SrcChar:
            return SyntaxColor("Src.Char", Rgb(0.839f, 0.616f, 0.522f));
        case core::ArColor::SrcNumber:
            return SyntaxColor("Src.Number", Rgb(0.710f, 0.808f, 0.659f));
        case core::ArColor::SrcComment:
            return SyntaxColor("Src.Comment", Rgb(0.416f, 0.600f, 0.333f));
        case core::ArColor::SrcIdentifier:
            return SyntaxColor("Src.Identifier", Rgb(0.812f, 0.831f, 0.847f));
        case core::ArColor::SrcFunction:
            return SyntaxColor("Src.Function", Rgb(0.863f, 0.863f, 0.667f));
        case core::ArColor::SrcOperator:
            return SyntaxColor("Src.Operator", Rgb(0.690f, 0.690f, 0.690f));
        case core::ArColor::SrcPunct:
            return SyntaxColor("Src.Punctuation", Rgb(0.565f, 0.565f, 0.565f));
        case core::ArColor::SrcGutter:
            return SyntaxColor("Src.Gutter", Rgb(0.353f, 0.392f, 0.439f));
        case core::ArColor::SrcHeader:
            return SyntaxColor("Src.Header", Rgb(0.498f, 0.655f, 0.816f));
        case core::ArColor::SrcMissing:
            return SyntaxColor("Src.Missing", Rgb(0.627f, 0.376f, 0.376f));

        case core::ArColor::Swatch: // RGB comes from ArSpan::payload; drawn as a filled rect
        case core::ArColor::Default:
        default:
            return text;
    }
}

} // namespace adm::ui
