#include "EmulatorView.h"
#include <GLFW/glfw3.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <nfd.h>
#include <stb_image_write.h> // declarations only; implementation in external/stb
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include "../../core/CdMedia.h"
#include "../../core/ClipboardImage.h"
#include "../../core/HardfileInspector.h"
#include "../../core/JsonRead.h"
#include "../../core/Logger.h"
#include "../../core/PrivateFeatures.h"
#include "../../core/SystemFiles.h"
#include "../../core/UserPreferences.h"
#include "../../core/ZipUtils.h"
#include "../ColorManager.h"
#include "../DriveChips.h"
#include "../EmulatorFilter.h" // glad.h — must precede any GL/GLFW header
#include "../IconsFontAwesome6.h"
#include "../MenuIcons.h"
#include "../RemoteSession.h"

namespace adm::ui {

namespace {

std::filesystem::path PathFromUtf8(const std::string& value)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::string PathToUtf8(const std::filesystem::path& value)
{
    const auto encoded = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

const std::filesystem::path& CustomConfigurationDirectory()
{
    static const std::filesystem::path directory = "./uae/configs/custom";
    return directory;
}

bool IsValidConfigurationName(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name.back() == ' ' || name.back() == '.')
        return false;
    constexpr std::string_view invalid = "<>:\"/\\|?*";
    return std::none_of(name.begin(), name.end(), [invalid](unsigned char c) {
        return c < 32 || invalid.find(static_cast<char>(c)) != std::string_view::npos;
    });
}

bool IsPathWithin(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    const auto normalizedChild = std::filesystem::absolute(child).lexically_normal();
    const auto normalizedParent = std::filesystem::absolute(parent).lexically_normal();
    auto childIt = normalizedChild.begin();
    for (auto parentIt = normalizedParent.begin(); parentIt != normalizedParent.end();
         ++parentIt, ++childIt) {
        if (childIt == normalizedChild.end() || *childIt != *parentIt)
            return false;
    }
    return true;
}

bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes,
                     std::string& outError)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || (!bytes.empty() && !stream.write(reinterpret_cast<const char*>(bytes.data()),
                                                    static_cast<std::streamsize>(bytes.size())))) {
        outError = "Unable to save drive media: " + path.filename().string();
        return false;
    }
    return true;
}

} // namespace

EmulatorView::EmulatorView() : m_emulator(std::make_unique<adm::AmigaEmulator>())
{
    try {
        std::filesystem::path emuPath = std::filesystem::canonical("./emulation");
        m_emulator->SetSystemDirectory(emuPath.string());
    } catch (const std::exception& e) {
        adm::core::Logger::Error("Emulator: Failed to resolve emulation directory: {}", e.what());
    }

    // DF0 and DF1 enabled by default; DF2 and DF3 disabled
    m_drives[0].enabled = true;
    m_drives[1].enabled = true;
    m_drives[2].enabled = false;
    m_drives[3].enabled = false;
}

EmulatorView::~EmulatorView()
{
    // The extraction thread calls into the core; it has to finish before the emulator goes.
    if (m_cdExtraction && m_cdExtraction->worker.joinable())
        m_cdExtraction->worker.join();
}

void EmulatorView::InitGraphics()
{
    // Create the post-process filter and compile its shader/FBO now, at startup, rather than
    // lazily on first use. Building GL objects inside a live ImGui frame can intermittently
    // hang NVIDIA's threaded driver; doing it here avoids that.
    if (!m_filter)
        m_filter = std::make_unique<EmulatorFilter>();
    m_filter->Initialize();
}

bool EmulatorView::LoadCore(const std::string& corePath) { return m_emulator->LoadCore(corePath); }

bool EmulatorView::IsCoreLoaded() const { return m_emulator->IsCoreLoaded(); }

void EmulatorView::UnloadCore() { m_emulator->UnloadCore(); }

bool EmulatorView::LoadUaeConfig(const std::string& configPath)
{
    if (!IsCoreLoaded()) {
        if (!m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath()))
            return false;
    }
    for (int i = 0; i < 4; i++)
        m_emulator->SetDriveEnabled(i, m_drives[i].enabled);

    Open();
    SetFocus();
    const bool hasHostMount = m_emulator->NeedsGeneratedConfig();
    std::string error;
    bool ok = hasHostMount ? m_emulator->StartConfiguredHostMounts(error, configPath)
                           : m_emulator->LoadUaeConfig(configPath);
    if (!ok && !error.empty())
        adm::core::Logger::Error("Emulator: {}", error);
    if (ok) {
        if (hasHostMount)
            PersistNormalizedHostMounts();
        m_selectedBaseUaeConfig = std::filesystem::path(configPath);
        m_selectedCustomConfigurationName.clear();
        FlushPendingDisks();
        if (m_onEmulatorReset)
            m_onEmulatorReset();
    }
    return ok;
}

bool EmulatorView::RunHostExecutable(const std::string& programPath, std::string& outError)
{
    if (!IsCoreLoaded()) {
        if (!m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath())) {
            outError = "failed to load the bundled PuAE core";
            return false;
        }
    }

    Open();
    bool ok = m_emulator->RunHostExecutable(programPath, outError);
    if (ok) {
        m_isPaused = false;
        if (m_onEmulatorReset)
            m_onEmulatorReset();
    }
    return ok;
}

bool EmulatorView::RunExecutable(const std::vector<uint8_t>& exeData, const std::string& filename,
                                 std::string& outError)
{
    if (!IsCoreLoaded()) {
        if (!m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath())) {
            outError = "failed to load the bundled PuAE core";
            return false;
        }
    }

    Open();
    bool ok = m_emulator->RunExecutable(exeData, filename);
    if (!ok) {
        outError = "failed to create or boot the compatibility ADF";
        return false;
    }
    m_isPaused = false;
    if (m_onEmulatorReset)
        m_onEmulatorReset();
    return true;
}

void EmulatorView::ClearUaeConfig()
{
    m_emulator->ClearUaeConfig();
    m_selectedBaseUaeConfig.reset();
    m_selectedCustomConfigurationName.clear();
    // Restart without the config file, triggering the normal reset flow
    StartEmpty();
}

bool EmulatorView::HasUaeConfig() const { return m_emulator && m_emulator->HasUaeConfig(); }

std::string EmulatorView::GetUaeConfigName() const
{
    return m_emulator ? m_emulator->GetUaeConfigName() : std::string{};
}

std::vector<std::filesystem::path> EmulatorView::GetCustomConfigurations() const
{
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    const auto& directory = CustomConfigurationDirectory();
    if (!std::filesystem::is_directory(directory, ec))
        return result;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json")
            result.push_back(entry.path());
        if (ec)
            break;
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.stem().string() < rhs.stem().string();
    });
    return result;
}

std::string EmulatorView::GetCurrentCustomConfigurationName() const
{
    return m_selectedCustomConfigurationName;
}

bool EmulatorView::WriteCustomConfiguration(const std::string& name, bool allowOverwrite,
                                            std::string& outError)
{
    outError.clear();
    if (!IsValidConfigurationName(name)) {
        outError = "Use a non-empty filename without < > : \" / \ | ? * characters.";
        return false;
    }

    const auto& directory = CustomConfigurationDirectory();
    const auto profilePath = directory / (name + ".json");
    const auto assetsPath = directory / (name + ".assets");
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        outError = "Unable to create the custom configuration directory: " + ec.message();
        return false;
    }

    const bool profileExists = std::filesystem::exists(profilePath, ec);
    const bool assetsExist = std::filesystem::exists(assetsPath, ec);
    if (!allowOverwrite) {
        if (profileExists || assetsExist) {
            outError = "A custom configuration with that name already exists.";
            return false;
        }
    } else if (!profileExists) {
        outError = "That custom configuration no longer exists.";
        return false;
    }

    // Stage the media beside the target so a half-written update never damages the
    // configuration that is already on disk; the staged set is swapped in at the end.
    const auto stagingPath = directory / (name + ".assets.staging");
    std::filesystem::remove_all(stagingPath, ec);
    std::filesystem::create_directory(stagingPath, ec);
    if (ec) {
        outError = "Unable to create configuration assets: " + ec.message();
        return false;
    }
    const std::filesystem::path assetsRelative(name + ".assets");
    auto discardStaging = [&stagingPath]() {
        std::error_code cleanup;
        std::filesystem::remove_all(stagingPath, cleanup);
    };

    nlohmann::json profile;
    profile["version"] = 1;
    profile["name"] = name;
    profile["modelId"] = GetCurrentModelId();
    profile["userVariables"] = m_emulator->GetUserVariables();

    if (m_selectedBaseUaeConfig) {
        core::EmulatorLaunchProfile baseProfile;
        baseProfile.baseUaeConfig = m_selectedBaseUaeConfig;
        auto rebasedConfig = core::UaeConfigBuilder::Build(baseProfile, outError);
        if (!rebasedConfig || !core::UaeConfigBuilder::WriteAtomic(stagingPath / "base.uae",
                                                                   *rebasedConfig, outError)) {
            discardStaging();
            return false;
        }
        profile["baseConfig"] = PathToUtf8(assetsRelative / "base.uae");
    }

    profile["hostMounts"] = nlohmann::json::array();
    for (const auto& mount : GetConfiguredHostMounts()) {
        nlohmann::json savedMount = {{"enabled", mount.enabled},
                                     {"path", PathToUtf8(mount.hostDirectory)},
                                     {"volumeLabel", mount.volumeLabel},
                                     {"readOnly", mount.readOnly},
                                     {"bootPriority", mount.bootPriority}};
        core::WriteHardfileFields(savedMount, mount);
        profile["hostMounts"].push_back(std::move(savedMount));
    }
    profile["hardDriveControllerRoms"] =
        core::ControllerRomsJson(m_emulator->GetHostMountService().GetControllerRoms());

    profile["drives"] = nlohmann::json::array();
    for (int index = 0; index < 4; ++index) {
        const auto& drive = m_drives[index];
        nlohmann::json savedDrive = {{"enabled", drive.enabled},
                                     {"hasDisk", drive.hasDisk},
                                     {"diskName", drive.diskName},
                                     {"sourcePath", drive.sourcePath},
                                     {"fileExt", drive.fileExt}};
        if (drive.hasDisk && !drive.pendingData.empty()) {
            const std::string extension = "." + (drive.fileExt.empty() ? "adf" : drive.fileExt);
            const std::string mediaName = "df" + std::to_string(index) + extension;
            if (!WriteBinaryFile(stagingPath / mediaName, drive.pendingData, outError)) {
                discardStaging();
                return false;
            }
            savedDrive["media"] = PathToUtf8(assetsRelative / mediaName);
        }
        profile["drives"].push_back(std::move(savedDrive));
    }

    // Keep the previous media until the new profile is safely written.
    const auto retiredPath = directory / (name + ".assets.retired");
    std::filesystem::remove_all(retiredPath, ec);
    if (assetsExist) {
        std::filesystem::rename(assetsPath, retiredPath, ec);
        if (ec) {
            outError = "Unable to replace the existing configuration assets: " + ec.message();
            discardStaging();
            return false;
        }
    }
    auto restorePreviousAssets = [&]() {
        std::error_code rollback;
        std::filesystem::remove_all(assetsPath, rollback);
        if (assetsExist)
            std::filesystem::rename(retiredPath, assetsPath, rollback);
    };
    std::filesystem::rename(stagingPath, assetsPath, ec);
    if (ec) {
        outError = "Unable to store the configuration assets: " + ec.message();
        restorePreviousAssets();
        discardStaging();
        return false;
    }
    if (!core::UaeConfigBuilder::WriteAtomic(profilePath, profile.dump(2) + "\n", outError)) {
        restorePreviousAssets();
        return false;
    }
    std::filesystem::remove_all(retiredPath, ec);
    m_selectedCustomConfigurationName = name;
    return true;
}

bool EmulatorView::SaveCustomConfiguration(const std::string& name, std::string& outError)
{
    return WriteCustomConfiguration(name, /*allowOverwrite=*/false, outError);
}

bool EmulatorView::UpdateCustomConfiguration(const std::string& configPath, std::string& outError)
{
    outError.clear();
    const auto path = PathFromUtf8(configPath);
    const auto& directory = CustomConfigurationDirectory();
    if (!IsPathWithin(path, directory) ||
        path.parent_path().lexically_normal() != directory.lexically_normal() ||
        path.extension() != ".json") {
        outError = "The selected custom configuration path is invalid.";
        return false;
    }
    return WriteCustomConfiguration(path.stem().string(), /*allowOverwrite=*/true, outError);
}

bool EmulatorView::LoadCustomConfiguration(const std::string& configPath, std::string& outError)
{
    outError.clear();
    const auto path = PathFromUtf8(configPath);
    const auto& directory = CustomConfigurationDirectory();
    if (!IsPathWithin(path, directory) || path.extension() != ".json") {
        outError = "The selected custom configuration path is invalid.";
        return false;
    }

    nlohmann::json profile;
    try {
        std::ifstream stream(path);
        if (!stream) {
            outError = "Unable to open the custom configuration.";
            return false;
        }
        stream >> profile;
        if (profile.value("version", 0) != 1)
            throw std::runtime_error("unsupported configuration version");
    } catch (const std::exception& error) {
        outError = std::string("Unable to read the custom configuration: ") + error.what();
        return false;
    }

    if (IsRunning())
        Stop();
    m_emulator->ClearUaeConfig();
    m_selectedBaseUaeConfig.reset();
    m_selectedCustomConfigurationName.clear();

    const std::string modelId = core::jsonread::ReadOr(profile, "modelId", "A500");
    const auto& models = adm::GetAllAmigaModels();
    auto model = std::find_if(models.begin(), models.end(),
                              [&](const auto& candidate) { return modelId == candidate.id; });
    if (model == models.end()) {
        outError = "The custom configuration refers to an unknown Amiga model.";
        return false;
    }
    m_emulator->SetModel(model->model);

    std::unordered_map<std::string, std::string> variables;
    if (profile.contains("userVariables") && profile["userVariables"].is_object())
        for (const auto& [key, value] : profile["userVariables"].items())
            if (value.is_string())
                variables[key] = value.get<std::string>();
    core::UserPreferences::MigrateLegacyFloppySound(variables);
    core::UserPreferences::DropOptionsTheCoreDoesNotHave(variables);
    m_emulator->SetUserVariables(variables);
    if (m_userPreferences)
        m_userPreferences->SetEmulatorSettings(
            std::map<std::string, std::string>(variables.begin(), variables.end()));

    std::array<core::HostDirectoryMount, 2> mounts{};
    for (size_t index = 0; index < mounts.size(); ++index) {
        mounts[index].device = static_cast<core::HostMountDevice>(index);
        mounts[index].volumeLabel = core::HostMountDeviceName(mounts[index].device);
        if (profile.contains("hostMounts") && profile["hostMounts"].is_array() &&
            index < profile["hostMounts"].size()) {
            const auto& saved = profile["hostMounts"][index];
            mounts[index].enabled = core::jsonread::ReadOr(saved, "enabled", false);
            mounts[index].hostDirectory = PathFromUtf8(core::jsonread::ReadOr(saved, "path", ""));
            mounts[index].volumeLabel =
                core::jsonread::ReadOr(saved, "volumeLabel", mounts[index].volumeLabel);
            mounts[index].readOnly = core::jsonread::ReadOr(saved, "readOnly", true);
            mounts[index].bootPriority = core::jsonread::ReadOr(saved, "bootPriority", -128);
            core::ReadHardfileFields(saved, mounts[index]);
        }
    }
    SetConfiguredHostMounts(mounts);
    if (profile.contains("hardDriveControllerRoms"))
        SetHardDriveControllerRoms(
            core::ControllerRomsFromJson(profile["hardDriveControllerRoms"]));

    for (int index = 0; index < 4; ++index) {
        EjectDrive(index);
        if (!profile.contains("drives") || !profile["drives"].is_array() ||
            static_cast<size_t>(index) >= profile["drives"].size())
            continue;
        const auto& saved = profile["drives"][index];
        m_drives[index].enabled = core::jsonread::ReadOr(saved, "enabled", index < 2);
        m_drives[index].diskName = core::jsonread::ReadOr(saved, "diskName", "");
        m_drives[index].sourcePath = core::jsonread::ReadOr(saved, "sourcePath", "");
        m_drives[index].fileExt = core::jsonread::ReadOr(saved, "fileExt", "adf");
        const std::string media = core::jsonread::ReadOr(saved, "media", "");
        if (!core::jsonread::ReadOr(saved, "hasDisk", false) || media.empty())
            continue;
        const auto mediaPath = directory / PathFromUtf8(media);
        if (!IsPathWithin(mediaPath, directory)) {
            outError = "The custom configuration contains an invalid media path.";
            return false;
        }
        std::ifstream stream(mediaPath, std::ios::binary);
        if (!stream) {
            outError = "Unable to open saved media for DF" + std::to_string(index) + ".";
            return false;
        }
        m_drives[index].pendingData.assign(std::istreambuf_iterator<char>(stream),
                                           std::istreambuf_iterator<char>());
        m_drives[index].hasDisk = true;
    }

    bool started = false;
    const std::string baseConfig = core::jsonread::ReadOr(profile, "baseConfig", "");
    if (!baseConfig.empty()) {
        const auto basePath = directory / PathFromUtf8(baseConfig);
        if (!IsPathWithin(basePath, directory)) {
            outError = "The custom configuration contains an invalid base configuration path.";
            return false;
        }
        started = LoadUaeConfig(PathToUtf8(basePath));
    } else {
        started = PowerOn();
    }
    if (!started) {
        outError = "The emulator could not start with the custom configuration.";
        return false;
    }
    m_selectedCustomConfigurationName =
        core::jsonread::ReadOr(profile, "name", path.stem().string());
    return true;
}

bool EmulatorView::DeleteCustomConfiguration(const std::string& configPath, std::string& outError)
{
    outError.clear();
    const auto path = PathFromUtf8(configPath);
    const auto& directory = CustomConfigurationDirectory();
    if (!IsPathWithin(path, directory) ||
        path.parent_path().lexically_normal() != directory.lexically_normal() ||
        path.extension() != ".json") {
        outError = "The selected custom configuration path is invalid.";
        return false;
    }
    if (m_selectedCustomConfigurationName == path.stem().string()) {
        outError = "Switch to another configuration before deleting the active one.";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::remove(path, ec) || ec) {
        outError = "Unable to delete the custom configuration";
        if (ec)
            outError += ": " + ec.message();
        return false;
    }
    const auto assetsPath = directory / (path.stem().string() + ".assets");
    std::filesystem::remove_all(assetsPath, ec);
    if (ec) {
        outError = "The configuration was deleted, but its saved media could not be removed: " +
                   ec.message();
        return false;
    }
    return true;
}

bool EmulatorView::RenameCustomConfiguration(const std::string& configPath,
                                             const std::string& newName, std::string& outError)
{
    outError.clear();
    const auto sourcePath = PathFromUtf8(configPath);
    const auto& directory = CustomConfigurationDirectory();
    if (!IsPathWithin(sourcePath, directory) ||
        sourcePath.parent_path().lexically_normal() != directory.lexically_normal() ||
        sourcePath.extension() != ".json") {
        outError = "The selected custom configuration path is invalid.";
        return false;
    }
    if (!IsValidConfigurationName(newName)) {
        outError = "Use a non-empty filename without < > : \" / \\ | ? * characters.";
        return false;
    }

    const std::string oldName = sourcePath.stem().string();
    if (newName == oldName)
        return true;
    const auto targetPath = directory / (newName + ".json");
    const auto sourceAssets = directory / (oldName + ".assets");
    const auto targetAssets = directory / (newName + ".assets");
    std::error_code ec;
    if (std::filesystem::exists(targetPath, ec) || std::filesystem::exists(targetAssets, ec)) {
        outError = "A custom configuration with that name already exists.";
        return false;
    }

    nlohmann::json profile;
    try {
        std::ifstream stream(sourcePath);
        if (!stream) {
            outError = "Unable to open the custom configuration.";
            return false;
        }
        stream >> profile;
        if (profile.value("version", 0) != 1)
            throw std::runtime_error("unsupported configuration version");
    } catch (const std::exception& error) {
        outError = std::string("Unable to read the custom configuration: ") + error.what();
        return false;
    }

    const bool hasAssets = std::filesystem::exists(sourceAssets, ec);
    if (ec) {
        outError = "Unable to inspect the configuration assets: " + ec.message();
        return false;
    }
    if (hasAssets) {
        std::filesystem::rename(sourceAssets, targetAssets, ec);
        if (ec) {
            outError = "Unable to rename the configuration assets: " + ec.message();
            return false;
        }
    }

    profile["name"] = newName;
    auto renameAssetReference = [&](nlohmann::json& value) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty())
            return;
        const auto filename = PathFromUtf8(value.get<std::string>()).filename();
        value = PathToUtf8(std::filesystem::path(newName + ".assets") / filename);
    };
    if (profile.contains("baseConfig"))
        renameAssetReference(profile["baseConfig"]);
    if (profile.contains("drives") && profile["drives"].is_array()) {
        for (auto& drive : profile["drives"]) {
            if (drive.contains("media"))
                renameAssetReference(drive["media"]);
        }
    }

    if (!core::UaeConfigBuilder::WriteAtomic(targetPath, profile.dump(2) + "\n", outError)) {
        if (hasAssets)
            std::filesystem::rename(targetAssets, sourceAssets, ec);
        return false;
    }
    if (!std::filesystem::remove(sourcePath, ec) || ec) {
        std::error_code rollbackError;
        std::filesystem::remove(targetPath, rollbackError);
        if (hasAssets)
            std::filesystem::rename(targetAssets, sourceAssets, rollbackError);
        outError = "Unable to replace the old configuration";
        if (ec)
            outError += ": " + ec.message();
        return false;
    }

    if (m_selectedCustomConfigurationName == oldName) {
        if (IsRunning()) {
            if (!LoadCustomConfiguration(PathToUtf8(targetPath), outError)) {
                outError = "The configuration was renamed, but could not be reloaded: " + outError;
                return false;
            }
        } else {
            m_selectedCustomConfigurationName = newName;
            m_emulator->ClearUaeConfig();
            m_selectedBaseUaeConfig.reset();
            const std::string baseConfig = profile.value("baseConfig", "");
            if (!baseConfig.empty())
                m_selectedBaseUaeConfig = directory / PathFromUtf8(baseConfig);
        }
    }
    return true;
}

void EmulatorView::SetConfiguredHostMounts(const std::array<core::HostDirectoryMount, 2>& mounts,
                                           bool persist)
{
    m_selectedCustomConfigurationName.clear();
    m_emulator->GetHostMountService().SetConfiguredMounts(mounts);
    if (std::any_of(mounts.begin(), mounts.end(), [](const auto& mount) { return mount.enabled; }))
        m_hardDriveEditorOpen = true;
    if (persist && m_userPreferences)
        m_userPreferences->SetHostDirectoryMounts(mounts);
}

void EmulatorView::SetHardDriveControllerRoms(const core::HardDriveControllerRoms& roms,
                                              bool persist)
{
    m_emulator->GetHostMountService().SetControllerRoms(roms);
    if (persist && m_userPreferences)
        m_userPreferences->SetHardDriveControllerRoms(roms);
}

const std::array<core::HostDirectoryMount, 2>& EmulatorView::GetConfiguredHostMounts() const
{
    return m_emulator->GetHostMountService().GetConfiguredMounts();
}

bool EmulatorView::ApplyHostMountsAndRestart(std::string& outError)
{
    if (!StartHostMountProfile(m_emulator->CreateLaunchProfile(m_selectedBaseUaeConfig), outError))
        return false;
    PersistNormalizedHostMounts();
    return true;
}

bool EmulatorView::StartHostMountProfile(const core::EmulatorLaunchProfile& profile,
                                         std::string& outError)
{
    if (!IsCoreLoaded() && !m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath())) {
        outError = "failed to load the bundled PuAE core";
        return false;
    }
    Open();
    if (!m_emulator->StartHostMountProfile(profile, outError))
        return false;
    if (std::any_of(profile.mounts.begin(), profile.mounts.end(),
                    [](const auto& mount) { return mount.enabled; }))
        m_hardDriveEditorOpen = true;
    FlushPendingDisks();
    m_isPaused = false;
    if (m_onEmulatorReset)
        m_onEmulatorReset();
    return true;
}

bool EmulatorView::LaunchWhdload(const std::filesystem::path& input, std::string& outError)
{
    outError.clear();
    if (!IsCoreLoaded() && !m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath())) {
        outError = "failed to load the bundled PuAE core";
        return false;
    }
    const auto source = core::WhdloadLaunch::Resolve(input, outError);
    if (!source)
        return false;

    const auto settings = GetWhdloadSettings();
    // The core reads this one itself at unload, to decide whether to press WHDLoad's QuitKey
    // so buffered saves reach WHDSaves:. Keep it matching what WHDLoad.prefs is told.
    m_emulator->SetUserVariable("puae_use_whdload_nowritecache",
                                settings.noWriteCache ? "enabled" : "disabled");
    adm::AmigaEmulator::WhdloadPayload payload;
    if (!m_emulator->PrepareWhdloadPayload(settings.buttonWait, settings.noWriteCache,
                                           settings.prefsMode, settings.theme, payload, outError))
        return false;

    // The core's own Kickstart copy (whdload_kscopy) has been seen to leave Devs/Kickstarts with
    // only the .RTB tables, so a slave stops with "object not found" on kick34005.A500. Put the
    // images there directly; this never overwrites what the core did manage to copy.
    std::vector<std::string> missingKickstarts;
    const int kickstarts = core::WhdloadLaunch::InstallKickstarts(
        payload.whdloadDirectory, PathFromUtf8(m_emulator->GetSystemDirectory()),
        &missingKickstarts);
    adm::core::Logger::Info(
        "WHDLoad: {} Kickstart image(s) in Devs:Kickstarts{}", kickstarts,
        missingKickstarts.empty()
            ? std::string()
            : fmt::format(" (not found: {})", fmt::join(missingKickstarts, ", ")));

    const auto saves = WhdloadSavesDirectory();
    std::error_code ec;
    std::filesystem::create_directories(saves, ec);
    if (ec) {
        outError = "cannot create the WHDSaves directory '" + saves.string() + "': " + ec.message();
        return false;
    }

    auto profile =
        core::WhdloadLaunch::BuildProfile(*source, payload.whdloadDirectory, saves,
                                          m_emulator->CreateLaunchProfile(m_selectedBaseUaeConfig));
    if (!StartHostMountProfile(profile, outError))
        return false;
    if (m_userPreferences)
        m_userPreferences->AddRecentFile(source->input.string());
    adm::core::Logger::Info("WHDLoad: launched '{}' ({}) on DH0", source->input.string(),
                            source->isArchive ? "archive" : "directory");
    return true;
}

