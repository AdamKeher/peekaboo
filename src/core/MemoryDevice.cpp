#include "MemoryDevice.h"
#include "GreaseweazelReader.h"
#include "Logger.h"

#include <cstring>
#include <fstream>
#include <new>

// ADFLib C headers - must be included after C++ headers to avoid macro conflicts
// Rename C++ keyword to avoid conflicts with ADFLib C headers
#define class klass
extern "C" {
#include <adf_dev.h>
#include <adf_dev_driver.h>
#include <adf_dev_drivers.h>
#include <adf_dev_type.h>
#include <adf_env.h>
}
#undef class

namespace adm {
namespace core {

// Forward declaration of driver functions
static struct AdfDevice* memdevOpen(const char* const name, const AdfAccessMode mode);
static ADF_RETCODE memdevClose(struct AdfDevice* const dev);
static ADF_RETCODE memdevReadSectors(const struct AdfDevice* const dev, const uint32_t block,
                                     const uint32_t lenBlocks, uint8_t* const buf);
static ADF_RETCODE memdevWriteSectors(const struct AdfDevice* const dev, const uint32_t block,
                                      const uint32_t lenBlocks, const uint8_t* const buf);
static bool memdevIsNative(void);
static bool memdevIsDevice(const char* const name);

// Sectors per track implied by a block count, for the fixed 80-cylinder / 2-head floppy
// geometry this driver presents. The lazy read path and memdevOpen both need this and used to
// derive it separately - memdevOpen correctly, ReadSector with a hardcoded 11 - which broke
// every HD image the moment it was read lazily.
static uint32_t GeometrySectorsPerTrack(uint32_t totalBlocks)
{
    constexpr uint32_t kCylinders = 80;
    constexpr uint32_t kHeads = 2;
    if (totalBlocks == 1760)
        return 11; // standard DD
    if (totalBlocks == 3520)
        return 22; // HD
    const uint32_t derived = totalBlocks / (kCylinders * kHeads);
    return derived > 0 ? derived : 11;
}

// Custom device driver structure - we use a special prefix to identify our files
static const struct AdfDeviceDriver adfDeviceDriverMemory = {.name = "memory",
                                                             .data = nullptr,
                                                             .createDev = nullptr,
                                                             .openDev = memdevOpen,
                                                             .closeDev = memdevClose,
                                                             .readSectors = memdevReadSectors,
                                                             .writeSectors = memdevWriteSectors,
                                                             .isNative = memdevIsNative,
                                                             .isDevice = memdevIsDevice};

// Thread-local storage for the active MemoryDevice during open
// This is a workaround since ADFLib's driver interface doesn't allow passing user data to openDev
static thread_local MemoryDevice* g_activeMemoryDevice = nullptr;

// Structure stored in drvData to link back to our MemoryDevice
struct MemoryDeviceData
{
    MemoryDevice* device;
    std::string filePath;
};

MemoryDevice::MemoryDevice() = default;

MemoryDevice::~MemoryDevice() { Close(); }

AdfDevice* MemoryDevice::Open(const std::string& path)
{
    Close(); // Close any existing device

    // Read the file into memory
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        Logger::Error("MemoryDevice: Failed to open file: {}", path);
        return nullptr;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    m_buffer.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(m_buffer.data()), size)) {
        Logger::Error("MemoryDevice: Failed to read file: {}", path);
        m_buffer.clear();
        return nullptr;
    }

    // Store original data for revert functionality
    m_originalData = m_buffer;

    m_filePath = path;
    m_blockSize = 512; // Standard ADF block size
    m_totalBlocks = static_cast<uint32_t>(m_buffer.size() / m_blockSize);
    m_sectorsPerTrack = GeometrySectorsPerTrack(m_totalBlocks);
    m_modifiedBlocks.clear();

    // Register our driver with ADFLib if not already done
    static bool driverRegistered = false;
    if (!driverRegistered) {
        adfAddDeviceDriver(&adfDeviceDriverMemory);
        driverRegistered = true;
    }

