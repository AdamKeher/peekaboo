#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm {

// What a caller is willing to take out of an archive.
enum class ZipPayload {
    // Only a ".adf" member will do — for callers that hand the bytes straight to a
    // mount/insert path with no decode step in between.
    AdfOnly,
    // Any member the decode pipeline understands (ADF, IPF, DMS, SCP, HUNK
    // executable), preferring a plain ADF when an archive holds several. TOSEC wraps
    // one file per archive and it is often not an ADF: the "Demos - Various - [EXE]"
    // category ships a HUNK executable, and thousands of entries are IPF.
    AnyDiskImage,
};

// Extracts the first matching member of a ZIP file into a byte buffer.
// Returns true on success, fills outData and outEntryName.
bool ExtractFromZip(const std::string& zipPath, ZipPayload want, std::vector<uint8_t>& outData,
                    std::string& outEntryName, std::string& outError);

// Extracts the first matching member of an in-memory ZIP buffer.
// Returns true on success, fills outData and outEntryName.
bool ExtractFromZipBuffer(const std::vector<uint8_t>& zipData, ZipPayload want,
                          std::vector<uint8_t>& outData, std::string& outEntryName,
                          std::string& outError);

} // namespace adm