bool EmulatorView::RunHardfile(const std::filesystem::path& image, std::string& outError)
{
    outError.clear();
    if (!IsCoreLoaded() && !m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath())) {
        outError = "failed to load the bundled PuAE core";
        return false;
    }
    std::error_code ec;
    if (image.empty() || !std::filesystem::is_regular_file(image, ec)) {
        outError = "the hardfile is not on disk: " + PathToUtf8(image);
        return false;
    }

    // Built the way a WHDLoad launch is: from the configured profile, so DH1, the controller
    // ROMs and any base config survive, with DH0 replaced for this run. A fresh descriptor, not
    // the saved slot's: a controller, unit or geometry left over from another image has no
    // business here. Read-only, because the document is open in the app and an image the guest
    // writes would lock it; UAE's own hardfile device, because every model has one.
    auto profile = m_emulator->CreateLaunchProfile(m_selectedBaseUaeConfig);
    profile.transient = true;
    profile.minimalBootRom = false;
    profile.bootIntent = core::EmulatorBootIntent::Directory;
    auto& boot = profile.mounts[0];
    boot = core::HostDirectoryMount{};
    boot.device = core::HostMountDevice::DH0;
    boot.enabled = true;
    boot.backingType = core::HostMountBackingType::HardfileImage;
    boot.hardfilePath = image;
    boot.volumeLabel = core::HostMountDeviceName(boot.device);
    boot.readOnly = true;
    boot.bootPriority = 0;
    boot.source = core::HostMountSource::TransientDocument;
    auto& other = profile.mounts[1];
    if (other.enabled) {
        other.bootPriority = -128; // the document boots, whatever DH1 is set to
        other.source = core::HostMountSource::TransientDocument;
    }
    if (!StartHostMountProfile(profile, outError))
        return false;
    adm::core::Logger::Info("Emulator: booting hardfile document '{}' from DH0 (read-only, "
                            "transient)",
                            PathToUtf8(image));
    return true;
}

void EmulatorView::DrawCdDriveMenu()
{
    const auto status = GetCdStatus();
    std::string name =
        status.source.empty() ? std::string("[Empty]") : status.source.filename().string();
    if (status.extracting)
        name = "[extracting] " + name;
    const std::string label = fmt::format("{} CD0: {}###CdDriveMenu", ICON_FA_COMPACT_DISC, name);
    if (!ImGui::BeginMenu(label.c_str()))
        return;
    if (ImGui::MenuItem("CD Drive..."))
        m_cdDriveWindowOpen = true;
    ImGui::Separator();
    ImGui::BeginDisabled(status.extracting);
    if (ImGui::MenuItem("Insert Disc..."))
        PickCdImage();
    ImGui::EndDisabled();
    if (status.extracting && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A disc archive is still being extracted.");
    ImGui::BeginDisabled(status.source.empty() && status.image.empty());
    if (ImGui::MenuItem("Eject"))
        EjectCd();
    ImGui::EndDisabled();
    ImGui::Separator();
    // Always the same rows, empty or not, so the menu does not change shape as a disc goes in.
    ImGui::TextDisabled("Image:   %s", status.image.empty() ? "-" : status.image.string().c_str());
    ImGui::TextDisabled("Machine: %s", !IsRunning()               ? "powered off"
                                       : !status.hasDrive         ? "no CD drive until restart"
                                       : status.inMachine.empty() ? "drive empty"
                                                                  : "disc in drive");
    if (status.restartPending)
        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f),
                           "This machine started without a CD drive; the disc goes in at the "
                           "next power-on.");
    if (!status.lastError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", status.lastError.c_str());
    ImGui::EndMenu();
}

namespace {

bool IsCdMediaPath(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext == ".cue" || ext == ".ccd" || ext == ".mds" || ext == ".nrg" || ext == ".chd" ||
           ext == ".iso" || ext == ".7z" || ext == ".zip";
}

} // namespace

void EmulatorView::DrawCdDriveWindow()
{
    ImGui::SetNextWindowSize(ImVec2(640, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_COMPACT_DISC " CD Drive###CD Drive", &m_cdDriveWindowOpen)) {
        ImGui::End();
        return;
    }

    const auto status = GetCdStatus();
    const bool running = IsRunning();
    const bool cdMachine = m_emulator->IsCdMachine();
    const bool hasDisc = !status.source.empty() || !status.image.empty();
    const bool fromArchive = !status.source.empty() && !status.image.empty() &&
                             status.source.lexically_normal() != status.image.lexically_normal();
    const ImVec4 amber(1.0f, 0.65f, 0.15f, 1.0f);
    const ImVec4 green(0.45f, 0.85f, 0.45f, 1.0f);
    const ImVec4 grey(0.55f, 0.55f, 0.55f, 1.0f);
    const ImVec4 red(1.0f, 0.35f, 0.35f, 1.0f);

    // Short state, used by the chip and repeated in words below.
    const char* state = "empty";
    ImVec4 stateColor = grey;
    if (!status.lastError.empty()) {
        state = "error";
        stateColor = red;
    } else if (status.extracting) {
        state = "extracting";
        stateColor = amber;
    } else if (running && !status.inMachine.empty()) {
        state = "in drive";
        stateColor = green;
    } else if (hasDisc) {
        state = "at power-on";
        stateColor = amber;
    }

    // One chip, the same strip as Floppy Drives and Hard Drives, so CD0 reads like its siblings.
    {
        DriveChip chip;
        chip.name = "CD0";
        // The drive is fitted: a CD32/CDTV always has one; any other machine gets one (over
        // SCSI) when it powers on with a disc chosen.
        chip.ledOn = status.hasDrive || (!running && (cdMachine || hasDisc));
        if (status.extracting || fromArchive) {
            chip.backingGlyph = ICON_FA_BOX_ARCHIVE;
            chip.backingColor = status.extracting ? amber : ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        } else if (hasDisc) {
            chip.backingGlyph = ICON_FA_COMPACT_DISC;
            chip.backingColor = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        } else {
            chip.backingGlyph = ICON_FA_MINUS;
            chip.backingColor = ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
        }
        chip.indicators = {
            {ICON_FA_ARROW_ROTATE_RIGHT, status.led != 0},
            {ICON_FA_COMPACT_DISC, running ? !status.inMachine.empty() : hasDisc},
        };
        chip.subtitle = state;
        chip.subtitleColor = stateColor;
        chip.tooltip = fmt::format(
            "CD0 - {}\n{}\n{}",
            status.source.empty() ? std::string("no disc") : PathToUtf8(status.source),
            status.led ? "Reading" : "Idle",
            !running          ? "Machine powered off"
            : status.hasDrive ? (status.inMachine.empty() ? "Drive empty" : "Disc in the drive")
                              : "This machine has no CD drive until it restarts");
        DrawDriveChips("CdDriveChips", {chip}, 0);
        ImGui::Separator();
        ImGui::Spacing();
    }

    // The disc by name, in the state colour, so what is loaded reads at a glance.
    if (hasDisc) {
        const auto& named = status.source.empty() ? status.image : status.source;
        ImGui::TextColored(stateColor, ICON_FA_COMPACT_DISC " %s",
                           PathToUtf8(named.stem()).c_str());
    } else {
        ImGui::TextDisabled(ICON_FA_COMPACT_DISC " No disc");
    }
    ImGui::Spacing();

    const float labelColumn = ImGui::CalcTextSize("In the machine").x + 24.0f;
    const auto rowLabel = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelColumn);
    };

    rowLabel("Disc");
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float buttonsWidth = ImGui::CalcTextSize("Insert...").x +
                                   ImGui::CalcTextSize(ICON_FA_EJECT " Eject").x +
                                   style.FramePadding.x * 4.0f + style.ItemSpacing.x * 2.0f;
        const float dropDownWidth = ImGui::GetFrameHeight();
        const float pathWidth =
            std::max(ImGui::GetFrameHeight() * 2.0f,
                     ImGui::GetContentRegionAvail().x - buttonsWidth - dropDownWidth);
        std::string sourceText = PathToUtf8(status.source);
        ImGui::SetNextItemWidth(pathWidth);
        ImGui::InputTextWithHint("##CdSource", "no disc chosen", &sourceText,
                                 ImGuiInputTextFlags_ReadOnly);
        if (!sourceText.empty())
            ImGui::SetItemTooltip("%s", sourceText.c_str());
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginDisabled(status.extracting);
        if (ImGui::ArrowButton("##CdRecents", ImGuiDir_Down))
            ImGui::OpenPopup("##CdRecentsPopup");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(status.extracting ? "A disc archive is still being extracted."
                                                : "Recent discs");
        ImGui::SetNextWindowSizeConstraints(ImVec2(pathWidth, 0.0f), ImVec2(pathWidth, 240.0f));
        if (ImGui::BeginPopup("##CdRecentsPopup")) {
            std::vector<std::string> recents;
            if (m_userPreferences)
                for (const auto& recent : m_userPreferences->Get().recentFiles)
                    if (IsCdMediaPath(recent))
                        recents.push_back(recent);
            if (recents.empty())
                ImGui::TextDisabled("No recent discs");
            for (const auto& recent : recents) {
                if (ImGui::Selectable(recent.c_str())) {
                    std::string error;
                    if (!InsertCd(PathFromUtf8(recent), error)) {
                        m_cdError = error;
                        adm::core::Logger::Error("CD: {}", error);
                    }
                }
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(status.extracting);
        if (ImGui::Button("Insert..."))
            PickCdImage();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(status.extracting
                                  ? "A disc archive is still being extracted."
                                  : "A .cue/.ccd/.mds/.nrg/.chd/.iso, or a .7z/.zip holding one "
                                    "(extracted into the disc cache first).");
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasDisc && !status.extracting);
        if (ImGui::Button(ICON_FA_EJECT " Eject"))
            EjectCd();
        ImGui::EndDisabled();
        if (!hasDisc && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The drive is empty.");
    }

    rowLabel("Image");
    if (status.extracting) {
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.0f, 0.0f),
                           "Extracting into the disc cache...");
    } else if (status.image.empty()) {
        ImGui::TextDisabled("-");
    } else {
        ImGui::TextUnformatted(PathToUtf8(status.image.filename()).c_str());
        if (fromArchive)
            ImGui::SetItemTooltip("%s\nExtracted from the archive into the disc cache.",
                                  PathToUtf8(status.image).c_str());
        else
            ImGui::SetItemTooltip("%s", PathToUtf8(status.image).c_str());
    }

    rowLabel("In the machine");
    if (!running) {
        ImGui::TextDisabled("Powered off%s", hasDisc ? " - the disc goes in at power-on" : "");
    } else if (!status.hasDrive) {
        ImGui::TextColored(hasDisc ? amber : grey, "No CD drive%s",
                           hasDisc ? " - the disc goes in at the next power-on" : "");
    } else if (status.inMachine.empty()) {
        ImGui::TextColored(grey, "Drive empty");
    } else {
        ImGui::TextColored(green, "%s",
                           PathToUtf8(PathFromUtf8(status.inMachine).filename()).c_str());
        ImGui::SetItemTooltip("%s", status.inMachine.c_str());
    }

    rowLabel("Machine");
    ImGui::TextWrapped("%s - %s", adm::GetAmigaModelInfo(m_emulator->GetCurrentModel()).id,
                       cdMachine ? "CD drive built in"
                                 : "a CD drive is fitted when it powers on with a disc");
    ImGui::SetItemTooltip("%s", adm::GetAmigaModelInfo(m_emulator->GetCurrentModel()).displayName);

    rowLabel("Activity");
    {
        const float radius = ImGui::GetTextLineHeight() * 0.4f;
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 center(cursor.x + radius, cursor.y + ImGui::GetFrameHeight() * 0.5f);
        // Same lamp as the status bar: dim when idle, green while the drive reads.
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddCircleFilled(center, radius,
                                  status.led ? IM_COL32(0, 255, 0, 255) : IM_COL32(0, 60, 0, 255));
        drawList->AddCircle(center, radius, IM_COL32(80, 80, 80, 255), 0, 1.0f);
        ImGui::Dummy(ImVec2(radius * 2.0f, 0.0f));
        ImGui::SameLine();
        ImGui::TextDisabled(status.led ? "reading" : "idle");
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!status.restartPending);
    if (ImGui::Button("Restart with Disc")) {
        if (!ApplyHostMountsAndRestart(m_cdError))
            adm::core::Logger::Error("CD: {}", m_cdError);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(status.restartPending
                              ? "Cold-starts this machine with a CD drive and the disc, and the "
                                "saved hard drives (a WHDLoad launch ends)."
                              : "Only needed when a disc is chosen for a machine that started "
                                "without a CD drive.");
    if (status.restartPending) {
        ImGui::SameLine();
        ImGui::TextColored(amber, "This machine started without a CD drive.");
    }
    if (!status.lastError.empty())
        ImGui::TextColored(red, "%s", status.lastError.c_str());

    ImGui::End();
}

void EmulatorView::DrawWhdloadSettingsMenu()
{
    if (!ImGui::BeginMenu("WHDLoad Settings"))
        return;
    auto settings = GetWhdloadSettings();
    bool changed = false;
    changed |= ImGui::MenuItem("Button wait", nullptr, &settings.buttonWait);
    ImGui::SetItemTooltip("Let slaves wait for a button on their loading and title screens.");
    changed |= ImGui::MenuItem("No write cache", nullptr, &settings.noWriteCache);
    ImGui::SetItemTooltip("Write saved games to WHDSaves: at once. Off buffers them until "
                          "WHDLoad's quit key, which stopping the machine never presses.");
    ImGui::SeparatorText("Before the game starts");
    const char* modes[] = {"Nothing", "Configuration window", "Splash window", "Both"};
    for (int mode = 0; mode < 4; ++mode) {
        if (ImGui::MenuItem(modes[mode], nullptr, settings.prefsMode == mode)) {
            settings.prefsMode = mode;
            changed = true;
        }
    }
    ImGui::SeparatorText("Theme");
    if (ImGui::MenuItem("Default (black)", nullptr, settings.theme == 0)) {
        settings.theme = 0;
        changed = true;
    }
    if (ImGui::MenuItem("Native (grey)", nullptr, settings.theme == 1)) {
        settings.theme = 1;
        changed = true;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Applied at the next launch");
    if (changed)
        SetWhdloadSettings(settings);
    ImGui::EndMenu();
}

void EmulatorView::PickWhdloadArchive()
{
    nfdchar_t* selected = nullptr;
    nfdfilteritem_t filters[1] = {{"WHDLoad archive or slave", "lha,lzh,lzx,slave,info"}};
    const nfdresult_t result = NFD_OpenDialog(&selected, filters, 1, nullptr);
    ImGui::GetIO().ClearInputKeys();
    if (result != NFD_OKAY || !selected)
        return;
    std::string error;
    if (!LaunchWhdload(PathFromUtf8(selected), error)) {
        m_hostMountUiError = "WHDLoad: " + error;
        adm::core::Logger::Error("WHDLoad: {}", error);
    }
    NFD_FreePath(selected);
}

std::string EmulatorView::GetCopyToGuestUnavailableReason() const
{
    if (!IsRunning())
        return "Start the emulator first.";
    if (!m_emulator->GetHostMountService().DropTargetDevice())
        return "No writable directory is mounted. Give DH0 or DH1 a host directory in Hard "
               "Drives and turn off read-only.";
    return {};
}

void EmulatorView::PickFileToCopyToGuest()
{
    if (!GetCopyToGuestUnavailableReason().empty())
        return;

    nfdchar_t* selected = nullptr;
    // No filter: anything the guest might want, from an ADF to a text file.
    const nfdresult_t result = NFD_OpenDialog(&selected, nullptr, 0, nullptr);
    ImGui::GetIO().ClearInputKeys();
    if (result != NFD_OKAY || !selected)
        return;

    std::string error;
    const auto guestPath =
        m_emulator->GetHostMountService().CopyFileToGuest(PathFromUtf8(selected), error);
    NFD_FreePath(selected);
    if (!guestPath) {
        m_hostMountUiError = "Copy to guest: " + error;
        adm::core::Logger::Error("Copy to guest: {}", error);
        return;
    }
    // Said out loud: the file is on the volume now, but a directory window the guest already
    // has open will not show it until that window is refreshed.
    m_hostMountUiError = "Copied to " + *guestPath + ".";
}

core::WhdloadSettings EmulatorView::GetWhdloadSettings() const
{
    return m_userPreferences ? m_userPreferences->Get().whdload : core::WhdloadSettings{};
}

void EmulatorView::SetWhdloadSettings(const core::WhdloadSettings& settings)
{
    if (m_userPreferences)
        m_userPreferences->SetWhdloadSettings(settings);
}

std::optional<core::HostDirectoryMount> EmulatorView::GetActiveWhdloadGame() const
{
    const auto& effective = m_emulator->GetHostMountService().GetEffectiveProfile();
    if (!effective || effective->mounts[0].source != core::HostMountSource::TransientWhdload ||
        !effective->mounts[0].enabled)
        return std::nullopt;
    return effective->mounts[0];
}

std::filesystem::path EmulatorView::WhdloadSavesDirectory()
{
    std::error_code ec;
    auto base = std::filesystem::current_path(ec);
    return (ec ? std::filesystem::path(".") : base) / "whdsaves";
}

std::filesystem::path EmulatorView::CdCacheDirectory()
{
    std::error_code ec;
    auto base = std::filesystem::current_path(ec);
    return (ec ? std::filesystem::path(".") : base) / "cdcache";
}

bool EmulatorView::InsertCd(const std::filesystem::path& input, std::string& outError)
{
    outError.clear();
    if (m_cdExtraction && !m_cdExtraction->finished) {
        outError = "a disc archive is still being extracted";
        return false;
    }
    const auto resolved = core::CdImageResolver::Resolve(input, CdCacheDirectory(), outError);
    if (!resolved)
        return false;
    m_cdError.clear();
    if (!resolved->needsExtraction) {
        ApplyCdImage(resolved->original, resolved->image);
        return true;
    }

    if (!IsCoreLoaded() && !m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath())) {
        outError = "failed to load the bundled PuAE core";
        return false;
    }
    if (!m_emulator->HasArchiveExtraction()) {
        outError = "this PuAE core cannot extract archives (needs core 1.84.0 or later)";
        return false;
    }
    if (m_cdExtraction && m_cdExtraction->worker.joinable())
        m_cdExtraction->worker.join();
    auto job = std::make_unique<CdExtractionJob>();
    job->archive = resolved->original;
    job->directory = resolved->extractDirectory;
    job->staging = resolved->extractDirectory;
    job->staging += "~";
    std::error_code ec;
    std::filesystem::remove_all(job->staging, ec);
    std::filesystem::create_directories(job->staging, ec);
    m_cdSource = resolved->original;
    adm::core::Logger::Info("CD: extracting '{}' into '{}'", job->archive.string(),
                            job->directory.string());
    auto* raw = job.get();
    const auto* emulator = m_emulator.get();
    job->worker = std::thread([raw, emulator]() {
        // The core's archivers are self-contained; nothing here touches the running machine.
        raw->ok = emulator->ExtractArchive(raw->archive, raw->staging);
        raw->finished = true;
    });
    m_cdExtraction = std::move(job);
    return true;
}

void EmulatorView::PollCdExtraction()
{
    if (!m_cdExtraction || !m_cdExtraction->finished || !m_cdExtraction->worker.joinable())
        return;
    m_cdExtraction->worker.join();
    auto job = std::move(m_cdExtraction);
    std::error_code ec;
    const auto image = core::CdImageResolver::FindImage(job->staging);
    if (!job->ok || !image) {
        std::filesystem::remove_all(job->staging, ec);
        if (job->cancelled) // the drive has moved on; this disc's failure is no news
            return;
        m_cdError = job->ok ? "the archive holds no disc image (.cue, .ccd, .mds, .nrg, .chd, .iso)"
                            : "the archive could not be extracted";
        adm::core::Logger::Error("CD: '{}': {}", job->archive.string(), m_cdError);
        m_cdSource.clear();
        return;
    }
    // Only a complete extraction takes the cache name, so a half-written one is never reused.
    std::filesystem::remove_all(job->directory, ec);
    std::filesystem::rename(job->staging, job->directory, ec);
    if (ec) {
        m_cdError = "could not move the extracted disc into the cache: " + ec.message();
        adm::core::Logger::Error("CD: {}", m_cdError);
        return;
    }
    core::CdImageResolver::WriteExtractionMarker(job->archive, job->directory);
    const auto cached = core::CdImageResolver::FindImage(job->directory);
    adm::core::Logger::Info("CD: extracted '{}'", job->archive.string());
    if (job->cancelled) {
        adm::core::Logger::Info("CD: '{}' was ejected while it extracted; left out of the drive",
                                job->archive.string());
        return;
    }
    if (cached)
        ApplyCdImage(job->archive, *cached);
}

void EmulatorView::ApplyCdImage(const std::filesystem::path& source,
                                const std::filesystem::path& image)
{
    m_cdSource = source;
    m_emulator->SetConfiguredCdImage(image);
    if (m_userPreferences) {
        m_userPreferences->SetCdImagePath(PathToUtf8(source));
        m_userPreferences->AddRecentFile(PathToUtf8(source));
    }
    if (IsRunning()) {
        if (m_emulator->HasCdDrive() && m_emulator->HasCdControl()) {
            if (m_emulator->InsertCd(PathToUtf8(image)))
                adm::core::Logger::Info("CD: inserted '{}'", image.string());
        } else {
            adm::core::Logger::Info("CD: this machine started without a CD drive; '{}' goes in at "
                                    "the next power-on",
                                    image.string());
        }
    }
}

void EmulatorView::EjectCd()
{
    if (m_cdExtraction) // the disc being extracted is ejected too
        m_cdExtraction->cancelled = true;
    m_cdSource.clear();
    m_cdError.clear();
    m_emulator->SetConfiguredCdImage({});
    if (m_userPreferences)
        m_userPreferences->SetCdImagePath({});
    if (IsRunning() && m_emulator->HasCdControl())
        m_emulator->EjectCd();
}

void EmulatorView::RestoreCdImage(const std::string& source)
{
    if (source.empty())
        return;
    if (m_cdExtraction) // another disc: the one being extracted does not go in after it
        m_cdExtraction->cancelled = true;
    std::string error;
    const auto resolved =
        core::CdImageResolver::Resolve(PathFromUtf8(source), CdCacheDirectory(), error);
    if (!resolved || resolved->needsExtraction) {
        // Not re-extracted behind the user's back at startup; the disc just is not there.
        adm::core::Logger::Info("CD: last disc '{}' is not available without extracting it",
                                source);
        return;
    }
    m_cdSource = resolved->original;
    m_emulator->SetConfiguredCdImage(resolved->image);
}

void EmulatorView::PickCdImage()
{
    nfdchar_t* selected = nullptr;
    nfdfilteritem_t filters[1] = {{"CD image or archive of one", "cue,ccd,mds,nrg,chd,iso,7z,zip"}};
    const nfdresult_t result = NFD_OpenDialog(&selected, filters, 1, nullptr);
    ImGui::GetIO().ClearInputKeys();
    if (result != NFD_OKAY || !selected)
        return;
    std::string error;
    if (!InsertCd(PathFromUtf8(selected), error)) {
        m_cdError = error;
        adm::core::Logger::Error("CD: {}", error);
    }
    NFD_FreePath(selected);
}

EmulatorView::CdDriveStatus EmulatorView::GetCdStatus() const
{
    CdDriveStatus status;
    status.source = m_cdSource;
    status.image = m_emulator->GetConfiguredCdImage();
    status.extracting = m_cdExtraction && !m_cdExtraction->finished && !m_cdExtraction->cancelled;
    status.lastError = m_cdError;
    const bool running = IsRunning();
    status.hasDrive = running && m_emulator->HasCdDrive();
    status.inMachine = running ? m_emulator->GetCdImage() : std::string();
    status.restartPending = running && !status.hasDrive && !status.image.empty();
    status.led = running ? m_emulator->GetCdLed() : 0;
    return status;
}

bool EmulatorView::HasPendingHostMountRestart() const
{
    return m_emulator->GetHostMountService().HasPendingRestart();
}

void EmulatorView::PickHostDirectory(int deviceIndex)
{
    if (deviceIndex < 0 || deviceIndex > 1)
        return;
    auto mounts = GetConfiguredHostMounts();
    const std::string current = mounts[deviceIndex].hostDirectory.string();
    nfdchar_t* selected = nullptr;
    const nfdresult_t result =
        NFD_PickFolder(&selected, current.empty() ? nullptr : current.c_str());
    ImGui::GetIO().ClearInputKeys();
    if (result != NFD_OKAY || !selected)
        return;
    MountHostDirectory(deviceIndex, selected);
    NFD_FreePath(selected);
}