    // Set up thread-local pointer so the driver can find us
    g_activeMemoryDevice = this;

    // Open using our custom driver
    // We use the "memory:" prefix to identify files that should use our driver
    std::string deviceName = "memory:" + path;
    m_device = adfDevOpenWithDriver("memory", deviceName.c_str(), ADF_ACCESS_MODE_READWRITE);

    g_activeMemoryDevice = nullptr;

    if (!m_device) {
        Logger::Error("MemoryDevice: Failed to create ADF device for: {}", path);
        m_buffer.clear();
        m_filePath.clear();
        return nullptr;
    }

    Logger::Info("MemoryDevice: Loaded {} ({} blocks) into memory buffer", path, m_totalBlocks);
    return m_device;
}

AdfDevice* MemoryDevice::Open(std::vector<uint8_t> data)
{
    Close();

    if (data.empty()) {
        Logger::Error("MemoryDevice: Cannot open empty buffer");
        return nullptr;
    }

    m_buffer = std::move(data);
    m_originalData = m_buffer; // Copy for revert functionality

    m_filePath = ""; // No file path provided
    m_blockSize = 512;
    m_totalBlocks = static_cast<uint32_t>(m_buffer.size() / m_blockSize);
    m_sectorsPerTrack = GeometrySectorsPerTrack(m_totalBlocks);
    m_modifiedBlocks.clear();

    // Register our driver with ADFLib if not already done
    static bool driverRegistered = false;
    if (!driverRegistered) {
        adfAddDeviceDriver(&adfDeviceDriverMemory);
        driverRegistered = true;
    }

    g_activeMemoryDevice = this;

    // Use a unique name for memory-only devices
    static int memCounter = 0;
    std::string deviceName = "memory:buffer_" + std::to_string(++memCounter);

    m_device = adfDevOpenWithDriver("memory", deviceName.c_str(), ADF_ACCESS_MODE_READWRITE);

    g_activeMemoryDevice = nullptr;

    if (!m_device) {
        Logger::Error("MemoryDevice: Failed to create ADF device for memory buffer");
        m_buffer.clear();
        m_originalData.clear();
        return nullptr;
    }

    Logger::Info("MemoryDevice: Loaded {} blocks from memory buffer", m_totalBlocks);
    return m_device;
}

AdfDevice* MemoryDevice::OpenLazy(GreaseweazelReader* reader)
{
    Close();

    if (!reader) {
        Logger::Error("MemoryDevice: Cannot open lazy with null reader");
        return nullptr;
    }

    m_lazyReader = reader;

    // Take a snapshot of whatever the reader has so far. This has to go through CopyBuffer(),
    // which holds the reader's lock: the reader thread is writing tracks into that buffer right
    // now, so an unlocked bulk copy tears against a track landing mid-memcpy. Blocks missing from
    // the snapshot are filled in on demand by ReadSector via CopyBlockIfLoaded.
    m_buffer = reader->CopyBuffer();
    m_buffer.resize(GreaseweazelReader::kTotalBytes, 0);

    m_filePath = "";
    m_blockSize = 512;
    m_totalBlocks = GreaseweazelReader::kTotalBlocks;
    m_sectorsPerTrack = GeometrySectorsPerTrack(m_totalBlocks);
    m_modifiedBlocks.clear();

    // Register our driver with ADFLib if not already done
    static bool driverRegistered = false;
    if (!driverRegistered) {
        adfAddDeviceDriver(&adfDeviceDriverMemory);
        driverRegistered = true;
    }

    g_activeMemoryDevice = this;

    static int lazyCounter = 0;
    std::string deviceName = "memory:floppy_" + std::to_string(++lazyCounter);

    m_device = adfDevOpenWithDriver("memory", deviceName.c_str(), ADF_ACCESS_MODE_READWRITE);

    g_activeMemoryDevice = nullptr;

    if (!m_device) {
        Logger::Error("MemoryDevice: Failed to create ADF device for lazy floppy reader");
        m_buffer.clear();
        m_lazyReader = nullptr;
        return nullptr;
    }

    Logger::Info("MemoryDevice: Opened lazy floppy reader ({} blocks)", m_totalBlocks);
    return m_device;
}

