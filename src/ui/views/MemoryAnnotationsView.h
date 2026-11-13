#pragma once

#include <cstdint>
#include <functional>

#include "AddressActions.h"

namespace adm::core {
class MemoryAnnotations;
}

namespace adm::ui {

// GUI front-end for the shared MemoryAnnotations store (the same annotations
// driven by the TM/TL/TMS/TMD console commands and shown in the disassembly
// views). Lets the user add, edit and delete labels and comments in a table.
class MemoryAnnotationsView
{
public:
    explicit MemoryAnnotationsView(core::MemoryAnnotations* annotations);

    void Draw();
    void Restore(bool open);
    bool* GetOpenPtr() { return &m_open; }

    void SetAddressContext(const AddressActionContext* ctx) { m_addrCtx = ctx; }

private:
    core::MemoryAnnotations* m_annotations = nullptr;
    const AddressActionContext* m_addrCtx = nullptr;

    bool m_open = false;
    bool m_noFocus = false;

    // Which field the add-row text targets (0 = comment, 1 = label, 2 = block).
    int m_addType = 0;

    // Add-row input state
    char m_addAddr[16] = {};
    char m_addText[256] = {};

    // Inline edit state. m_editAddr identifies the row; m_editField selects which
    // cell is being edited (0 = comment, 1 = label, 2 = block). m_editing is false
    // when idle. The buffer is sized for multi-line comment blocks.
    bool m_editing = false;
    uint32_t m_editAddr = 0;
    int m_editField = 0;
    char m_editText[2048] = {};
};

} // namespace adm::ui