void EmulatorView::PickHardfile(int deviceIndex)
{
    if (deviceIndex < 0 || deviceIndex > 1)
        return;
    const auto& current = GetConfiguredHostMounts()[deviceIndex].hardfilePath;
    const std::string start = current.empty() ? std::string() : current.parent_path().string();
    nfdchar_t* selected = nullptr;
    nfdfilteritem_t filters[1] = {{"Amiga hardfile", "hdf,hdz"}};
    const nfdresult_t result =
        NFD_OpenDialog(&selected, filters, 1, start.empty() ? nullptr : start.c_str());
    ImGui::GetIO().ClearInputKeys();
    if (result != NFD_OKAY || !selected)
        return;
    MountHardfile(deviceIndex, selected);
    NFD_FreePath(selected);
}

void EmulatorView::MountHardfile(int deviceIndex, const std::string& path)
{
    if (deviceIndex < 0 || deviceIndex > 1 || path.empty())
        return;
    auto mounts = GetConfiguredHostMounts();
    auto& mount = mounts[deviceIndex];
    mount.backingType = core::HostMountBackingType::HardfileImage;
    mount.hardfilePath = PathFromUtf8(path);
    mount.enabled = true;
    if (m_userPreferences)
        m_userPreferences->AddRecentFile(path);
    SetConfiguredHostMounts(mounts);
    m_selectedHardDrive = deviceIndex;
    m_hardDriveEditorOpen = true;
}

void EmulatorView::MountHostDirectory(int deviceIndex, const std::string& path)
{
    if (deviceIndex < 0 || deviceIndex > 1 || path.empty())
        return;
    auto mounts = GetConfiguredHostMounts();
    mounts[deviceIndex].hostDirectory = PathFromUtf8(path);
    mounts[deviceIndex].enabled = true;
    if (mounts[deviceIndex].volumeLabel.empty())
        mounts[deviceIndex].volumeLabel = core::HostMountDeviceName(mounts[deviceIndex].device);
    if (m_userPreferences)
        m_userPreferences->AddRecentDirectory(path);
    SetConfiguredHostMounts(mounts);
    m_hardDriveEditorOpen = true;
}

namespace {

// One vocabulary for HostMountRuntimeState, shared by the chip subtitle (short) and the detail
// panel (verbose), so the two can never disagree about what a device is doing.
const char* HostMountStateShortLabel(core::HostMountRuntimeState state)
{
    switch (state) {
        case core::HostMountRuntimeState::Disabled:
            return "off";
        case core::HostMountRuntimeState::Configured:
            return "saved";
        case core::HostMountRuntimeState::PendingRestart:
            return "pending";
        case core::HostMountRuntimeState::Effective:
            return "live";
        case core::HostMountRuntimeState::Confirmed:
            return "mounted";
        case core::HostMountRuntimeState::Failed:
            return "failed";
    }
    return "";
}

const char* HostMountStateLabel(core::HostMountRuntimeState state)
{
    switch (state) {
        case core::HostMountRuntimeState::Disabled:
            return "Disabled";
        case core::HostMountRuntimeState::Configured:
            return "Configured";
        case core::HostMountRuntimeState::PendingRestart:
            return "Pending restart";
        case core::HostMountRuntimeState::Effective:
            return "Effective; awaiting guest";
        case core::HostMountRuntimeState::Confirmed:
            return "Mounted by guest";
        case core::HostMountRuntimeState::Failed:
            return "Failed";
    }
    return "";
}

ImVec4 HostMountStateColor(core::HostMountRuntimeState state)
{
    switch (state) {
        case core::HostMountRuntimeState::Disabled:
        case core::HostMountRuntimeState::Configured:
            return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
        case core::HostMountRuntimeState::PendingRestart:
            return ImVec4(1.0f, 0.65f, 0.15f, 1.0f);
        case core::HostMountRuntimeState::Effective:
            return ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
        case core::HostMountRuntimeState::Confirmed:
            return ImVec4(0.35f, 0.9f, 0.35f, 1.0f);
        case core::HostMountRuntimeState::Failed:
            return ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

} // namespace

void EmulatorView::DrawHardDriveEditor(bool embedded)
{
    if (!embedded) {
        ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(ICON_FA_HARD_DRIVE " Hard Drives###Hard Drives",
                          &m_hardDriveEditorOpen)) {
            ImGui::End();
            return;
        }
    }

    auto mounts = GetConfiguredHostMounts();
    const auto& service = m_emulator->GetHostMountService();
    bool changed = false;

    if (service.IsTransientOverrideActive()) {
        if (const auto game = GetActiveWhdloadGame()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f),
                               "WHDLoad is running '%s' on DH0. Saved settings are unchanged; "
                               "Apply and Restart returns to them.",
                               game->volumeLabel.c_str());
            ImGui::SetItemTooltip("%s", game->hostDirectory.string().c_str());
        } else if (const auto& booted = service.GetEffectiveMounts()[0];
                   booted.enabled && booted.source == core::HostMountSource::TransientDocument) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f),
                               "'%s' is booted from DH0 for this run. Saved settings are "
                               "unchanged; Apply and Restart returns to them.",
                               booted.hardfilePath.filename().string().c_str());
            ImGui::SetItemTooltip("%s", booted.hardfilePath.string().c_str());
        } else {
            ImGui::TextColored(
                ImVec4(1.0f, 0.65f, 0.15f, 1.0f),
                "Bartman currently overrides DH0/DH1. Saved settings are unchanged.");
        }
    }
    if (!service.GetLastError().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", service.GetLastError().c_str());
    }

    // One chip per device, the same strip the Floppy Drives window uses. DH0/DH1 have no
    // cylinder or motor, so they carry the indicators that mean something here: what backs the
    // drive, read-only vs read-write, and whether the setting is live or only saved.
    {
        std::vector<DriveChip> chips;
        chips.reserve(2);
        for (int i = 0; i < 2; ++i) {
            const auto& m = mounts[i];
            const auto state = service.GetRuntimeState(m.device);
            DriveChip chip;
            chip.name = core::HostMountDeviceName(m.device);
            chip.ledOn = m.enabled;
            chip.backingGlyph = m.IsHardfile() ? ICON_FA_HARD_DRIVE : ICON_FA_FOLDER;
            chip.backingColor =
                m.enabled ? ImVec4(0.85f, 0.85f, 0.85f, 1.0f) : ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
            chip.indicators = {{ICON_FA_LOCK, m.readOnly}};
            chip.subtitle = HostMountStateShortLabel(state);
            chip.subtitleColor = HostMountStateColor(state);
            const std::string where =
                m.IsHardfile()
                    ? fmt::format("Hardfile on {} unit {}",
                                  core::HardDriveControllerName(m.controller), m.controllerUnit)
                    : std::string("Host directory");
            chip.tooltip = fmt::format(
                "{} - {}\n{}\n{}\n{}", chip.name,
                m.HostPath().empty() ? std::string("nothing chosen") : m.HostPath().string(), where,
                m.readOnly ? "Read-only" : "Read-write (guest changes hit the host)",
                HostMountStateLabel(state));
            chips.push_back(std::move(chip));
        }
        const int clicked = DrawDriveChips("HardDriveChips", chips, m_selectedHardDrive);
        if (clicked >= 0)
            m_selectedHardDrive = clicked;
        ImGui::Separator();
        ImGui::Spacing();
    }

    {
        const int index =
            (m_selectedHardDrive >= 0 && m_selectedHardDrive < 2) ? m_selectedHardDrive : 0;
        auto& mount = mounts[index];
        ImGui::PushID(index);
        const float labelColumn = ImGui::CalcTextSize("Controller ROM").x + 24.0f;
        const auto rowLabel = [&](const char* label) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(labelColumn);
        };

        if (ImGui::Checkbox("Enable", &mount.enabled))
            changed = true;
        ImGui::SameLine();
        {
            const auto state = service.GetRuntimeState(mount.device);
            const bool unconfirmable = state == core::HostMountRuntimeState::Effective &&
                                       !core::HostMountService::IsGuestConfirmable(mount);
            ImGui::TextColored(HostMountStateColor(state), "%s",
                               unconfirmable ? "In the machine" : HostMountStateLabel(state));
            if (unconfirmable && ImGui::IsItemHovered())
                ImGui::SetTooltip("A drive on %s is attached to the emulated controller; the guest "
                                  "is not asked to confirm it.",
                                  core::HardDriveControllerName(mount.controller));
        }

        rowLabel("Backing");
        {
            int backing = mount.IsHardfile() ? 1 : 0;
            const char* backings[] = {"Host directory", "Hardfile image (.hdf, .hdz)"};
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::Combo("##Backing", &backing, backings, 2)) {
                mount.backingType = backing == 1 ? core::HostMountBackingType::HardfileImage
                                                 : core::HostMountBackingType::Directory;
                changed = true;
            }
        }

        const bool hardfile = mount.IsHardfile();
        std::filesystem::path& activePath = hardfile ? mount.hardfilePath : mount.hostDirectory;
        rowLabel(hardfile ? "Hardfile" : "Directory");
        {
            std::string hostPath = activePath.string();
            const ImGuiStyle& style = ImGui::GetStyle();
            const float browseWidth =
                ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.0f;
            const float pathControlWidth =
                std::max(ImGui::GetFrameHeight() * 2.0f,
                         ImGui::GetContentRegionAvail().x - browseWidth - style.ItemSpacing.x);
            const float dropDownWidth = ImGui::GetFrameHeight();

            ImGui::BeginGroup();
            ImGui::SetNextItemWidth(pathControlWidth - dropDownWidth);
            if (ImGui::InputText("##HostPath", &hostPath)) {
                activePath = PathFromUtf8(hostPath);
                changed = true;
            }
            ImGui::SameLine(0.0f, 0.0f);
            if (ImGui::ArrowButton("##HostPathRecents", ImGuiDir_Down))
                ImGui::OpenPopup("##HostPathRecentsPopup");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(hardfile ? "Recent hardfiles" : "Recent directories");

            ImGui::SetNextWindowSizeConstraints(ImVec2(pathControlWidth, 0.0f),
                                                ImVec2(pathControlWidth, 240.0f));
            if (ImGui::BeginPopup("##HostPathRecentsPopup")) {
                std::vector<std::string> recents;
                if (m_userPreferences) {
                    if (hardfile) {
                        for (const auto& recent : m_userPreferences->Get().recentFiles) {
                            std::string ext = std::filesystem::path(recent).extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(),
                                           [](unsigned char c) { return std::tolower(c); });
                            if (ext == ".hdf" || ext == ".hdz")
                                recents.push_back(recent);
                        }
                    } else {
                        recents = m_userPreferences->Get().recentDirectories;
                    }
                }
                if (recents.empty())
                    ImGui::TextDisabled(hardfile ? "No recent hardfiles" : "No recent directories");
                for (const auto& recent : recents) {
                    if (ImGui::Selectable(recent.c_str())) {
                        activePath = PathFromUtf8(recent);
                        changed = true;
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::EndGroup();

            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                nfdchar_t* selected = nullptr;
                const std::string current =
                    hardfile ? activePath.parent_path().string() : activePath.string();
                nfdresult_t result = NFD_ERROR;
                if (hardfile) {
                    nfdfilteritem_t filters[1] = {{"Amiga hardfile", "hdf,hdz"}};
                    result = NFD_OpenDialog(&selected, filters, 1,
                                            current.empty() ? nullptr : current.c_str());
                } else {
                    result = NFD_PickFolder(&selected, current.empty() ? nullptr : current.c_str());
                }
                ImGui::GetIO().ClearInputKeys();
                if (result == NFD_OKAY && selected) {
                    activePath = PathFromUtf8(selected);
                    if (m_userPreferences) {
                        if (hardfile)
                            m_userPreferences->AddRecentFile(selected);
                        else
                            m_userPreferences->AddRecentDirectory(selected);
                    }
                    NFD_FreePath(selected);
                    changed = true;
                }
            }
        }

        rowLabel("Volume label");
        ImGui::BeginDisabled(hardfile);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputText("##VolumeLabel", &mount.volumeLabel))
            changed = true;
        ImGui::EndDisabled();
        if (hardfile && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("A hardfile's volumes are named by its own filesystems.");

        rowLabel("Access");
        {
            std::string extension = mount.hardfilePath.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            const bool compressed = hardfile && extension == ".hdz";
            int access = (mount.readOnly || compressed) ? 0 : 1;
            const char* accessModes[] = {"Read-only", "Read-write"};
            ImGui::BeginDisabled(compressed);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("##Access", &access, accessModes, 2)) {
                if (access == 1 && mount.readOnly) {
                    m_rwWarningMount = index;
                } else {
                    mount.readOnly = true;
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            if (compressed && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("A .hdz is decompressed into memory, so writes could never "
                                  "reach the file.");
        }

        rowLabel("Boot");
        {
            bool bootable = mount.bootPriority > -128;
            if (ImGui::Checkbox("Bootable", &bootable)) {
                mount.bootPriority = bootable ? 0 : -128;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputInt("Priority", &mount.bootPriority)) {
                mount.bootPriority = std::clamp(mount.bootPriority, -128, 127);
                changed = true;
            }
        }

        // The hardfile settings are always laid out, disabled for a directory, so the window
        // does not reflow when the backing changes.
        ImGui::SeparatorText("Hardfile");
        ImGui::BeginDisabled(!hardfile);
        const std::string modelId = GetCurrentModelId();

        rowLabel("Controller");
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo("##Controller", core::HardDriveControllerName(mount.controller))) {
            for (auto controller :
                 {core::HardDriveController::Uae, core::HardDriveController::IdeMainboard,
                  core::HardDriveController::ScsiA590A2091, core::HardDriveController::ScsiA4091}) {
                const std::string reason =
                    core::UaeConfigBuilder::ControllerUnavailableReason(controller, modelId);
                ImGui::BeginDisabled(!reason.empty());
                if (ImGui::Selectable(core::HardDriveControllerName(controller),
                                      controller == mount.controller)) {
                    mount.controller = controller;
                    mount.controllerUnit =
                        std::min(mount.controllerUnit,
                                 core::UaeConfigBuilder::MaxControllerUnit(controller));
                    changed = true;
                }
                ImGui::EndDisabled();
                if (!reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Not available on this machine: %s.", reason.c_str());
            }
            ImGui::EndCombo();
        }
        {
            const std::string reason =
                core::UaeConfigBuilder::ControllerUnavailableReason(mount.controller, modelId);
            if (hardfile && !reason.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f), "%s",
                                   ICON_FA_TRIANGLE_EXCLAMATION);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The selected machine cannot take this controller: %s. The "
                                      "drive will not appear.",
                                      reason.c_str());
            }
        }
        ImGui::SameLine();
        const int maxUnit = core::UaeConfigBuilder::MaxControllerUnit(mount.controller);
        ImGui::BeginDisabled(maxUnit == 0);
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputInt(mount.controller == core::HardDriveController::IdeMainboard
                                ? "Unit (0 master, 1 slave)"
                                : "Unit / SCSI ID",
                            &mount.controllerUnit)) {
            mount.controllerUnit = std::clamp(mount.controllerUnit, 0, maxUnit);
            changed = true;
        }
        ImGui::EndDisabled();

        rowLabel("Controller ROM");
        {
            const bool card = mount.controller == core::HardDriveController::ScsiA590A2091 ||
                              mount.controller == core::HardDriveController::ScsiA4091;
            auto roms = service.GetControllerRoms();
            std::filesystem::path& rom = mount.controller == core::HardDriveController::ScsiA4091
                                             ? roms.a4091
                                             : roms.a590A2091;
            ImGui::BeginDisabled(!card);
            std::string romText = card ? rom.string() : std::string();
            ImGui::SetNextItemWidth(
                ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Browse...").x -
                ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
            bool romChanged = false;
            if (ImGui::InputTextWithHint("##ControllerRom",
                                         card ? "boot ROM file, e.g. amiga-boot-a590.rom"
                                              : "only the SCSI cards need a boot ROM",
                                         &romText)) {
                rom = PathFromUtf8(romText);
                romChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse...##Rom")) {
                nfdchar_t* selected = nullptr;
                nfdfilteritem_t filters[1] = {{"ROM image", "rom,bin"}};
                const nfdresult_t result = NFD_OpenDialog(&selected, filters, 1, nullptr);
                ImGui::GetIO().ClearInputKeys();
                if (result == NFD_OKAY && selected) {
                    rom = PathFromUtf8(selected);
                    NFD_FreePath(selected);
                    romChanged = true;
                }
            }
            ImGui::EndDisabled();
            if (!card && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("UAE and built-in IDE need no ROM.");
            if (romChanged)
                SetHardDriveControllerRoms(roms);
        }

        rowLabel("Geometry");
        {
            int mode = static_cast<int>(mount.geometry.mode);
            const char* modes[] = {"Automatic", "Read from RDB", "Manual"};
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("##GeometryMode", &mode, modes, 3)) {
                mount.geometry.mode = static_cast<core::HardfileGeometryMode>(mode);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Automatic uses the RDB when the image has one, otherwise "
                                  "32 sectors / 1 surface / 2 reserved / 512-byte blocks.");
            ImGui::BeginDisabled(mount.geometry.mode != core::HardfileGeometryMode::Manual);
            // The four values get their own row under the mode, lined up with the other fields,
            // so the window does not have to be wide enough for both.
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            bool firstField = true;
            const auto geometryField = [&](const char* label, int& value, int lo, int hi) {
                ImGui::SameLine(firstField ? labelColumn : 0.0f);
                firstField = false;
                ImGui::SetNextItemWidth(70.0f);
                if (ImGui::InputInt(label, &value, 0, 0)) {
                    value = std::clamp(value, lo, hi);
                    changed = true;
                }
            };
            geometryField("Sectors", mount.geometry.sectors, 1, 65535);
            geometryField("Surfaces", mount.geometry.surfaces, 1, 255);
            geometryField("Reserved", mount.geometry.reserved, 0, 1024);
            geometryField("Block", mount.geometry.blockSize, 512, 32768);
            ImGui::EndDisabled();
        }

        DrawHardfilePartitionTable(index, mount);
        ImGui::EndDisabled();

        ImGui::Spacing();
        const bool canOpen = mount.enabled && !mount.HostPath().empty() &&
                             (hardfile ? static_cast<bool>(m_onOpenHardfileDocument)
                                       : static_cast<bool>(m_onOpenHostDirectory));
        ImGui::BeginDisabled(!canOpen);
        if (ImGui::Button("Open Document")) {
            if (hardfile)
                m_onOpenHardfileDocument(mount.hardfilePath);
            else
                m_onOpenHostDirectory(mount);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            mount.enabled = false;
            mount.hostDirectory.clear();
            mount.hardfilePath.clear();
            mount.backingType = core::HostMountBackingType::Directory;
            mount.controller = core::HardDriveController::Uae;
            mount.controllerUnit = 0;
            mount.geometry = {};
            mount.volumeLabel = core::HostMountDeviceName(mount.device);
            mount.readOnly = true;
            mount.bootPriority = -128;
            changed = true;
        }
        ImGui::PopID();
    }

    if (changed)
        SetConfiguredHostMounts(mounts);

    if (m_rwWarningMount >= 0)
        ImGui::OpenPopup("Enable read-write host access?");
    if (ImGui::BeginPopupModal("Enable read-write host access?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool hardfileWarning = m_rwWarningMount >= 0 && m_rwWarningMount < 2 &&
                                     GetConfiguredHostMounts()[m_rwWarningMount].IsHardfile();
        ImGui::TextWrapped(hardfileWarning
                               ? "Guest writes go straight into the hardfile. Continue?"
                               : "Guest changes affect the real host directory immediately. "
                                 "Continue?");
        if (ImGui::Button("Enable Read-write", ImVec2(150, 0))) {
            if (m_rwWarningMount >= 0 && m_rwWarningMount < 2) {
                mounts = GetConfiguredHostMounts();
                mounts[m_rwWarningMount].readOnly = false;
                SetConfiguredHostMounts(mounts);
            }
            m_rwWarningMount = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0))) {
            m_rwWarningMount = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Always drawn, disabled when there is nothing to apply, rather than appearing and vanishing:
    // a control that is not there says nothing about why it is not there.
    {
        const bool pending = HasPendingHostMountRestart();
        ImGui::Separator();
        ImGui::BeginDisabled(!pending);
        // Auto-width: a fixed 150 px clipped the label to "Apply and Re" on a HiDPI display.
        if (ImGui::Button("Apply and Restart")) {
            if (!ApplyHostMountsAndRestart(m_hostMountUiError))
                adm::core::Logger::Error("Emulator: {}", m_hostMountUiError);
        }
        ImGui::EndDisabled();
        // A disabled item is not hovered by default, so the reason needs AllowWhenDisabled.
        if (!pending && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Nothing to apply - the running machine already matches these "
                              "settings.");
        ImGui::SameLine();
        if (pending)
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f),
                               "Mount changes require a cold restart.");
        else
            ImGui::TextDisabled("Applied.");
    }
    if (!m_hostMountUiError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_hostMountUiError.c_str());
    }

    if (!embedded)
        ImGui::End();
}

void EmulatorView::DrawHardfilePartitionTable(int slot, const core::HostDirectoryMount& mount)
{
    if (slot < 0 || slot > 1)
        return;
    auto& cache = m_hardfileInspection[static_cast<size_t>(slot)];
    if (mount.IsHardfile() && !mount.hardfilePath.empty()) {
        // Re-read only when the file itself changed. A stat per frame is cheap; parsing the RDB
        // every frame is not worth it.
        std::error_code timeError;
        std::error_code sizeError;
        const auto mtime = std::filesystem::last_write_time(mount.hardfilePath, timeError);
        const auto size = std::filesystem::file_size(mount.hardfilePath, sizeError);
        if (cache.path != mount.hardfilePath || cache.mtime != mtime || cache.size != size) {
            cache.path = mount.hardfilePath;
            cache.mtime = mtime;
            cache.size = sizeError ? 0 : size;
            cache.error.clear();
            cache.info = core::HardfileInspector::Inspect(mount.hardfilePath, cache.error);
        }
    } else if (!cache.path.empty() || cache.info) {
        cache = {};
    }

    const auto megabytes = [](uint64_t bytes) {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    };
    if (!mount.IsHardfile() || mount.hardfilePath.empty()) {
        ImGui::TextDisabled("Partitions: no hardfile chosen");
    } else if (!cache.info) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Cannot read the hardfile: %s",
                           cache.error.c_str());
    } else if (cache.info->hasRdb) {
        const auto& info = *cache.info;
        ImGui::Text("RDB at block %u: %u cylinders x %u heads x %u sectors, %.1f MB%s%s%s",
                    info.rdbBlock, info.cylinders, info.heads, info.sectors,
                    megabytes(info.sizeBytes), info.diskVendor.empty() ? "" : "  ",
                    info.diskVendor.c_str(),
                    info.diskProduct.empty() ? "" : (" " + info.diskProduct).c_str());
    } else {
        const auto& info = *cache.info;
        ImGui::Text("No RDB: a single %s volume, %.1f MB. Automatic geometry %d/%d/%d/%d.",
                    core::HardfileInspector::DosTypeText(info.rawDosType).c_str(),
                    megabytes(info.sizeBytes), info.guessSectors, info.guessSurfaces,
                    info.guessReserved, info.guessBlockSize);
    }
    if (cache.info) {
        for (const auto& warning : cache.info->warnings)
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.15f, 1.0f), "%s %s",
                               ICON_FA_TRIANGLE_EXCLAMATION, warning.c_str());
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginTable("##RdbPartitions", 6, flags, ImVec2(0.0f, rowHeight * 6.5f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Partition");
        ImGui::TableSetupColumn("Filesystem");
        ImGui::TableSetupColumn("Cylinders");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Boot");
        ImGui::TableSetupColumn("Flags");
        ImGui::TableHeadersRow();
        if (cache.info && mount.IsHardfile()) {
            for (const auto& partition : cache.info->partitions) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(partition.driveName.c_str());
                ImGui::TableNextColumn();
                const std::string description =
                    core::HardfileInspector::DosTypeDescription(partition.dosType);
                ImGui::Text("%s%s%s",
                            core::HardfileInspector::DosTypeText(partition.dosType).c_str(),
                            description.empty() ? "" : "  ", description.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u - %u", partition.lowCylinder, partition.highCylinder);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f MB", megabytes(partition.blockCount * 512));
                ImGui::TableNextColumn();
                if (partition.bootable)
                    ImGui::Text("%d", partition.bootPriority);
                else
                    ImGui::TextDisabled("no");
                ImGui::TableNextColumn();
                std::string flagsText;
                if (partition.noMount)
                    flagsText += "no-mount ";
                if (!partition.checksumValid)
                    flagsText += "bad checksum";
                ImGui::TextUnformatted(flagsText.c_str());
            }
        }
        ImGui::EndTable();
    }
}

bool EmulatorView::StartEmpty()
{
    if (!IsCoreLoaded()) {
        if (!m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath()))
            return false;
    }
    // Sync drive enabled state to emulator before starting
    for (int i = 0; i < 4; i++)
        m_emulator->SetDriveEnabled(i, m_drives[i].enabled);

    Open();
    SetFocus();
    const bool hasHostMount = m_emulator->NeedsGeneratedConfig();
    std::string error;
    bool ok = hasHostMount ? m_emulator->StartConfiguredHostMounts(error, m_selectedBaseUaeConfig)
                           : m_emulator->StartEmpty();
    if (!ok && !error.empty())
        adm::core::Logger::Error("Emulator: {}", error);
    if (ok) {
        if (hasHostMount)
            PersistNormalizedHostMounts();
        FlushPendingDisks();
        if (m_onEmulatorReset)
            m_onEmulatorReset();
    }
    return ok;
}

