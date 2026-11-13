#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm::core {

struct McpHost;

// The reverse-engineering half of the MCP surface: memory snapshots and diffs,
// address explanation, AmigaOS library-call tracing and DOS breakpoints, loaded
// segment/hunk mapping, ADF filesystem access, reset survivors, and the rolling
// CPU trace with its search modes.
//
// These live apart from McpToolHandler for the same reason McpConsoleTools does:
// one file per coherent group of tools, rather than one file that keeps growing.
// The schema list and the dispatcher are kept next to each other so a new tool
// cannot be declared without a handler or vice versa.

// Appends this group's tool schemas to the manifest array.
void AppendMcpReToolManifest(nlohmann::json& tools);

// Handles a tool by name. Returns nullopt when the name is not one of ours, so
// the caller can keep looking.
std::optional<nlohmann::json> DispatchMcpReTool(const std::string& name, const nlohmann::json& args,
                                                const McpHost& host);

// Every tool name this module owns, in manifest order. Used by the tests and to
// keep the two lists above honest.
const std::vector<std::string>& McpReToolNames();

} // namespace adm::core
