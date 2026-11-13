#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../core/ImageLoader.h"

namespace adm::core {
class AudioEngine;
class UserPreferences;
struct DiskDocument;
} // namespace adm::core

namespace adm::ui {

class FontManager;
class TextureManager;

/**
 * @brief What the launcher cannot decide for itself.
 *
 * Reporting a failure means raising the error modal, and saving through a viewer means telling
 * the rest of the application the document changed. Both belong to Application.
 */
struct ViewerLauncherOps
{
    std::function<void(const std::string& message)> reportError;
    std::function<void(core::DiskDocument* doc, bool preserveNavigation)> updateDocumentViews;
    std::function<void(core::DiskDocument* doc)> updateHostDirectoryDocument;
};

/**
 * @brief Opens the per-file viewer windows a document can spawn.
 *
 * Four near-identical handlers - text, image, audio module, Amiga executable - each of which
 * reads the file (from the volume, or from the host directory when the document is one),
 * constructs the right window and parks it on the document that owns it.
 *
 * The reading half is the same in all four and the differences are entirely in what is done
 * with the bytes, which is why they belong together rather than next to their respective views.
 *
 * Extracted from Application.
 */
class ViewerLauncher
{
public:
    /// All four are owned by Application and outlive this object.
    void Init(FontManager* fontManager, TextureManager* textureManager,
              core::AudioEngine* audioEngine, core::UserPreferences* userPrefs,
              ViewerLauncherOps ops);

    void OpenFileViewer(core::DiskDocument* doc, uint32_t sector, const std::string& filename);
    void OpenImageViewer(core::DiskDocument* doc, uint32_t sector, const std::string& filename);
    void OpenAudioPlayer(core::DiskDocument* doc, uint32_t sector, const std::string& filename);
    void OpenHunkViewer(core::DiskDocument* doc, uint32_t sector, const std::string& filename);

private:
    // The three non-text handlers open with the same read-from-volume-or-host-directory block.
    // Left duplicated: this extraction is code motion, and de-duplicating them is a separate
    // change with its own risk.

    FontManager* m_fontManager = nullptr;
    TextureManager* m_textureManager = nullptr;
    core::AudioEngine* m_audioEngine = nullptr;
    core::UserPreferences* m_userPrefs = nullptr;
    ViewerLauncherOps m_ops;

    core::ImageLoader m_imageLoader;
};

} // namespace adm::ui
