#include "LibraryCallAnnotator.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace adm::core {

namespace {

// Strips a trailing ".library" / ".device" / ".resource" so "exec.library"
// renders as the conventional "exec/Func()" shorthand.
std::string ShortLibName(const std::string& full)
{
    static const char* kSuffixes[] = {".library", ".device", ".resource"};
    for (const char* s : kSuffixes) {
        size_t n = std::char_traits<char>::length(s);
        if (full.size() > n && full.compare(full.size() - n, n, s) == 0)
            return full.substr(0, full.size() - n);
    }
    return full;
}

} // namespace

bool LibraryCallAnnotator::LoadTables(const std::string& path)
{
    m_tables.clear();
    try {
        std::ifstream f(path);
        if (!f.is_open())
            return false;
        auto json = nlohmann::json::parse(f);
        if (!json.is_object())
            return false;
        for (auto& [libName, table] : json.items()) {
            if (libName.empty() || libName[0] == '_' || !table.is_object())
                continue; // skip "_comment" and any non-table entries
            auto& dst = m_tables[libName];
            for (auto& [offStr, fn] : table.items()) {
                if (!fn.is_string())
                    continue;
                long off = std::strtol(offStr.c_str(), nullptr, 10);
                uint32_t lvo = static_cast<uint32_t>(off < 0 ? -off : off);
                dst[lvo] = fn.get<std::string>();
            }
        }
    } catch (const std::exception&) {
        m_tables.clear();
        return false;
    }
    return !m_tables.empty();
}

bool LibraryCallAnnotator::ParseLibCall(const std::string& mnemonic, int& aReg, uint32_t& lvo)
{
    // Opcode must be JSR or JMP (case-insensitive), optionally size-suffixed.
    size_t b = mnemonic.find_first_not_of(" \t");
    if (b == std::string::npos)
        return false;
    auto eq = [&](const char* op) {
        for (int i = 0; i < 3; ++i)
            if (std::toupper(static_cast<unsigned char>(mnemonic[b + i])) != op[i])
                return false;
        return true;
    };
    if (b + 3 > mnemonic.size() || (!eq("JSR") && !eq("JMP")))
        return false;

    // Locate "(aN" — the base register of a d16(An) effective address. UAE renders
    // this as "(a6,-$01c8)"; also accept the "-$01c8(a6)" spelling.
    for (size_t i = 0; i + 2 < mnemonic.size(); ++i) {
        if (mnemonic[i] != '(')
            continue;
        char r = static_cast<char>(std::tolower(static_cast<unsigned char>(mnemonic[i + 1])));
        if (r != 'a')
            continue;
        char d = mnemonic[i + 2];
        if (d < '0' || d > '7')
            continue;
        int reg = d - '0';

        // Form 1: "(aN,<disp>)" — displacement after the comma.
        size_t j = i + 3;
        if (j < mnemonic.size() && mnemonic[j] == ',') {
            ++j;
            while (j < mnemonic.size() && mnemonic[j] == ' ')
                ++j;
            if (j < mnemonic.size() && mnemonic[j] == '-') {
                ++j;
                int base = 10;
                if (j < mnemonic.size() && mnemonic[j] == '$') {
                    base = 16;
                    ++j;
                }
                char* end = nullptr;
                long val = std::strtol(mnemonic.c_str() + j, &end, base);
                if (end != mnemonic.c_str() + j && val > 0) {
                    aReg = reg;
                    lvo = static_cast<uint32_t>(val);
                    return true;
                }
            }
            continue;
        }

        // Form 2: "-<disp>(aN)" — displacement immediately precedes '('.
        size_t k = i; // points at '('
        // walk back over the displacement token
        size_t e = k; // one past the last disp char
        size_t s = k;
        while (s > 0 && (std::isxdigit(static_cast<unsigned char>(mnemonic[s - 1])) ||
                         mnemonic[s - 1] == '$' || mnemonic[s - 1] == '-'))
            --s;
        if (s < e && mnemonic[s] == '-') {
            size_t p = s + 1;
            int nbase = 10;
            if (p < e && mnemonic[p] == '$') {
                nbase = 16;
                ++p;
            }
            char* end = nullptr;
            long val = std::strtol(mnemonic.c_str() + p, &end, nbase);
            if (end == mnemonic.c_str() + e && val > 0) {
                aReg = reg;
                lvo = static_cast<uint32_t>(val);
                return true;
            }
        }
    }
    return false;
}

std::string LibraryCallAnnotator::ReadLibraryName(uint32_t base, const Context& ctx) const
{
    if (!ctx.readByte)
        return {};

    auto readLong = [&](uint32_t addr, uint32_t& out) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t byte = 0;
            if (!ctx.readByte(addr + i, byte))
                return false;
            v = (v << 8) | byte;
        }
        out = v;
        return true;
    };

    // struct Library begins with struct Node; ln_Name is the APTR at offset 10.
    uint32_t namePtr = 0;
    if (!readLong(base + 10, namePtr) || namePtr == 0)
        return {};

    std::string name;
    for (int i = 0; i < 40; ++i) {
        uint8_t c = 0;
        if (!ctx.readByte(namePtr + i, c))
            return {};
        if (c == 0)
            break;
        if (c < 0x20 || c > 0x7E)
            return {}; // non-printable -> not a real library name
        name.push_back(static_cast<char>(c));
    }
    return name;
}

std::optional<std::string> LibraryCallAnnotator::Annotate(const std::string& mnemonic,
                                                          const Context& ctx) const
{
    if (m_tables.empty())
        return std::nullopt;

    int aReg = 0;
    uint32_t lvo = 0;
    if (!ParseLibCall(mnemonic, aReg, lvo))
        return std::nullopt;

    // Strategy (a): default to exec.library.
    std::string libName = "exec.library";

    // Strategy (c): if the live base register and memory are available, identify
    // the library by its node name. A name we recognise overrides the default; a
    // name we can read but don't have a table for suppresses the annotation (the
    // call is into some other library, so labelling it "exec" would be wrong).
    if (ctx.readAReg) {
        uint32_t base = 0;
        if (ctx.readAReg(aReg, base)) {
            std::string resolved = ReadLibraryName(base, ctx);
            if (!resolved.empty()) {
                if (m_tables.count(resolved))
                    libName = resolved;
                else
                    return std::nullopt;
            }
        }
    }

    auto libIt = m_tables.find(libName);
    if (libIt == m_tables.end())
        return std::nullopt;
    auto fnIt = libIt->second.find(lvo);
    if (fnIt == libIt->second.end())
        return std::nullopt;

    return fmt::format("  ; {}/{}()", ShortLibName(libName), fnIt->second);
}

} // namespace adm::core
