#pragma once

#include <cstdint>
#include <string>

namespace adm::core {

// Puts a still image on the system clipboard.
//
// `rgb` is a tightly packed, top-row-first RGB8 buffer of `width` * `height` * 3 bytes - the shape
// AmigaEmulator hands back from its display texture. Windows writes a CF_DIB directly; the other
// platforms have no in-process clipboard API worth linking, so the image is written to a temporary
// PNG and handed to the platform's clipboard tool (osascript on macOS, xclip on Linux).
//
// Returns false and fills `outError` with something worth showing the user on failure.
bool CopyImageToClipboard(const uint8_t* rgb, int width, int height, std::string& outError);

} // namespace adm::core
