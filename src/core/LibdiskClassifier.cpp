#include "LibdiskClassifier.h"

#include "Logger.h"

#include <cstdint>
#include <cstring>
#include <filesystem>

#ifdef ADM_HAVE_LIBDISK
// We deliberately do NOT include <libdisk/disk.h>: it declares a struct member
// named `typename`, which is a C++ keyword (extern "C" changes linkage, not
// parsing). The classifier only calls libdisk functions and never touches its
// struct fields, so we declare the minimal prototypes ourselves. `enum
// track_type` is int-compatible and C linkage does no parameter-type checking,
// so passing the type as int is ABI-identical.
extern "C" {
struct disk;
struct stream;
struct disk* disk_create(const char* name, unsigned int flags);
void disk_close(struct disk*);
struct stream* stream_soft_open(uint8_t* data, uint16_t* speed, uint32_t bitlen,
                                unsigned int data_rpm);
void stream_close(struct stream*);
void stream_reset(struct stream*);
int track_write_raw_from_stream(struct disk*, unsigned int tracknr, int type, struct stream*);
const char* disk_get_format_id_name(int type);
const char* disk_get_format_desc_name(int type);
}
#define ADM_DISKFL_rpm(rpm) ((rpm) << 2) // DISKFL_rpm() from libdisk/disk.h
#endif

namespace adm::core {

// libdisk disks created via the ADF container have a fixed 160-track geometry
// (80 cylinders x 2 heads); track_write_raw_from_stream indexes that directly.
namespace {
constexpr uint32_t kLibdiskTracks = 160;
constexpr unsigned kAmigaDataRpm = 300; // DD: ~2us cells over one 300rpm revolution
} // namespace

LibdiskClassifier::LibdiskClassifier()
{
#ifdef ADM_HAVE_LIBDISK
    // libdisk picks the container by filename suffix; an .adf target gives us a
    // 160-track in-memory disk to write probe results into (we discard them).
    auto tmp = std::filesystem::temp_directory_path() / "adm_libdisk_probe.adf";
    m_tempPath = tmp.string();
    struct disk* d = disk_create(m_tempPath.c_str(), ADM_DISKFL_rpm(kAmigaDataRpm));
    if (!d) {
        Logger::Warn("libdisk: could not create probe disk; protection ID disabled");
        m_tempPath.clear();
        return;
    }
    m_disk = d;
#endif
}

LibdiskClassifier::~LibdiskClassifier()
{
#ifdef ADM_HAVE_LIBDISK
    if (m_disk)
        disk_close(static_cast<struct disk*>(m_disk));
#endif
    if (!m_tempPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(m_tempPath, ec);
    }
}

std::string LibdiskClassifier::Classify(const uint8_t* mfm, size_t lenBytes, uint32_t trackNumber)
{
#ifdef ADM_HAVE_LIBDISK
    if (!m_disk || !mfm || lenBytes == 0 || trackNumber >= kLibdiskTracks)
        return {};

    // The soft stream turns our MFM bitcells into flux timing for libdisk.
    struct stream* s = stream_soft_open(const_cast<uint8_t*>(mfm), nullptr,
                                        static_cast<uint32_t>(lenBytes * 8), kAmigaDataRpm);
    if (!s)
        return {};

    std::string result;
    // Probe every handler in enum order; first non-raw handler that decodes the
    // track wins (this mirrors disk-analyse's probe_stream()).
    for (int j = 0;; ++j) {
        const char* id = disk_get_format_id_name(j);
        if (!id)
            break;
        if (std::strncmp(id, "raw_", 4) == 0)
            continue;    // raw handlers match anything — skip
        stream_reset(s); // rewind so each handler sees the track from the start
        if (track_write_raw_from_stream(static_cast<struct disk*>(m_disk), trackNumber, j, s) ==
            0) {
            const char* desc = disk_get_format_desc_name(j);
            result = desc ? desc : id;
            break;
        }
    }

    stream_close(s);
    return result;
#else
    (void)mfm;
    (void)lenBytes;
    (void)trackNumber;
    return {};
#endif
}

} // namespace adm::core