bool EmulatorView::LoadADF(const std::vector<uint8_t>& adfData, uint32_t documentId,
                           const std::string& fileExt)
{
    if (!IsCoreLoaded()) {
        if (!m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath()))
            return false;
    }
    // Sync drive enabled state to emulator before loading game
    for (int i = 0; i < 4; i++)
        m_emulator->SetDriveEnabled(i, m_drives[i].enabled);
    Open();
    SetFocus();
    const bool hasHostMount = m_emulator->NeedsGeneratedConfig();
    bool ok = false;
    if (hasHostMount) {
        // Queue DF0 before loading the generated config. FlushPendingDisks inserts it through the
        // existing multidrive disk-control API before the first emulated frame runs.
        m_drives[0].hasDisk = true;
        m_drives[0].documentId = documentId;
        m_drives[0].pendingData = adfData;
        m_drives[0].fileExt = fileExt;
        std::string error;
        ok = m_emulator->StartConfiguredHostMounts(error, m_selectedBaseUaeConfig);
        if (!ok && !error.empty())
            adm::core::Logger::Error("Emulator: {}", error);
    } else {
        ok = m_emulator->LoadADF(adfData, fileExt);
    }
    if (ok) {
        if (hasHostMount)
            PersistNormalizedHostMounts();
        // Update drive 0 state to match what was actually loaded into the emulator.
        // Without this, FlushPendingDisks would re-insert stale pendingData from a prior
        // MountDisk call, overriding the disk that was just loaded.
        m_drives[0].hasDisk = true;
        m_drives[0].documentId = documentId;
        m_drives[0].pendingData = adfData;
        FlushPendingDisks();
        if (m_onEmulatorReset)
            m_onEmulatorReset();
    }
    return ok;
}

bool EmulatorView::RunExecutable(const std::vector<uint8_t>& exeData, const std::string& name,
                                 uint32_t documentId)
{
    if (!IsCoreLoaded()) {
        if (!m_emulator->LoadCore(adm::AmigaEmulator::GetDefaultCorePath()))
            return false;
    }
    // Sync drive enabled state to emulator before loading game
    for (int i = 0; i < 4; i++)
        m_emulator->SetDriveEnabled(i, m_drives[i].enabled);
    Open();
    SetFocus();
    bool ok = m_emulator->RunExecutable(exeData, name);
    if (ok) {
        m_drives[0].documentId = documentId;
        FlushPendingDisks();
        if (m_onEmulatorReset)
            m_onEmulatorReset();
    }
    return ok;
}

bool EmulatorView::IsRunning() const
{
    return m_emulator && m_emulator->IsCoreLoaded() && m_emulator->IsGameLoaded();
}

void EmulatorView::TogglePause()
{
    if (m_isPaused)
        NotifyMachineWillRun(); // about to resume: hand the machine back from the timeline
    m_isPaused = !m_isPaused;
    if (m_isPaused) {
        ReleaseMouse();
    } else if (IsRunning()) {
        m_runIsStepOver = false; // a manual Play/continue is a sustained run
        // Re-arm the breakpoint trace before unpausing. When resuming from a breakpoint
        // the CPU's PC is still AT the breakpoint address (the debugger stops before the
        // instruction executes), so without arming the same breakpoint re-fires
        // immediately — the emulator advances one instruction and breaks again (the
        // "stutter"). ArmDebugTrace() sets adm_skip_bp_check so that first instruction is
        // stepped over cleanly. No-op when no breakpoints/trace are active. This covers
        // every resume path (toolbar button, F5, menu); the console 'g' arms separately.
        if (m_emulator->HasArmDebug())
            m_emulator->ArmDebugTrace();
    }
}

void EmulatorView::ToggleTurbo()
{
    // Refuse rather than silently desync a live drive. See AnyDriveBridged().
    if (!m_isTurbo && AnyDriveBridged())
        return;
    if (m_isPaused && IsRunning()) {
        m_isTurbo = !m_isTurbo;
        TogglePause();
    } else {
        m_isTurbo = !m_isTurbo;
    }
    m_emulator->SetSpeedMultiplier(m_isTurbo ? 10.0 : 1.0);
    m_emulator->SetMuted(m_isTurbo || m_isMuted);
}

void EmulatorView::StepFrame()
{
    if (m_isPaused && IsRunning()) {
        NotifyMachineWillRun();
        m_emulator->StepOneFrame();
        FinishSteppedFrame();
    }
}

// A log-mode watchpoint (or a DOS breakpoint whose name predicate did not match) stops
// retro_run() part-way through the frame. The running path pumps those through
// HandlePostStepStop(), which records the hit and lets the machine continue; a manual frame
// step had no such pump, so the hits were dropped on the floor and the "frame" was really a
// truncated fragment ending at the first access. Drain the filtered breaks here and carry the
// frame through to its boundary. A genuine breakpoint still stops the step where it fired.
void EmulatorView::FinishSteppedFrame()
{
    constexpr int kMaxIters = 600; // a PAL frame is ~313 lines; this comfortably clears one
    for (int i = 0; i < kMaxIters; ++i) {
        if (!m_emulator->IsDebugBreakPending())
            return; // the step completed cleanly
        if (!m_debugBreakFilter || !m_debugBreakFilter()) {
            // A real stop. Surface it the same way the running path does.
            if (m_onDebugBreak)
                m_onDebugBreak();
            return;
        }
        // Re-arm the trace exactly as a manual resume does, so the instruction we stopped on
        // is stepped over rather than re-firing forever.
        if (m_emulator->HasArmDebug())
            m_emulator->ArmDebugTrace();
        if (!m_emulator->IsPartialFrame())
            return; // the break landed on the frame boundary — the frame is already complete
        m_emulator->RunForStep(); // finish the interrupted frame
    }
}

void EmulatorView::EndFrame()
{
    // Complete the current frame to the next boundary (stepping past breakpoints) and stay
    // paused, so a mid-frame breakpoint stop becomes a viewable whole frame.
    if (m_isPaused && IsRunning()) {
        NotifyMachineWillRun();
        m_emulator->RunToFrameEnd();
    }
}

bool EmulatorView::RunUntilFrame(uint64_t targetFrame, bool turbo)
{
    if (!IsRunning())
        return false;
    const uint64_t cur = m_emulator->GetTotalFrameCount();
    if (targetFrame <= cur)
        return false;

    NotifyMachineWillRun();
    m_runUntilFrame = targetFrame;
    // Keep the emulator's catch-up batch from running past the target — without this a warp-speed
    // run stops wherever the frame accumulator ran out, tens of frames late.
    m_emulator->SetFrameStopTarget(targetFrame);

    if (turbo && !m_isTurbo) {
        m_isTurbo = true;
        m_runUntilTurboAuto = true;
        m_emulator->SetSpeedMultiplier(10.0);
        m_emulator->SetMuted(true);
    }

    // Resume execution (mirror TogglePause's un-pause path) so StepEmulator advances frames.
    if (m_isPaused) {
        m_isPaused = false;
        m_runIsStepOver = false;
        if (m_emulator->HasArmDebug())
            m_emulator->ArmDebugTrace();
    }
    return true;
}

void EmulatorView::StepInstruction()
{
    if (m_isPaused && IsRunning()) {
        NotifyMachineWillRun();
        std::string output;
        m_emulator->ExecuteDebugCommand("t", output);
        m_emulator->ArmDebugTrace();
        m_emulator->RunForStep();
    }
}

bool EmulatorView::StepOver()
{
    if (!(m_isPaused && IsRunning()))
        return false;
    NotifyMachineWillRun();

    // Step-over only differs from step-into for subroutine calls (BSR/JSR), which
    // push a return address and come back via RTS. For every other instruction
    // (including RTS/JMP/taken branches) there is nothing to step "over", and
    // using 'z' — which runs to the *following* address — would never return.
    // So only calls get the run-to-return treatment; everything else single-steps.
    bool isCall = false;
    adm::AmigaEmulator::CpuState st;
    char mnemo[128] = {0};
    char hex[64] = {0};
    uint32_t nextPc = 0;
    if (m_emulator->GetCpuState(st) &&
        m_emulator->Disassemble(st.pc, mnemo, sizeof(mnemo), hex, sizeof(hex), nextPc)) {
        std::string m(mnemo);
        size_t s = m.find_first_not_of(' ');
        if (s != std::string::npos)
            m = m.substr(s);
        isCall = (m.rfind("BSR", 0) == 0 || m.rfind("JSR", 0) == 0);
    }

    if (!isCall) {
        StepInstruction();
        return false;
    }

    // 'z' arms a one-shot trace (TRACE_MATCH_PC) at the return address. A called
    // subroutine can span many emulated frames (e.g. a disk read), so we cannot run
    // a single frame here — resume and let StepEmulator()/Draw() auto-pause when the
    // trace (or any breakpoint hit inside the callee) fires.
    std::string output;
    m_emulator->ExecuteDebugCommand("z", output);
    m_emulator->ArmDebugTrace();
    m_runIsStepOver = true; // transient run-to-return, not a sustained Play
    m_isPaused = false;
    return true;
}

void EmulatorView::Stop()
{
    // Finalize any in-progress GIF before the core (and its framebuffer) goes away.
    if (m_gifRecorder.IsActive()) {
        m_gifRecorder.End();
        m_gifStatus = fmt::format("Saved {} frame(s) — stopped (emulator stopped): {}",
                                  m_gifFramesCaptured, m_gifPath);
    }
    m_emulator->UnloadCore();
    // UnloadCore drops the emulator's own record of which slots were bridged. This
    // view's record deliberately stays: the assignment ("DF0 is the XCopy on COM4")
    // is the user's, not the core's, and the next start re-applies it through
    // FlushPendingDisks(). Until then IsBridgePending() is true for those slots.
    m_isPaused = false;
    // Forget held keys: keys held across a core reload would otherwise emit a phantom
    // release into the freshly started core.
    m_prevKeys.fill(false);
    m_keysActive = false;
    ReleaseMouse();
}

void EmulatorView::Reset()
{
    // Warm boot: the machine stays up and every drive keeps its disk, so the disk that was
    // booted before the reset boots again. Frames have to keep running for the injected
    // Ctrl+Amiga+Amiga to be released, hence the unpause before anything else.
    m_isPaused = false;
    if (!m_emulator->Reset()) {
        // Cold fallback rebuilt the machine from the config/DF0 image alone — put the media
        // this view tracks back into the other drives.
        FlushPendingDisks();
    }
    if (m_onEmulatorReset)
        m_onEmulatorReset();
}

void EmulatorView::Eject() { EjectDrive(0); }

void EmulatorView::DrawVolumeSlider(ImVec2 buttonMin, ImVec2 buttonMax)
{
    // Not a popup: one would take the click meant for the button under it, and a click there is
    // the mute. A window of its own over the button, up while the mouse is on either or the
    // slider is held.
    if (!m_volumeOpen)
        return;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float w = ImGui::GetFrameHeight() * 1.2f;
    const float h = ImGui::GetFrameHeight() * 6.0f;
    const ImVec2 size(std::max(w, ImGui::CalcTextSize("100%").x) + style.WindowPadding.x * 2,
                      h + ImGui::GetTextLineHeightWithSpacing() + style.WindowPadding.y * 2);
    const float x = (buttonMin.x + buttonMax.x - size.x) * 0.5f;
    const float y = buttonMin.y - size.y - 2.0f;
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(size);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    bool overWindow = false, held = false;
    if (ImGui::Begin("##emulator_volume", nullptr, kFlags)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        int volume = m_emulator->GetVolume();
        const std::string label = fmt::format("{}%", volume);
        ImGui::SetCursorPosX((size.x - ImGui::CalcTextSize(label.c_str()).x) * 0.5f);
        ImGui::TextUnformatted(label.c_str());
        ImGui::SetCursorPosX((size.x - w) * 0.5f);
        bool save = false;
        if (ImGui::VSliderInt("##volume", ImVec2(w, h), &volume, 0, 100, ""))
            m_emulator->SetVolume(volume);
        held = ImGui::IsItemActive();
        save |= ImGui::IsItemDeactivatedAfterEdit();
        // the wheel over it, five a notch
        const float wheel = ImGui::GetIO().MouseWheel;
        if (ImGui::IsWindowHovered() && wheel != 0.0f) {
            m_emulator->SetVolume(volume + static_cast<int>(wheel * 5.0f));
            save = true;
        }
        if (save && m_userPreferences)
            m_userPreferences->SetEmulatorVolume(m_emulator->GetVolume());
        overWindow = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    }
    ImGui::End();
    // the button, the window and the gap between them as one, so the mouse can cross it
    const ImVec2 m = ImGui::GetIO().MousePos;
    const bool overButton = m.x >= std::min(buttonMin.x, x) &&
                            m.x < std::max(buttonMax.x, x + size.x) && m.y >= y &&
                            m.y < buttonMax.y;
    if (!overWindow && !overButton && !held)
        m_volumeOpen = false;
}

void EmulatorView::SetUserPreferences(core::UserPreferences* prefs)
{
    m_userPreferences = prefs;
    if (prefs)
        m_emulator->SetVolume(prefs->Get().emulatorVolume);
}

void EmulatorView::SetMuted(bool mute)
{
    m_isMuted = mute;
    if (m_emulator) {
        m_emulator->SetMuted(m_isTurbo || m_isMuted);
    }
}

void EmulatorView::ReleaseMouse()
{
    if (m_mouseCaptured) {
        m_mouseCaptured = false;
        if (m_remoteMouse) {
            // Roughly one dropped jump per recentre means the warps reach the RDP client; far
            // fewer means it is ignoring them.
            adm::core::Logger::Info("Emulator: remote mouse capture released, {} recentres, {} "
                                    "warp-sized jumps dropped",
                                    m_remoteWarps, m_remoteJumpsDropped);
            ReleaseCursorClip();
        }
        m_remoteMouse = false;
        if (m_window) {
            glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }
}

void EmulatorView::RecentreRemoteCursor()
{
    int width = 0;
    int height = 0;
    glfwGetWindowSize(m_window, &width, &height);
    // RDP coalesces SetCursorPos calls and seems to drop one that lands where the last one did,
    // so the target wobbles a pixel either side of the centre. Without it only the first
    // recentre of a capture ever reached the client.
    static constexpr int kWobble[] = {0, 1, -1};
    m_remoteWobble = (m_remoteWobble + 1) % 3;
    glfwSetCursorPos(m_window, width / 2 + kWobble[m_remoteWobble], height / 2);
    ++m_remoteWarps;
}

void EmulatorView::ReadRemoteMouseDelta(int16_t& deltaX, int16_t& deltaY)
{
    deltaX = 0;
    deltaY = 0;
    if (!m_window)
        return;

    ClipCursorToWindow(m_window);

    double x = 0.0;
    double y = 0.0;
    int width = 0;
    int height = 0;
    glfwGetCursorPos(m_window, &x, &y);
    glfwGetWindowSize(m_window, &width, &height);
    if (width <= 0 || height <= 0)
        return;

    const double dx = x - m_remoteLastX;
    const double dy = y - m_remoteLastY;
    m_remoteLastX = x;
    m_remoteLastY = y;

    // A recentre arrives as one big jump - often merged with the user's own motion, and
    // sometimes after the client has already sent a stale position back - so no single sample
    // says whether a warp has landed. A hand cannot move a sixth of the window in one frame,
    // though, so any jump that large is a warp (in either direction) and is dropped.
    const double maxMotion = std::min(width, height) / 6.0;
    if (std::abs(dx) < maxMotion && std::abs(dy) < maxMotion) {
        deltaX = static_cast<int16_t>(std::lround(dx));
        deltaY = static_cast<int16_t>(std::lround(dy));
    } else {
        ++m_remoteJumpsDropped;
    }

    // The clip parks the cursor on the window border; recentre once it gets there. Only at the
    // edge, so most motion never involves a warp at all.
    const double margin = std::max(8.0, std::min(width, height) * 0.02);
    if (x < margin || x > width - 1 - margin || y < margin || y > height - 1 - margin)
        RecentreRemoteCursor();
}

void EmulatorView::MountDisk(int drive, const std::vector<uint8_t>& data, const std::string& name,
                             uint32_t documentId, const std::string& fileExt, bool writeProtected)
{
    if (drive < 0 || drive > 3)
        return;

    m_selectedCustomConfigurationName.clear();

    m_drives[drive].hasDisk = true;
    m_drives[drive].diskName = name;
    m_drives[drive].documentId = documentId;
    m_drives[drive].fileExt = fileExt;
    m_drives[drive].pendingData = data;
    m_drives[drive].writeProtected = writeProtected;

    // Told to the core before the insert, because InsertDisk() re-applies the slot's
    // remembered value at the end - the libretro insert path clears it.
    if (m_emulator)
        m_emulator->SetFloppyWriteProtect(drive, writeProtected);

    // If emulator is running, insert immediately
    if (IsRunning()) {
        m_emulator->InsertDisk(static_cast<unsigned>(drive), data, fileExt);
    }
}

void EmulatorView::SetDriveWriteProtect(int drive, bool writeProtected)
{
    if (drive < 0 || drive > 3)
        return;
    const bool changed = m_drives[drive].writeProtected != writeProtected;
    m_drives[drive].writeProtected = writeProtected;
    if (!m_emulator)
        return;
    m_emulator->SetFloppyWriteProtect(drive, writeProtected);

    // Moving the tab means taking the disk out - no stock drive has a switch you can
    // reach with a disk in it. AmigaDOS re-reads a disk's protection only when it sees
    // a disk change, so a running machine is told the same way the hardware would tell
    // it: the volume goes away and comes back about two seconds later, carrying
    // everything the guest has written so far. Both directions, because a disk that
    // has just been unlocked is equally news.
    if (changed && m_drives[drive].hasDisk && !m_drives[drive].isBridged && IsRunning())
        m_emulator->ReinsertDisk(static_cast<unsigned>(drive));
}

bool EmulatorView::PowerOn()
{
    // A named or user UAE configuration remains the machine's base after Stop/Power On.
    // Loading it also flushes all saved/current floppy media into the configured drives.
    if (m_selectedBaseUaeConfig) {
        const std::string customName = m_selectedCustomConfigurationName;
        const bool started = LoadUaeConfig(PathToUtf8(*m_selectedBaseUaeConfig));
        if (started)
            m_selectedCustomConfigurationName = customName;
        return started;
    }

    // The boot disk physically becomes the emulator's DF0, so boot from DF0 when it
    // holds a disk; otherwise start to Kickstart. Either path ends in FlushPendingDisks
    // (inside LoadADF / StartEmpty), which inserts every other populated drive.
    if (m_drives[0].hasDisk && !m_drives[0].pendingData.empty()) {
        return LoadADF(m_drives[0].pendingData, m_drives[0].documentId, m_drives[0].fileExt);
    }
    return StartEmpty();
}

void EmulatorView::EjectDrive(int drive)
{
    if (drive < 0 || drive > 3)
        return;

    m_selectedCustomConfigurationName.clear();

    // If emulator is running, eject from core
    if (IsRunning()) {
        m_emulator->EjectDisk(static_cast<unsigned>(drive));
    }

    m_drives[drive].hasDisk = false;
    m_drives[drive].diskName.clear();
    m_drives[drive].sourcePath.clear();
    m_drives[drive].pendingData.clear();
    m_drives[drive].documentId = 0;
    // An empty slot goes back to the safe default, so the next image cannot inherit
    // an unlocked slot from the last one.
    SetDriveWriteProtect(drive, true);

    // Ejecting a live drive means letting go of the hardware, not ejecting a disk
    // from it - there is no image to remove, and EjectDisk() above did nothing.
    if (m_drives[drive].isBridged)
        DetachBridgeDrive(drive);
}

bool EmulatorView::AttachBridgeDrive(int drive, int driverIndex, const std::string& comPort,
                                     int bridgeMode, int bridgeDensity, int driveCable,
                                     bool autoCache, bool smartSpeed, bool allowWrites)
{
    if (drive < 0 || drive > 3 || !m_emulator)
        return false;

    adm::AmigaEmulator::BridgeConfig cfg;
    cfg.driverIndex = driverIndex;
    cfg.comPort = comPort;
    cfg.bridgeMode = bridgeMode;
    cfg.bridgeDensity = bridgeDensity;
    cfg.driveCable = driveCable;
    cfg.autoCache = autoCache;
    cfg.smartSpeed = smartSpeed;
    cfg.allowWrites = allowWrites;

    // The same checks AmigaEmulator::AttachBridgeDrive() applies, made here so a
    // request recorded while the machine is stopped is rejected now rather than
    // failing silently at the next start. An empty port would make the driver scan
    // every serial device (~819 ms inside the emulation thread); an over-long config
    // would be truncated by the core's TCHAR[32] profile field.
    if (comPort.empty() || adm::AmigaEmulator::BuildBridgeConfigString(cfg).empty())
        return false;

    // One device cannot be opened twice. Two slots on one port would have the second
    // open fail a second after the first succeeded, with a message in the core log
    // and nothing else to say why.
    for (int d = 0; d < 4; ++d) {
        if (d != drive && m_drives[d].isBridged && m_drives[d].bridgeComPort == comPort)
            return false;
    }

    if (m_emulator->IsCoreLoaded()) {
        if (!m_emulator->AttachBridgeDrive(static_cast<unsigned>(drive), cfg))
            return false;
    }
    // With no core the request is only recorded; FlushPendingDisks() applies it when
    // the machine starts.

    const auto drivers = m_emulator->GetBridgeDriverList();
    const std::string hardware =
        (driverIndex >= 0 && driverIndex < static_cast<int>(drivers.size()))
            ? drivers[driverIndex]
            : std::string("FloppyBridge");
    SetDriveBridged(drive, true, hardware, comPort);
    m_drives[drive].bridgeConfig = cfg;
    // A live drive has to exist as far as the core's configuration is concerned:
    // DF2/DF3 default to disabled, and a bridge on a disabled slot has no drive to
    // back. Enabling here means the next start's generated config includes it.
    m_drives[drive].enabled = true;
    m_emulator->SetDriveEnabled(drive, true);
    return true;
}

void EmulatorView::DetachBridgeDrive(int drive)
{
    if (drive < 0 || drive > 3)
        return;
    if (m_emulator)
        m_emulator->DetachBridgeDrive(static_cast<unsigned>(drive));
    SetDriveBridged(drive, false);
}

bool EmulatorView::IsBridgePending(int drive) const
{
    if (drive < 0 || drive > 3 || !m_drives[drive].isBridged)
        return false;
    // The emulator's record is per loaded core: true only once the request has been
    // written into the core's prefs, false again after UnloadCore().
    return !(m_emulator && m_emulator->IsBridgeDrive(static_cast<unsigned>(drive)));
}

void EmulatorView::SetDriveBridged(int drive, bool bridged, const std::string& hardware,
                                   const std::string& comPort)
{
    if (drive < 0 || drive > 3)
        return;
    m_drives[drive].isBridged = bridged;
    m_drives[drive].bridgeHardware = bridged ? hardware : std::string();
    m_drives[drive].bridgeComPort = bridged ? comPort : std::string();
    if (!bridged)
        m_drives[drive].bridgeConfig.reset();
    if (bridged) {
        // The core clears the slot's df when the bridge attaches, so the host's idea of
        // what is mounted has to follow or the two disagree about DF0's contents.
        m_drives[drive].hasDisk = false;
        m_drives[drive].diskName.clear();
        m_drives[drive].sourcePath.clear();
        m_drives[drive].pendingData.clear();
        m_drives[drive].documentId = 0;
    }
}

void EmulatorView::FlushPendingDisks()
{
    if (!IsRunning())
        return;

    for (int i = 0; i < 4; i++) {
        // A live drive owns its slot. An image that reached this slot anyway (a boot
        // path that fills DF0 before asking) loses: the core clears the slot's file
        // when the bridge attaches, so the host's record has to agree.
        if (m_drives[i].isBridged && m_drives[i].bridgeConfig) {
            if (m_drives[i].hasDisk) {
                adm::core::Logger::Warn(
                    "DF{} is a live physical drive; the queued image was not inserted", i);
                m_drives[i].hasDisk = false;
                m_drives[i].diskName.clear();
                m_drives[i].sourcePath.clear();
                m_drives[i].pendingData.clear();
                m_drives[i].documentId = 0;
            }
            if (!m_emulator->IsBridgeDrive(static_cast<unsigned>(i))) {
                if (m_emulator->AttachBridgeDrive(static_cast<unsigned>(i),
                                                  *m_drives[i].bridgeConfig)) {
                    adm::core::Logger::Info("Live floppy: DF{} -> {} on {} (re-attached on start; "
                                            "the device opens on the next vertical blank)",
                                            i, m_drives[i].bridgeHardware,
                                            m_drives[i].bridgeComPort);
                } else {
                    // The core is loaded but refused - almost always a core without the
                    // bridge exports. Drop the assignment rather than show a live drive
                    // that does not exist.
                    adm::core::Logger::Error("Live floppy: DF{} could not be attached on start "
                                             "(does this core have FloppyBridge support?)",
                                             i);
                    SetDriveBridged(i, false);
                }
            }
            continue;
        }
        if (m_drives[i].hasDisk && !m_drives[i].pendingData.empty()) {
            m_emulator->SetFloppyWriteProtect(i, m_drives[i].writeProtected);
            m_emulator->InsertDisk(static_cast<unsigned>(i), m_drives[i].pendingData,
                                   m_drives[i].fileExt);
        }
    }
}

void EmulatorView::SetDriveEnabled(int drive, bool enabled)
{
    if (drive < 0 || drive > 3)
        return;
    m_selectedCustomConfigurationName.clear();
    m_drives[drive].enabled = enabled;
}

const EmulatorView::DriveState& EmulatorView::GetDriveState(int drive) const
{
    static DriveState empty;
    if (drive < 0 || drive > 3)
        return empty;
    return m_drives[drive];
}

void EmulatorView::SetDriveDiskName(int drive, const std::string& name)
{
    if (drive < 0 || drive > 3)
        return;
    m_drives[drive].hasDisk = true;
    m_drives[drive].diskName = name;
}

bool EmulatorView::IsDocumentLoaded(uint32_t documentId) const
{
    if (documentId == 0)
        return false;
    for (int i = 0; i < 4; i++) {
        if (m_drives[i].hasDisk && m_drives[i].documentId == documentId)
            return true;
    }
    return false;
}

// ── Draw ──────────────────────────────────────────────────────────────────────────────────

void EmulatorView::Draw()
{
    m_advancedLastFrame = m_advancedThisFrame;
    m_advancedThisFrame = false;
    if (m_emulator && !m_advancedLastFrame)
        m_emulator->ResyncFrameClock();

    // Synced every host frame regardless of window visibility, so the core-side harvest tracks
    // turbo/pause state immediately rather than on whichever call happens to touch the emulator.
    if (m_emulator)
        m_emulator->SetDebugHarvestSuppressed(m_isTurbo && IsRunning() && !m_isPaused);

    // The Display Filters window is independent of the emulator window, so draw it before the
    // early-out below — the user can tweak filters with the emulator window closed.
    DrawFiltersWindow();
    DrawGifCaptureWindow();
    PollCdExtraction();
    // Not while fullscreen: the workspace dockspaces aren't submitted then, so a docked window
    // drawn now finds no host node and undocks (see the emulator window below).
    if (m_hardDriveEditorOpen && !m_isFullscreen)
        DrawHardDriveEditor(false);
    if (m_cdDriveWindowOpen && !m_isFullscreen)
        DrawCdDriveWindow();

    if (!m_isOpen) {
        if (m_runWhenHidden)
            StepEmulator();
        return;
    }

    bool wasOpen = m_isOpen;

    if (m_isFullscreen) {
        // While fullscreen, the dockable window is NOT submitted at all. Submitting it (even
        // empty) runs BeginDocked, which undocks it from its node the moment a fullscreen overlay
        // covers the dockspace. Leaving it unsubmitted means ImGui keeps its saved dock settings,
        // so it re-docks back into its original node (and tab group) when we resume on exit.
        // The overlay below owns stepping, rendering and input.
        DrawFullscreenOverlay();
    } else {
        ImGui::SetNextWindowSize(ImVec2(720, 576), ImGuiCond_FirstUseEver);

        if (m_shouldFocus) {
            if (!m_suppressNextFocus)
                ImGui::SetNextWindowFocus();
            m_shouldFocus = false;
            m_suppressNextFocus = false;
        }

        ImGuiWindowFlags windowFlags =
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (m_noFocusOnAppearing) {
            windowFlags |= ImGuiWindowFlags_NoFocusOnAppearing;
            m_noFocusOnAppearing = false;
        }

        bool windowVisible = ImGui::Begin(ICON_FA_GAMEPAD " Amiga Emulator###Amiga Emulator",
                                          &m_isOpen, windowFlags);
        if (windowVisible) {
            DrawEmulatorContent();
        } else if (m_runWhenHidden) {
            StepEmulator();
        }
        ImGui::End();
    }

    // Window was closed via the X button — unload the core
    if (wasOpen && !m_isOpen) {
        if (IsCoreLoaded()) {
            m_emulator->UnloadCore();
        }
        m_isPaused = false;
        ReleaseMouse();

        // Ensure we exit fullscreen if the window is closed
        if (m_isFullscreen) {
            SetFullscreen(false);
        }
    }
}

void EmulatorView::DrawFiltersWindow()
{
    if (!m_filtersWindowOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_EYE " Display Filters", &m_filtersWindowOpen)) {
        ImGui::End();
        return;
    }

    EmulatorFilterParams& p = m_filterParams;

    // Presets are a starting point: picking one overwrites the whole param set, after which the
    // user can keep tweaking the individual dials below.
    const auto& presets = EmulatorFilterPresets();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##filterPreset", "Apply a preset...")) {
        for (const auto& preset : presets) {
            if (ImGui::Selectable(preset.name.c_str()))
                p = preset.params;
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Combine effects below, or start from a preset.");

    // Reserve a uniform label column on the right, sized to the widest label so every slider
    // is the same width regardless of its label length.
    const char* kLabels[] = {"Strength", "Mask",     "Glow",       "Curve", "Vignette",
                             "Gamma",    "Contrast", "Saturation", "Warmth"};
    float labelColW = 0.0f;
    for (const char* l : kLabels)
        labelColW = std::max(labelColW, ImGui::CalcTextSize(l).x);

    // One effect's slider: the bar stretches to fill the row minus the fixed label column; the
    // label is then drawn right-aligned within that column (SliderFloat's own label is hidden).
    auto slider = [labelColW](const char* text, const char* id, float* v, float lo, float hi) {
        float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(-(labelColW + spacing));
        ImGui::SliderFloat(id, v, lo, hi, "%.2f");
        ImGui::SameLine(0.0f, spacing);
        float textW = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (labelColW - textW));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
    };

    ImGui::Separator();
    ImGui::Checkbox("Scanlines", &p.scanlines);
    ImGui::BeginDisabled(!p.scanlines);
    ImGui::Indent();
    slider("Strength", "##scanline", &p.scanlineStrength, 0.0f, 1.0f);
    ImGui::Unindent();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Checkbox("CRT Mask + Glow", &p.crt);
    ImGui::BeginDisabled(!p.crt);
    ImGui::Indent();
    slider("Mask", "##crtMask", &p.maskStrength, 0.0f, 1.0f);
    slider("Glow", "##crtGlow", &p.glowStrength, 0.0f, 0.5f);
    ImGui::Unindent();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Checkbox("Curvature + Vignette", &p.curvature);
    ImGui::BeginDisabled(!p.curvature);
    ImGui::Indent();
    slider("Curve", "##curveAmount", &p.curveAmount, 0.0f, 2.0f);
    slider("Vignette", "##curveVignette", &p.vignetteStrength, 0.0f, 1.0f);
    ImGui::Unindent();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Checkbox("Color Grade", &p.colorGrade);
    ImGui::BeginDisabled(!p.colorGrade);
    ImGui::Indent();
    slider("Gamma", "##gradeGamma", &p.gamma, 0.5f, 2.0f);
    slider("Contrast", "##gradeContrast", &p.contrast, 0.5f, 1.5f);
    slider("Saturation", "##gradeSaturation", &p.saturation, 0.0f, 2.0f);
    slider("Warmth", "##gradeWarmth", &p.warmth, -0.2f, 0.2f);
    ImGui::Unindent();
    ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button("Turn All Off"))
        p.scanlines = p.crt = p.curvature = p.colorGrade = false;

    ImGui::End();
}

