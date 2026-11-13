#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace adm {
namespace core {

// Fills `out` with `count` bytes from the operating system's CSPRNG. Returns false if the OS
// source failed, in which case the buffer contents are unspecified.
bool SecureRandomBytes(uint8_t* out, size_t count);

// Lowercase hex string of `byteCount` CSPRNG bytes (2 * byteCount characters). Returns an empty
// string if the OS source failed — callers treating the result as a credential must reject an
// empty string rather than fall back to a weaker generator.
std::string SecureRandomHex(size_t byteCount);

} // namespace core
} // namespace adm
