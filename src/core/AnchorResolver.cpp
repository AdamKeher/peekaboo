#include "AnchorResolver.h"

#include <cctype>

#include <fmt/format.h>

#include "ArMemoryAccess.h"

namespace adm::core {

namespace {

int HexVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

} // namespace

const char* ToString(ResolveState s)
{
    switch (s) {
        case ResolveState::Unresolved: return "unresolved";
        case ResolveState::Resolved: return "resolved";
        case ResolveState::NotFound: return "not found";
        case ResolveState::Ambiguous: return "ambiguous";
        case ResolveState::VerifyMismatch: return "verify mismatch";
        case ResolveState::Invalid: return "invalid";
    }
    return "?";
}

std::optional<BytePattern> BytePattern::Parse(const std::string& text, std::string& err)
{
    BytePattern p;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '?') {
            // "??" is one wildcard byte; a lone "?" is accepted as the same thing rather
            // than silently meaning half a byte.
            p.bytes.push_back(0x00);
            p.mask.push_back(0x00);
            ++i;
            if (i < text.size() && text[i] == '?')
                ++i;
            continue;
        }
        const int hi = HexVal(c);
        if (hi < 0) {
            err = fmt::format("unexpected '{}' in pattern", c);
            return std::nullopt;
        }
        // Skip whitespace between the two nibbles so "5 3" is still one byte.
        size_t j = i + 1;
        while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j])))
            ++j;
        if (j >= text.size()) {
            err = "pattern ends on a half byte";
            return std::nullopt;
        }
        const int lo = HexVal(text[j]);
        if (lo < 0) {
            err = fmt::format("unexpected '{}' in pattern", text[j]);
            return std::nullopt;
        }
        p.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        p.mask.push_back(0xFF);
        i = j + 1;
    }
    if (p.bytes.empty()) {
        err = "pattern is empty";
        return std::nullopt;
    }
    return p;
}

std::string BytePattern::ToString() const
{
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i)
            out += ' ';
        if (mask[i] == 0x00)
            out += "??";
        else
            out += fmt::format("{:02X}", bytes[i]);
    }
    return out;
}

bool BytePattern::MatchesAt(const uint8_t* data) const
{
    for (size_t i = 0; i < bytes.size(); ++i) {
        if ((data[i] & mask[i]) != (bytes[i] & mask[i]))
            return false;
    }
    return true;
}

std::string Anchor::Describe(std::optional<uint32_t> resolved) const
{
    const std::string tail =
        resolved ? fmt::format(" -> ${:08X}", *resolved) : std::string(" -> ?");
    switch (kind) {
        case AnchorKind::Absolute:
            return fmt::format("${:08X}", addr);
        case AnchorKind::Seglist:
            return fmt::format("seg {} + ${:X}{}", seg, offset, tail);
        case AnchorKind::Signature:
            return fmt::format("sig [{}]{}{}", pattern.ToString(),
                               targetOffset ? fmt::format(" {:+d}", targetOffset) : "", tail);
        case AnchorKind::Operand:
            return fmt::format("operand [{}] @{}{}", pattern.ToString(), operandAt, tail);
    }
    return "?";
}

std::vector<uint32_t> AnchorResolver::FindPattern(const BytePattern& pattern,
                                                  const ArMemoryAccess& mem, size_t maxHits)
{
    std::vector<uint32_t> hits;
    if (pattern.Empty() || maxHits == 0)
        return hits;

    // Anchor the scan on the first non-wildcard byte so a pattern that starts with wildcards
    // still skips cheaply instead of testing every offset in full.
    size_t firstFixed = 0;
    while (firstFixed < pattern.Size() && pattern.mask[firstFixed] == 0x00)
        ++firstFixed;
    const bool allWild = firstFixed >= pattern.Size();
    const uint8_t anchorByte = allWild ? 0 : pattern.bytes[firstFixed];

    const size_t n = pattern.Size();
    for (const auto& r : mem.Regions()) {
        if (!r.hostPtr || r.size < n)
            continue;
        const size_t last = r.size - n;
        for (size_t off = 0; off <= last; ++off) {
            if (!allWild && r.hostPtr[off + firstFixed] != anchorByte)
                continue;
            if (!pattern.MatchesAt(r.hostPtr + off))
                continue;
            hits.push_back(r.base + static_cast<uint32_t>(off));
            if (hits.size() >= maxHits)
                return hits;
        }
    }
    return hits;
}

