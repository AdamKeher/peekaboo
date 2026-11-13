#include "ViewerLauncher.h"

#include <memory>
#include <vector>

#include "../core/AdfWrapper.h"
#include "../core/AudioEngine.h"
#include "../core/AudioModule.h"
#include "../core/DiskDocument.h"
#include "../core/HunkParser.h"
#include "../core/Logger.h"
#include "../core/UserPreferences.h"
#include "FontManager.h"
#include "TextureManager.h"
#include "views/AudioPlayer.h"
#include "views/FileViewer.h"
#include "views/HunkViewer.h"
#include "views/ImageViewer.h"

namespace adm::ui {

void ViewerLauncher::Init(FontManager* fontManager, TextureManager* textureManager,
                          core::AudioEngine* audioEngine, core::UserPreferences* userPrefs,
                          ViewerLauncherOps ops)
{
    m_fontManager = fontManager;
    m_textureManager = textureManager;
    m_audioEngine = audioEngine;
    m_userPrefs = userPrefs;
    m_ops = std::move(ops);
}

void ViewerLauncher::OpenFileViewer(core::DiskDocument* doc, uint32_t sector,
                                    const std::string& filename)
{
    auto view = std::make_unique<ui::FileViewer>();

    // Populate fonts
    for (const auto& fontInfo : m_fontManager->GetAvailableFonts()) {
        view->AddFont(fontInfo.name, fontInfo.ptr);
    }
    view->SetCurrentFont(m_userPrefs->Get().fontName);

    view->SetTitle("File Viewer - " + filename);
    if (doc->kind == core::DocumentKind::HostDirectory && doc->hostDirectory) {
        auto entry = doc->hostEntryIds.find(sector);
        std::vector<uint8_t> data;
        std::string error;
        if (entry == doc->hostEntryIds.end() ||
            !doc->hostDirectory->ReadFile(entry->second, data, error)) {
            core::Logger::Error("Failed to read host file '{}': {}", filename, error);
            return;
        }
        view->SetContent(data);
        view->SetFileMetadata(sector, 0, filename);
        if (!doc->hostDirectory->IsReadOnly()) {
            view->SetOnSaveCallback([this, doc](uint32_t entryNumber, uint32_t, const std::string&,
                                                const std::vector<uint8_t>& bytes) -> uint32_t {
                auto entry = doc->hostEntryIds.find(entryNumber);
                std::string error;
                if (entry == doc->hostEntryIds.end() ||
                    !doc->hostDirectory->WriteFile(entry->second, bytes, error)) {
                    core::Logger::Error("Host directory: {}", error);
                    return 0;
                }
                doc->hostDirectory->RefreshNow(error);
                if (m_ops.updateHostDirectoryDocument)
                    m_ops.updateHostDirectoryDocument(doc);
                return entryNumber;
            });
        }
        view->Open();
        doc->fileViewers.push_back(std::move(view));
        return;
    }
    view->SetContent(doc->volume.ReadFileContent(sector));

    // Analyze block to get parent sector
    auto details = doc->volume.AnalyzeBlock(sector, doc->volume.ReadBlock(sector));
    view->SetFileMetadata(sector, details.parent, filename);

    view->SetOnSaveCallback([this, doc](uint32_t header, uint32_t parent, const std::string& name,
                                        const std::vector<uint8_t>& data) -> uint32_t {
        // Space check
        auto fileDetails = doc->volume.AnalyzeBlock(header, doc->volume.ReadBlock(header));
        int64_t oldSize = fileDetails.byteSize;
        int64_t newSize = data.size();

        uint32_t blocksOld = doc->volume.CalculateRequiredBlocks(oldSize);
        uint32_t blocksNew = doc->volume.CalculateRequiredBlocks(newSize);
        int64_t blocksNeeded = (int64_t)blocksNew - (int64_t)blocksOld;

        uint32_t freeBlocks = doc->volume.GetFreeBlocks();

        if (blocksNeeded > 0 && freeBlocks < (uint32_t)blocksNeeded) {
            m_ops.reportError(core::AdfHandler::InsufficientSpaceMessage(
                "Save", (uint32_t)blocksNeeded, freeBlocks));
            return 0;
        }

        if (!doc->volume.DeleteFile(parent, name)) {
            m_ops.reportError("Failed to delete old file before saving.");
            return 0;
        }

        if (!doc->volume.ImportFile(parent, name, data)) {
            m_ops.reportError("Failed to save new file content.\n(Partial file removed)");
            return 0;
        }

        uint32_t newHeader = doc->volume.FindFile(parent, name);
        if (newHeader == 0) {
            m_ops.reportError("Saved file could not be located on disk.");
            return 0;
        }

        if (m_ops.updateDocumentViews)
            m_ops.updateDocumentViews(doc, true);
        return newHeader;
    });

    view->Open();
    doc->fileViewers.push_back(std::move(view));
}

void ViewerLauncher::OpenImageViewer(core::DiskDocument* doc, uint32_t sector,
                                     const std::string& filename)
{
    std::string content;
    if (doc->kind == core::DocumentKind::HostDirectory && doc->hostDirectory) {
        auto entry = doc->hostEntryIds.find(sector);
        std::vector<uint8_t> bytes;
        std::string error;
        if (entry != doc->hostEntryIds.end() &&
            doc->hostDirectory->ReadFile(entry->second, bytes, error))
            content.assign(bytes.begin(), bytes.end());
    } else {
        content = doc->volume.ReadFileContent(sector);
    }
    if (content.empty()) {
        core::Logger::Error("Failed to read file content for image viewer: {}", filename);
        m_ops.reportError("Failed to read file: " + filename);
        return;
    }

    auto image = m_imageLoader.Decode(content, filename);
    if (!image.IsValid()) {
        core::Logger::Error("Failed to decode image: {} - {}", filename, image.errorMessage);
        m_ops.reportError("Failed to decode image:\n" + filename + "\n\n" + image.errorMessage);
        return;
    }

    auto viewer = std::make_unique<ui::ImageViewer>(m_textureManager);
    viewer->SetTitle("Image - " + filename);
    viewer->SetImage(image);
    viewer->Open();
    doc->imageViewers.push_back(std::move(viewer));
}

void ViewerLauncher::OpenAudioPlayer(core::DiskDocument* doc, uint32_t sector,
                                     const std::string& filename)
{
    std::string content;
    if (doc->kind == core::DocumentKind::HostDirectory && doc->hostDirectory) {
        auto entry = doc->hostEntryIds.find(sector);
        std::vector<uint8_t> bytes;
        std::string error;
        if (entry != doc->hostEntryIds.end() &&
            doc->hostDirectory->ReadFile(entry->second, bytes, error))
            content.assign(bytes.begin(), bytes.end());
    } else {
        content = doc->volume.ReadFileContent(sector);
    }
    if (content.empty()) {
        core::Logger::Error("Failed to read file content for audio player: {}", filename);
        m_ops.reportError("Failed to read file: " + filename);
        return;
    }

    std::vector<uint8_t> data(content.begin(), content.end());

    auto module = std::make_unique<core::AudioModule>();
    module->SetSampleRate(m_audioEngine->GetSampleRate());

    if (!module->Load(data, filename)) {
        core::Logger::Error("Failed to load audio module: {}", filename);
        m_ops.reportError("Failed to load audio file:\n" + filename +
                          "\n\nThe file format may not be supported.");
        return;
    }

    auto player = std::make_unique<ui::AudioPlayer>(m_audioEngine);
    player->SetTitle("Audio Player - " + filename);
    player->SetModule(std::move(module));
    player->Open();
    doc->audioPlayers.push_back(std::move(player));
}

void ViewerLauncher::OpenHunkViewer(core::DiskDocument* doc, uint32_t sector,
                                    const std::string& filename)
{
    std::string content;
    if (doc->kind == core::DocumentKind::HostDirectory && doc->hostDirectory) {
        auto entry = doc->hostEntryIds.find(sector);
        std::vector<uint8_t> bytes;
        std::string error;
        if (entry != doc->hostEntryIds.end() &&
            doc->hostDirectory->ReadFile(entry->second, bytes, error))
            content.assign(bytes.begin(), bytes.end());
    } else {
        content = doc->volume.ReadFileContent(sector);
    }
    if (content.empty()) {
        core::Logger::Error("Failed to read file content for hunk viewer: {}", filename);
        m_ops.reportError("Failed to read file: " + filename);
        return;
    }

    std::vector<uint8_t> data(content.begin(), content.end());

    if (!core::HunkParser::IsAmigaExecutable(data)) {
        core::Logger::Error("Not a valid Amiga executable: {}", filename);
        m_ops.reportError("Not a valid Amiga executable:\n" + filename);
        return;
    }

    auto result = core::HunkParser::Parse(data);

    auto viewer = std::make_unique<ui::HunkViewer>();
    viewer->SetTitle("Hunk Viewer - " + filename);
    viewer->SetData(result, filename);
    viewer->Open();
    doc->hunkViewers.push_back(std::move(viewer));
}

} // namespace adm::ui