// Nearest-neighbour resample of a top-row-first RGBA8 image. Used to apply the display's pixel
// aspect ratio to captured GIF frames: the raw framebuffer has non-square Amiga pixels (e.g. a
// single-field 720x288 buffer) that must be scaled to square-pixel dimensions so the GIF isn't
// vertically squished. Nearest-neighbour keeps the pixel-art look and cleanly doubles scanlines.
static void ResizeRGBANearest(const std::vector<uint8_t>& src, int sw, int sh,
                              std::vector<uint8_t>& dst, int dw, int dh)
{
    dst.resize(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        int sy = static_cast<int>(static_cast<int64_t>(y) * sh / dh);
        if (sy >= sh)
            sy = sh - 1;
        for (int x = 0; x < dw; ++x) {
            int sx = static_cast<int>(static_cast<int64_t>(x) * sw / dw);
            if (sx >= sw)
                sx = sw - 1;
            const uint8_t* sp = &src[(static_cast<size_t>(sy) * sw + sx) * 4];
            uint8_t* dp = &dst[(static_cast<size_t>(y) * dw + x) * 4];
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = sp[3];
        }
    }
}

// Aspect-corrected output dimensions for a captured frame: keep the native width and derive the
// height from the emulator's display aspect ratio, so a square-pixel GIF viewer shows the frame at
// the same proportions as the on-screen image (see RenderEmulatorImage).
void EmulatorView::GetGifOutputSize(int srcW, int srcH, int& outW, int& outH) const
{
    outW = srcW;
    outH = srcH;
    float aspect = m_emulator->GetAspectRatio();
    if (aspect > 0.0f && srcW > 0) {
        int corrected = static_cast<int>(std::lround(srcW / aspect));
        if (corrected > 0)
            outH = corrected;
    }
}

void EmulatorView::CaptureGifFrameIfRecording()
{
    // Capture was requested on a stopped emulator: now that it's running, begin recording as soon
    // as the first frame has been rendered. Falls through to capture that same frame below.
    if (m_gifPendingStart) {
        if (!IsRunning()) {
            m_gifPendingStart = false;
            m_gifStatus = "Could not start the emulator.";
            return;
        }
        std::vector<uint8_t> probe;
        int pw = 0, ph = 0;
        if (!m_emulator->CaptureFrameRGBA(probe, pw, ph) || pw <= 0 || ph <= 0)
            return; // no frame yet — wait for the next one
        int outW = 0, outH = 0;
        GetGifOutputSize(pw, ph, outW, outH);
        if (!m_gifRecorder.Begin(m_gifPath, outW, outH, 2)) {
            m_gifStatus = fmt::format("Could not open '{}' for writing.", m_gifPath);
            m_gifPendingStart = false;
            return;
        }
        m_gifFramesCaptured = 0;
        m_gifStatus = "Recording...";
        m_gifPendingStart = false;
    }

    if (!m_gifRecorder.IsActive())
        return;

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!m_emulator->CaptureFrameRGBA(rgba, w, h)) {
        // No frame yet — wait for the next emulated frame rather than aborting.
        return;
    }

    int outW = 0, outH = 0;
    GetGifOutputSize(w, h, outW, outH);

    // gif-h requires every frame to share the dimensions passed to Begin(). A resolution switch
    // mid-capture (e.g. lores↔hires) ends the recording cleanly with what we have so far.
    if (outW != m_gifRecorder.Width() || outH != m_gifRecorder.Height()) {
        m_gifRecorder.End();
        m_gifStatus = fmt::format("Saved {} frame(s) — stopped early (resolution changed): {}",
                                  m_gifFramesCaptured, m_gifPath);
        return;
    }

    std::vector<uint8_t> scaled;
    ResizeRGBANearest(rgba, w, h, scaled, outW, outH);

    if (m_gifRecorder.WriteFrame(scaled.data(), outW, outH)) {
        ++m_gifFramesCaptured;
    }

    if (m_gifFramesCaptured >= m_gifTargetFrames) {
        m_gifRecorder.End();
        m_gifStatus = fmt::format("Saved {} frames to {}", m_gifFramesCaptured, m_gifPath);
        // Pause the emulator once the final frame is captured (toolbar "Pause Emulator").
        if (!m_isPaused)
            TogglePause();
    }
}

namespace {

// A greyed line that wraps. The capture window is narrow, and TextDisabled clips mid-word.
void TextDisabledWrapped(const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

} // namespace

void EmulatorView::DrawGifCaptureWindow()
{
    // Every frame, open or not: a stopped FFmpeg recording finishes in the background, and its
    // result should be waiting when the window is next opened.
    if (m_emulator)
        m_emulator->PollVideoCapture();
    if (!m_gifWindowOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    // "###GIF Capture" keeps the id the window had before it also recorded video, so the
    // MCP UI tools still find it by that name and the title is free to change again.
    if (!ImGui::Begin(ICON_FA_FILM " GIF & Video Capture###GIF Capture", &m_gifWindowOpen)) {
        ImGui::End();
        return;
    }

    const bool recording = m_gifRecorder.IsActive();

    ImGui::TextWrapped("Record the next N emulated frames to an animated GIF.");
    ImGui::Spacing();

    // Number of frames to capture
    ImGui::BeginDisabled(recording);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Frames", &m_gifTargetFrames))
        m_gifTargetFrames = std::clamp(m_gifTargetFrames, 1, 100000);
    ImGui::SetItemTooltip("How many emulated frames to record (~50 frames = 1 second PAL).");

    // Output filename + file picker
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputTextWithHint("##gifPath", "output.gif", &m_gifPath);
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(80.0f, 0.0f))) {
        nfdchar_t* outPath = nullptr;
        nfdfilteritem_t filters[1] = {{"Animated GIF", "gif"}};
        std::string defaultName = m_gifPath.empty()
                                      ? "capture.gif"
                                      : std::filesystem::path(m_gifPath).filename().string();
        nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1, nullptr, defaultName.c_str());

        // NFD blocks and can swallow key-up events; clear ImGui's key state afterwards.
        ImGui::GetIO().ClearInputKeys();

        if (result == NFD_OKAY && outPath) {
            m_gifPath = outPath;
            NFD_FreePath(outPath);
            // Ensure a .gif extension.
            std::string ext = std::filesystem::path(m_gifPath).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (ext != ".gif")
                m_gifPath += ".gif";
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (recording) {
        float fraction = m_gifTargetFrames > 0
                             ? static_cast<float>(m_gifFramesCaptured) / m_gifTargetFrames
                             : 0.0f;
        std::string overlay = fmt::format("{} / {}", m_gifFramesCaptured, m_gifTargetFrames);
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay.c_str());
        if (ImGui::Button(ICON_FA_STOP "  Stop", ImVec2(-1.0f, 0.0f))) {
            m_gifRecorder.End();
            m_gifStatus = fmt::format("Saved {} frames to {}", m_gifFramesCaptured, m_gifPath);
        }
    } else {
        // Capture is allowed whenever an output file is set: if the emulator is stopped, the
        // capture starts it (and stops it again once the final frame is recorded).
        const bool canStart = IsRunning() || static_cast<bool>(m_onStartEmulator);
        const bool canCapture = canStart && !m_gifPath.empty();
        ImGui::BeginDisabled(!canCapture);
        if (ImGui::Button(ICON_FA_CIRCLE_DOT "  Capture", ImVec2(-1.0f, 0.0f))) {
            // Make sure the emulator is running first (start it if nothing is loaded).
            if (!IsRunning() && m_onStartEmulator)
                m_onStartEmulator();

            if (!IsRunning()) {
                m_gifStatus = "Could not start the emulator.";
            } else {
                // Resume the emulator so frames advance (toolbar "Resume Emulator").
                if (m_isPaused)
                    TogglePause();

                m_gifFramesCaptured = 0;

                // Begin now if a frame already exists; otherwise wait for the first one (e.g. the
                // emulator was just started this click and hasn't rendered yet).
                int w = 0, h = 0;
                std::vector<uint8_t> probe;
                if (m_emulator->CaptureFrameRGBA(probe, w, h) && w > 0 && h > 0) {
                    int outW = 0, outH = 0;
                    GetGifOutputSize(w, h, outW, outH);
                    if (!m_gifRecorder.Begin(m_gifPath, outW, outH, 2)) {
                        m_gifStatus = fmt::format("Could not open '{}' for writing.", m_gifPath);
                    } else {
                        m_gifStatus = "Recording...";
                    }
                } else {
                    m_gifPendingStart = true;
                    m_gifStatus = "Starting emulator...";
                }
            }
        }
        ImGui::EndDisabled();

        if (m_gifPath.empty())
            TextDisabledWrapped("Choose an output file first.");
        else if (!IsRunning())
            TextDisabledWrapped("Capture will start the emulator.");
        else if (m_isPaused)
            TextDisabledWrapped("Capture will resume the emulator.");
    }

    if (!m_gifStatus.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_gifStatus.c_str());
    }

    // In the same window as the GIF rather than one of its own: they are two formats of the
    // same job, and the choice between them is the point.
    DrawVideoCaptureSection();

    ImGui::End();
}

void EmulatorView::LoadVideoCaptureSettings()
{
    if (m_videoSettingsLoaded)
        return;
    m_videoSettingsLoaded = true;
    if (!m_userPreferences)
        return;
    const auto& prefs = m_userPreferences->Get();
    if (const auto* info = core::FindVideoCaptureCodec(prefs.videoCaptureCodec)) {
        m_videoCodec = info->codec;
        m_videoCodecChosen = true;
    }
    m_videoQuality = prefs.videoCaptureQuality;
    m_videoPath = prefs.videoCapturePath;
    m_ffmpegPath = prefs.ffmpegPath;
}

void EmulatorView::SaveVideoCaptureSettings()
{
    if (!m_userPreferences)
        return;
    const auto& info = core::GetVideoCaptureCodecInfo(m_videoCodec);
    m_userPreferences->SetVideoCapture(m_videoCodecChosen ? info.id : "", m_videoQuality,
                                       m_videoPath);
    m_userPreferences->SetFfmpegPath(m_ffmpegPath);
}

void EmulatorView::StartFfmpegProbe()
{
    m_ffmpegProbed = false;
    // Assigning over a std::async future still running blocks until it ends - on the UI
    // thread. Another probe is asked for once this one is done.
    if (m_ffmpegProbe.valid() &&
        m_ffmpegProbe.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        m_ffmpegProbeAgain = true;
        return;
    }
    m_ffmpegProbeAgain = false;
    const std::string path = m_ffmpegPath;
    m_ffmpegProbe =
        std::async(std::launch::async, [path] { return core::FfmpegTools::Probe(path); });
}

void EmulatorView::DrawVideoCaptureSection()
{
    LoadVideoCaptureSettings();
    if (!m_ffmpegProbed && !m_ffmpegProbe.valid())
        StartFfmpegProbe();
    if (m_ffmpegProbe.valid() &&
        m_ffmpegProbe.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
        m_ffmpegProbeAgain) {
        m_ffmpegProbe.get(); // of a path since changed
        StartFfmpegProbe();
    }
    if (m_ffmpegProbe.valid() &&
        m_ffmpegProbe.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_ffmpegTools = m_ffmpegProbe.get();
        m_ffmpegProbed = true;
        // Nothing chosen yet: the codec that plays everywhere if FFmpeg can make it, else the
        // built-in lossless one. A saved choice is kept even if FFmpeg has since gone; Record
        // then says why it cannot start.
        if (!m_videoCodecChosen)
            m_videoCodec = m_ffmpegTools.UnavailableReason(core::VideoCaptureCodec::H264).empty()
                               ? core::VideoCaptureCodec::H264
                               : core::VideoCaptureCodec::Zmbv;
    }

    const auto codecReason = [this](const core::VideoCaptureCodecInfo& c) -> std::string {
        if (c.needsFfmpeg && !m_ffmpegProbed)
            return "Looking for FFmpeg...";
        return m_ffmpegTools.UnavailableReason(c.codec);
    };
    // The file follows the codec: a different container is a different extension.
    const auto matchExtension = [this]() {
        if (m_videoPath.empty())
            return;
        auto path = PathFromUtf8(m_videoPath);
        path.replace_extension(core::GetVideoCaptureCodecInfo(m_videoCodec).extension);
        m_videoPath = PathToUtf8(path);
    };

    const auto& info = core::GetVideoCaptureCodecInfo(m_videoCodec);
    const bool recording = m_emulator && m_emulator->IsVideoRecording();
    const bool finishing = m_emulator && m_emulator->IsVideoFinishing();
    const float buttonWidth = 80.0f;

    ImGui::Spacing();
    ImGui::SeparatorText("Video + sound");

    ImGui::BeginDisabled(recording || finishing);

    // Codec. Entries FFmpeg cannot make stay in the list, greyed, with the reason on hover.
    ImGui::SetNextItemWidth(-buttonWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("Codec##video", info.label)) {
        size_t count = 0;
        const auto* codecs = core::GetVideoCaptureCodecs(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& c = codecs[i];
            const std::string reason = codecReason(c);
            ImGui::BeginDisabled(!reason.empty());
            if (ImGui::Selectable(c.label, c.codec == m_videoCodec)) {
                m_videoCodec = c.codec;
                m_videoCodecChosen = true;
                matchExtension();
                SaveVideoCaptureSettings();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (reason.empty())
                    ImGui::SetTooltip("%s", c.description);
                else
                    ImGui::SetTooltip("%s\n\n%s", c.description, reason.c_str());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", info.description);

    // Quality, for the lossy codecs. Shown for the lossless ones too, disabled, so the window
    // does not change shape as the codec does.
    const bool lossy = !info.lossless;
    int quality = m_videoQuality >= 0 ? m_videoQuality : info.defaultQuality;
    ImGui::BeginDisabled(!lossy);
    ImGui::SetNextItemWidth(-buttonWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::SliderInt("Quality##video", &quality, 0, 51, lossy ? "CRF %d" : "lossless")) {
        m_videoQuality = quality;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
        SaveVideoCaptureSettings();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Lower is sharper and larger. 18 looks lossless for H.264; 23 is its "
                          "usual default. H.265 reaches the same look a few steps higher.");

    // File.
    const std::string hint = std::string("capture") + info.extension;
    ImGui::SetNextItemWidth(-buttonWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##videoPath", hint.c_str(), &m_videoPath);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        matchExtension();
        SaveVideoCaptureSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...##video", ImVec2(buttonWidth, 0.0f))) {
        nfdchar_t* outPath = nullptr;
        const std::string extension = std::string(info.extension).substr(1);
        nfdfilteritem_t filters[1] = {{info.label, extension.c_str()}};
        const std::string defaultName =
            m_videoPath.empty() ? hint : PathFromUtf8(m_videoPath).filename().string();
        const nfdresult_t result =
            NFD_SaveDialog(&outPath, filters, 1, nullptr, defaultName.c_str());
        // NFD blocks and can swallow key-up events; clear ImGui's key state afterwards.
        ImGui::GetIO().ClearInputKeys();
        if (result == NFD_OKAY && outPath) {
            m_videoPath = outPath;
            NFD_FreePath(outPath);
            matchExtension();
            SaveVideoCaptureSettings();
        }
    }

    // The Exocet video: the frame with the grid, the timeline and the memory around it, at 4K.
    // Greyed with the reason when there is no pack, or the codec cannot take 4K. Not offered at
    // all while Exocet is hidden: nothing has set it up (core/PrivateFeatures.h).
    bool exocetVideo = false;
    if (m_setUpExocetVideo) {
        std::string exocetReason =
            m_exocetVideoUnavailable ? m_exocetVideoUnavailable() : "Exocet is not available.";
        if (exocetReason.empty() && !info.needsFfmpeg)
            exocetReason =
                "The 4K Exocet video needs one of FFmpeg's codecs: H.264, H.265 or FFV1.";
        exocetVideo = m_videoExocet && exocetReason.empty();
        ImGui::BeginDisabled(!exocetReason.empty());
        bool exocetChecked = exocetVideo;
        if (ImGui::Checkbox("Exocet video (4K)##video", &exocetChecked))
            m_videoExocet = exocetChecked;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", exocetReason.empty()
                                        ? "3840x2160: the Amiga's picture top left, the Exocet "
                                          "grid top right; along the bottom the timeline, the "
                                          "memory over the drives, and Paula's voices over the "
                                          "DMA slots."
                                        : exocetReason.c_str());
    }

    // FFmpeg: found, or not and how to fix that.
    if (!m_ffmpegProbed) {
        TextDisabledWrapped("Looking for FFmpeg...");
    } else if (m_ffmpegTools.found) {
        // "FFmpeg 9.0-essentials_build-www.gyan.dev" wraps over three lines here; the release
        // number is what matters, and the rest goes in the tooltip.
        std::string shortVersion = m_ffmpegTools.version;
        const auto dash = shortVersion.find('-');
        if (dash != std::string::npos && shortVersion.size() > 7 &&
            std::isdigit(static_cast<unsigned char>(shortVersion[7])))
            shortVersion.resize(dash);
        TextDisabledWrapped(shortVersion + (m_ffmpegPath.empty() ? " (on PATH)" : " (chosen)"));
        ImGui::SetItemTooltip("%s\n%s", m_ffmpegTools.version.c_str(),
                              m_ffmpegPath.empty() ? "Found on PATH." : m_ffmpegPath.c_str());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.15f, 1.0f));
        ImGui::TextWrapped("%s H.264, H.265 and FFV1 need it; ZMBV does not.",
                           m_ffmpegTools.error.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button("Locate FFmpeg...##video")) {
        nfdchar_t* outPath = nullptr;
#ifdef _WIN32
        nfdfilteritem_t filters[1] = {{"FFmpeg", "exe"}};
        const nfdresult_t result = NFD_OpenDialog(&outPath, filters, 1, nullptr);
#else
        const nfdresult_t result = NFD_OpenDialog(&outPath, nullptr, 0, nullptr);
#endif
        ImGui::GetIO().ClearInputKeys();
        if (result == NFD_OKAY && outPath) {
            m_ffmpegPath = outPath;
            NFD_FreePath(outPath);
            SaveVideoCaptureSettings();
            StartFfmpegProbe();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_ffmpegPath.empty());
    if (ImGui::Button("Use PATH##video")) {
        m_ffmpegPath.clear();
        SaveVideoCaptureSettings();
        StartFfmpegProbe();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Forget the chosen FFmpeg and use the one on PATH.");

    ImGui::EndDisabled(); // recording || finishing

    // Record / Stop.
    if (recording) {
        const uint64_t bytes = m_emulator->GetVideoBytesWritten();
        ImGui::Text("Recording: %u frames, %.1f MB", m_emulator->GetVideoFramesRecorded(),
                    static_cast<double>(bytes) / (1024.0 * 1024.0));
        if (ImGui::Button(ICON_FA_STOP "  Stop##video", ImVec2(-1.0f, 0.0f))) {
            m_emulator->StopVideoRecording();
            m_videoStatus.clear(); // the emulator's status carries the result
        }
    } else if (finishing) {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_CLOCK "  Writing the file...##video", ImVec2(-1.0f, 0.0f));
        ImGui::EndDisabled();
        TextDisabledWrapped(fmt::format("{} frames: FFmpeg is putting the picture and sound "
                                        "together.",
                                        m_emulator->GetVideoFramesRecorded()));
    } else {
        std::string reason = codecReason(info);
        if (reason.empty() && m_videoPath.empty())
            reason = "Choose an output file first.";
        if (reason.empty() && !IsRunning())
            reason = "Start the emulator first.";
        ImGui::BeginDisabled(!reason.empty());
        if (ImGui::Button(ICON_FA_CIRCLE_DOT "  Record##video", ImVec2(-1.0f, 0.0f))) {
            if (m_isPaused)
                TogglePause(); // frames only advance while it is running
            matchExtension();
            AmigaEmulator::VideoCaptureOptions options;
            options.codec = m_videoCodec;
            options.quality = m_videoQuality;
            options.ffmpegExecutable = m_ffmpegTools.executable;
            std::string error;
            if (exocetVideo && m_setUpExocetVideo)
                error = m_setUpExocetVideo(options);
            if (!error.empty() || !m_emulator->StartVideoRecording(m_videoPath, options, error))
                m_videoStatus = fmt::format("Could not start recording: {}", error);
            else
                m_videoStatus.clear();
            SaveVideoCaptureSettings();
        }
        ImGui::EndDisabled();
        if (!reason.empty())
            TextDisabledWrapped(reason);
    }

    // What happened last - a failure to start, or how the last recording ended - right under the
    // button, where it is seen. Further down it fell below the fold of the window.
    if (!m_videoStatus.empty())
        ImGui::TextWrapped("%s", m_videoStatus.c_str());
    else if (!recording && !finishing && m_emulator && !m_emulator->GetVideoCaptureStatus().empty())
        ImGui::TextWrapped("%s", m_emulator->GetVideoCaptureStatus().c_str());

    // What this codec costs at the size the machine is running, said before somebody records a
    // whole demo with it. The size depends on the screen mode: a superhires field is quadrupled.
    int outW = 0, outH = 0, srcW = 0, srcH = 0;
    const bool haveSize = m_emulator && m_emulator->GetVideoCaptureSize(outW, outH, srcW, srcH);
    std::string note;
    if (exocetVideo)
        note = "Records 3840x2160, the machine's picture enlarged without blurring. Where "
               "FFmpeg cannot encode 4K as fast as the machine runs, the machine waits for it; "
               "the film keeps its timing. ";
    else if (haveSize)
        note = (outW == srcW && outH == srcH)
                   ? fmt::format("Records {}x{}. ", outW, outH)
                   : fmt::format("Records {}x{} - the machine's {}x{}, enlarged without "
                                 "blurring. ",
                                 outW, outH, srcW, srcH);
    switch (m_videoCodec) {
        case core::VideoCaptureCodec::Uncompressed: {
            const double mbPerSecond =
                haveSize ? outW * 3.0 * outH * 50.0 / (1024.0 * 1024.0) : 62.0;
            note += fmt::format("Uncompressed: about {:.0f} MB a second, and it stops itself at "
                                "the 4 GB AVI limit, after about {:.0f} seconds.",
                                mbPerSecond, 3.75 * 1024.0 / mbPerSecond);
            break;
        }
        case core::VideoCaptureCodec::Zmbv:
            note += "Lossless, usually well under a megabyte a second. Plays in VLC or mpv.";
            break;
        case core::VideoCaptureCodec::H264:
        case core::VideoCaptureCodec::H265:
            note += "Encoded by FFmpeg as it records. The sound is added when you stop, which "
                    "takes a moment.";
            break;
        case core::VideoCaptureCodec::Ffv1:
            note += "Lossless, a few megabytes a second. Encoded by FFmpeg; the sound is added "
                    "when you stop.";
            break;
    }
    TextDisabledWrapped(note);
}

// Reads the display texture back as aspect-corrected RGB8, top row first - the shape both the
// clipboard copy and the PNG save want. The texture itself is RGBA at the raw framebuffer size, so
// the alpha is dropped and the result is stretched to square pixels exactly as GIF capture does.
bool EmulatorView::CaptureScreenshotRGB(std::vector<uint8_t>& rgb, int& w, int& h,
                                        std::string& outError)
{
    rgb.clear();
    w = h = 0;

    if (!IsRunning()) {
        outError = "the emulator is not running";
        return false;
    }

    std::vector<uint8_t> rgba;
    int srcW = 0, srcH = 0;
    if (!m_emulator->CaptureFrameRGBA(rgba, srcW, srcH) || srcW <= 0 || srcH <= 0) {
        outError = "no frame has been rendered yet";
        return false;
    }

    int outW = 0, outH = 0;
    GetGifOutputSize(srcW, srcH, outW, outH);

    std::vector<uint8_t> scaled;
    if (outW != srcW || outH != srcH) {
        ResizeRGBANearest(rgba, srcW, srcH, scaled, outW, outH);
    } else {
        scaled = std::move(rgba);
    }

    rgb.resize(static_cast<size_t>(outW) * outH * 3);
    const uint8_t* src = scaled.data();
    uint8_t* dst = rgb.data();
    for (size_t i = 0, n = static_cast<size_t>(outW) * outH; i < n; ++i, src += 4, dst += 3) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
    }

    w = outW;
    h = outH;
    return true;
}

void EmulatorView::CopyScreenshotToClipboard()
{
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    std::string error;
    if (!CaptureScreenshotRGB(rgb, w, h, error)) {
        adm::core::Logger::Error("Screenshot: {}", error);
        return;
    }
    if (!adm::core::CopyImageToClipboard(rgb.data(), w, h, error)) {
        adm::core::Logger::Error("Screenshot: {}", error);
        return;
    }
    adm::core::Logger::Info("Screenshot: copied {}x{} to the clipboard", w, h);
}

void EmulatorView::SaveScreenshotToFile()
{
    // Grab the frame before the file dialog goes up: NFD blocks, and the emulator keeps running
    // behind it, so asking afterwards would save a frame the user never chose.
    std::vector<uint8_t> rgb;
    int w = 0, h = 0;
    std::string error;
    if (!CaptureScreenshotRGB(rgb, w, h, error)) {
        adm::core::Logger::Error("Screenshot: {}", error);
        return;
    }

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"PNG Image", "png"}};
    nfdresult_t result = NFD_SaveDialog(&outPath, filters, 1, nullptr, "screenshot.png");

    // NFD blocks and can swallow key-up events; clear ImGui's key state afterwards.
    ImGui::GetIO().ClearInputKeys();

    if (result != NFD_OKAY || !outPath)
        return;

    std::string path = outPath;
    NFD_FreePath(outPath);

    // Ensure a .png extension - the save is always PNG regardless of what was typed.
    std::string ext = PathFromUtf8(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (ext != ".png")
        path += ".png";

    // Encode to memory and write through std::ofstream: stb's own file writer goes via fopen(),
    // which on Windows cannot open a path outside the active ANSI code page.
    std::vector<uint8_t> png;
    auto writeCb = [](void* ctx, void* data, int size) {
        auto* vec = static_cast<std::vector<uint8_t>*>(ctx);
        const auto* bytes = static_cast<const uint8_t*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
    };
    if (!stbi_write_png_to_func(writeCb, &png, w, h, 3, rgb.data(), w * 3) || png.empty()) {
        adm::core::Logger::Error("Screenshot: could not encode the PNG");
        return;
    }

    std::ofstream out(PathFromUtf8(path), std::ios::binary);
    if (!out) {
        adm::core::Logger::Error("Screenshot: could not open '{}' for writing", path);
        return;
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
        adm::core::Logger::Error("Screenshot: could not write '{}'", path);
        return;
    }
    adm::core::Logger::Info("Screenshot: saved {}x{} to {}", w, h, path);
}

