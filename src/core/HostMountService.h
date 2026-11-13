#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace adm::core {

// Archive: an .lha/.lzx mounted as a volume through UAE's archive filesystem (the game of a
// WHDLoad launch). Its path is kept in hostDirectory, like a directory's.
enum class HostMountBackingType { Directory, HardfileImage, Archive };
// Where a hardfile is attached. Uae is UAE's own hardfile device and works on every model;
// IdeMainboard is the machine's own IDE (Gayle on A600/A1200, the A4000 controller); the two
// SCSI cards are Zorro boards that need their boot ROM supplied.
enum class HardDriveController { Uae, IdeMainboard, ScsiA590A2091, ScsiA4091 };
enum class HardfileGeometryMode { Auto, Rdb, Manual };
enum class HostMountDevice { DH0 = 0, DH1 = 1 };
// The three Transient sources are launches that override the saved DH0/DH1 for one run: a
// Bartman compile-and-run, a WHDLoad game, and Run on a hard-disk image document.
enum class HostMountSource {
    Preferences,
    RestoredSession,
    TransientBartman,
    TransientWhdload,
    TransientDocument
};
enum class EmulatorBootIntent { Default, Floppy, Directory };
enum class HostMountRuntimeState {
    Disabled,
    Configured,
    PendingRestart,
    Effective,
    Confirmed,
    Failed
};

struct HardfileGeometry
{
    // Auto: the RDB when the image has one, otherwise the core's 32/1/2/512 guess (16 surfaces
    // from 1 GB). Rdb: always let UAE read the RDB. Manual: the four values below.
    HardfileGeometryMode mode = HardfileGeometryMode::Auto;
    int sectors = 32;
    int surfaces = 1;
    int reserved = 2;
    int blockSize = 512;

    bool operator==(const HardfileGeometry&) const = default;
};

// Boot ROMs for the SCSI cards. A card is present in the machine exactly when its ROM is set
// and a hardfile is attached to it.
struct HardDriveControllerRoms
{
    std::filesystem::path a590A2091;
    std::filesystem::path a4091;

    bool operator==(const HardDriveControllerRoms&) const = default;
};

struct HostDirectoryMount
{
    HostMountBackingType backingType = HostMountBackingType::Directory;
    HostMountDevice device = HostMountDevice::DH0;
    bool enabled = false;
    std::filesystem::path hostDirectory;
    std::string volumeLabel;
    bool readOnly = true;
    int bootPriority = -128;
    HostMountSource source = HostMountSource::Preferences;
    // HardfileImage backing. Kept separate from hostDirectory so switching the backing back and
    // forth does not lose either path.
    std::filesystem::path hardfilePath;
    HardDriveController controller = HardDriveController::Uae;
    int controllerUnit = 0;
    HardfileGeometry geometry;

    bool IsHardfile() const { return backingType == HostMountBackingType::HardfileImage; }
    const std::filesystem::path& HostPath() const
    {
        return IsHardfile() ? hardfilePath : hostDirectory;
    }

    bool operator==(const HostDirectoryMount&) const = default;
};

// A volume a launch adds besides DH0/DH1, such as WHDLoad: and WHDSaves:.
struct ExtraVolume
{
    std::string device;
    std::string volumeLabel;
    std::filesystem::path hostDirectory;
    bool readOnly = true;
    int bootPriority = -128;

    bool operator==(const ExtraVolume&) const = default;
};

struct EmulatorLaunchProfile
{
    std::string modelId;
    std::optional<std::filesystem::path> baseUaeConfig;
    std::array<HostDirectoryMount, 2> mounts;
    HardDriveControllerRoms controllerRoms;
    // Mounted ahead of DH0/DH1 and after them, in this order (unit numbers follow it).
    std::vector<ExtraVolume> volumesBefore;
    std::vector<ExtraVolume> volumesAfter;
    EmulatorBootIntent bootIntent = EmulatorBootIntent::Default;
    bool transient = false;
    // boot_rom_uae=min, which the Bartman launcher uses.
    bool minimalBootRom = false;
    // The disc in CD0 at power-on (cdimage0=). A machine that is not a CD32 or CDTV also gets
    // scsi=true, the uaescsi.device path the core uses for a CD on other models.
    std::filesystem::path cdImage;

    bool operator==(const EmulatorLaunchProfile&) const = default;
};

struct HostMountActivity
{
    uint64_t reads = 0;
    uint64_t writes = 0;
    int latchedStatus = 0; // 0 idle, 1 read, 2 write
    int recentStatus = 0;
    std::chrono::steady_clock::time_point recentUntil{};
    bool readOnlyViolationReported = false;
};

class UaeConfigBuilder
{
public:
    // Controller-token and ROM-line helpers shared with the UI and the console.
    static std::string ControllerToken(HardDriveController controller, int unit);
    static int MaxControllerUnit(HardDriveController controller);
    // Empty when `modelId` (a PEEKaboo model id such as "A1200") can take the controller,
    // otherwise the reason it cannot.
    static std::string ControllerUnavailableReason(HardDriveController controller,
                                                   const std::string& modelId);
    // The hardfile2 geometry fields "sectors,surfaces,reserved,blocksize" for a mount.
    static std::optional<std::string> HardfileGeometryFields(const HostDirectoryMount& mount,
                                                             std::string& outError);
    static bool ValidateControllerRoms(const std::array<HostDirectoryMount, 2>& mounts,
                                       const HardDriveControllerRoms& roms, std::string& outError);
    static bool ValidateMounts(std::array<HostDirectoryMount, 2>& mounts, std::string& outError,
                               bool testWritable = true);
    static std::optional<std::string> SerializeMount(const HostDirectoryMount& mount,
                                                     std::string& outError);
    static std::optional<std::string> Build(const EmulatorLaunchProfile& profile,
                                            std::string& outError);
    static bool WriteAtomic(const std::filesystem::path& path, const std::string& contents,
                            std::string& outError);
};

