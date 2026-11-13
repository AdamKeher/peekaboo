#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace adm::validation {

enum class FailureLayer { GuestToRaw, RawToDisplayed, Render, Infrastructure };

struct ComparisonFailure
{
    FailureLayer layer = FailureLayer::Infrastructure;
    std::string path;
    std::string message;
    nlohmann::json expected;
    nlohmann::json actual;
};

struct ComparisonResult
{
    std::vector<ComparisonFailure> failures;
    [[nodiscard]] bool Passed() const { return failures.empty(); }
};

// Rules are JSON objects whose leaves contain a comparison mode. Supported modes are exact,
// masked, range, sequence, set and image. A plain JSON leaf is shorthand for exact.
class SemanticComparator
{
public:
    ComparisonResult Compare(const nlohmann::json& rules, const nlohmann::json& actual,
                             FailureLayer layer) const;

    // Compare both seams and attribute a failure to the first layer that diverges.
    ComparisonResult CompareLayers(const nlohmann::json& rules, const nlohmann::json& raw,
                                   const nlohmann::json& displayed) const;

    static std::string StableHash(const nlohmann::json& value);
    static const char* LayerName(FailureLayer layer);

private:
    void CompareNode(const nlohmann::json& rule, const nlohmann::json* actual,
                     const std::string& path, FailureLayer layer, ComparisonResult& result) const;
};

} // namespace adm::validation