void EmulatorView::DrawEmulatorContent()
{
    bool running = IsRunning();

    // A machine whose Kickstart is not in the ROM folder boots the core's free AROS replacement
    // instead - never silently. A line of its own above the
    // picture, so its buttons are not clicks that capture the mouse.
    if (running) {
        const std::string notice = MissingKickstartNotice();
        if (!notice.empty() && notice != m_kickNoticeHidden) {
            // The buttons first: a narrow window clips the end of the line, not them
            if (m_onFindSystemFiles) {
                if (ImGui::SmallButton("Find ROMs"))
                    m_onFindSystemFiles();
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Hide##kicknotice"))
                m_kickNoticeHidden = notice;
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", notice.c_str());
        }
    }

    // Reserve space for toolbar at bottom (one or two rows depending on window width)
    float toolbarRows = m_needsTwoRowToolbar ? 2.0f : 1.0f;
    float toolbarHeight =
        toolbarRows * ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    float winWidth = ImGui::GetContentRegionAvail().x;
    float winHeight = ImGui::GetContentRegionAvail().y - toolbarHeight;

    ImGui::BeginChild("DisplayArea", ImVec2(winWidth, winHeight), false,
                      ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollbar);

    if (running) {
        StepEmulator();
        RenderEmulatorImage();
        HandleInput();
    } else {
        ImVec2 region = ImGui::GetContentRegionAvail();
        ImVec2 textSize = ImGui::CalcTextSize("No emulation running");
        ImGui::SetCursorPos(ImVec2((region.x - textSize.x) * 0.5f, (region.y - textSize.y) * 0.5f));
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No emulation running");
    }

    ImGui::EndChild();

    DrawToolbar();
}

// Advance the emulator one frame (no-op when not running or paused). Shared by the docked window
// content and the fullscreen overlay so the core is stepped exactly once per frame.
void EmulatorView::StepEmulator()
{
    if (!IsRunning() || m_isPaused)
        return;

    m_emulator->Update();
    m_advancedThisFrame = true;
    CaptureGifFrameIfRecording();
    HandlePostStepStop();
}

// After an Update(): pause + notify if a debug break fired or a RunUntilFrame target was
// reached. Any stop cancels a pending run-until-frame and restores turbo if RunUntilFrame
// enabled it. Shared by StepEmulator and the run-when-hidden stepping paths.
void EmulatorView::HandlePostStepStop()
{
    bool breakPending = m_emulator->IsDebugBreakPending();
    // A log-mode watchpoint records its hit and keeps running: breaking on every access
    // would make a wide read watchpoint unusable. Re-arm the trace exactly as a manual
    // resume does, so the machine continues cleanly instead of stuttering on the break.
    if (breakPending && m_debugBreakFilter && m_debugBreakFilter()) {
        breakPending = false;
        if (m_emulator->HasArmDebug())
            m_emulator->ArmDebugTrace();
    }
    const bool frameTargetHit =
        m_runUntilFrame && m_emulator->GetTotalFrameCount() >= *m_runUntilFrame;
    if (!breakPending && !frameTargetHit)
        return;

    if (m_runUntilFrame) {
        m_runUntilFrame.reset();
        m_emulator->SetFrameStopTarget(0);
        if (m_runUntilTurboAuto) {
            m_isTurbo = false;
            m_runUntilTurboAuto = false;
            m_emulator->SetSpeedMultiplier(1.0);
            m_emulator->SetMuted(m_isMuted);
        }
    }
    m_isPaused = true;
    m_runIsStepOver = false; // the transient step-over run (if any) has ended
    ReleaseMouse();
    if (m_onDebugBreak)
        m_onDebugBreak();
}

// Render the emulator framebuffer, aspect-fit and centred within the current window's content
// region, plus any registered overlays. Assumes the emulator is running.
void EmulatorView::RenderEmulatorImage()
{
    int emuWidth = m_emulator->GetWidth();
    int emuHeight = m_emulator->GetHeight();

    ImVec2 region = ImGui::GetContentRegionAvail();
    if (emuWidth <= 0 || emuHeight <= 0 || region.x <= 0.0f || region.y <= 0.0f)
        return;

    float aspect = m_emulator->GetAspectRatio();

    // Raster-frame mode insets the picture inside a larger rect standing for the whole beam, so
    // the offscreen colour clocks have somewhere to be drawn. The picture keeps its aspect ratio
    // — the margins grow around it — because the raster is sized as a multiple of the picture in
    // each axis rather than being fitted independently.
    const bool rasterMode = IsRasterFrameMode();
    const float rasterMulX = rasterMode ? m_rasterFrameMap.RasterToImageX() : 1.0f;
    const float rasterMulY = rasterMode ? m_rasterFrameMap.RasterToImageY() : 1.0f;
    const float outerAspect = aspect * (rasterMulX / rasterMulY);

    // Fit the outer rect (the raster in raster mode, the picture otherwise) to the region.
    float outerWidth = region.x;
    float outerHeight = region.x / outerAspect;
    if (outerHeight > region.y) {
        outerHeight = region.y;
        outerWidth = region.y * outerAspect;
    }

    const float renderWidth = outerWidth / rasterMulX;
    const float renderHeight = outerHeight / rasterMulY;

    const float outerX = (region.x - outerWidth) * 0.5f;
    const float outerY = (region.y - outerHeight) * 0.5f;

    // Offset of the picture inside the outer rect. Zero unless the raster surrounds it.
    const float insetX = rasterMode ? m_rasterFrameMap.ImageU0() * outerWidth : 0.0f;
    const float insetY = rasterMode ? m_rasterFrameMap.ImageV0() * outerHeight : 0.0f;

    const ImVec2 originPos = ImGui::GetCursorPos();
    const ImVec2 originScreen = ImGui::GetCursorScreenPos();
    const ImVec2 rasterMin = {originScreen.x + outerX, originScreen.y + outerY};
    const ImVec2 rasterMax = {rasterMin.x + outerWidth, rasterMin.y + outerHeight};

    ImGui::SetCursorPos(ImVec2(originPos.x + outerX + insetX, originPos.y + outerY + insetY));

    // Optionally run the framebuffer through the display post-process filters. The pass
    // renders off-screen at the on-screen size, so scanline/mask frequencies land on real
    // pixels; it falls back to the raw texture on any failure.
    GLuint displayTex = m_emulator->GetTextureID();
    if (m_filterParams.AnyEnabled() && m_filter) {
        GLuint filtered =
            m_filter->Apply(displayTex, emuWidth, emuHeight, (int)std::lround(renderWidth),
                            (int)std::lround(renderHeight), m_filterParams);
        if (filtered)
            displayTex = filtered;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
    ImGui::Image((ImTextureID)(uintptr_t)displayTex, ImVec2(renderWidth, renderHeight));
    dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);

    // Existing overlays (sprite boxes, DIW/DDF geometry, blit provenance) keep receiving the
    // picture's rect. That is what makes them correct in raster mode for free: every one of them
    // maps an Amiga coordinate through AmigaScreenMap, which describes the texture, so anchoring
    // them to where the texture actually is needs no change. Coordinates that fall outside the
    // picture extrapolate linearly into the blanking margin, which is where they belong.
    if (m_onDrawEmulatorOverlay) {
        ImVec2 imgMin = ImGui::GetItemRectMin();
        ImVec2 imgMax = ImGui::GetItemRectMax();
        m_onDrawEmulatorOverlay(ImGui::GetWindowDrawList(), imgMin, imgMax);
    }

    if (rasterMode && m_onDrawRasterOverlay) {
        ImVec2 imgMin = ImGui::GetItemRectMin();
        ImVec2 imgMax = ImGui::GetItemRectMax();
        m_onDrawRasterOverlay(ImGui::GetWindowDrawList(), rasterMin, rasterMax, imgMin, imgMax);
    }

    if (m_showPerfOverlay && m_emulator) {
        ImVec2 imgMin = ImGui::GetItemRectMin();
        ImVec2 imgMax = ImGui::GetItemRectMax();
        DrawPerfOverlay(ImGui::GetWindowDrawList(), imgMin, imgMax);
    }

    if (m_pixelPick)
        DrawPixelPick(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::IsItemHovered());
}

void EmulatorView::BeginPixelPick(std::function<void(bool, uint32_t)> done)
{
    if (auto previous = std::move(m_pixelPick)) {
        m_pixelPick = nullptr;
        previous(false, 0);
    }
    m_pixelPick = std::move(done);
}

void EmulatorView::DrawPixelPick(ImVec2 imgMin, ImVec2 imgMax, bool hovered)
{
    auto finish = [&](bool picked, uint32_t rgba) {
        auto done = std::move(m_pixelPick);
        m_pixelPick = nullptr;
        m_pixelPickFrame.clear();
        m_pixelPickEndFrame = ImGui::GetFrameCount();
        done(picked, rgba);
    };
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        finish(false, 0);
        return;
    }
    if (!hovered || imgMax.x <= imgMin.x || imgMax.y <= imgMin.y)
        return;
    // the display texture as the core drew it - before the display filters - read back while
    // the pointer is over it, so a running picture is picked as it is now
    int w = 0, h = 0;
    if (!m_emulator->CaptureFrameRGBA(m_pixelPickFrame, w, h) || w <= 0 || h <= 0)
        return;
    const ImVec2 mouse = ImGui::GetMousePos();
    const int x = std::clamp(
        static_cast<int>((mouse.x - imgMin.x) / (imgMax.x - imgMin.x) * static_cast<float>(w)), 0,
        w - 1);
    const int y = std::clamp(
        static_cast<int>((mouse.y - imgMin.y) / (imgMax.y - imgMin.y) * static_cast<float>(h)), 0,
        h - 1);
    const uint8_t* p = &m_pixelPickFrame[(static_cast<size_t>(y) * w + x) * 4];
    const uint32_t rgba = static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
                          static_cast<uint32_t>(p[2]) << 8 | 0xFFu;

    // the texel under the pointer framed, and what it is beside it
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float sx = (imgMax.x - imgMin.x) / static_cast<float>(w);
    const float sy = (imgMax.y - imgMin.y) / static_cast<float>(h);
    const ImVec2 t0(imgMin.x + x * sx, imgMin.y + y * sy);
    const ImVec2 t1(t0.x + std::max(sx, 1.0f), t0.y + std::max(sy, 1.0f));
    dl->AddRect(ImVec2(t0.x - 2, t0.y - 2), ImVec2(t1.x + 2, t1.y + 2), IM_COL32(0, 0, 0, 255));
    dl->AddRect(ImVec2(t0.x - 1, t0.y - 1), ImVec2(t1.x + 1, t1.y + 1),
                IM_COL32(255, 255, 255, 255));
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::BeginTooltip();
    ImGui::ColorButton("##pick", ImVec4(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1.0f),
                       ImGuiColorEditFlags_NoTooltip,
                       ImVec2(ImGui::GetFrameHeight() * 2, ImGui::GetFrameHeight()));
    ImGui::SameLine();
    ImGui::Text("#%02X%02X%02X  at %d,%d", p[0], p[1], p[2], x, y);
    ImGui::TextDisabled("Click to take it, Esc or right-click to cancel");
    ImGui::EndTooltip();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        finish(true, rgba);
}

// Fullscreen presentation. A separate borderless window covers the main viewport and renders the
// emulator on top of everything else. The dockable "Amiga Emulator" window is left completely
// untouched, so exiting fullscreen restores the exact prior dock/float position and size.
void EmulatorView::DrawFullscreenOverlay()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);

    if (m_enteringFullscreen) {
        ImGui::SetNextWindowFocus();
        m_enteringFullscreen = false;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));

    bool visible = ImGui::Begin("AmigaEmulatorFullscreen", nullptr, flags);
    if (visible) {
        // Same content as the docked window — emulator image plus the full toolbar chrome
        // (turbo, LEDs, drive menu, and the button to return to windowed mode).
        DrawEmulatorContent();
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    DrawFullscreenExitButton();
}

// Animated, auto-hiding "exit fullscreen" button that slides in from the top of the screen when
// the mouse rests near the top edge.
void EmulatorView::DrawFullscreenExitButton()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 mousePos = io.MousePos;

    // 1. Logic for Appearing
    bool mouseAtTop = (mousePos.y >= viewport->Pos.y && mousePos.y <= viewport->Pos.y + 50.0f);
    bool isMouseStill = (std::abs(io.MouseDelta.x) < 1.0f && std::abs(io.MouseDelta.y) < 1.0f);

    if (mouseAtTop && isMouseStill) {
        m_mouseHoverTopTimer += io.DeltaTime;
    } else {
        m_mouseHoverTopTimer = 0.0f;
    }

    if (m_mouseHoverTopTimer >= 2.0f) {
        m_showExitButton = true;
    }

    // 2. Logic for Disappearing
    // We assume the button area ends around Y=80 (approx button height + padding + offset)
    // If the button is visible, we check if mouse is below it.
    float buttonBottomArea = viewport->Pos.y + 100.0f;
    if (mousePos.y > buttonBottomArea) {
        m_mouseAwayTimer += io.DeltaTime;
    } else {
        m_mouseAwayTimer = 0.0f;
    }

    if (m_mouseAwayTimer >= 1.0f) {
        m_showExitButton = false;
    }

    // 3. Animation
    float targetY = m_showExitButton ? 20.0f : -60.0f;
    float speed = 200.0f * io.DeltaTime;
    if (m_exitButtonY < targetY) {
        m_exitButtonY = std::min(targetY, m_exitButtonY + speed);
    } else if (m_exitButtonY > targetY) {
        m_exitButtonY = std::max(targetY, m_exitButtonY - speed);
    }

    // 4. Rendering
    if (m_exitButtonY > -50.0f) {
        ImGui::SetNextWindowPos(
            ImVec2(viewport->Pos.x + (viewport->Size.x * 0.5f), viewport->Pos.y + m_exitButtonY),
            ImGuiCond_Always, ImVec2(0.5f, 0.0f));

        ImGuiWindowFlags overlayFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("ExitFullscreenOverlay", nullptr, overlayFlags);

        // Scale the icon size to match the button size
        ImGui::SetWindowFontScale(1.5f);

        float buttonSize = 48.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, buttonSize * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

        if (ImGui::Button(ICON_FA_CIRCLE_XMARK "##ExitFullscreen",
                          ImVec2(buttonSize, buttonSize))) {
            SetFullscreen(false);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Exit Fullscreen");
        }

        ImGui::PopStyleVar(2);
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

void EmulatorView::DrawToolbar()
{
    bool running = IsRunning();

    // Position toolbar at the bottom of the content area (one or two rows)
    float toolbarRows = m_needsTwoRowToolbar ? 2.0f : 1.0f;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() -
                         toolbarRows * ImGui::GetFrameHeightWithSpacing() -
                         ImGui::GetStyle().WindowPadding.y);
    ImGui::Separator();

    // Drives menu first (leftmost)
    DrawDriveMenu();

    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    // Goes through SetMuted() rather than poking m_isMuted directly, so this button and the
    // Debugger -> Audio window's mute are the same code path and cannot drift apart.
    // A click mutes; resting the mouse on it puts the volume's slider up over it.
    const int volume = m_emulator->GetVolume();
    const char* icon = m_isMuted || volume == 0 ? ICON_FA_VOLUME_XMARK
                       : volume < 50            ? ICON_FA_VOLUME_LOW
                                                : ICON_FA_VOLUME_HIGH;
    if (ImGui::Button(fmt::format("{}##Mute", icon).c_str()))
        SetMuted(!m_isMuted);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(m_isMuted ? "Unmute" : "Mute");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        m_volumeOpen = true;
    DrawVolumeSlider(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    // Read back from the emulator each frame rather than mirroring it here: Preferences can also
    // change the floppy volume, and a local copy would drift out of step with it.
    bool floppySound = m_emulator->GetFloppySound();
    if (ImGui::Checkbox("Disk Sound", &floppySound)) {
        if (running)
            m_emulator->SetFloppySound(floppySound);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const bool bridged = AnyDriveBridged();
    ImGui::BeginDisabled(!running || bridged);
    if (ImGui::Checkbox("Turbo", &m_isTurbo)) {
        if (running) {
            m_emulator->SetSpeedMultiplier(m_isTurbo ? 10.0 : 1.0);
            m_emulator->SetMuted(m_isTurbo || m_isMuted);
        }
    }
    ImGui::EndDisabled();
    if (bridged && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Turbo is unavailable while a drive is backed by real hardware.\n"
                          "FloppyBridge has to be serviced in real time to stay in step with\n"
                          "the disk's actual rotation - running the CPU faster desyncs it.");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button(m_isFullscreen ? ICON_FA_COMPRESS "##Fullscreen"
                                     : ICON_FA_EXPAND "##Fullscreen")) {
        SetFullscreen(!m_isFullscreen);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(m_isFullscreen ? "Exit Fullscreen" : "Fullscreen");
    ImGui::EndDisabled();

    // Pixel magnifier ("loupe") toggle. Highlighted while active; hover the display to zoom in.
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    bool magnifierActive = m_showMagnifier;
    if (magnifierActive)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS_PLUS "##Magnifier"))
        m_showMagnifier = !m_showMagnifier;
    if (magnifierActive)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(m_showMagnifier ? "Hide Magnifier" : "Magnifier (hover to zoom)");
    ImGui::EndDisabled();

    // Measure right edge of left controls to detect overlap with right-aligned indicators
    float leftEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;

    // Calculate total width of right-aligned controls
    float radius = 6.0f;
    float padding = ImGui::GetStyle().WindowPadding.x;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    ImVec2 pwrSize = ImGui::CalcTextSize("PWR");
    float rightWidth = pwrSize.x + gap + radius * 2;
    if (running) {
        for (int i = 0; i < 4; i++) {
            if (i == 0 || m_drives[i].enabled) {
                char label[8];
                snprintf(label, sizeof(label), "DF%d:", i);
                float driveWidth = ImGui::CalcTextSize(label).x + gap +
                                   SevenSegmentWidth(ImGui::GetTextLineHeight()) + gap * 2;
                rightWidth += driveWidth;
            }
        }

        const auto& hostMounts = m_emulator->GetHostMountService().GetEffectiveMounts();
        const int hostCount = static_cast<int>(std::count_if(
            hostMounts.begin(), hostMounts.end(), [](const auto& mount) { return mount.enabled; }));
        if (hostCount > 0) {
            const int indicators = m_emulator->HasPerUnitHdActivity() ? hostCount : 1;
            const float hostWidth = ImGui::CalcTextSize("DH0:").x + gap + radius * 2 + gap * 2;
            rightWidth += hostWidth * indicators;
        }
        if (m_emulator->HasCdDrive())
            rightWidth += ImGui::CalcTextSize("CD0:").x + gap + radius * 2 + gap * 2;
    }

    // Update two-row flag: switch to two rows when left and right controls would overlap
    float rightStartX = ImGui::GetWindowWidth() - padding - rightWidth;
    m_needsTwoRowToolbar = (leftEndX + gap > rightStartX);

    // Draw right-aligned indicators; if two rows, let cursor advance to next row naturally
    if (running) {
        if (!m_needsTwoRowToolbar)
            ImGui::SameLine();
        DrawDiskActivity();
        ImGui::SameLine();
        DrawPowerLed();
    } else {
        if (!m_needsTwoRowToolbar)
            ImGui::SameLine();
        DrawPowerLed();
    }
}