// Owns configured/effective/pending state and the generated-config workspace. Emulator startup
// remains in AmigaEmulator, so every client ultimately enters PuAE through LoadUaeConfig().
class HostMountService
{
public:
    HostMountService();

    void SetConfiguredMount(HostMountDevice device, const HostDirectoryMount& mount);
    void SetConfiguredMounts(const std::array<HostDirectoryMount, 2>& mounts);
    void SetControllerRoms(const HardDriveControllerRoms& roms);
    const HardDriveControllerRoms& GetControllerRoms() const { return m_controllerRoms; }
    const std::array<HostDirectoryMount, 2>& GetConfiguredMounts() const { return m_configured; }
    const std::array<HostDirectoryMount, 2>& GetEffectiveMounts() const { return m_effective; }
    const std::array<HostMountActivity, 2>& GetActivity() const { return m_activity; }
    const std::array<bool, 2>& GetConfirmedMounts() const { return m_confirmed; }

    EmulatorLaunchProfile CreateConfiguredProfile(
        const std::optional<std::filesystem::path>& baseConfig = std::nullopt) const;
    bool PrepareLaunch(const EmulatorLaunchProfile& profile, const std::filesystem::path& workspace,
                       std::filesystem::path& outConfig, std::string& outError,
                       EmulatorLaunchProfile* outEffectiveProfile = nullptr);
    void MarkEffective(const EmulatorLaunchProfile& profile);
    void MarkFailed(const std::string& error, bool logError = true);
    void ClearEffective();

    bool HasPendingRestart() const;
    bool IsTransientOverrideActive() const { return m_transientOverride; }
    const std::string& GetLastError() const { return m_lastError; }
    const std::optional<EmulatorLaunchProfile>& GetEffectiveProfile() const
    {
        return m_effectiveProfile;
    }
    // Moves on every MarkEffective and ClearEffective, so a caller that derives something from
    // the effective mounts can tell whether they have changed without comparing them.
    uint64_t GetEffectiveGeneration() const { return m_effectiveGeneration; }

    // PuAE numbers every filesystem2/hardfile2 unit in config order, whatever controller it is
    // on. MarkEffective records that order (base-config units first, then what the builder
    // emitted), so a unit maps back to the matching effective descriptor.
    // Where a host file dropped on the machine would land, or nullopt when nothing can take it.
    // A directory-backed, writable, enabled mount: a hardfile is an image whose filesystem only
    // the guest understands, and a read-only mount is read-only to the host too.
    std::optional<HostMountDevice> DropTargetDevice() const;
    // Copies `file` into that mount. Returns the AmigaDOS path it landed on ("DH0:name"), or
    // nullopt with the reason. Never overwrites: a clashing name gets a numbered suffix, because
    // silently replacing a file the guest is using is worse than an odd name.
    std::optional<std::string> CopyFileToGuest(const std::filesystem::path& file,
                                               std::string& outError) const;

    std::optional<HostMountDevice> DeviceForUnit(int unit) const;
    // Whether the guest can confirm a device. Only units the UAE boot ROM mounts are visible to
    // adm_get_active_filesystem_units; a hardfile on built-in IDE or a SCSI card never is.
    static bool IsGuestConfirmable(const HostDirectoryMount& mount);
    void RecordActivity(int unit, int status, uint32_t count = 1);
    int ConsumeActivity(HostMountDevice device);
    int GetRecentActivityStatus(HostMountDevice device, std::chrono::steady_clock::time_point now =
                                                            std::chrono::steady_clock::now()) const;
    void ConfirmActiveUnits(const std::vector<int>& units);
    bool CheckConfirmationTimeout(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    HostMountRuntimeState GetRuntimeState(HostMountDevice device) const;

private:
    std::array<HostDirectoryMount, 2> m_configured;
    std::array<HostDirectoryMount, 2> m_effective;
    std::array<HostMountActivity, 2> m_activity{};
    std::array<bool, 2> m_confirmed{};
    std::optional<EmulatorLaunchProfile> m_effectiveProfile;
    uint64_t m_effectiveGeneration = 0;
    HardDriveControllerRoms m_controllerRoms;
    HardDriveControllerRoms m_effectiveControllerRoms;
    std::string m_lastError;
    bool m_transientOverride = false;
    std::vector<std::optional<HostMountDevice>> m_unitDevices;
    std::chrono::steady_clock::time_point m_confirmationDeadline{};
    bool m_waitingForConfirmation = false;
    bool m_confirmationTimeoutReported = false;
};

const char* HostMountDeviceName(HostMountDevice device);
const char* HardDriveControllerName(HardDriveController controller);
const char* HardDriveControllerId(HardDriveController controller); // "uae", "ide", "a2091", "a4091"
std::optional<HardDriveController> ParseHardDriveController(const std::string& value);
const char* HardfileGeometryModeId(HardfileGeometryMode mode); // "auto", "rdb", "manual"
std::optional<HardfileGeometryMode> ParseHardfileGeometryMode(const std::string& value);

} // namespace adm::core
