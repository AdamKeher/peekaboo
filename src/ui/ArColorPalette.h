#pragma once

#include "core/ArConsoleTypes.h"

#include <imgui.h>

namespace adm::ui {

// Resolves a console semantic colour (ArColor) to a concrete ImU32. The 68k
// disassembly token colours (ArColor::Asm*) are read from ColorManager
// (definitions/colors_syntax.json) with built-in fallbacks; the remaining
// colours follow the AR console's theme-derived palette. Shared by the AR
// console terminal and the disassembly window so the two never diverge.
ImU32 ResolveArColor(core::ArColor color);

} // namespace adm::ui