void MemoryDevice::Close()
{
    if (m_device) {
        adfDevClose(m_device);
        m_device = nullptr;
    }
    m_buffer.clear();
    m_originalData.clear();
    m_filePath.clear();
    m_modifiedBlocks.clear();
    m_totalBlocks = 0;
    m_sectorsPerTrack = 11;
    m_lazyReader = nullptr;
}

bool MemoryDevice::IsOpen() const { return m_device != nullptr && !m_buffer.empty(); }

bool MemoryDevice::Save()
{
    if (!IsOpen() || m_filePath.empty()) {
        Logger::Error("MemoryDevice: No file to save");
        return false;
    }

    return SaveAs(m_filePath);
}

bool MemoryDevice::SaveAs(const std::string& path)
{
    if (!IsOpen()) {
        Logger::Error("MemoryDevice: No data to save");
        return false;
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        Logger::Error("MemoryDevice: Failed to open file for writing: {}", path);
        return false;
    }

    if (!file.write(reinterpret_cast<const char*>(m_buffer.data()),
                    static_cast<std::streamsize>(m_buffer.size()))) {
        Logger::Error("MemoryDevice: Failed to write file: {}", path);
        return false;
    }

    Logger::Info("MemoryDevice: Saved {} bytes to {}", m_buffer.size(), path);
    return true;
}

bool MemoryDevice::IsBlockCached(uint32_t block) const
{
    if (m_lazyReader) {
        return m_lazyReader->IsBlockLoaded(block);
    }
    return true; // Always cached if fully loaded in memory
}

bool MemoryDevice::IsBlockModified(uint32_t block) const
{
    return m_modifiedBlocks.find(block) != m_modifiedBlocks.end();
}

std::vector<bool> MemoryDevice::GetBlockModificationStatus() const
{
    std::vector<bool> status(m_totalBlocks, false);
    for (uint32_t block : m_modifiedBlocks) {
        if (block < m_totalBlocks) {
            status[block] = true;
        }
    }
    return status;
}

uint32_t MemoryDevice::GetTotalBlocks() const { return m_totalBlocks; }

bool MemoryDevice::ReadSector(uint32_t sector, uint32_t size, uint8_t* buf)
{
    // In lazy mode, sync from reader's buffer for any newly-loaded tracks
    if (m_lazyReader) {
        for (uint32_t i = 0; i < size; ++i) {
            uint32_t blk = sector + i;
            // Only sync if the block hasn't been modified by the user
            if (IsBlockModified(blk))
                continue;

            size_t off = static_cast<size_t>(blk) * m_blockSize;
            if (off + m_blockSize > m_buffer.size())
                continue;

            // Atomically check-and-copy under the reader's lock, so a concurrent track write on
            // the reader thread can't tear the bytes we copy. Returns false if the track isn't
            // loaded yet, in which case we queue its cylinder for background reading.
            if (!m_lazyReader->CopyBlockIfLoaded(blk, m_buffer.data() + off)) {
                // Global track = block / sectors-per-track; cylinder = track / 2 heads.
                // Must use the buffer's real geometry: with DD's 11 hardcoded here, an HD image
                // asked for a cylinder twice the right number - past the 79 the drive has, so the
                // block's own cylinder was never queued and the read sat on zeros forever.
                const uint32_t track = blk / m_sectorsPerTrack;
                const int cylinder = static_cast<int>(track / 2);
                m_lazyReader->RequestCylinder(cylinder);
            }
        }
    }

    size_t offset = static_cast<size_t>(sector) * m_blockSize;
    size_t bytes = static_cast<size_t>(size) * m_blockSize;

    if (offset + bytes > m_buffer.size()) {
        Logger::Error("MemoryDevice: Read out of bounds: sector={}, size={}", sector, size);
        return false;
    }

    std::memcpy(buf, m_buffer.data() + offset, bytes);
    return true;
}

