#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>

namespace adm::core {

class DefinitionManager
{
public:
    nlohmann::json GetDefinition(const std::string& path);

private:
    std::map<std::string, nlohmann::json> m_cache;
    nlohmann::json Load(const std::string& path);
};

} // namespace adm::core
