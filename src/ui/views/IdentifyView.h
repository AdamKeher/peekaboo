#pragma once

#include <functional>
#include <memory>
#include <string>

#include "../../core/DiskIdentifier.h"
#include "../TextureManager.h"

namespace adm::ui {

class IdentifyResultPanel;

class IdentifyView
{
public:
    IdentifyView(TextureManager* textureMgr);
    ~IdentifyView();

    // Open the identify window (identification is managed per-document).
    void Open();
    // Point the view at a new document's identifier. Non-blocking.
    void SetDocument(const std::string& filePath, core::DiskIdentifier* identifier);
    void Clear();
    void Close();
    void Draw();

    // Called when the user clicks "Find in TOSEC Browser"; receives the MD5 of the disk.
    void SetOnFindInTosec(std::function<void(const std::string&)> cb)
    {
        m_onFindInTosec = std::move(cb);
    }

    // Called when the user clicks Identify. Owns creating the active document's identifier if
    // it has none, so this is also what reaches the window before any identification has run.
    void SetOnIdentify(std::function<void()> cb) { m_onIdentify = std::move(cb); }

    // Tell the window whether a mounted disk is available to identify. Drives the Identify
    // button's enabled state; pushed every frame by the caller.
    void SetDocumentLoaded(bool loaded) { m_documentLoaded = loaded; }

    bool IsOpen() const { return m_isOpen; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

private:
    void DrawTosecIdentification(const core::IdentifyResult& result);
    void DrawIdentifyButton(bool busy);
    void DrawSpinnerState(const std::string& status);
    void DrawErrorBanner(const core::IdentifyResult& result);
    void DrawFilenameAnalysis();
    void DrawContent(const core::IdentifyResult& result, core::IdentifyState state,
                     const std::string& status);

    // The ScreenScraper/Pouet sections (data + images) are rendered by the shared
    // IdentifyResultPanel. Push the result to it only when it meaningfully changes
    // (tracked via a cheap signature) so it doesn't re-decode textures every frame.
    void UpdatePanel(const core::IdentifyResult& result);
    static std::string ResultSignature(const core::IdentifyResult& result);

    bool m_isOpen = false;
    bool m_documentLoaded = false;
    std::string m_currentFilePath;
    core::DiskIdentifier* m_identifier = nullptr; // Non-owning; owned by DiskDocument
    TextureManager* m_textureManager = nullptr;

    std::unique_ptr<IdentifyResultPanel> m_panel;
    std::string m_panelSig;

    std::function<void(const std::string&)> m_onFindInTosec;
    std::function<void()> m_onIdentify;
};

} // namespace adm::ui
