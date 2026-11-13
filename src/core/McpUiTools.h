#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm::core {

struct McpHost;

// The host-UI half of the MCP surface: Peekaboo's own windows, dock layout, panels, workspaces,
// application screenshots, its log, and synthetic input into its ImGui.
//
// Every other tool group looks INTO the guest machine. This one looks at Peekaboo itself, which is
// what makes it usable when the thing being debugged is Peekaboo's UI rather than the Amiga's.
// Kept apart from McpToolHandler for the same reason McpReTools and McpConsoleTools are: one file
// per coherent group, with the schema list and the dispatcher next to each other so a tool cannot
// be declared without a handler or vice versa.

// Appends this group's tool schemas to the manifest array.
void AppendMcpUiToolManifest(nlohmann::json& tools);

// Handles a tool by name. Returns nullopt when the name is not one of ours, so the caller can keep
// looking.
std::optional<nlohmann::json> DispatchMcpUiTool(const std::string& name, const nlohmann::json& args,
                                                const McpHost& host);

// Every tool name this module owns, in manifest order. Used by the tests and to keep the two lists
// above honest.
const std::vector<std::string>& McpUiToolNames();

} // namespace adm::core
