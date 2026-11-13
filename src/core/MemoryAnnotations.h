#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "LaunchOptions.h"

namespace adm::core {

// One user annotation attached to a 32-bit memory address. Either field may be
// empty; a label renders on its own line above the instruction at that address,
// while a comment renders as a trailing "; ..." on the instruction line.
struct Annotation
{
    std::string comment; // trailing "; ..."     (empty = none)
    std::string label;   // line-above name      (empty = none)
    std::string block;   // multi-line "; ..." header above the label (empty = none)

    bool empty() const { return comment.empty() && label.empty() && block.empty(); }
};

// Shared store for user annotations attached to 32-bit memory addresses (the
// TM/TL/TMS/TMD console commands and the Annotations window both operate on
// this). Backed by an ordered map so listings come out sorted by address
// ascending. Persisted to memory_annotations.json in the working directory so
// annotations survive across sessions.
class MemoryAnnotations
{
public:
    MemoryAnnotations() { Load(); }

    // Sets the comment/label at addr, overwriting any existing one, then saves.
    // An empty string clears that field; if the entry becomes empty it is erased.
    void SetComment(uint32_t addr, const std::string& text);
    void SetLabel(uint32_t addr, const std::string& text);
    // Sets the multi-line comment block at addr (raw text, '\n'-separated; the ';'
    // prefixes and "; ====" dividers are presentation, added at render time).
    void SetBlock(uint32_t addr, const std::string& text);

    // Returns the comment/label/block at addr, or nullopt if there isn't one.
    std::optional<std::string> GetComment(uint32_t addr) const;
    std::optional<std::string> GetLabel(uint32_t addr) const;
    std::optional<std::string> GetBlock(uint32_t addr) const;

    // Removes the comment/label/block at addr (erasing the entry if it becomes
    // empty). Returns true if one existed.
    bool RemoveComment(uint32_t addr);
    bool RemoveLabel(uint32_t addr);
    bool RemoveBlock(uint32_t addr);
    // Removes the whole entry (both fields) at addr. Returns true if one existed.
    bool Remove(uint32_t addr);

    // Removes every annotation, then saves.
    void Clear();

    bool Empty() const { return m_map.empty(); }
    size_t Size() const { return m_map.size(); }
    const std::map<uint32_t, Annotation>& All() const { return m_map; }

    // Builds the trailing "  ; comment  ; comment2" for a disassembly line,
    // matching the instruction address and any $hex operand addresses in
    // mnemonic. Empty when nothing matches. Each distinct address contributes at
    // most once. Only comments participate (labels render separately).
    std::string BuildAnnotationSuffix(uint32_t insAddr, const std::string& mnemonic) const;

    // Builds a "  <label>" reference suffix for any $hex operand address in
    // mnemonic that carries a label (e.g. a branch/jump target). The
    // instruction's own address is excluded — that label renders as the line
    // above. Empty when no operand address is labeled.
    std::string BuildBranchLabelSuffix(uint32_t insAddr, const std::string& mnemonic) const;

    // Extracts candidate operand addresses ($hex literals) from a disassembled
    // mnemonic, skipping #$ immediates. Exposed for unit testing.
    static std::vector<uint32_t> ExtractOperandAddresses(const std::string& mnemonic);

    // Formats a raw '\n'-separated comment block into renderable lines: a top
    // "; ====" divider (dividerCols '=' signs), each source line as "; <line>",
    // then a matching bottom divider. Empty block ⇒ empty vector. dividerCols is
    // clamped to at least 1. Caller applies the (grey) comment colour.
    static std::vector<std::string> BuildCommentBlockLines(const std::string& block,
                                                           int dividerCols);

private:
    void Load();
    void Save() const;

    std::map<uint32_t, Annotation> m_map;
    std::string m_path = ProfilePath("memory_annotations.json").string();
};

} // namespace adm::core
