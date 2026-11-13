#include "ZipUtils.h"

#include "Constants.h"

#include <zip.h>
#include <algorithm>
#include <filesystem>
#include <iterator>

namespace adm {

namespace {

// Members the decode pipeline (Application::ConvertToAdfBytes) can turn into a
// mountable image, in preference order. ".adf" leads so ZipPayload::AdfOnly is just
// the first entry of this list.
const char* const kImageExts[] = {".adf", ".ipf", ".dms", ".scp", ".exe"};

std::string LowerExtension(const std::string& entryName)
{
    std::string ext = std::filesystem::path(entryName).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext;
}

// Locate the member to extract. Returns its index, or -1 with outError set.
zip_int64_t FindEntry(zip_t* archive, ZipPayload want, std::string& outEntryName,
                      std::string& outError)
{
    const size_t extCount = (want == ZipPayload::AdfOnly) ? 1 : std::size(kImageExts);
    const zip_int64_t numEntries = zip_get_num_entries(archive, 0);

    zip_int64_t bestIndex = -1;
    size_t bestRank = extCount; // position in kImageExts; lower wins

    // Tracked for the single-member fallback below.
    zip_int64_t onlyFile = -1;
    int fileCount = 0;

    for (zip_int64_t i = 0; i < numEntries; ++i) {
        const char* name = zip_get_name(archive, i, 0);
        if (!name || name[0] == '\0')
            continue;

        std::string entryName(name);
        if (entryName.back() == '/') // directory entry
            continue;

        ++fileCount;
        onlyFile = i;

        const std::string ext = LowerExtension(entryName);
        for (size_t rank = 0; rank < bestRank; ++rank) {
            if (ext == kImageExts[rank]) {
                bestRank = rank;
                bestIndex = i;
                outEntryName = entryName;
                break;
            }
        }
    }

    if (bestIndex >= 0)
        return bestIndex;

    // A TOSEC archive holds exactly one file, and its extension is not always one we
    // list (".rom", or none at all). Take it and let the caller's content detection
    // decide, rather than refusing on the name alone.
    if (want == ZipPayload::AnyDiskImage && fileCount == 1) {
        outEntryName = zip_get_name(archive, onlyFile, 0);
        return onlyFile;
    }

    outError = (want == ZipPayload::AdfOnly)
                   ? "No ADF file found in ZIP archive"
                   : "No Amiga disk image or executable found in ZIP archive";
    return -1;
}

// Stat + read a single archive entry into outData, with full error and size-bomb checking.
// Closes the archive in all paths. Returns true on success.
bool ReadEntryAndClose(zip_t* archive, zip_int64_t index, const std::string& entryName,
                       std::vector<uint8_t>& outData, std::string& outError)
{
    zip_stat_t sb;
    zip_stat_init(&sb);
    if (zip_stat_index(archive, index, 0, &sb) != 0 || !(sb.valid & ZIP_STAT_SIZE)) {
        outError = "Failed to stat ZIP entry: " + entryName;
        zip_close(archive);
        return false;
    }

    if (sb.size > core::constants::disk::MaxZipAdfSize) {
        outError = "ZIP entry '" + entryName + "' is too large (" + std::to_string(sb.size) +
                   " bytes) — refusing to extract";
        zip_close(archive);
        return false;
    }

    zip_file_t* zf = zip_fopen_index(archive, index, 0);
    if (!zf) {
        outError = "Failed to open entry in ZIP: " + entryName;
        zip_close(archive);
        return false;
    }

    outData.resize(static_cast<size_t>(sb.size));
    zip_int64_t bytesRead = zip_fread(zf, outData.data(), sb.size);
    zip_fclose(zf);
    zip_close(archive);

    if (bytesRead != static_cast<zip_int64_t>(sb.size)) {
        outError = "Failed to read full entry from ZIP";
        return false;
    }

    return true;
}

} // namespace

bool ExtractFromZip(const std::string& zipPath, ZipPayload want, std::vector<uint8_t>& outData,
                    std::string& outEntryName, std::string& outError)
{
    int err = 0;
    zip_t* archive = zip_open(zipPath.c_str(), 0, &err);
    if (!archive) {
        zip_error_t error;
        zip_error_init_with_code(&error, err);
        outError = std::string("Failed to open ZIP archive: ") + zip_error_strerror(&error);
        zip_error_fini(&error);
        return false;
    }

    zip_int64_t index = FindEntry(archive, want, outEntryName, outError);
    if (index < 0) {
        zip_close(archive);
        return false;
    }

    return ReadEntryAndClose(archive, index, outEntryName, outData, outError);
}

bool ExtractFromZipBuffer(const std::vector<uint8_t>& zipData, ZipPayload want,
                          std::vector<uint8_t>& outData, std::string& outEntryName,
                          std::string& outError)
{
    zip_error_t ze;
    zip_error_init(&ze);

    zip_source_t* src = zip_source_buffer_create(zipData.data(), zipData.size(), 0, &ze);
    if (!src) {
        outError = std::string("Failed to create ZIP source: ") + zip_error_strerror(&ze);
        zip_error_fini(&ze);
        return false;
    }

    zip_t* archive = zip_open_from_source(src, ZIP_RDONLY, &ze);
    if (!archive) {
        outError = std::string("Failed to open ZIP from buffer: ") + zip_error_strerror(&ze);
        zip_error_fini(&ze);
        zip_source_free(src);
        return false;
    }
    zip_error_fini(&ze);

    zip_int64_t index = FindEntry(archive, want, outEntryName, outError);
    if (index < 0) {
        zip_close(archive);
        return false;
    }

    return ReadEntryAndClose(archive, index, outEntryName, outData, outError);
}

} // namespace adm
