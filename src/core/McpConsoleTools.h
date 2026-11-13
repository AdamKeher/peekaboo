#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm::core {

// A dedicated MCP surface for an existing AR-console command family. The command remains owned by
// ArCommandInterpreter; these entries only describe how MCP names and invokes it. Keeping this
// registry declarative prevents the MCP layer from acquiring a second copy of every parser.
struct McpConsoleToolSpec
{
    const char* name;
    std::vector<const char*> commands;
    const char* description;
};

const std::vector<McpConsoleToolSpec>& McpConsoleToolSpecs();

// Adds every console-backed tool's MCP declaration to an existing manifest array.
void AppendMcpConsoleToolManifest(nlohmann::json& tools);

// Dispatches a console-backed tool. nullopt means that name is not in the registry.
std::optional<nlohmann::json>
DispatchMcpConsoleTool(const std::string& name, const nlohmann::json& args,
                       const std::function<std::string(const std::string&)>& execConsole);

} // namespace adm::core
