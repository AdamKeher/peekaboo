#pragma once

namespace adm::core {
class McpToolHandler;
}

namespace adm::ui {

// Shows the rolling log of MCP tool calls Claude has made (tool, args, result, duration). Read-only
// window over McpToolHandler's activity ring buffer — both run on the main thread, so no locking.
class McpActivityView
{
public:
    explicit McpActivityView(core::McpToolHandler* handler) : m_handler(handler) {}

    void Draw();

    bool* GetOpenStatePtr() { return &m_isOpen; }
    void Restore(bool open)
    {
        m_isOpen = open;
        m_noFocusOnAppearing = true;
    }

private:
    core::McpToolHandler* m_handler = nullptr;
    bool m_isOpen = false;
    bool m_noFocusOnAppearing = false;
    bool m_autoScroll = true;
};

} // namespace adm::ui
