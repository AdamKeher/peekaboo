#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace adm::ui {

class PanelView
{
public:
    PanelView(std::string name, uint32_t id);
    void Draw();

    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& n) { m_name = n; }
    uint32_t GetId() const { return m_id; }
    bool* GetOpenStatePtr() { return &m_isOpen; }

    void SetOnRenameRequested(std::function<void(uint32_t)> cb) { m_onRename = std::move(cb); }
    void SetOnDeleteRequested(std::function<void(uint32_t)> cb) { m_onDelete = std::move(cb); }

    void SetPermanent(bool permanent) { m_permanent = permanent; }

    // Pin a fixed icon glyph (e.g. ICON_FA_FOLDER_OPEN) shown before the panel name. When set, it
    // overrides the default behaviour of mirroring the first docked child window's icon.
    void SetIcon(std::string icon) { m_icon = std::move(icon); }

    // Suppress focus-stealing the next time this panel first appears. Used when restoring
    // panels at startup/workspace-load so a freshly-recreated panel doesn't grab focus.
    void SuppressFocusOnAppear() { m_noFocusOnAppearing = true; }

private:
    std::string m_name;
    std::string m_icon;
    uint32_t m_id;
    bool m_isOpen = true;
    bool m_permanent = false;
    bool m_noFocusOnAppearing = false;
    std::function<void(uint32_t)> m_onRename;
    std::function<void(uint32_t)> m_onDelete;
};

} // namespace adm::ui