ResolveResult AnchorResolver::Resolve(const Anchor& anchor, const ArMemoryAccess& mem,
                                      const std::vector<AmigaSegment>& segments,
                                      const BytePattern& verify)
{
    ResolveResult out;

    // --- locate the anchor site ------------------------------------------------------
    uint32_t site = 0; // where the verify bytes are expected
    uint32_t target = 0;

    switch (anchor.kind) {
        case AnchorKind::Absolute:
            site = target = anchor.addr;
            break;

        case AnchorKind::Seglist: {
            if (anchor.seg < 0 || static_cast<size_t>(anchor.seg) >= segments.size()) {
                out.state = ResolveState::NotFound;
                out.note = segments.empty()
                               ? "no AmigaDOS process running (seglist unavailable)"
                               : fmt::format("segment {} of {} does not exist", anchor.seg,
                                             segments.size());
                return out;
            }
            const AmigaSegment& sg = segments[static_cast<size_t>(anchor.seg)];
            if (anchor.offset >= sg.size) {
                out.state = ResolveState::NotFound;
                out.note = fmt::format("offset ${:X} is past the end of segment {} (${:X})",
                                       anchor.offset, anchor.seg, sg.size);
                return out;
            }
            site = target = sg.base + anchor.offset;
            break;
        }

        case AnchorKind::Signature:
        case AnchorKind::Operand: {
            if (anchor.pattern.Empty()) {
                out.state = ResolveState::Invalid;
                out.note = "anchor has no pattern";
                return out;
            }
            if (anchor.kind == AnchorKind::Operand &&
                (anchor.operandSize != 2 && anchor.operandSize != 4)) {
                out.state = ResolveState::Invalid;
                out.note = "operand size must be 2 or 4";
                return out;
            }
            if (anchor.kind == AnchorKind::Operand &&
                (anchor.operandAt < 0 ||
                 static_cast<size_t>(anchor.operandAt) + static_cast<size_t>(anchor.operandSize) >
                     anchor.pattern.Size())) {
                out.state = ResolveState::Invalid;
                out.note = "operand lies outside the pattern";
                return out;
            }

            // Two hits is all it takes to know the answer is ambiguous; there is no point
            // scanning the rest of memory to count them.
            const auto hits = FindPattern(anchor.pattern, mem, 2);
            if (hits.empty()) {
                out.state = ResolveState::NotFound;
                out.note = "pattern not found in memory";
                return out;
            }
            if (hits.size() > 1) {
                out.state = ResolveState::Ambiguous;
                out.note = fmt::format("pattern matches at ${:08X} and ${:08X} (at least) - "
                                       "lengthen it",
                                       hits[0], hits[1]);
                return out;
            }
            site = hits[0];
            if (anchor.kind == AnchorKind::Signature) {
                target = static_cast<uint32_t>(static_cast<int64_t>(site) + anchor.targetOffset);
            } else {
                // Read the variable's address out of the matched instruction's operand.
                bool ok = false;
                const uint32_t at = site + static_cast<uint32_t>(anchor.operandAt);
                target = anchor.operandSize == 4 ? mem.ReadLong(at, &ok)
                                                 : mem.ReadWord(at, &ok);
                if (!ok) {
                    out.state = ResolveState::NotFound;
                    out.note = fmt::format("operand at ${:08X} is unmapped", at);
                    return out;
                }
                // A 16-bit absolute operand is sign-extended by the 68000.
                if (anchor.operandSize == 2)
                    target = static_cast<uint32_t>(static_cast<int32_t>(
                        static_cast<int16_t>(static_cast<uint16_t>(target))));
            }
            break;
        }
    }

    // --- guard bytes -----------------------------------------------------------------
    if (!verify.Empty()) {
        std::vector<uint8_t> got(verify.Size());
        std::vector<bool> valid;
        const size_t read = mem.Read(site, got.data(), got.size(), &valid);
        if (read != got.size()) {
            out.state = ResolveState::VerifyMismatch;
            out.addr = target;
            out.note = fmt::format("cannot read the {} verify byte(s) at ${:08X}", verify.Size(),
                                   site);
            return out;
        }
        if (!verify.MatchesAt(got.data())) {
            out.state = ResolveState::VerifyMismatch;
            out.addr = target;
            std::string gotStr;
            for (size_t i = 0; i < got.size(); ++i)
                gotStr += fmt::format("{}{:02X}", i ? " " : "", got[i]);
            out.note = fmt::format("expected [{}] at ${:08X}, found [{}]", verify.ToString(), site,
                                   gotStr);
            return out;
        }
    }

    out.state = ResolveState::Resolved;
    out.addr = target;
    return out;
}

} // namespace adm::core
