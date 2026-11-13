#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace adm::ui {

/**
 * @brief Helper for loading JSON structure definitions.
 */
class DefinitionLoader
{
public:
    /**
     * @brief Loads a JSON definition from the specified path.
     * Searches in current directory and parent directories.
     * @param path Relative path to the JSON file.
     * @return The parsed JSON object, or an empty object if not found/invalid.
     */
    static nlohmann::json Load(const std::string& path);
};

} // namespace adm::ui