void EmulatorView::MountFromFileDialog(int drive)
{
    if (drive < 0 || drive > 3)
        return;

    nfdchar_t* outPath = nullptr;
    nfdfilteritem_t filters[1] = {{"Amiga Disk File", "adf,zip"}};
    nfdresult_t result = NFD_OpenDialog(&outPath, filters, 1, nullptr);

    // Reset ImGui input state because NFD_OpenDialog blocks and can miss key-up events
    ImGui::GetIO().ClearInputKeys();

    if (result != NFD_OKAY || !outPath)
        return;

    std::string filePath(outPath);
    NFD_FreePath(outPath);

    std::string ext = std::filesystem::path(filePath).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::string diskName = std::filesystem::path(filePath).filename().string();

    if (ext == ".zip") {
        std::vector<uint8_t> adfData;
        std::string entryName, error;
        // Drive-insert takes the bytes as-is, with no decode step, so only an ADF
        // member is usable here.
        if (adm::ExtractFromZip(filePath, adm::ZipPayload::AdfOnly, adfData, entryName, error)) {
            diskName = entryName;
            MountDisk(drive, adfData, diskName);
            m_drives[drive].sourcePath = filePath;
        } else {
            adm::core::Logger::Error("Emulator: Drive menu: {}", error);
        }
    } else {
        // Read ADF file
        std::ifstream file(filePath, std::ios::binary);
        if (file) {
            std::vector<uint8_t> adfData((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
            MountDisk(drive, adfData, diskName);
            m_drives[drive].sourcePath = filePath;
        }
    }
}

void EmulatorView::DrawDriveMenu()
{
    if (ImGui::Button(ICON_FA_FLOPPY_DISK " Drives")) {
        ImGui::OpenPopup("DrivesPopup");
    }

    if (ImGui::BeginPopup("DrivesPopup")) {
        if (ImGui::MenuItem("Hard Drives..."))
            m_hardDriveEditorOpen = true;
        for (int slot = 0; slot < 2; ++slot) {
            const std::string label = fmt::format("Mount Hardfile on DH{}...", slot);
            if (ImGui::MenuItem(label.c_str()))
                PickHardfile(slot);
        }
        ImGui::Separator();
        DrawCdDriveMenu();
        ImGui::Separator();
        if (ImGui::MenuItem("Launch WHDLoad Archive..."))
            PickWhdloadArchive();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Boot an .lha/.lzx WHDLoad install (or a game directory, .slave or "
                ".info) on DH0 with the core's own WHDLoad. Uses the selected machine; "
                "saved hard drives are left as they are.");
        DrawWhdloadSettingsMenu();
        if (HasPendingHostMountRestart()) {
            if (ImGui::MenuItem("Apply Hard Drives and Restart")) {
                if (!ApplyHostMountsAndRestart(m_hostMountUiError))
                    adm::core::Logger::Error("Emulator: {}", m_hostMountUiError);
            }
        }
        ImGui::Separator();
        for (int i = 0; i < 4; i++) {
            const bool bridged = m_drives[i].isBridged;
            std::string label;
            if (bridged) {
                // A live drive has no image name to show; say what hardware it is and
                // whether the core has it yet. IsBridgePending() is true while the
                // machine is stopped, so a stopped emulator reads "pending" here and
                // "live" the moment it starts.
                label =
                    fmt::format("DF{}: [Live{}] {} on {}", i, IsBridgePending(i) ? ", pending" : "",
                                m_drives[i].bridgeHardware.empty() ? std::string("drive")
                                                                   : m_drives[i].bridgeHardware,
                                m_drives[i].bridgeComPort);
            } else if (m_drives[i].hasDisk) {
                label = fmt::format("DF{}: {}", i, m_drives[i].diskName);
            } else {
                label = fmt::format("DF{}: [Empty]", i);
            }

            if (ImGui::BeginMenu(label.c_str())) {
                if (bridged) {
                    // Detaching is the only thing to do with a live slot from here: an
                    // image cannot share it, so Mount and Images below are locked.
                    if (ImGui::MenuItem(ICON_FA_EJECT " Detach live drive"))
                        DetachBridgeDrive(i);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Hand the physical drive back and make DF%d a "
                                          "file-backed, empty slot. Configure live drives in "
                                          "Disk > Floppy Drives.",
                                          i);
                    ImGui::TextDisabled("Backed by a real, spinning floppy");
                    ImGui::Separator();
                }

                // Eject
                ImGui::BeginDisabled(!m_drives[i].hasDisk || bridged);
                if (ImGui::MenuItem("Eject")) {
                    EjectDrive(i);
                }
                ImGui::EndDisabled();

                // Write protect. Reaches the guest: the core's forcedwrprot feeds
                // CIAA PRA /WPRO, so AmigaDOS reports "disk is write protected"
                // rather than the write silently going nowhere.
                {
                    const bool wp = m_drives[i].writeProtected;
                    ImGui::BeginDisabled(bridged);
                    if (MenuItemIcon(wp ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN, "Write protected",
                                     nullptr, wp) &&
                        m_onSetWriteProtect) {
                        m_onSetWriteProtect(i, !wp);
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        if (bridged)
                            ImGui::SetTooltip("The tab on the physical disk decides for a live "
                                              "drive.");
                        else
                            ImGui::SetTooltip("Refuses writes from both the application and the "
                                              "emulated Amiga.");
                    }
                }

                // Mount
                ImGui::BeginDisabled(!m_drives[i].enabled || bridged);
                if (ImGui::MenuItem("Mount..."))
                    MountFromFileDialog(i);
                ImGui::EndDisabled();

                // Enabled checkbox (DF1-DF3 only, DF0 always enabled). A live slot is
                // enabled by definition - the bridge needs a drive to back.
                if (i > 0) {
                    bool enabled = m_drives[i].enabled;
                    ImGui::BeginDisabled(bridged);
                    if (ImGui::Checkbox("Enabled", &enabled)) {
                        SetDriveEnabled(i, enabled);
                    }
                    ImGui::EndDisabled();
                }

                // Images submenu: mount any open document directly
                if (m_getDocumentInfos) {
                    auto docs = m_getDocumentInfos();
                    if (!docs.empty()) {
                        ImGui::Separator();
                        ImGui::BeginDisabled(!m_drives[i].enabled || bridged);
                        if (ImGui::BeginMenu("Images")) {
                            ImGui::EndDisabled();
                            for (const auto& doc : docs) {
                                bool isMounted = doc.mountedOnDrive >= 0;
                                ImGui::BeginDisabled(isMounted || doc.isHardDiskImage);
                                std::string label = doc.name;
                                if (isMounted)
                                    label += fmt::format("  (DF{})", doc.mountedOnDrive);
                                if (ImGui::MenuItem(label.c_str()) && m_onMountDocument)
                                    m_onMountDocument(i, doc.id);
                                ImGui::EndDisabled();
                                if (doc.isHardDiskImage &&
                                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                    ImGui::SetTooltip("A hard-disk image - mount it on DH0/DH1 "
                                                      "with Mount Hardfile above.");
                            }
                            ImGui::EndMenu();
                        } else {
                            ImGui::EndDisabled();
                        }
                    }
                }

                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }
}

void EmulatorView::DrawDiskActivity()
{
    float radius = 6.0f;
    float padding = ImGui::GetStyle().WindowPadding.x;
    float gap = ImGui::GetStyle().ItemSpacing.x;

    // Measure power LED section width: "PWR" + gap + LED
    ImVec2 pwrSize = ImGui::CalcTextSize("PWR");
    float powerLedWidth = pwrSize.x + gap + radius * 2;

    // Calculate total width for all active drives
    float totalDriveWidth = 0.0f;
    int activeDriveCount = 0;

    // We need to calculate the width first to position correctly from the right
    // Loop through drives to see which ones are enabled/active
    for (int i = 3; i >= 0; i--) {
        // Show DF0 always, others if enabled
        if (i == 0 || m_drives[i].enabled) {
            char label[8];
            snprintf(label, sizeof(label), "DF%d:", i);

            ImVec2 dfSize = ImGui::CalcTextSize(label);

            // Width = Label + gap + cylinder readout + gap (separator). There is no separate
            // activity LED: the readout itself is the lamp.
            float driveWidth =
                dfSize.x + gap + SevenSegmentWidth(ImGui::GetTextLineHeight()) + gap * 2;
            totalDriveWidth += driveWidth;
            activeDriveCount++;
        }
    }

    const auto& hostMounts = m_emulator->GetHostMountService().GetEffectiveMounts();
    const int hostCount = static_cast<int>(std::count_if(
        hostMounts.begin(), hostMounts.end(), [](const auto& mount) { return mount.enabled; }));
    if (hostCount > 0) {
        const int indicators = m_emulator->HasPerUnitHdActivity() ? hostCount : 1;
        totalDriveWidth +=
            (ImGui::CalcTextSize("DH0:").x + gap + radius * 2 + gap * 2) * indicators;
    }
    if (m_emulator->HasCdDrive())
        totalDriveWidth += ImGui::CalcTextSize("CD0:").x + gap + radius * 2 + gap * 2;

    // Total right-aligned width: drives + power LED
    float totalWidth = totalDriveWidth + powerLedWidth;

    // Position from right edge
    float startX = ImGui::GetWindowWidth() - padding - totalWidth;

    ImGui::SetCursorPosX(startX);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Draw drives from left to right (DF0 -> DF3)
    for (int i = 0; i < 4; i++) {
        if (i == 0 || m_drives[i].enabled) {
            char label[8];
            snprintf(label, sizeof(label), "DF%d:", i);
            int track = m_emulator->GetCurrentTrack(i);

            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", label);
            ImGui::SameLine();

            // The head position as the same seven-segment readout the Floppy Drives window
            // uses. No "Cyl:" label - the tooltip says what it is, and the digits are the
            // point of a status bar that has to stay narrow.
            //
            // The readout is also the drive lamp, which is why there is no separate LED beside
            // it: dim when the drive is idle, bright green while it reads, red while it writes.
            const bool active = m_emulator->IsDriveActive(i);
            const bool writing = m_emulator->IsDriveWriting(i);
            const ImVec4 segColor = !active   ? ImVec4(0.28f, 0.50f, 0.28f, 1.0f)
                                    : writing ? ImVec4(1.00f, 0.35f, 0.35f, 1.0f)
                                              : ImVec4(0.45f, 0.95f, 0.45f, 1.0f);

            const float segH = ImGui::GetTextLineHeight();
            const ImVec2 segPos = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(SevenSegmentWidth(segH), segH));
            DrawSevenSegment(drawList, segPos, segH, track, ImGui::GetColorU32(segColor),
                             ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.07f)));
            ImGui::SetItemTooltip("DF%d head at cylinder %d - %s", i, track,
                                  !active   ? "idle"
                                  : writing ? "writing"
                                            : "reading");

            ImGui::SameLine(0.0f, gap * 2.0f);
        }
    }
    DrawHardDriveActivity();
    DrawCdActivity();
}

void EmulatorView::DrawCdActivity()
{
    if (!m_emulator->HasCdDrive())
        return;
    const float radius = 6.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const auto status = GetCdStatus();
    const float cursorY = ImGui::GetCursorPosY();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "CD0:");
    ImGui::SameLine();
    const float cursorX = ImGui::GetCursorPosX();
    const float centerY = ImGui::GetItemRectMin().y + ImGui::GetTextLineHeight() * 0.5f;
    const ImVec2 center(ImGui::GetWindowPos().x + cursorX + radius, centerY);
    // Same lamp as DH: dim when idle, green while the drive reads.
    const ImU32 color = status.led ? IM_COL32(0, 255, 0, 255) : IM_COL32(0, 60, 0, 255);
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddCircleFilled(center, radius, color);
    drawList->AddCircle(center, radius, IM_COL32(80, 80, 80, 255), 0, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(center.x - radius - 2, center.y - radius - 2));
    if (ImGui::InvisibleButton("##CdLamp", ImVec2(radius * 2 + 4, radius * 2 + 4)))
        m_cdDriveWindowOpen = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("CD0: %s\n%s\nClick for the CD Drive window.",
                          status.inMachine.empty() ? "empty" : status.inMachine.c_str(),
                          status.source.empty() ? "" : status.source.string().c_str());
    ImGui::SetCursorPos(ImVec2(cursorX + radius * 2 + gap * 2, cursorY));
}

namespace {

// "Work" for a directory; "hardfile on Built-in IDE unit 0" for an image.
std::string DescribeMountForTooltip(const core::HostDirectoryMount& mount)
{
    if (mount.backingType == core::HostMountBackingType::Archive)
        return fmt::format("{} (archive)", mount.volumeLabel);
    if (!mount.IsHardfile())
        return mount.volumeLabel;
    return fmt::format("hardfile on {} unit {}", core::HardDriveControllerName(mount.controller),
                       mount.controllerUnit);
}

} // namespace

void EmulatorView::DrawHardDriveActivity()
{
    const auto& service = m_emulator->GetHostMountService();
    const auto& mounts = service.GetEffectiveMounts();
    const bool hasMount =
        std::any_of(mounts.begin(), mounts.end(), [](const auto& mount) { return mount.enabled; });
    if (!hasMount)
        return;

    const float radius = 6.0f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 windowPosition = ImGui::GetWindowPos();

    auto drawIndicator = [&](const char* label, int status, const char* tooltip) {
        const float cursorY = ImGui::GetCursorPosY();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", label);
        ImGui::SameLine();
        const float cursorX = ImGui::GetCursorPosX();
        const float centerY = ImGui::GetItemRectMin().y + ImGui::GetTextLineHeight() * 0.5f;
        const ImVec2 center(windowPosition.x + cursorX + radius, centerY);
        const ImU32 color = status == 2   ? IM_COL32(255, 0, 0, 255)
                            : status == 1 ? IM_COL32(0, 255, 0, 255)
                                          : IM_COL32(0, 60, 0, 255);
        drawList->AddCircleFilled(center, radius, color);
        drawList->AddCircle(center, radius, IM_COL32(80, 80, 80, 255), 0, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(center.x - radius - 2, center.y - radius - 2));
        ImGui::InvisibleButton(label, ImVec2(radius * 2 + 4, radius * 2 + 4));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        ImGui::SetCursorPos(ImVec2(cursorX + radius * 2 + gap * 2, cursorY));
    };

    if (!m_emulator->HasPerUnitHdActivity()) {
        const int status = m_emulator->ConsumeAggregateHdActivity();
        std::string tooltip =
            fmt::format("Aggregate hard-drive activity (older PuAE core)\nOperations: {}",
                        m_emulator->GetAggregateHdActivityCount());
        for (const auto& mount : mounts) {
            if (mount.enabled) {
                tooltip += fmt::format("\n{}: {}\n{}\n{}", core::HostMountDeviceName(mount.device),
                                       DescribeMountForTooltip(mount), mount.HostPath().string(),
                                       mount.readOnly ? "Read-only" : "Read-write");
            }
        }
        drawIndicator("HD:", status, tooltip.c_str());
        return;
    }

    for (int index = 0; index < 2; ++index) {
        if (!mounts[index].enabled)
            continue;
        const auto device = static_cast<core::HostMountDevice>(index);
        const int status = m_emulator->GetHostMountService().ConsumeActivity(device);
        const auto& activity = service.GetActivity()[index];
        const std::string tooltip = fmt::format(
            "{}: {}\n{}\n{}\nReads: {}  Writes: {}", core::HostMountDeviceName(device),
            DescribeMountForTooltip(mounts[index]), mounts[index].HostPath().string(),
            mounts[index].readOnly ? "Read-only" : "Read-write", activity.reads, activity.writes);
        char label[8];
        std::snprintf(label, sizeof(label), "DH%d:", index);
        drawIndicator(label, status, tooltip.c_str());
    }
}

void EmulatorView::PersistNormalizedHostMounts()
{
    if (!m_userPreferences)
        return;
    const auto& configured = m_emulator->GetHostMountService().GetConfiguredMounts();
    if (m_userPreferences->Get().hostDirectoryMounts != configured)
        m_userPreferences->SetHostDirectoryMounts(configured);
}

void EmulatorView::DrawPowerLed()
{
    bool running = IsRunning();
    float radius = 6.0f;
    float padding = ImGui::GetStyle().WindowPadding.x;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    ImVec2 pwrSize = ImGui::CalcTextSize("PWR");
    float powerLedWidth = pwrSize.x + gap + radius * 2;

    // Position: right-aligned, rightmost element
    float pwrStartX = ImGui::GetWindowWidth() - padding - powerLedWidth;

    ImGui::SetCursorPosX(pwrStartX);

    // The whole block is the button - label as well as lamp. A 12 px circle is a small target,
    // and the label reads as part of the same control, so clicking it and getting nothing is
    // surprising. Submitted before the visuals so they can react to hover.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float blockH = std::max(pwrSize.y, radius * 2.0f);
    ImGui::InvisibleButton("##power_led", ImVec2(powerLedWidth, blockH));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        if (running) {
            m_emulator->UnloadCore();
        } else {
            PowerOn();
        }
    }
    if (hovered) {
        ImGui::SetTooltip(running ? "Power Off" : "Power On");
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float midY = origin.y + blockH * 0.5f;

    drawList->AddText(ImVec2(origin.x, midY - pwrSize.y * 0.5f),
                      ImGui::GetColorU32(hovered ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                                 : ImVec4(0.7f, 0.7f, 0.7f, 1.0f)),
                      "PWR");

    // LED circle
    const ImVec2 center(origin.x + powerLedWidth - radius, midY);

    // Determine LED color based on power state
    // Note: PuAE's libretro build has gui_led() as a stub, so gui_data.powerled
    // and powerled_brightness are never populated. We show on/off based on running state.
    ImU32 ledColor;
    if (running) {
        ledColor = IM_COL32(255, 0, 0, 255); // Bright red when running
    } else {
        ledColor = IM_COL32(60, 0, 0, 255); // Dark red when off
    }

    drawList->AddCircleFilled(center, radius, ledColor);
    drawList->AddCircle(center, radius,
                        hovered ? IM_COL32(200, 200, 200, 255) : IM_COL32(80, 80, 80, 255), 0,
                        1.0f);
}

