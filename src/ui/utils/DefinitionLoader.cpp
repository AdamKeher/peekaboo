#include "DefinitionLoader.h"
#include <fstream>
#include <vector>

namespace adm::ui {

nlohmann::json DefinitionLoader::Load(const std::string& path)
{
    std::vector<std::string> searchPaths = {path, "../../" + path};
    for (const auto& searchPath : searchPaths) {
        std::ifstream f(searchPath);
        if (f.is_open()) {
            try {
                return nlohmann::json::parse(f);
            } catch (...) {
                // Ignore parse errors, try next path or return empty
            }
        }
    }
    return nlohmann::json();
}

} // namespace adm::ui
