#pragma once

#include <cstdint>

#include "views/AddressActions.h"

namespace adm::ui {

// Shared clickable memory-address hyperlink. Renders `addr` (formatted with `fmt`, a printf string
// containing one integer conversion such as "$%06X") in the app-wide hyperlink colour with a hover
// underline + hand cursor. Left-click runs the nominated default action (`def`); right-click opens
// the shared address-actions menu. `idSuffix` keeps the right-click popup id unique when several
// links share a frame (e.g. a column name).
void DrawAddressLink(const char* idSuffix, uint32_t addr, const char* fmt,
                     const AddressActionContext& ctx,
                     AddressLinkHost host = AddressLinkHost::Window,
                     AddressDefault def = AddressDefault::Memory);

// Runs the left-click default action for `addr`. Exposed so views with a bespoke link affordance
// (e.g. the CPU register links) can share the dispatch.
void RunAddressDefaultAction(uint32_t addr, const AddressActionContext& ctx, AddressLinkHost host,
                             AddressDefault def);

// Draws the shared right-click actions menu body. Call between ImGui::BeginPopup/EndPopup. Exposed
// so views with a bespoke link affordance can present the identical menu.
void DrawAddressMenuBody(uint32_t addr, const AddressActionContext& ctx, AddressLinkHost host,
                         AddressDefault def);

// Draws the global label/comment editor modal opened from the menu. Call once per frame, after all
// views have drawn (DebuggerView::Draw does this).
void DrawAddressLinkModals();

} // namespace adm::ui