void EmulatorView::HandleInput()
{
    ImGuiIO& io = ImGui::GetIO();

    // Check if the current window (Child: DisplayArea) is hovered or focused.
    bool isHovered = ImGui::IsWindowHovered();
    bool isFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool applicationFocused =
        !m_window || glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE;

    // 1. Mouse Capture Management
    if (m_mouseCaptured) {
        // ImGui focus can briefly move between docked windows while processing a mouse click, so
        // it must not control capture lifetime. Release only for the explicit shortcuts or when
        // the operating-system window loses focus (for example, Alt-Tab).
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
            ImGui::IsKeyPressed(ImGuiKey_F12, false) || !applicationFocused)
            ReleaseMouse();
    } else {
        // Capture mouse on Left Click inside the display area (skipped when an overlay blocks
        // capture)
        // the eyedropper's click, the frame it ends on too, is the pick's and not the Amiga's
        bool captureBlocked = (m_mouseCaptureBlocker && m_mouseCaptureBlocker()) || m_pixelPick ||
                              ImGui::GetFrameCount() <= m_pixelPickEndFrame + 1;
        if (isHovered && !captureBlocked && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_mouseCaptured = true;
            m_suppressCaptureClick = true;
            ImGui::SetWindowFocus();
            if (m_window) {
                // GLFW's disabled cursor fakes relative motion by warping the cursor back to the
                // centre after every move. Over Remote Desktop the client keeps sending its own
                // absolute pointer position and applies the warp late or not at all, so motion
                // either jumps wildly or stops dead at the client window's edge. There, hide
                // the cursor instead and measure motion ourselves, discarding stale positions.
                m_remoteMouse = IsRemoteSession();
                if (m_remoteMouse) {
                    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
                    m_remoteWarps = 0;
                    m_remoteJumpsDropped = 0;
                    glfwGetCursorPos(m_window, &m_remoteLastX, &m_remoteLastY);
                    ClipCursorToWindow(m_window);
                    RecentreRemoteCursor();
                } else {
                    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                }
            }
        }
    }

    // Map ImGui keys to RetroKeys
    using K = adm::AmigaEmulator::RetroKey;
    struct KeyMap
    {
        ImGuiKey imgui;
        K retro;
    };

    static const KeyMap kKeys[] = {{ImGuiKey_Backspace, K::Backspace},
                                   {ImGuiKey_Tab, K::Tab},
                                   {ImGuiKey_Enter, K::Return},
                                   {ImGuiKey_Escape, K::Escape},
                                   {ImGuiKey_Space, K::Space},
                                   {ImGuiKey_Delete, K::Delete},
                                   {ImGuiKey_UpArrow, K::Up},
                                   {ImGuiKey_DownArrow, K::Down},
                                   {ImGuiKey_LeftArrow, K::Left},
                                   {ImGuiKey_RightArrow, K::Right},
                                   {ImGuiKey_Insert, K::Insert},
                                   {ImGuiKey_Home, K::Help},
                                   {ImGuiKey_End, K::End},
                                   {ImGuiKey_PageUp, K::PageUp},
                                   {ImGuiKey_PageDown, K::PageDown},
                                   {ImGuiKey_F1, K::F1},
                                   {ImGuiKey_F2, K::F2},
                                   {ImGuiKey_F3, K::F3},
                                   {ImGuiKey_F4, K::F4},
                                   {ImGuiKey_F5, K::F5},
                                   {ImGuiKey_F6, K::F6},
                                   {ImGuiKey_F7, K::F7},
                                   {ImGuiKey_F8, K::F8},
                                   {ImGuiKey_F9, K::F9},
                                   {ImGuiKey_F10, K::F10},
                                   {ImGuiKey_F11, K::F11},
                                   // Punctuation
                                   {ImGuiKey_GraveAccent, K::Backquote},
                                   {ImGuiKey_Minus, K::Minus},
                                   {ImGuiKey_Equal, K::Equals},
                                   {ImGuiKey_LeftBracket, K::LeftBracket},
                                   {ImGuiKey_RightBracket, K::RightBracket},
                                   {ImGuiKey_Backslash, K::Backslash},
                                   {ImGuiKey_Semicolon, K::Semicolon},
                                   {ImGuiKey_Apostrophe, K::Quote},
                                   {ImGuiKey_Comma, K::Comma},
                                   {ImGuiKey_Period, K::Period},
                                   {ImGuiKey_Slash, K::Slash},
                                   // Numbers
                                   {ImGuiKey_0, K::Num0},
                                   {ImGuiKey_1, K::Num1},
                                   {ImGuiKey_2, K::Num2},
                                   {ImGuiKey_3, K::Num3},
                                   {ImGuiKey_4, K::Num4},
                                   {ImGuiKey_5, K::Num5},
                                   {ImGuiKey_6, K::Num6},
                                   {ImGuiKey_7, K::Num7},
                                   {ImGuiKey_8, K::Num8},
                                   {ImGuiKey_9, K::Num9},
                                   // Letters
                                   {ImGuiKey_A, K::A},
                                   {ImGuiKey_B, K::B},
                                   {ImGuiKey_C, K::C},
                                   {ImGuiKey_D, K::D},
                                   {ImGuiKey_E, K::E},
                                   {ImGuiKey_F, K::F},
                                   {ImGuiKey_G, K::G},
                                   {ImGuiKey_H, K::H},
                                   {ImGuiKey_I, K::I},
                                   {ImGuiKey_J, K::J},
                                   {ImGuiKey_K, K::K},
                                   {ImGuiKey_L, K::L},
                                   {ImGuiKey_M, K::M},
                                   {ImGuiKey_N, K::N},
                                   {ImGuiKey_O, K::O},
                                   {ImGuiKey_P, K::P},
                                   {ImGuiKey_Q, K::Q},
                                   {ImGuiKey_R, K::R},
                                   {ImGuiKey_S, K::S},
                                   {ImGuiKey_T, K::T},
                                   {ImGuiKey_U, K::U},
                                   {ImGuiKey_V, K::V},
                                   {ImGuiKey_W, K::W},
                                   {ImGuiKey_X, K::X},
                                   {ImGuiKey_Y, K::Y},
                                   {ImGuiKey_Z, K::Z},
                                   // Modifiers
                                   {ImGuiKey_CapsLock, K::CapsLock},
                                   {ImGuiKey_LeftShift, K::LShift},
                                   {ImGuiKey_RightShift, K::RShift},
                                   {ImGuiKey_LeftCtrl, K::LCtrl},
                                   {ImGuiKey_RightCtrl, K::RCtrl},
                                   {ImGuiKey_LeftAlt, K::LAlt},
                                   {ImGuiKey_RightAlt, K::RAlt},
                                   {ImGuiKey_LeftSuper, K::LSuper},
                                   {ImGuiKey_RightSuper, K::RSuper},
                                   // Numeric keypad
                                   {ImGuiKey_NumLock, K::NumLock},
                                   {ImGuiKey_Keypad0, K::Kp0},
                                   {ImGuiKey_Keypad1, K::Kp1},
                                   {ImGuiKey_Keypad2, K::Kp2},
                                   {ImGuiKey_Keypad3, K::Kp3},
                                   {ImGuiKey_Keypad4, K::Kp4},
                                   {ImGuiKey_Keypad5, K::Kp5},
                                   {ImGuiKey_Keypad6, K::Kp6},
                                   {ImGuiKey_Keypad7, K::Kp7},
                                   {ImGuiKey_Keypad8, K::Kp8},
                                   {ImGuiKey_Keypad9, K::Kp9},
                                   {ImGuiKey_KeypadDecimal, K::KpPeriod},
                                   {ImGuiKey_KeypadDivide, K::KpDivide},
                                   {ImGuiKey_KeypadMultiply, K::KpMultiply},
                                   {ImGuiKey_KeypadSubtract, K::KpMinus},
                                   {ImGuiKey_KeypadAdd, K::KpPlus},
                                   {ImGuiKey_KeypadEnter, K::KpEnter},
                                   {ImGuiKey_KeypadEqual, K::KpEquals}};

    bool handleKeyboard = m_mouseCaptured;

    // When the keyboard joystick is active, the cursor keys and A/S keys drive
    // the joystick instead of being delivered to the Amiga keyboard.
    auto isJoystickKey = [](ImGuiKey k) {
        return k == ImGuiKey_UpArrow || k == ImGuiKey_DownArrow || k == ImGuiKey_LeftArrow ||
               k == ImGuiKey_RightArrow || k == ImGuiKey_A || k == ImGuiKey_S;
    };

    // Held-key state lives in members (not function-local statics): statics were shared
    // across view instances and survived core Stop/Start, and the per-frame unordered_set
    // rebuild was ~110 hash inserts + ~300 lookups per frame while captured.
    auto prevDown = [this](int code) {
        return code >= 0 && code < static_cast<int>(m_prevKeys.size()) && m_prevKeys[code];
    };
    if (handleKeyboard) {
        std::array<bool, kRetroKeyStates> currentKeys{};
        for (const auto& map : kKeys) {
            const int code = static_cast<int>(map.retro);
            if (m_keyboardJoystickEnabled && isJoystickKey(map.imgui)) {
                // If the key was previously held as a normal key, release it.
                if (prevDown(code))
                    m_emulator->SendKeyEvent(false, map.retro);
                continue;
            }
            bool down = ImGui::IsKeyDown(map.imgui);
            if (down && code >= 0 && code < static_cast<int>(currentKeys.size())) {
                currentKeys[code] = true;
            }
            // Press / release transitions
            if (down && !prevDown(code)) {
                m_emulator->SendKeyEvent(true, map.retro);
            }
            if (!down && prevDown(code)) {
                m_emulator->SendKeyEvent(false, map.retro);
            }
        }
        m_prevKeys = currentKeys;
        m_keysActive = true;
    } else if (m_keysActive) {
        for (const auto& map : kKeys) {
            if (prevDown(static_cast<int>(map.retro))) {
                m_emulator->SendKeyEvent(false, map.retro);
            }
        }
        m_prevKeys.fill(false);
        m_keysActive = false;
    }

    // Keyboard joystick: drive port-1 joystick from cursor keys + A/S. Works
    // whenever the emulator window is focused (no mouse capture required).
    if (m_keyboardJoystickEnabled && m_emulator) {
        bool active = isFocused;
        m_emulator->SetKeyboardJoyButton(RETRO_DEVICE_ID_JOYPAD_UP,
                                         active && ImGui::IsKeyDown(ImGuiKey_UpArrow));
        m_emulator->SetKeyboardJoyButton(RETRO_DEVICE_ID_JOYPAD_DOWN,
                                         active && ImGui::IsKeyDown(ImGuiKey_DownArrow));
        m_emulator->SetKeyboardJoyButton(RETRO_DEVICE_ID_JOYPAD_LEFT,
                                         active && ImGui::IsKeyDown(ImGuiKey_LeftArrow));
        m_emulator->SetKeyboardJoyButton(RETRO_DEVICE_ID_JOYPAD_RIGHT,
                                         active && ImGui::IsKeyDown(ImGuiKey_RightArrow));
        // A = fire button 1, S = fire button 2
        m_emulator->SetKeyboardJoyButton(RETRO_DEVICE_ID_JOYPAD_B,
                                         active && ImGui::IsKeyDown(ImGuiKey_A));
        m_emulator->SetKeyboardJoyButton(RETRO_DEVICE_ID_JOYPAD_A,
                                         active && ImGui::IsKeyDown(ImGuiKey_S));
    }

    if (!m_mouseCaptured) {
        return;
    }

    int16_t deltaX = static_cast<int16_t>(io.MouseDelta.x);
    int16_t deltaY = static_cast<int16_t>(io.MouseDelta.y);
    if (m_remoteMouse)
        ReadRemoteMouseDelta(deltaX, deltaY);

    bool leftBtn = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool rightBtn = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    bool middleBtn = ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    if (m_suppressCaptureClick) {
        leftBtn = false;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_suppressCaptureClick = false;
        }
    }

    m_emulator->SetMouseState(deltaX, deltaY, leftBtn, rightBtn, middleBtn);
}

void EmulatorView::SetControllerType(const std::string& name, const std::string& type)
{
    if (type == "CD32 Controller") {
        m_emulator->SetControllerType(name, adm::ControllerType::CD32Controller);
    } else {
        m_emulator->SetControllerType(name, adm::ControllerType::StandardJoystick);
    }
}

void EmulatorView::SetKeyboardJoystickEnabled(bool enabled)
{
    m_keyboardJoystickEnabled = enabled;
    if (m_emulator)
        m_emulator->SetKeyboardJoystickEnabled(enabled);
}

std::vector<adm::AmigaEmulator::ConnectedController> EmulatorView::GetConnectedControllers() const
{
    if (m_emulator)
        return m_emulator->GetConnectedControllers();
    return {};
}

void EmulatorView::RefreshConnectedControllers()
{
    if (m_emulator)
        m_emulator->RefreshConnectedControllers();
}

void EmulatorView::SetModel(adm::AmigaModel model)
{
    if (!m_emulator)
        return;
    const bool restart = IsRunning();
    m_emulator->ClearUaeConfig();
    m_selectedBaseUaeConfig.reset();
    m_selectedCustomConfigurationName.clear();
    const bool configured = m_emulator->SelectModelPreset(model);
    if (m_userPreferences) {
        const auto& variables = m_emulator->GetUserVariables();
        m_userPreferences->SetEmulatorModel(adm::GetAmigaModelInfo(model).id);
        m_userPreferences->SetEmulatorSettings(
            std::map<std::string, std::string>(variables.begin(), variables.end()));
    }
    if (restart) {
        if (!configured) {
            adm::core::Logger::Warn(
                "Emulator: in-place configuration power cycle failed; reloading the core");
            Stop();
        }
        if (!PowerOn())
            adm::core::Logger::Error("Emulator: failed to restart with the selected default model");
        else
            m_isPaused = false;
    }
}

void EmulatorView::SetMachineSetting(const std::string& key, const std::string& value)
{
    if (!m_emulator)
        return;
    m_emulator->SetUserVariable(key, value);
    if (m_userPreferences) {
        const auto& variables = m_emulator->GetUserVariables();
        m_userPreferences->SetEmulatorSettings(
            std::map<std::string, std::string>(variables.begin(), variables.end()));
    }
}

void EmulatorView::SetSerialPort(const core::SerialPortTarget& target)
{
    if (!m_emulator)
        return;
    m_emulator->SetSerialPort(target);
    if (m_userPreferences)
        m_userPreferences->SetHostPorts(target.ToCoreName(),
                                        PathToUtf8(m_emulator->GetParallelOutput()));
}

void EmulatorView::SetParallelOutput(const std::filesystem::path& path)
{
    if (!m_emulator)
        return;
    m_emulator->SetParallelOutput(path);
    if (m_userPreferences)
        m_userPreferences->SetHostPorts(m_emulator->GetSerialPort().ToCoreName(), PathToUtf8(path));
}

void EmulatorView::SetMidiDevices(int outDevice, int inDevice)
{
    if (!m_emulator)
        return;
    m_emulator->SetMidiDevices(outDevice, inDevice);
    if (m_userPreferences)
        m_userPreferences->SetMidiDevices(m_emulator->GetMidiOutDevice(),
                                          m_emulator->GetMidiInDevice());
}

bool EmulatorView::RestartMachine(std::string& outError)
{
    if (!m_emulator || !IsRunning()) {
        outError = "the machine is not running; the settings apply when it is powered on";
        return false;
    }
    // PuAE snapshots core options in retro_init(), so a reload inside the same initialised core
    // would rebuild the old machine first. Reinitialising makes this Stop followed by Power On.
    if (!m_emulator->ReinitCore()) {
        adm::core::Logger::Warn("Emulator: in-place power cycle failed; reloading the core");
        Stop();
    }
    if (!PowerOn()) {
        outError = "the machine did not start again";
        return false;
    }
    m_isPaused = false;
    return true;
}

std::string EmulatorView::GetRomDirectory() const
{
    return m_emulator ? m_emulator->GetSystemDirectory() : std::string();
}

std::string EmulatorView::MissingKickstartNotice()
{
    // Looked at every two seconds, not every frame: it touches the file system
    const auto now = std::chrono::steady_clock::now();
    if (now - m_kickNoticeChecked < std::chrono::seconds(2))
        return m_kickNotice;
    m_kickNoticeChecked = now;
    m_kickNotice.clear();
    // A configuration of the user's own names its own ROM
    if (!m_emulator || !m_selectedCustomConfigurationName.empty() || m_selectedBaseUaeConfig)
        return m_kickNotice;

    const auto& vars = m_emulator->GetUserVariables();
    std::string kickstart = "auto";
    if (const auto it = vars.find("puae_kickstart"); it != vars.end() && !it->second.empty())
        kickstart = it->second;
    if (kickstart == "aros")
        return m_kickNotice; // AROS by choice
    const auto romDir = PathFromUtf8(m_emulator->GetSystemDirectory());
    const std::string model = GetCurrentModelId();
    // Automatic: the preset's ROMs. A bare name (what a preset writes): that one, which the core
    // also loads under its Amiga Forever and TOSEC names. A path: that file.
    std::vector<std::string> needed;
    if (kickstart == "auto") {
        needed = core::PresetRomNames(model);
    } else if (const auto picked = PathFromUtf8(kickstart); picked.has_parent_path()) {
        std::error_code ec;
        if (!std::filesystem::exists(picked, ec))
            m_kickNotice = fmt::format("The Kickstart {} is not there: this machine is running "
                                       "AROS, the free Kickstart.",
                                       PathToUtf8(picked.filename()));
        return m_kickNotice;
    } else {
        needed = {kickstart};
    }
    for (const auto& rom : needed) {
        if (!core::FindPresetRom(romDir, rom, {})) {
            m_kickNotice = fmt::format("No {} in the ROM folder: this {} is running AROS, the "
                                       "free Kickstart.",
                                       rom, model);
            break;
        }
    }
    return m_kickNotice;
}

std::string EmulatorView::GetModelUnavailableReason(adm::AmigaModel model) const
{
    auto& cache = m_modelAvailability;
    const auto now = std::chrono::steady_clock::now();
    const std::string& romDirectory = m_emulator->GetSystemDirectory();
    if (cache.romDirectory != romDirectory || now - cache.refreshed > std::chrono::seconds(2)) {
        cache.romDirectory = romDirectory;
        cache.refreshed = now;
        cache.reasons.clear();
    }
    const int key = static_cast<int>(model);
    if (const auto it = cache.reasons.find(key); it != cache.reasons.end())
        return it->second;
    std::string reason = core::MachineRoms::MissingRoms(adm::GetAmigaModelInfo(model).id,
                                                        PathFromUtf8(romDirectory));
    cache.reasons.emplace(key, reason);
    return reason;
}

std::string EmulatorView::GetCurrentModelId() const
{
    if (m_emulator)
        return adm::GetAmigaModelInfo(m_emulator->GetCurrentModel()).id;
    return "A500";
}

void EmulatorView::SetFullscreen(bool fullscreen)
{
    if (m_isFullscreen != fullscreen) {
        if (fullscreen) { // Entering fullscreen
            // Reset exit button state so it works correctly on re-entry
            m_mouseHoverTopTimer = 0.0f;
            m_mouseAwayTimer = 0.0f;
            m_exitButtonY = -60.0f;
            m_showExitButton = false;
            m_enteringFullscreen = true;
        } else { // Exiting
            // Re-focus the dockable window when it is resubmitted so keyboard focus returns to
            // it. It re-docks automatically from its retained settings.
            m_shouldFocus = true;
            ReleaseMouse();
        }

        m_isFullscreen = fullscreen;
        if (m_onFullscreenToggle) {
            m_onFullscreenToggle(fullscreen);
        }
    }
}

void EmulatorView::DrawPerfOverlay(ImDrawList* dl, ImVec2 imgMin, ImVec2 imgMax)
{
    (void)imgMax; // not needed for fixed top-left panel
    // Linearise circular buffer into fpsArr / msArr
    int count = 0, head = 0;
    const auto* history = m_emulator->GetPerfHistory(count, head);
    const auto stats = m_emulator->GetPerfStats();
    const bool showDemoStats = stats.demoAvailable;

    // Panel geometry
    const float padding = 6.0f;
    const float lineHeight = ImGui::GetTextLineHeight();
    const float rowSpacing = ImGui::GetStyle().ItemSpacing.y;

    const float graphW = 300.0f;
    const float graphH = 100.0f;
    const float graphPad = 30.0f; // left/right margin for y-axis labels

    const ImU32 colPlain = IM_COL32(200, 200, 200, 255);
    const ImU32 colDim = IM_COL32(140, 140, 140, 255);
    const ImU32 colGood = IM_COL32(80, 220, 80, 255);
    const ImU32 colWarn = IM_COL32(240, 200, 60, 255);
    const ImU32 colBad = IM_COL32(220, 60, 60, 255);
    auto fpsColour = [&](float fps) {
        if (fps >= 48.0f)
            return colGood;
        if (fps >= 40.0f)
            return colWarn;
        return colBad;
    };

    // Every row is one line of text on a uniform pitch, so the profile block below simply
    // appends to the list and the panel grows to match.
    struct Row
    {
        std::string text;
        ImU32 color;
    };
    std::vector<Row> rows;
    char buf[128];

    snprintf(buf, sizeof(buf), "Emu %5.1f fps", stats.currentFps);
    rows.push_back({buf, fpsColour(stats.currentFps)});

    snprintf(buf, sizeof(buf), "Core %4.1f ms", stats.avgCoreMs);
    rows.push_back({buf, colPlain});

    if (showDemoStats) {
        snprintf(buf, sizeof(buf), "Demo %5.1f fps", stats.demoFps);
        rows.push_back({buf, fpsColour(stats.demoFps)});

        snprintf(buf, sizeof(buf), "Idle %5.1f%%", stats.idlePercent);
        rows.push_back({buf, IM_COL32(100, 210, 120, 255)});
    }

    // The running program's own profiler, when it is a Crass-engine build with PROFILE compiled
    // in. Deliberately independent of demoAvailable: the Amiga program counts these itself out
    // of CIA-B's HSYNC counter, so they need neither the CPU trace nor the Bartman idle markers
    // that Demo fps / Idle % are derived from. With nothing registered no row is added at all,
    // and the overlay is exactly what it was.
    // A crAss engine tool, so only with private features on (core/PrivateFeatures.h).
    const size_t firstProfileRow = rows.size();
    static const core::CrassProfileStats kNoProfile{};
    const auto& prof = core::PrivateFeatures() ? m_emulator->GetCrassProfileStats() : kNoProfile;
    if (prof.present && !prof.timingOk) {
        rows.push_back({"Profile: line counter not running", colDim});
    } else if (prof.present) {
        if (prof.lastFields == 0) {
            // No window has completed yet, so only the live count means anything.
            snprintf(buf, sizeof(buf), "Frames  --  (%u rendered)",
                     static_cast<unsigned>(prof.rendered));
            rows.push_back({buf, colDim});
        } else {
            snprintf(
                buf, sizeof(buf), "Frames %u/%u  %u drop  gap %u",
                static_cast<unsigned>(prof.lastRendered), static_cast<unsigned>(prof.lastFields),
                static_cast<unsigned>(prof.lastDropped), static_cast<unsigned>(prof.lastWorstGap));
            rows.push_back({buf, prof.lastDropped == 0     ? colGood
                                 : (prof.lastDropped <= 2) ? colWarn
                                                           : colBad});
        }

        // One field's worth of raster lines. A section whose worst case exceeds that cannot fit
        // inside a frame, which is where the next dropped field comes from.
        const unsigned fieldLines = prof.linesPer2Fields / 2u;
        for (const auto& sec : prof.sections) {
            ImU32 color = colPlain;
            if (prof.lastFields == 0) {
                snprintf(buf, sizeof(buf), "%s  %u", sec.name.c_str(),
                         static_cast<unsigned>(sec.lastLines));
                color = colDim;
            } else if (sec.lastCount == 0) {
                snprintf(buf, sizeof(buf), "%s  not called", sec.name.c_str());
                color = colDim;
            } else {
                snprintf(buf, sizeof(buf), "%s  %u  (%u-%u-%u)", sec.name.c_str(),
                         static_cast<unsigned>(sec.lastLines), static_cast<unsigned>(sec.lastMin),
                         static_cast<unsigned>(sec.lastAvg), static_cast<unsigned>(sec.lastMax));
                if (sec.lastMax > fieldLines)
                    color = colWarn;
            }
            rows.push_back({buf, color});
        }
    }

    // Fixed placeholders keep the panel from breathing as the fps digits change; the profile
    // rows are measured as they read, because a section name is whatever the program called it.
    float textWidth =
        std::max(ImGui::CalcTextSize("Emu 000.0 fps").x, ImGui::CalcTextSize("Core 00.0 ms").x);
    if (showDemoStats) {
        textWidth = std::max(textWidth, ImGui::CalcTextSize("Demo 000.0 fps").x);
        textWidth = std::max(textWidth, ImGui::CalcTextSize("Idle 100.0%").x);
    }
    for (size_t i = firstProfileRow; i < rows.size(); ++i)
        textWidth = std::max(textWidth, ImGui::CalcTextSize(rows[i].text.c_str()).x);

    const float rowCount = static_cast<float>(rows.size());
    float panelW = padding * 2.0f + textWidth;
    // The graph keeps its 300 px plot; the panel only ever widens past it to fit longer text.
    if (m_showPerfGraph)
        panelW = std::max(panelW, padding * 2.0f + graphW + graphPad * 2.0f);

    float panelH = padding * 2.0f + lineHeight * rowCount + rowSpacing * (rowCount - 1.0f);
    if (m_showPerfGraph)
        panelH += rowSpacing + graphH + 18.0f; // extra for axis labels

    // Position: top-left corner of the emulator image
    ImVec2 panelMin(imgMin.x + 8.0f, imgMin.y + 8.0f);
    ImVec2 panelMax(panelMin.x + panelW, panelMin.y + panelH);

    // Background
    dl->AddRectFilled(panelMin, panelMax, IM_COL32(10, 10, 10, 180), 4.0f);
    dl->AddRect(panelMin, panelMax, IM_COL32(80, 80, 80, 120), 4.0f);

    float tx = panelMin.x + padding;
    float ty = panelMin.y + padding;
    for (const auto& row : rows) {
        dl->AddText(ImVec2(tx, ty), row.color, row.text.c_str());
        ty += lineHeight + rowSpacing;
    }
    ty -= rowSpacing; // the last row carries no trailing gap

    if (!m_showPerfGraph || count == 0)
        return;

    // Build arrays in chronological order (oldest first)
    std::vector<float> fpsArr(count), msArr(count);
    int oldest = (head - count + adm::AmigaEmulator::kPerfHistorySize) %
                 adm::AmigaEmulator::kPerfHistorySize;
    for (int i = 0; i < count; ++i) {
        int idx = (oldest + i) % adm::AmigaEmulator::kPerfHistorySize;
        fpsArr[i] = history[idx].rollingFps;
        msArr[i] = history[idx].coreMs;
    }

    // Graph area
    float gx = panelMin.x + padding + graphPad;
    float gy = ty + ImGui::GetStyle().ItemSpacing.y + 4.0f;
    ImVec2 gMin(gx, gy);
    ImVec2 gMax(gx + graphW, gy + graphH);

    dl->AddRectFilled(gMin, gMax, IM_COL32(20, 20, 20, 200));

    // Reference lines
    // FPS ref at 50 fps — maps to y within 0-60 range
    float fps60 = 60.0f, fps0 = 0.0f;
    float ms40 = 40.0f, ms0 = 0.0f;
    auto fpsToY = [&](float fps) { return gMax.y - (fps - fps0) / (fps60 - fps0) * graphH; };
    auto msToY = [&](float ms) { return gMax.y - (ms - ms0) / (ms40 - ms0) * graphH; };

    // Dashed reference at 50 fps
    float refY50 = fpsToY(50.0f);
    for (float rx = gMin.x; rx < gMax.x; rx += 8.0f)
        dl->AddLine(ImVec2(rx, refY50), ImVec2(rx + 4.0f, refY50), IM_COL32(180, 180, 180, 60));

    // Dashed reference at 20 ms
    float refYMs = msToY(20.0f);
    for (float rx = gMin.x; rx < gMax.x; rx += 8.0f)
        dl->AddLine(ImVec2(rx, refYMs), ImVec2(rx + 4.0f, refYMs), IM_COL32(200, 160, 60, 60));

    // Draw FPS line (green)
    if (count > 1) {
        float xStep = graphW / float(count - 1);
        for (int i = 0; i + 1 < count; ++i) {
            float x0 = gMin.x + i * xStep;
            float x1 = gMin.x + (i + 1) * xStep;
            float y0 = fpsToY(std::min(fpsArr[i], fps60));
            float y1 = fpsToY(std::min(fpsArr[i + 1], fps60));
            y0 = std::max(gMin.y, std::min(gMax.y, y0));
            y1 = std::max(gMin.y, std::min(gMax.y, y1));
            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(80, 220, 80, 220), 1.5f);
        }
    }

    // Draw core ms line (yellow)
    if (count > 1) {
        float xStep = graphW / float(count - 1);
        for (int i = 0; i + 1 < count; ++i) {
            float x0 = gMin.x + i * xStep;
            float x1 = gMin.x + (i + 1) * xStep;
            float y0 = msToY(std::min(msArr[i], ms40));
            float y1 = msToY(std::min(msArr[i + 1], ms40));
            y0 = std::max(gMin.y, std::min(gMax.y, y0));
            y1 = std::max(gMin.y, std::min(gMax.y, y1));
            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(240, 200, 60, 220), 1.5f);
        }
    }

    // Axis labels (left: FPS scale, right: ms scale)
    float smallFont = lineHeight * 0.75f; // approximate small font size
    (void)smallFont;

    // Left axis labels
    dl->AddText(ImVec2(gMin.x - graphPad, gMin.y - 2.0f), IM_COL32(140, 140, 140, 220), "60");
    dl->AddText(ImVec2(gMin.x - graphPad, gMax.y - lineHeight + 2.0f), IM_COL32(140, 140, 140, 220),
                "0");

    // Right axis labels
    dl->AddText(ImVec2(gMax.x + 2.0f, gMin.y - 2.0f), IM_COL32(140, 140, 140, 220), "40ms");
    dl->AddText(ImVec2(gMax.x + 2.0f, gMax.y - lineHeight + 2.0f), IM_COL32(140, 140, 140, 220),
                "0");

    // Legend inside graph top
    float legX = gMin.x + 4.0f;
    float legY = gMin.y + 2.0f;
    dl->AddText(ImVec2(legX, legY), IM_COL32(80, 220, 80, 220), "FPS");
    float fpsLabelW = ImGui::CalcTextSize("FPS").x;
    dl->AddLine(ImVec2(legX + fpsLabelW + 2.0f, legY + lineHeight * 0.5f),
                ImVec2(legX + fpsLabelW + 18.0f, legY + lineHeight * 0.5f),
                IM_COL32(80, 220, 80, 220), 1.5f);

    float coreX = legX + fpsLabelW + 22.0f;
    dl->AddText(ImVec2(coreX, legY), IM_COL32(240, 200, 60, 220), "Core");
    float coreLabelW = ImGui::CalcTextSize("Core").x;
    dl->AddLine(ImVec2(coreX + coreLabelW + 2.0f, legY + lineHeight * 0.5f),
                ImVec2(coreX + coreLabelW + 18.0f, legY + lineHeight * 0.5f),
                IM_COL32(240, 200, 60, 220), 1.5f);
}

} // namespace adm::ui
