#include "SemanticComparator.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace adm::validation {
namespace {

bool IsRule(const nlohmann::json& value) { return value.is_object() && value.contains("mode"); }

void AddFailure(ComparisonResult& result, FailureLayer layer, const std::string& path,
                const std::string& message, const nlohmann::json& expected,
                const nlohmann::json* actual)
{
    result.failures.push_back(
        {layer, path, message, expected, actual ? *actual : nlohmann::json(nullptr)});
}

std::string Canonical(const nlohmann::json& value)
{
    if (value.is_object()) {
        std::vector<std::string> keys;
        for (const auto& [key, unused] : value.items())
            keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        std::string out = "{";
        for (const auto& key : keys)
            out += nlohmann::json(key).dump() + ":" + Canonical(value.at(key)) + ",";
        return out + "}";
    }
    if (value.is_array()) {
        std::string out = "[";
        for (const auto& item : value)
            out += Canonical(item) + ",";
        return out + "]";
    }
    return value.dump();
}

} // namespace

ComparisonResult SemanticComparator::Compare(const nlohmann::json& rules,
                                             const nlohmann::json& actual, FailureLayer layer) const
{
    ComparisonResult result;
    CompareNode(rules, &actual, "$", layer, result);
    return result;
}

ComparisonResult SemanticComparator::CompareLayers(const nlohmann::json& rules,
                                                   const nlohmann::json& raw,
                                                   const nlohmann::json& displayed) const
{
    auto result = Compare(rules, raw, FailureLayer::GuestToRaw);
    auto display =
        Compare(rules, displayed,
                result.Passed() ? FailureLayer::RawToDisplayed : FailureLayer::GuestToRaw);
    result.failures.insert(result.failures.end(), display.failures.begin(), display.failures.end());
    return result;
}

void SemanticComparator::CompareNode(const nlohmann::json& rule, const nlohmann::json* actual,
                                     const std::string& path, FailureLayer layer,
                                     ComparisonResult& result) const
{
    if (!actual) {
        AddFailure(result, layer, path, "field is missing", rule, nullptr);
        return;
    }

    if (!IsRule(rule) && rule.is_object()) {
        if (!actual->is_object()) {
            AddFailure(result, layer, path, "expected an object", rule, actual);
            return;
        }
        for (const auto& [key, child] : rule.items()) {
            const nlohmann::json* value = actual->contains(key) ? &actual->at(key) : nullptr;
            CompareNode(child, value, path + "." + key, layer, result);
        }
        return;
    }

    const std::string mode = IsRule(rule) ? rule.value("mode", "exact") : "exact";
    const auto expected = IsRule(rule) ? rule.value("value", nlohmann::json()) : rule;

    if (mode == "exact") {
        if (*actual != expected)
            AddFailure(result, layer, path, "exact value differs", expected, actual);
    } else if (mode == "masked") {
        if (!actual->is_number_unsigned() && !actual->is_number_integer()) {
            AddFailure(result, layer, path, "masked comparison requires an integer", expected,
                       actual);
            return;
        }
        const uint64_t mask = rule.value("mask", UINT64_MAX);
        if ((actual->get<uint64_t>() & mask) != (expected.get<uint64_t>() & mask))
            AddFailure(result, layer, path, "masked bits differ", expected, actual);
    } else if (mode == "range") {
        if (!actual->is_number()) {
            AddFailure(result, layer, path, "range comparison requires a number", rule, actual);
            return;
        }
        const double value = actual->get<double>();
        const double min = rule.at("min").get<double>();
        const double max = rule.at("max").get<double>();
        if (value < min || value > max)
            AddFailure(result, layer, path, "value is outside the inclusive range", rule, actual);
    } else if (mode == "sequence") {
        if (!actual->is_array() || !expected.is_array()) {
            AddFailure(result, layer, path, "sequence comparison requires arrays", expected,
                       actual);
            return;
        }
        size_t cursor = 0;
        for (const auto& wanted : expected) {
            while (cursor < actual->size() && actual->at(cursor) != wanted)
                ++cursor;
            if (cursor == actual->size()) {
                AddFailure(result, layer, path, "ordered item is missing", wanted, actual);
                break;
            }
            ++cursor;
        }
    } else if (mode == "set") {
        if (!actual->is_array() || !expected.is_array()) {
            AddFailure(result, layer, path, "set comparison requires arrays", expected, actual);
            return;
        }
        std::vector<std::string> left, right;
        for (const auto& value : expected)
            left.push_back(Canonical(value));
        for (const auto& value : *actual)
            right.push_back(Canonical(value));
        std::sort(left.begin(), left.end());
        std::sort(right.begin(), right.end());
        if (left != right)
            AddFailure(result, layer, path, "set members differ", expected, actual);
    } else if (mode == "image") {
        // Image builders provide deterministic pixel hashes. Crop/palette/tolerance metadata stays
        // in the rule and is included in failure artifacts, while the semantic seam compares hash.
        const auto expectedHash = rule.value("hash", std::string());
        const auto actualHash =
            actual->is_string() ? actual->get<std::string>() : actual->value("hash", std::string());
        if (expectedHash != actualHash)
            AddFailure(result, FailureLayer::Render, path, "image hash differs", rule, actual);
    } else {
        AddFailure(result, FailureLayer::Infrastructure, path,
                   "unknown comparison mode '" + mode + "'", rule, actual);
    }
}

std::string SemanticComparator::StableHash(const nlohmann::json& value)
{
    // FNV-1a is intentionally simple and stable across hosts; this detects drift, it is not a
    // security primitive.
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : Canonical(value)) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

const char* SemanticComparator::LayerName(FailureLayer layer)
{
    switch (layer) {
        case FailureLayer::GuestToRaw:
            return "guest-to-raw";
        case FailureLayer::RawToDisplayed:
            return "raw-to-displayed";
        case FailureLayer::Render:
            return "render";
        case FailureLayer::Infrastructure:
            return "infrastructure";
    }
    return "infrastructure";
}

} // namespace adm::validation