bool MemoryDevice::WriteSector(uint32_t sector, uint32_t size, const uint8_t* buf)
{
    size_t offset = static_cast<size_t>(sector) * m_blockSize;
    size_t bytes = static_cast<size_t>(size) * m_blockSize;

    if (offset + bytes > m_buffer.size()) {
        Logger::Error("MemoryDevice: Write out of bounds: sector={}, size={}", sector, size);
        return false;
    }

    std::memcpy(m_buffer.data() + offset, buf, bytes);

    // Track modified blocks
    for (uint32_t i = 0; i < size; ++i) {
        m_modifiedBlocks.insert(sector + i);
    }

    return true;
}

bool MemoryDevice::RevertBlock(uint32_t block)
{
    if (!IsBlockModified(block)) {
        return true; // Already matches disk (technically)
    }

    // If we have original data in memory (always true for ZIP/Memory loading, and now for file
    // loading too)
    if (!m_originalData.empty()) {
        size_t offset = static_cast<size_t>(block) * m_blockSize;
        if (offset + m_blockSize > m_originalData.size()) {
            Logger::Error("MemoryDevice: Revert block out of bounds: {}", block);
            return false;
        }

        std::memcpy(m_buffer.data() + offset, m_originalData.data() + offset, m_blockSize);
        m_modifiedBlocks.erase(block);
        return true;
    }

    // Fallback to file reading if m_originalData is empty (shouldn't happen with new logic, but
    // safe to keep)
    if (m_filePath.empty()) {
        Logger::Error("MemoryDevice: No file path to revert from and no original data");
        return false;
    }

    // Open file to read original data
    std::ifstream file(m_filePath, std::ios::binary);
    if (!file) {
        Logger::Error("MemoryDevice: Failed to open file for revert: {}", m_filePath);
        return false;
    }

    size_t offset = static_cast<size_t>(block) * m_blockSize;
    if (offset + m_blockSize > m_buffer.size()) {
        Logger::Error("MemoryDevice: Revert block out of bounds: {}", block);
        return false;
    }

    file.seekg(offset);
    if (!file.read(reinterpret_cast<char*>(m_buffer.data() + offset), m_blockSize)) {
        Logger::Error("MemoryDevice: Failed to read block {} from file", block);
        return false;
    }

    // Remove from modified set
    m_modifiedBlocks.erase(block);

    return true;
}

std::vector<uint8_t> MemoryDevice::ReadBlockFromDisk(uint32_t block) const
{
    size_t offset = static_cast<size_t>(block) * m_blockSize;

    // Check m_originalData first
    if (!m_originalData.empty()) {
        if (offset + m_blockSize > m_originalData.size()) {
            Logger::Error("MemoryDevice: Block out of bounds: {}", block);
            return {};
        }
        std::vector<uint8_t> data(m_blockSize);
        std::memcpy(data.data(), m_originalData.data() + offset, m_blockSize);
        return data;
    }

    if (m_filePath.empty()) {
        Logger::Error("MemoryDevice: No file path to read from");
        return {};
    }

    if (offset + m_blockSize > m_buffer.size()) {
        Logger::Error("MemoryDevice: Block out of bounds: {}", block);
        return {};
    }

    // Open file to read original data
    std::ifstream file(m_filePath, std::ios::binary);
    if (!file) {
        Logger::Error("MemoryDevice: Failed to open file for reading: {}", m_filePath);
        return {};
    }

    std::vector<uint8_t> data(m_blockSize);
    file.seekg(offset);
    if (!file.read(reinterpret_cast<char*>(data.data()), m_blockSize)) {
        Logger::Error("MemoryDevice: Failed to read block {} from file", block);
        return {};
    }

    return data;
}

// ADFLib driver callback implementations

