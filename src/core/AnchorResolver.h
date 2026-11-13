#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SegListResolver.h"

namespace adm::core {

class ArMemoryAccess;

/**
 * @brief A byte pattern with per-byte wildcards.
 *
 * Parsed from text like "5380 4EB9 ?? ?? ?? ?? 6600": hex digit pairs are literal bytes,
 * "??" is a wildcard byte. Whitespace is insignificant, so "5380" and "53 80" are the same
 * pattern. `mask` runs parallel to `bytes`: 0xFF means the byte must match, 0x00 means any
 * byte matches.
 *
 * Wildcards are what make a signature survive relocation — the address operands of a
 * JSR/JMP change between builds and releases while the opcodes around them do not.
 */
struct BytePattern
{
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;

    bool Empty() const { return bytes.empty(); }
    size_t Size() const { return bytes.size(); }

    /// Parses pattern text. Returns nullopt and sets err on malformed input.
    static std::optional<BytePattern> Parse(const std::string& text, std::string& err);

    /// Renders back to canonical "53 80 ?? ??" form (space-separated, uppercase).
    std::string ToString() const;

    /// True when `data` (at least Size() bytes) satisfies the pattern.
    bool MatchesAt(const uint8_t* data) const;
};

enum class AnchorKind
{
    Absolute,  ///< A fixed address. Fine for OS-takeover loaders, brittle otherwise.
    Seglist,   ///< AmigaDOS segment + offset, resolved through SegListResolver.
    Signature, ///< Masked byte scan; the target sits at matchBase + targetOffset.
    Operand    ///< Masked byte scan of an instruction; the target is read out of its operand.
};

/**
 * @brief How to find an address in the running machine.
 *
 * Absolute addresses do not survive a different release of the same game, nor a different
 * memory configuration. The other three kinds each fix one of those problems:
 *
 *  - Seglist survives relocation: an AmigaDOS-loaded program lands wherever AllocMem put it,
 *    which moves when Fast RAM is added, but segment+offset is stable.
 *  - Signature survives a different build of the same code, as long as the surrounding
 *    opcodes are unchanged and the varying address operands are wildcarded.
 *  - Operand is for *variables*, which have no signature of their own — a lives counter is a
 *    lone byte with no stable neighbourhood. Instead it matches the instruction that writes
 *    the variable and reads the variable's address out of that instruction's operand. This
 *    is the inverse of what the TF console command already does.
 */
struct Anchor
{
    AnchorKind kind = AnchorKind::Absolute;

    uint32_t addr = 0; ///< Absolute

    int seg = 0;         ///< Seglist: index into the process's segment list
    uint32_t offset = 0; ///< Seglist: byte offset within that segment

    BytePattern pattern; ///< Signature / Operand
    int targetOffset = 0; ///< Signature: target = matchBase + targetOffset (may be negative)
    int operandAt = 0;    ///< Operand: byte offset of the operand within the match
    int operandSize = 4;  ///< Operand: 2 (absolute short) or 4 (absolute long)

    /// Human-readable form for listings, e.g. "seg 2 + $1A4C -> $00214A4C".
    std::string Describe(std::optional<uint32_t> resolved) const;
};

enum class ResolveState
{
    Unresolved,     ///< Not looked up yet.
    Resolved,       ///< Found, and the verify bytes matched.
    NotFound,       ///< The signature/segment is not present in memory right now.
    Ambiguous,      ///< The signature matched in more than one place — refusing to guess.
    VerifyMismatch, ///< Found, but the bytes there are not what the entry expected.
    Invalid         ///< The anchor itself is malformed (empty pattern, bad operand size).
};

const char* ToString(ResolveState s);

struct ResolveResult
{
    ResolveState state = ResolveState::Unresolved;
    uint32_t addr = 0;
    std::string note; ///< Why it failed, for the UI and the PR listing.
};

/**
 * @brief Turns an Anchor into a live address, or explains why it can't.
 *
 * Deliberately decoupled from the emulator: it reads through ArMemoryAccess and takes the
 * segment list as data, so it can be unit-tested against a synthetic memory image the same
 * way SegListResolver is.
 */
class AnchorResolver
{
public:
    /**
     * @brief Resolve an anchor and check its guard bytes.
     * @param verify Bytes expected at the *anchor site* — the address itself for Absolute and
     *        Seglist, the match base for Signature and Operand. Empty skips the check.
     *
     * A failed verify is reported as VerifyMismatch rather than resolving, which is what stops
     * a trainer authored against one crack from poking a random address in another.
     */
    static ResolveResult Resolve(const Anchor& anchor, const ArMemoryAccess& mem,
                                 const std::vector<AmigaSegment>& segments,
                                 const BytePattern& verify);

    /**
     * @brief Masked scan for `pattern` across every mapped region.
     * @param maxHits Stop after this many matches (2 is enough to detect ambiguity).
     *
     * Scans ROM as well as RAM: code signatures are frequently in Kickstart-adjacent or
     * write-protected regions, and a read-only match is still a valid anchor.
     */
    static std::vector<uint32_t> FindPattern(const BytePattern& pattern, const ArMemoryAccess& mem,
                                             size_t maxHits);
};

} // namespace adm::core
