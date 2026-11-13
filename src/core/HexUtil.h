#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace adm::core {

// Standard base64, used to carry binary payloads (PNG captures) inside MCP's JSON results. Shared
// by the emulator `screenshot` tool and the host-side `screenshot_app` / `screenshot_window` tools.
inline std::string Base64Encode(const std::vector<uint8_t>& data)
{
    static const char* kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back(kBase64[(n >> 6) & 63]);
        out.push_back(kBase64[n & 63]);
    }
    if (i < data.size()) {
        uint32_t n = data[i] << 16;
        bool two = (i + 1 < data.size());
        if (two)
            n |= data[i + 1] << 8;
        out.push_back(kBase64[(n >> 18) & 63]);
        out.push_back(kBase64[(n >> 12) & 63]);
        out.push_back(two ? kBase64[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// The inverse, for payloads coming IN over MCP (load_document_bytes). Whitespace is skipped, so a
// wrapped encoding decodes; '=' padding ends the data. Anything else outside the alphabet, or a
// length that cannot be base64, is nullopt rather than a guess.
inline std::optional<std::vector<uint8_t>> Base64Decode(std::string_view s)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(s.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0, count = 0;
    bool padded = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)))
            continue;
        if (c == '=') {
            padded = true;
            continue;
        }
        const int v = value(c);
        if (v < 0 || padded)
            return std::nullopt;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        count++;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    if (count % 4 == 1)
        return std::nullopt;
    return out;
}

// Parses a hex address token, tolerating a leading "$" or "0x"/"0X" prefix. The
// remaining characters must all be hex digits (and there must be at least one),
// so trailing garbage is rejected rather than silently truncated. Returns nullopt
// on any malformed input. This is the single parser behind the debugger console,
// the Breakpoints window, the MCP tools and Application's address handling.
inline std::optional<uint32_t> ParseHexAddress(std::string_view s)
{
    // Trim surrounding whitespace.
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    if (s.empty())
        return std::nullopt;

    if (s.front() == '$')
        s.remove_prefix(1);
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s.remove_prefix(2);
    if (s.empty())
        return std::nullopt;

    uint32_t value = 0;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return std::nullopt;
        int digit = (c <= '9') ? (c - '0') : (c <= 'F') ? (c - 'A' + 10) : (c - 'a' + 10);
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return value;
}

// Canonical 8-digit uppercase hex form of an address ("0000157C"), used as the
// key/display string throughout the debugger views.
inline std::string FormatAddr8(uint32_t addr)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", addr);
    return buf;
}

} // namespace adm::core