static struct AdfDevice* memdevOpen(const char* const name, const AdfAccessMode mode)
{
    (void)mode; // We always open read/write for our buffer

    MemoryDevice* memdev = g_activeMemoryDevice;
    if (!memdev) {
        adfEnv.eFct("memdevOpen: No active MemoryDevice");
        return nullptr;
    }

    // Allocate our private data FIRST — with the old order a throwing `new` leaked the
    // malloc'd AdfDevice. nothrow keeps this C callback exception-free.
    MemoryDeviceData* data = new (std::nothrow) MemoryDeviceData();
    if (!data) {
        adfEnv.eFct("memdevOpen: allocation error");
        return nullptr;
    }
    data->device = memdev;
    data->filePath = name;

    // Allocate the AdfDevice structure (freed by adflib with free(), so it must be malloc'd)
    struct AdfDevice* dev = static_cast<struct AdfDevice*>(malloc(sizeof(struct AdfDevice)));
    if (!dev) {
        delete data;
        adfEnv.eFct("memdevOpen: malloc error");
        return nullptr;
    }

    // Standard DD floppy geometry (most common ADF)
    uint32_t totalBlocks = memdev->GetTotalBlocks();

    // Detect geometry based on size
    const uint32_t cylinders = 80;
    const uint32_t heads = 2;
    const uint32_t sectors = GeometrySectorsPerTrack(totalBlocks);

    dev->readOnly = false;
    dev->geometry.cylinders = cylinders;
    dev->geometry.heads = heads;
    dev->geometry.sectors = sectors;
    dev->geometry.blockSize = 512;
    dev->sizeBlocks = totalBlocks;

    dev->type = adfDevGetTypeByGeometry(&dev->geometry);
    dev->klass =
        (dev->type != ADF_DEVTYPE_UNKNOWN) ? adfDevTypeGetClass(dev->type) : ADF_DEVCLASS_FLOP;

    dev->nVol = 0;
    dev->volList = nullptr;
    dev->mounted = false;
    dev->name = strdup(name);
    if (!dev->name) { // adflib dereferences the name unchecked
        delete data;
        free(dev);
        adfEnv.eFct("memdevOpen: strdup error");
        return nullptr;
    }
    dev->drv = &adfDeviceDriverMemory;
    dev->drvData = data;

    dev->rdb.status = ADF_DEV_RDB_STATUS_NOTFOUND;
    dev->rdb.block = nullptr;

    return dev;
}

static ADF_RETCODE memdevClose(struct AdfDevice* const dev)
{
    if (dev->drvData) {
        MemoryDeviceData* data = static_cast<MemoryDeviceData*>(dev->drvData);
        delete data;
    }
    free(dev->name);
    free(dev);
    return ADF_RC_OK;
}

static ADF_RETCODE memdevReadSectors(const struct AdfDevice* const dev, const uint32_t block,
                                     const uint32_t lenBlocks, uint8_t* const buf)
{
    MemoryDeviceData* data = static_cast<MemoryDeviceData*>(dev->drvData);
    if (!data || !data->device) {
        return ADF_RC_ERROR;
    }

    if (data->device->ReadSector(block, lenBlocks, buf)) {
        return ADF_RC_OK;
    }
    return ADF_RC_ERROR;
}

static ADF_RETCODE memdevWriteSectors(const struct AdfDevice* const dev, const uint32_t block,
                                      const uint32_t lenBlocks, const uint8_t* const buf)
{
    MemoryDeviceData* data = static_cast<MemoryDeviceData*>(dev->drvData);
    if (!data || !data->device) {
        return ADF_RC_ERROR;
    }

    if (data->device->WriteSector(block, lenBlocks, buf)) {
        return ADF_RC_OK;
    }
    return ADF_RC_ERROR;
}

static bool memdevIsNative(void) { return false; }

static bool memdevIsDevice(const char* const name)
{
    // Match files with our "memory:" prefix
    return name && strncmp(name, "memory:", 7) == 0;
}

} // namespace core
} // namespace adm
