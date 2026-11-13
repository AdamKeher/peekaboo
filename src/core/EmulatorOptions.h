#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace adm::core {

/// One selectable value of a PuAE core option: the string the core is handed, and the label the
/// Emulator Preferences dialog shows for it.
struct EmulatorOptionValue
{
    const char* value = nullptr;
    const char* label = nullptr;
};

/// A PuAE core option Peekaboo manages on the user's behalf.
///
/// One table, two readers. AmigaEmulator answers the core's GET_VARIABLE out of `defaultValue`,
/// and PreferencesView builds its combo boxes out of `values`. They used to be separate
/// transcriptions of libretro-core.c's option_defs_us[], which is how the dialog ended up
/// offering puae_immediate_blits="disabled" (the core only parses "false") and showing a
/// puae_floppy_write_redirect default the emulator never applied. Keeping both in one table is
/// what makes "what the dialog offers" and "what the core is told" the same thing, and it is
/// what tests/EmulatorOptionsTest.cpp checks against the shipped core.
struct EmulatorOptionInfo
{
    const char* key = nullptr;
    /// What the core is told while the user has not chosen anything. Empty when Peekaboo has no
    /// opinion of its own and the core's declared default should stand.
    const char* defaultValue = "";
    /// nullptr for an option with no fixed set of values (a free numeric such as the CPU
    /// throttle). Never empty otherwise.
    const EmulatorOptionValue* values = nullptr;
    std::size_t valueCount = 0;

    bool HasDefault() const { return defaultValue != nullptr && defaultValue[0] != '\0'; }
};

/// Every option the Emulator Preferences dialog can write.
///
/// puae_kickstart is deliberately absent: the core builds its value list at runtime from the
/// ROMs it finds in the system directory, so the dialog builds that combo from its own scan.
const std::vector<EmulatorOptionInfo>& GetEmulatorOptions();

/// nullptr when `key` is not an option Peekaboo manages.
const EmulatorOptionInfo* FindEmulatorOption(std::string_view key);

/// Peekaboo's default for `key`, or "" when it has none (unknown key included).
std::string_view EmulatorOptionDefault(std::string_view key);

/// True when `value` is one the dialog may select for `key`. An option with no fixed value list
/// accepts anything; an unknown key accepts nothing.
bool IsEmulatorOptionValue(std::string_view key, std::string_view value);

} // namespace adm::core
