#include "DefinitionManager.h"
#include <fstream>
#include <vector>

namespace adm::core {

nlohmann::json DefinitionManager::GetDefinition(const std::string& path)
{
    if (path.empty()) {
        return nlohmann::json();
    }

    if (m_cache.find(path) == m_cache.end()) {
        m_cache[path] = Load(path);
    }
    return m_cache[path];
}

nlohmann::json DefinitionManager::Load(const std::string& path)
{
    std::vector<std::string> searchPaths = {path, "../../" + path};
    for (const auto& searchPath : searchPaths) {
        std::ifstream f(searchPath);
        if (f.is_open()) {
            try {
                return nlohmann::json::parse(f);
            } catch (...) {
                // Ignore parse errors
            }
        }
    }
    return nlohmann::json();
}

} // namespace adm::core
