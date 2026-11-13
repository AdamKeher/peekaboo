#include "MemoryAnnotations.h"

#include <cctype>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "Disassembler.h"

namespace adm::core {

void MemoryAnnotations::SetComment(uint32_t addr, const std::string& text)
{
    if (text.empty()) {
        RemoveComment(addr);
        return;
    }
    m_map[addr].comment = text;
    Save();
}

void MemoryAnnotations::SetLabel(uint32_t addr, const std::string& text)
{
    if (text.empty()) {
        RemoveLabel(addr);
        return;
    }
    m_map[addr].label = text;
    Save();
}

void MemoryAnnotations::SetBlock(uint32_t addr, const std::string& text)
{
    if (text.empty()) {
        RemoveBlock(addr);
        return;
    }
    m_map[addr].block = text;
    Save();
}

std::optional<std::string> MemoryAnnotations::GetComment(uint32_t addr) const
{
    auto it = m_map.find(addr);
    if (it == m_map.end() || it->second.comment.empty())
        return std::nullopt;
    return it->second.comment;
}

std::optional<std::string> MemoryAnnotations::GetLabel(uint32_t addr) const
{
    auto it = m_map.find(addr);
    if (it == m_map.end() || it->second.label.empty())
        return std::nullopt;
    return it->second.label;
}

std::optional<std::string> MemoryAnnotations::GetBlock(uint32_t addr) const
{
    auto it = m_map.find(addr);
    if (it == m_map.end() || it->second.block.empty())
        return std::nullopt;
    return it->second.block;
}

bool MemoryAnnotations::RemoveComment(uint32_t addr)
{
    auto it = m_map.find(addr);
    if (it == m_map.end() || it->second.comment.empty())
        return false;
    it->second.comment.clear();
    if (it->second.empty())
        m_map.erase(it);
    Save();
    return true;
}

bool MemoryAnnotations::RemoveLabel(uint32_t addr)
{
    auto it = m_map.find(addr);
    if (it == m_map.end() || it->second.label.empty())
        return false;
    it->second.label.clear();
    if (it->second.empty())
        m_map.erase(it);
    Save();
    return true;
}

bool MemoryAnnotations::RemoveBlock(uint32_t addr)
{
    auto it = m_map.find(addr);
    if (it == m_map.end() || it->second.block.empty())
        return false;
    it->second.block.clear();
    if (it->second.empty())
        m_map.erase(it);
    Save();
    return true;
}

bool MemoryAnnotations::Remove(uint32_t addr)
{
    auto it = m_map.find(addr);
    if (it == m_map.end())
        return false;
    m_map.erase(it);
    Save();
    return true;
}

void MemoryAnnotations::Clear()
{
    m_map.clear();
    Save();
}

std::vector<uint32_t> MemoryAnnotations::ExtractOperandAddresses(const std::string& mnemonic)
{
    std::vector<uint32_t> out;
    int parenDepth = 0;
    for (size_t i = 0; i < mnemonic.size(); ++i) {
        if (mnemonic[i] == '(') {
            ++parenDepth;
            continue;
        }
        if (mnemonic[i] == ')') {
            if (parenDepth > 0)
                --parenDepth;
            continue;
        }
        if (mnemonic[i] != '$')
            continue;
        // Skip immediates such as "#$0005" — those are values, not addresses.
        if (i > 0 && mnemonic[i - 1] == '#')
            continue;
        // Skip register-relative displacements like "(a6,-$01c8)" — the $hex is an
        // offset off an address register, not an absolute address. (The LibraryCall
        // annotator decodes those calls separately.)
        if (parenDepth > 0)
            continue;
        size_t j = i + 1;
        while (j < mnemonic.size() && std::isxdigit(static_cast<unsigned char>(mnemonic[j])))
            ++j;
        if (j == i + 1)
            continue; // lone '$'
        // Skip the "-$01c8(a6)" displacement spelling: a $hex immediately followed by
        // '(' is also a d16(An) offset, not an address.
        if (j < mnemonic.size() && mnemonic[j] == '(') {
            i = j - 1;
            continue;
        }
        try {
            unsigned long v = std::stoul(mnemonic.substr(i + 1, j - i - 1), nullptr, 16);
            out.push_back(static_cast<uint32_t>(v));
        } catch (...) {
            // ignore overflow / parse failures
        }
        i = j - 1;
    }
    return out;
}

std::vector<std::string> MemoryAnnotations::BuildCommentBlockLines(const std::string& block,
                                                                   int dividerCols)
{
    std::vector<std::string> lines;
    if (block.empty())
        return lines;
    if (dividerCols < 1)
        dividerCols = 1;

    const std::string divider = ";" + std::string(static_cast<size_t>(dividerCols), '=');
    lines.push_back(divider);
    // Split on '\n'; a trailing newline yields a final empty "; " line, which is fine.
    size_t start = 0;
    while (true) {
        size_t nl = block.find('\n', start);
        std::string line =
            block.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        lines.push_back("; " + line);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    lines.push_back(divider);
    return lines;
}

std::string MemoryAnnotations::BuildAnnotationSuffix(uint32_t insAddr,
                                                     const std::string& mnemonic) const
{
    if (m_map.empty())
        return {};

    // Candidate addresses: the instruction itself, then each operand address.
    // Dedupe while preserving order so each comment appears at most once.
    std::vector<uint32_t> candidates;
    candidates.push_back(insAddr);
    for (uint32_t a : ExtractOperandAddresses(mnemonic))
        candidates.push_back(a);

    std::string suffix;
    std::vector<uint32_t> seen;
    for (uint32_t addr : candidates) {
        bool dup = false;
        for (uint32_t s : seen)
            if (s == addr) {
                dup = true;
                break;
            }
        if (dup)
            continue;
        seen.push_back(addr);

        auto it = m_map.find(addr);
        if (it != m_map.end() && !it->second.comment.empty())
            suffix += "  ; " + it->second.comment;
    }
    return suffix;
}

std::string MemoryAnnotations::BuildBranchLabelSuffix(uint32_t insAddr,
                                                      const std::string& mnemonic) const
{
    if (m_map.empty())
        return {};

    // Resolve the branch/jump target(s). Relative branches (BRA/BSR/Bcc/DBcc)
    // encode a displacement as a "#$disp" immediate — not the destination — so
    // they must be resolved against the PC; ExtractOperandAddresses skips them.
    // Absolute jumps/calls (JMP/JSR $abs) carry the target as a $hex operand.
    std::vector<uint32_t> targets;
    uint32_t relTarget = 0;
    bool conditional = false;
    if (Disassembler::ParseRelativeBranch(mnemonic, insAddr, relTarget, conditional))
        targets.push_back(relTarget);
    for (uint32_t a : ExtractOperandAddresses(mnemonic))
        targets.push_back(a);

    std::string suffix;
    std::vector<uint32_t> seen;
    for (uint32_t addr : targets) {
        if (addr == insAddr)
            continue; // the instruction's own label renders as the line above
        bool dup = false;
        for (uint32_t s : seen)
            if (s == addr) {
                dup = true;
                break;
            }
        if (dup)
            continue;
        seen.push_back(addr);

        auto it = m_map.find(addr);
        if (it != m_map.end() && !it->second.label.empty())
            suffix += "  <" + it->second.label + ">";
    }
    return suffix;
}

void MemoryAnnotations::Save() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [addr, ann] : m_map) {
        nlohmann::json e;
        e["address"] = addr;
        if (!ann.label.empty())
            e["label"] = ann.label;
        if (!ann.comment.empty())
            e["comment"] = ann.comment;
        if (!ann.block.empty())
            e["block"] = ann.block;
        arr.push_back(std::move(e));
    }

    std::ofstream out(m_path);
    if (out.is_open())
        out << arr.dump(4);
}

void MemoryAnnotations::Load()
{
    if (!std::filesystem::exists(m_path))
        return;
    std::ifstream in(m_path);
    if (!in.is_open())
        return;
    try {
        nlohmann::json j;
        in >> j;
        if (!j.is_array())
            return;
        for (const auto& e : j) {
            if (!e.contains("address") || !e["address"].is_number_unsigned())
                continue;
            uint32_t addr = e["address"].get<uint32_t>();
            Annotation ann;
            // Legacy schema used a single "remark" key — map it onto comment.
            if (e.contains("remark") && e["remark"].is_string())
                ann.comment = e["remark"].get<std::string>();
            if (e.contains("comment") && e["comment"].is_string())
                ann.comment = e["comment"].get<std::string>();
            if (e.contains("label") && e["label"].is_string())
                ann.label = e["label"].get<std::string>();
            if (e.contains("block") && e["block"].is_string())
                ann.block = e["block"].get<std::string>();
            if (!ann.empty())
                m_map[addr] = std::move(ann);
        }
    } catch (...) {
        m_map.clear();
    }
}

} // namespace adm::core
