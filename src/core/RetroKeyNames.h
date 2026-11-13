#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Name <-> code lookup for AmigaEmulator::RetroKey, plus an ASCII-to-keystroke map.
//
// The MCP input tools take keys as human-readable names ("F1", "Return", "Num1") rather
// than raw numbers, and type_text needs to turn a string into a sequence of key presses.
// Both live here so the table has one home.
//
// Raw Amiga (CIA) key codes are deliberately not handled: PuAE owns the RETROK -> Amiga
// raw-code translation inside the core (libretro-mapper.c), so a RETROK press already
// produces the correct raw code on the emulated keyboard serial line.

namespace adm::core {

// One typed character: the key to press plus whether Shift must be held with it.
struct KeyStroke
{
    int code = 0;
    bool shift = false;
};

// Resolve a key name to its RetroKey code. Matching is case-insensitive and ignores
// spaces/underscores/hyphens, so "page up", "PageUp" and "PAGE_UP" are the same key.
// Returns false if the name is unknown.
bool LookupRetroKey(const std::string& name, int& outCode);

// Canonical name for a RetroKey code, or nullptr if the code is not in the table.
const char* RetroKeyName(int code);

// Every key name in the table, in declaration order. Used to build tool schemas and
// error messages that tell the caller what is actually accepted.
std::vector<std::string> AllRetroKeyNames();

// Map one ASCII character to the keystroke that produces it on a US layout, which is
// what PuAE's default keyboard translation assumes. Handles '\n' (Return), '\t' (Tab)
// and '\b' (Backspace). Returns false for characters with no mapping.
bool AsciiToKeyStroke(char c, KeyStroke& out);

// Modifier-name -> RetroKey code for the modifier keys press_key can hold down
// ("shift", "ctrl", "alt", "lshift", "rctrl", "lmeta", ...). Returns false if unknown.
bool LookupModifierKey(const std::string& name, int& outCode);

} // namespace adm::core
