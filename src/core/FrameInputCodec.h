#pragma once

#include <cstdint>
#include <vector>

#include "ByteStream.h"
#include "FrameInput.h"

namespace adm::core {

// One frame's input, as bytes. This is the encoding FrameHistory has always written inside
// its timeline entries; it was factored out here when scenario files needed to write the
// same records standalone. The byte layout is therefore fixed by an existing on-disk format
// and must not change without a version bump on BOTH readers.
//
//   joypad[0]      uint16
//   joypad[1]      uint16
//   mouseDeltaX    int16 (as uint16)
//   mouseDeltaY    int16 (as uint16)
//   mouseButtons   uint8
//   keysDown       uint32 count, then count x uint16
//   keyEvents      uint32 count, then count x { uint16 key, uint8 down }
void WriteFrameInput(std::vector<uint8_t>& out, const FrameInput& in);

// Reads one record. Returns false on a truncated or implausible stream, having consumed an
// unspecified amount of it — callers treat a false as fatal to the whole stream. The count
// fields are checked against what is actually left before they are trusted, so a corrupt
// length cannot make this reserve gigabytes.
bool ReadFrameInput(ByteReader& r, FrameInput& out);

} // namespace adm::core
