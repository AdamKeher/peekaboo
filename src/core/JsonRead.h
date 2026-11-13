#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

namespace adm::core::jsonread {

// Typed reads of JSON that came from outside the program (.exocet, kit.json, .peek, .pkscn,
// prefs, custom configurations). nlohmann's value()/get<T>() throw on a type mismatch or a
// non-object, and the paths that read these files have no catch. These never throw: a key that
// is missing, the wrong type or out of the target type's range leaves `out` alone (ReadIf, false)
// or gives `def` (ReadOr). Templated on the JSON type so json and ordered_json both work.

// One value, not a key: the same rules, for array elements.
template <class T, class J> bool Get(const J& v, T& out)
{
    if constexpr (std::is_same_v<T, bool>) {
        if (!v.is_boolean())
            return false;
        out = v.template get<bool>();
        return true;
    } else if constexpr (std::is_enum_v<T>) {
        std::underlying_type_t<T> u{};
        if (!Get(v, u))
            return false;
        out = static_cast<T>(u);
        return true;
    } else if constexpr (std::is_integral_v<T>) {
        if (v.is_number_unsigned()) {
            const auto u = v.template get<uint64_t>();
            if (u > static_cast<uint64_t>(std::numeric_limits<T>::max()))
                return false;
            out = static_cast<T>(u);
            return true;
        }
        if (!v.is_number_integer())
            return false;
        const auto s = v.template get<int64_t>();
        if constexpr (std::is_signed_v<T>) {
            if (s < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
                s > static_cast<int64_t>(std::numeric_limits<T>::max()))
                return false;
        } else {
            if (s < 0 || static_cast<uint64_t>(s) > std::numeric_limits<T>::max())
                return false;
        }
        out = static_cast<T>(s);
        return true;
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!v.is_number())
            return false;
        out = v.template get<T>();
        return true;
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!v.is_string())
            return false;
        out = v.template get<std::string>();
        return true;
    } else {
        static_assert(sizeof(T) == 0, "jsonread::Get: unsupported type");
    }
}

template <class T, class J> bool ReadIf(const J& j, std::string_view key, T& out)
{
    if (!j.is_object())
        return false;
    const auto it = j.find(std::string(key));
    return it != j.end() && Get(*it, out);
}

template <class T, class J> T ReadOr(const J& j, std::string_view key, T def)
{
    ReadIf(j, key, def);
    return def;
}

// ReadOr with a string literal default reads a std::string
template <class J> std::string ReadOr(const J& j, std::string_view key, const char* def)
{
    return ReadOr<std::string>(j, key, std::string(def));
}

} // namespace adm::core::jsonread
