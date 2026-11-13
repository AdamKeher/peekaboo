// Tests for the trainer lock store and the anchor resolver.
//
// The interesting behaviour here is not the bookkeeping, it is the promise the feature rests
// on: an entry is only ever enabled once its anchor resolves AND its guard bytes match, so a
// trainer authored against one release can never poke a random address in another. Those
// paths are what most of this file pins down.
//
// Both classes read through ArMemoryAccess and take the segment list as plain data, so a
// synthetic memory image is all that is needed - no emulator, no core.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../src/core/AnchorResolver.h"
#include "../src/core/ArMemoryAccess.h"
#include "../src/core/MemoryLocks.h"

using namespace adm::core;

namespace {

// A stand-in Amiga address space: one writable "chip" region at $000000 and one read-only
// "rom" region at $F80000, both backed by vectors we can poke directly.
class FakeMemory
{
public:
    FakeMemory() : m_chip(0x1000, 0x00), m_rom(0x100, 0xEE)
    {
        std::vector<ArMemoryAccess::Region> regions;
        regions.push_back(
            {m_chip.data(), 0x000000, static_cast<uint32_t>(m_chip.size()), "chip", true});
        regions.push_back(
            {m_rom.data(), 0xF80000, static_cast<uint32_t>(m_rom.size()), "rom", false});
        m_mem.SetRegions(std::move(regions));
    }

    ArMemoryAccess& Mem() { return m_mem; }
    uint8_t& Chip(uint32_t addr) { return m_chip[addr]; }

    void PutBytes(uint32_t addr, std::initializer_list<uint8_t> bytes)
    {
        uint32_t i = 0;
        for (uint8_t b : bytes)
            m_chip[addr + i++] = b;
    }

    void PutLong(uint32_t addr, uint32_t v)
    {
        m_chip[addr + 0] = static_cast<uint8_t>(v >> 24);
        m_chip[addr + 1] = static_cast<uint8_t>(v >> 16);
        m_chip[addr + 2] = static_cast<uint8_t>(v >> 8);
        m_chip[addr + 3] = static_cast<uint8_t>(v);
    }

private:
    std::vector<uint8_t> m_chip;
    std::vector<uint8_t> m_rom;
    ArMemoryAccess m_mem;
};

BytePattern Pat(const std::string& text)
{
    std::string err;
    auto p = BytePattern::Parse(text, err);
    EXPECT_TRUE(p.has_value()) << "pattern '" << text << "' failed to parse: " << err;
    return p.value_or(BytePattern{});
}

// A MemoryLocks that persists to a scratch file rather than the working directory, and
// cleans up after itself.
class ScopedLocks
{
public:
    explicit ScopedLocks(std::string path) : m_path(std::move(path))
    {
        std::remove(m_path.c_str());
        m_locks.SetPath(m_path);
    }
    ~ScopedLocks() { std::remove(m_path.c_str()); }

    MemoryLocks& operator*() { return m_locks; }
    MemoryLocks* operator->() { return &m_locks; }
    const std::string& Path() const { return m_path; }

private:
    std::string m_path;
    MemoryLocks m_locks;
};

// ---- BytePattern -------------------------------------------------------------------------

TEST(BytePatternTest, ParsesHexAndWildcardsIgnoringWhitespace)
{
    std::string err;
    auto p = BytePattern::Parse("53 80 ?? 4EB9", err);
    ASSERT_TRUE(p.has_value()) << err;
    EXPECT_EQ(p->Size(), 5u);
    EXPECT_EQ(p->mask[0], 0xFF);
    EXPECT_EQ(p->bytes[0], 0x53);
    EXPECT_EQ(p->mask[2], 0x00) << "?? must be a wildcard byte";
    EXPECT_EQ(p->bytes[3], 0x4E);
    EXPECT_EQ(p->ToString(), "53 80 ?? 4E B9");
}

TEST(BytePatternTest, RejectsMalformedInput)
{
    std::string err;
    EXPECT_FALSE(BytePattern::Parse("53 8", err).has_value()) << "half a byte";
    EXPECT_FALSE(BytePattern::Parse("53 ZZ", err).has_value()) << "not hex";
    EXPECT_FALSE(BytePattern::Parse("   ", err).has_value()) << "empty";
}

TEST(BytePatternTest, WildcardBytesMatchAnything)
{
    const BytePattern p = Pat("4E B9 ?? ?? ?? ??");
    const uint8_t a[] = {0x4E, 0xB9, 0x00, 0x12, 0x34, 0x56};
    const uint8_t b[] = {0x4E, 0xB9, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t c[] = {0x4E, 0xB8, 0x00, 0x12, 0x34, 0x56};
    EXPECT_TRUE(p.MatchesAt(a));
    EXPECT_TRUE(p.MatchesAt(b)) << "the operand bytes must not affect the match";
    EXPECT_FALSE(p.MatchesAt(c)) << "a fixed byte that differs must fail";
}

// ---- AnchorResolver ----------------------------------------------------------------------

TEST(AnchorResolverTest, AbsoluteResolvesToItself)
{
    FakeMemory fm;
    Anchor a;
    a.kind = AnchorKind::Absolute;
    a.addr = 0x0400;

    const auto r = AnchorResolver::Resolve(a, fm.Mem(), {}, {});
    EXPECT_EQ(r.state, ResolveState::Resolved);
    EXPECT_EQ(r.addr, 0x0400u);
}

TEST(AnchorResolverTest, VerifyBytesGateAnAbsoluteAnchor)
{
    FakeMemory fm;
    fm.PutBytes(0x0400, {0x4E, 0x71});

    Anchor a;
    a.kind = AnchorKind::Absolute;
    a.addr = 0x0400;

    EXPECT_EQ(AnchorResolver::Resolve(a, fm.Mem(), {}, Pat("4E71")).state, ResolveState::Resolved);

    const auto bad = AnchorResolver::Resolve(a, fm.Mem(), {}, Pat("4EB9"));
    EXPECT_EQ(bad.state, ResolveState::VerifyMismatch);
    EXPECT_NE(bad.note.find("4E 71"), std::string::npos)
        << "the message should report what was actually there: " << bad.note;
}

TEST(AnchorResolverTest, SeglistAddsTheRuntimeSegmentBase)
{
    FakeMemory fm;
    // Segment 1 loaded at $0800 - the sort of relocation that changes when Fast RAM is added.
    const std::vector<AmigaSegment> segs = {{0x0200, 0x100}, {0x0800, 0x400}};

    Anchor a;
    a.kind = AnchorKind::Seglist;
    a.seg = 1;
    a.offset = 0x40;

    const auto r = AnchorResolver::Resolve(a, fm.Mem(), segs, {});
    EXPECT_EQ(r.state, ResolveState::Resolved);
    EXPECT_EQ(r.addr, 0x0840u) << "seg base + offset";
}

TEST(AnchorResolverTest, SeglistReportsWhyItCannotResolve)
{
    FakeMemory fm;
    Anchor a;
    a.kind = AnchorKind::Seglist;
    a.seg = 1;
    a.offset = 0x40;

    const auto none = AnchorResolver::Resolve(a, fm.Mem(), {}, {});
    EXPECT_EQ(none.state, ResolveState::NotFound);
    EXPECT_NE(none.note.find("no AmigaDOS process"), std::string::npos) << none.note;

    const std::vector<AmigaSegment> small = {{0x0200, 0x100}, {0x0800, 0x10}};
    const auto past = AnchorResolver::Resolve(a, fm.Mem(), small, {});
    EXPECT_EQ(past.state, ResolveState::NotFound) << "offset past the end of the segment";
}

TEST(AnchorResolverTest, SignatureFindsTheCodeAndAppliesTheTargetOffset)
{
    FakeMemory fm;
    // JSR $00123456 followed by BNE - the address operand is wildcarded so the pattern still
    // matches a build that links the callee somewhere else.
    fm.PutBytes(0x0300, {0x4E, 0xB9, 0x00, 0x12, 0x34, 0x56, 0x66, 0x00});

    Anchor a;
    a.kind = AnchorKind::Signature;
    a.pattern = Pat("4EB9 ?? ?? ?? ?? 6600");

    const auto r = AnchorResolver::Resolve(a, fm.Mem(), {}, {});
    EXPECT_EQ(r.state, ResolveState::Resolved);
    EXPECT_EQ(r.addr, 0x0300u);

    a.targetOffset = 6;
    EXPECT_EQ(AnchorResolver::Resolve(a, fm.Mem(), {}, {}).addr, 0x0306u);
}

TEST(AnchorResolverTest, SignatureRefusesToGuessWhenItMatchesTwice)
{
    FakeMemory fm;
    fm.PutBytes(0x0300, {0x4E, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x66, 0x00});
    fm.PutBytes(0x0500, {0x4E, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x66, 0x00});

    Anchor a;
    a.kind = AnchorKind::Signature;
    a.pattern = Pat("4EB9 ?? ?? ?? ?? 6600");

    const auto r = AnchorResolver::Resolve(a, fm.Mem(), {}, {});
    EXPECT_EQ(r.state, ResolveState::Ambiguous) << "two matches must not silently pick the first";
    EXPECT_NE(r.note.find("lengthen"), std::string::npos) << r.note;
}

TEST(AnchorResolverTest, SignatureReportsNotFoundRatherThanResolvingToZero)
{
    FakeMemory fm;
    Anchor a;
    a.kind = AnchorKind::Signature;
    a.pattern = Pat("DE AD BE EF");

    EXPECT_EQ(AnchorResolver::Resolve(a, fm.Mem(), {}, {}).state, ResolveState::NotFound);
}

TEST(AnchorResolverTest, OperandAnchorReadsTheVariableAddressOutOfTheInstruction)
{
    FakeMemory fm;
    // SUBQ.B #1,$00000640 - the classic "decrement the lives counter" shape. The variable
    // itself has no signature, so the anchor matches the instruction and takes the operand.
    fm.PutBytes(0x0300, {0x53, 0x39});
    fm.PutLong(0x0302, 0x00000640);

    Anchor a;
    a.kind = AnchorKind::Operand;
    a.pattern = Pat("5339 ?? ?? ?? ??");
    a.operandAt = 2;
    a.operandSize = 4;

    const auto r = AnchorResolver::Resolve(a, fm.Mem(), {}, {});
    EXPECT_EQ(r.state, ResolveState::Resolved);
    EXPECT_EQ(r.addr, 0x0640u) << "resolved address must be the operand, not the instruction";
}

TEST(AnchorResolverTest, OperandAnchorRejectsAnImpossibleShape)
{
    FakeMemory fm;
    fm.PutBytes(0x0300, {0x53, 0x39, 0x00, 0x00, 0x06, 0x40});

    Anchor a;
    a.kind = AnchorKind::Operand;
    a.pattern = Pat("5339 ?? ?? ?? ??");
    a.operandAt = 2;

    a.operandSize = 3;
    EXPECT_EQ(AnchorResolver::Resolve(a, fm.Mem(), {}, {}).state, ResolveState::Invalid)
        << "only 2 and 4 byte absolute operands exist";

    a.operandSize = 4;
    a.operandAt = 4; // 4 + 4 > pattern length
    EXPECT_EQ(AnchorResolver::Resolve(a, fm.Mem(), {}, {}).state, ResolveState::Invalid);
}

TEST(AnchorResolverTest, FindPatternScansReadOnlyRegionsToo)
{
    // Code signatures frequently sit in write-protected memory; a ROM match is still a
    // perfectly good anchor.
    FakeMemory fm;
    const auto hits = AnchorResolver::FindPattern(Pat("EE EE EE EE"), fm.Mem(), 1);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_GE(hits[0], 0xF80000u);
}

// ---- MemoryLocks -------------------------------------------------------------------------

TEST(MemoryLocksTest, AddAssignsUniqueIdsAndFindRetrievesThem)
{
    ScopedLocks locks("test_locks_ids.json");
    LockEntry a;
    a.desc = "first";
    a.anchor.addr = 0x100;
    LockEntry b;
    b.desc = "second";
    b.anchor.addr = 0x200;

    const std::string ida = locks->Add(a);
    const std::string idb = locks->Add(b);
    EXPECT_NE(ida, idb);
    ASSERT_NE(locks->Find(ida), nullptr);
    EXPECT_EQ(locks->Find(ida)->desc, "first");
    EXPECT_EQ(locks->Size(), 2u);
}

TEST(MemoryLocksTest, EnablingResolvesFirstAndRefusesAnUnverifiedAddress)
{
    FakeMemory fm;
    fm.PutBytes(0x0400, {0x12, 0x34});
    LockContext ctx;
    ctx.mem = &fm.Mem();

    ScopedLocks locks("test_locks_verify.json");
    LockEntry e;
    e.desc = "infinite lives";
    e.mode = LockMode::Set;
    e.anchor.kind = AnchorKind::Absolute;
    e.anchor.addr = 0x0400;
    e.size = 1;
    e.value = 9;
    e.verify = Pat("99"); // deliberately wrong
    const std::string id = locks->Add(e);

    std::string err;
    EXPECT_FALSE(locks->SetEnabled(id, true, ctx, err));
    EXPECT_FALSE(locks->Find(id)->enabled) << "a failed verify must leave the entry off";
    EXPECT_EQ(locks->Find(id)->state, ResolveState::VerifyMismatch);
    EXPECT_TRUE(locks->BuildProtections().empty())
        << "an unverified entry must never reach the core";

    // Correct the guard bytes and it turns on.
    locks->Find(id)->verify = Pat("12");
    EXPECT_TRUE(locks->SetEnabled(id, true, ctx, err)) << err;
    EXPECT_TRUE(locks->Find(id)->enabled);
    ASSERT_EQ(locks->BuildProtections().size(), 1u);
    EXPECT_EQ(locks->BuildProtections()[0].addr, 0x0400u);
}

TEST(MemoryLocksTest, BuildProtectionsMapsModesOntoTheCoreTable)
{
    FakeMemory fm;
    LockContext ctx;
    ctx.mem = &fm.Mem();
    ScopedLocks locks("test_locks_build.json");

    LockEntry blk;
    blk.desc = "freeze buffer";
    blk.mode = LockMode::Block;
    blk.anchor.addr = 0x0100;
    blk.len = 32;
    const std::string idb = locks->Add(blk);

    LockEntry set;
    set.desc = "force score";
    set.mode = LockMode::Set;
    set.anchor.addr = 0x0200;
    set.size = 2;
    set.value = 0x270F;
    const std::string ids = locks->Add(set);

    std::string err;
    ASSERT_TRUE(locks->SetEnabled(idb, true, ctx, err)) << err;
    ASSERT_TRUE(locks->SetEnabled(ids, true, ctx, err)) << err;

    const auto specs = locks->BuildProtections();
    ASSERT_EQ(specs.size(), 2u);
    EXPECT_EQ(specs[0].mode, 0);
    EXPECT_EQ(specs[0].len, 32u);
    EXPECT_EQ(specs[1].mode, 1);
    EXPECT_EQ(specs[1].size, 2);
    EXPECT_EQ(specs[1].value, 0x270Fu);
    EXPECT_EQ(specs[1].len, 2u) << "a forced value protects exactly its own width";

    // Disabling takes it back out of the table.
    ASSERT_TRUE(locks->SetEnabled(idb, false, ctx, err)) << err;
    EXPECT_EQ(locks->BuildProtections().size(), 1u);
}

TEST(MemoryLocksTest, PatchWritesOnEnableAndRestoresOnDisable)
{
    FakeMemory fm;
    fm.PutBytes(0x0300, {0x53, 0x39, 0x00, 0x00});
    LockContext ctx;
    ctx.mem = &fm.Mem();
    ScopedLocks locks("test_locks_patch.json");

    LockEntry e;
    e.desc = "NOP the decrement";
    e.mode = LockMode::Patch;
    e.anchor.addr = 0x0300;
    e.bytes = {0x4E, 0x71, 0x4E, 0x71};
    const std::string id = locks->Add(e);

    std::string err;
    ASSERT_TRUE(locks->SetEnabled(id, true, ctx, err)) << err;
    EXPECT_EQ(fm.Chip(0x0300), 0x4E);
    EXPECT_EQ(fm.Chip(0x0301), 0x71);
    EXPECT_TRUE(locks->Find(id)->applied);
    EXPECT_TRUE(locks->BuildProtections().empty())
        << "a patch is already in memory - it must not also occupy a core protection slot";

    ASSERT_TRUE(locks->SetEnabled(id, false, ctx, err)) << err;
    EXPECT_EQ(fm.Chip(0x0300), 0x53) << "original bytes must come back";
    EXPECT_EQ(fm.Chip(0x0301), 0x39);
    EXPECT_FALSE(locks->Find(id)->applied);
}

TEST(MemoryLocksTest, PatchIntoReadOnlyMemoryFailsWithoutClaimingSuccess)
{
    FakeMemory fm;
    LockContext ctx;
    ctx.mem = &fm.Mem();
    ScopedLocks locks("test_locks_ro.json");

    LockEntry e;
    e.desc = "patch the rom";
    e.mode = LockMode::Patch;
    e.anchor.addr = 0xF80000;
    e.bytes = {0x4E, 0x71};
    const std::string id = locks->Add(e);

    std::string err;
    EXPECT_FALSE(locks->SetEnabled(id, true, ctx, err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(locks->Find(id)->enabled);
    EXPECT_FALSE(locks->Find(id)->applied);
}

TEST(MemoryLocksTest, ResolveAllDisablesEntriesThatStopResolving)
{
    FakeMemory fm;
    fm.PutBytes(0x0300, {0x4E, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x66, 0x00});
    LockContext ctx;
    ctx.mem = &fm.Mem();
    ScopedLocks locks("test_locks_rescan.json");

    LockEntry e;
    e.desc = "sig lock";
    e.mode = LockMode::Block;
    e.anchor.kind = AnchorKind::Signature;
    e.anchor.pattern = Pat("4EB9 ?? ?? ?? ?? 6600");
    e.len = 2;
    const std::string id = locks->Add(e);

    std::string err;
    ASSERT_TRUE(locks->SetEnabled(id, true, ctx, err)) << err;
    ASSERT_EQ(locks->BuildProtections().size(), 1u);

    // The code is overwritten - a new level loaded over it, say. The lock must not keep
    // protecting an address that now belongs to something else.
    fm.PutBytes(0x0300, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    locks->ResolveAll(ctx);

    EXPECT_FALSE(locks->Find(id)->enabled);
    EXPECT_EQ(locks->Find(id)->state, ResolveState::NotFound);
    EXPECT_TRUE(locks->BuildProtections().empty());
}

TEST(MemoryLocksTest, FindCoveringSpansTheWholeProtectedRange)
{
    FakeMemory fm;
    LockContext ctx;
    ctx.mem = &fm.Mem();
    ScopedLocks locks("test_locks_cover.json");

    LockEntry e;
    e.desc = "range";
    e.mode = LockMode::Block;
    e.anchor.addr = 0x0100;
    e.len = 16;
    const std::string id = locks->Add(e);
    std::string err;
    ASSERT_TRUE(locks->SetEnabled(id, true, ctx, err)) << err;

    EXPECT_EQ(locks->FindCovering(0x00FF), nullptr);
    ASSERT_NE(locks->FindCovering(0x0100), nullptr);
    ASSERT_NE(locks->FindCovering(0x010F), nullptr);
    EXPECT_EQ(locks->FindCovering(0x0110), nullptr);

    EXPECT_EQ(locks->RemoveCovering(0x0108), 1u);
    EXPECT_TRUE(locks->Empty());
}

TEST(MemoryLocksTest, RoundTripsThroughJsonPreservingEveryAnchorKind)
{
    const std::string path = "test_locks_roundtrip.json";
    std::remove(path.c_str());

    {
        MemoryLocks locks;
        locks.SetPath(path);

        LockEntry abs;
        abs.desc = "absolute set";
        abs.mode = LockMode::Set;
        abs.anchor.kind = AnchorKind::Absolute;
        abs.anchor.addr = 0x0010ABCD;
        abs.size = 2;
        abs.value = 0x270F;
        abs.verify = Pat("12 34");
        abs.group = "Player";
        locks.Add(abs);

        LockEntry seg;
        seg.desc = "seglist block";
        seg.mode = LockMode::Block;
        seg.anchor.kind = AnchorKind::Seglist;
        seg.anchor.seg = 2;
        seg.anchor.offset = 0x1A4C;
        seg.len = 8;
        locks.Add(seg);

        LockEntry sig;
        sig.desc = "signature patch";
        sig.mode = LockMode::Patch;
        sig.anchor.kind = AnchorKind::Signature;
        sig.anchor.pattern = Pat("4EB9 ?? ?? ?? ??");
        sig.anchor.targetOffset = 6;
        sig.bytes = {0x4E, 0x71, 0x4E, 0x71};
        locks.Add(sig);

        LockEntry op;
        op.desc = "operand set";
        op.mode = LockMode::Set;
        op.anchor.kind = AnchorKind::Operand;
        op.anchor.pattern = Pat("5339 ?? ?? ?? ??");
        op.anchor.operandAt = 2;
        op.anchor.operandSize = 4;
        op.size = 1;
        op.value = 9;
        locks.Add(op);
    }

    // The store loads in its constructor, so rather than re-pointing an instance, read the
    // file back and push it through the same EntryFromJson the loader uses.
    nlohmann::json j;
    {
        std::ifstream f(path);
        f >> j;
    }
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 4u);

    std::string err;
    auto e0 = MemoryLocks::EntryFromJson(j[0], err);
    ASSERT_TRUE(e0.has_value()) << err;
    EXPECT_EQ(e0->desc, "absolute set");
    EXPECT_EQ(e0->group, "Player");
    EXPECT_EQ(e0->mode, LockMode::Set);
    EXPECT_EQ(e0->anchor.kind, AnchorKind::Absolute);
    EXPECT_EQ(e0->anchor.addr, 0x0010ABCDu);
    EXPECT_EQ(e0->size, 2);
    EXPECT_EQ(e0->value, 0x270Fu);
    EXPECT_EQ(e0->verify.ToString(), "12 34");

    auto e1 = MemoryLocks::EntryFromJson(j[1], err);
    ASSERT_TRUE(e1.has_value()) << err;
    EXPECT_EQ(e1->anchor.kind, AnchorKind::Seglist);
    EXPECT_EQ(e1->anchor.seg, 2);
    EXPECT_EQ(e1->anchor.offset, 0x1A4Cu);
    EXPECT_EQ(e1->len, 8u);

    auto e2 = MemoryLocks::EntryFromJson(j[2], err);
    ASSERT_TRUE(e2.has_value()) << err;
    EXPECT_EQ(e2->anchor.kind, AnchorKind::Signature);
    EXPECT_EQ(e2->anchor.pattern.ToString(), "4E B9 ?? ?? ?? ??");
    EXPECT_EQ(e2->anchor.targetOffset, 6);
    EXPECT_EQ(e2->mode, LockMode::Patch);
    ASSERT_EQ(e2->bytes.size(), 4u);
    EXPECT_EQ(e2->bytes[0], 0x4E);

    auto e3 = MemoryLocks::EntryFromJson(j[3], err);
    ASSERT_TRUE(e3.has_value()) << err;
    EXPECT_EQ(e3->anchor.kind, AnchorKind::Operand);
    EXPECT_EQ(e3->anchor.operandAt, 2);
    EXPECT_EQ(e3->anchor.operandSize, 4);

    std::remove(path.c_str());
}

TEST(MemoryLocksTest, RejectsEntriesThatWouldBeSilentNoOps)
{
    std::string err;
    nlohmann::json j;
    j["desc"] = "bad size";
    j["mode"] = "set";
    j["addr"] = "0x1000";
    j["size"] = 3; // the core only knows 1/2/4
    EXPECT_FALSE(MemoryLocks::EntryFromJson(j, err).has_value());

    nlohmann::json k;
    k["desc"] = "zero length";
    k["mode"] = "block";
    k["addr"] = "0x1000";
    k["len"] = 0;
    EXPECT_FALSE(MemoryLocks::EntryFromJson(k, err).has_value());

    nlohmann::json m;
    m["desc"] = "empty patch";
    m["mode"] = "patch";
    m["addr"] = "0x1000";
    EXPECT_FALSE(MemoryLocks::EntryFromJson(m, err).has_value());

    nlohmann::json n;
    n["desc"] = "no anchor at all";
    n["mode"] = "block";
    EXPECT_FALSE(MemoryLocks::EntryFromJson(n, err).has_value());
}

TEST(MemoryLocksTest, AcceptsABareAddrAsAbsoluteAnchorSugar)
{
    // Hand-written files and imported .cht entries use the terse form.
    nlohmann::json j;
    j["desc"] = "terse";
    j["mode"] = "set";
    j["addr"] = "0x0010ABCD";
    j["size"] = 1;
    j["value"] = 99;

    std::string err;
    auto e = MemoryLocks::EntryFromJson(j, err);
    ASSERT_TRUE(e.has_value()) << err;
    EXPECT_EQ(e->anchor.kind, AnchorKind::Absolute);
    EXPECT_EQ(e->anchor.addr, 0x0010ABCDu);
}

TEST(MemoryLocksTest, RevisionMovesOnEveryChangeSoTheCoreTableCanBeRebuiltLazily)
{
    ScopedLocks locks("test_locks_rev.json");
    const uint64_t start = locks->Revision();

    LockEntry e;
    e.desc = "x";
    e.anchor.addr = 0x100;
    const std::string id = locks->Add(e);
    const uint64_t afterAdd = locks->Revision();
    EXPECT_GT(afterAdd, start);

    locks->Remove(id);
    EXPECT_GT(locks->Revision(), afterAdd);
}

} // namespace

// ---- MemoryLocks::SetValue ---------------------------------------------------------------
//
// This is what the register windows' Set button calls when the register is already locked.
// Mutating the entry through the non-const Find() would skip Save() and the revision bump,
// so the lock would keep forcing the old value until something else touched the store - the
// exact "it resets to the previous value" bug this method exists to prevent.

TEST(MemoryLocksTest, SetValueRetargetsALockAndBumpsTheRevision)
{
    ScopedLocks locks("locks_setvalue.json");
    LockEntry e;
    e.mode = LockMode::Set;
    e.anchor.kind = AnchorKind::Absolute;
    e.anchor.addr = 0x0200;
    e.size = 1;
    e.value = 0x05;
    e.len = 1;
    e.enabled = true;
    const std::string id = locks->Add(e);

    const uint64_t before = locks->Revision();
    std::string err;
    ASSERT_TRUE(locks->SetValue(id, 0x63, 1, err)) << err;

    const LockEntry* got = locks->Find(id);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->value, 0x63u);
    EXPECT_EQ(got->size, 1);
    // The entry stays on: the point is to keep holding, at a new value.
    EXPECT_TRUE(got->enabled);
    // Without the bump the core would keep enforcing the old value - one integer compare per
    // frame is the only thing that tells Application to re-push the table.
    EXPECT_GT(locks->Revision(), before);
}

TEST(MemoryLocksTest, SetValueWidensAndKeepsLenTrackingSize)
{
    ScopedLocks locks("locks_setvalue_width.json");
    LockEntry e;
    e.mode = LockMode::Set;
    e.anchor.kind = AnchorKind::Absolute;
    e.anchor.addr = 0xDFF180;
    e.size = 1;
    e.value = 0x0F;
    e.len = 1;
    const std::string id = locks->Add(e);

    std::string err;
    ASSERT_TRUE(locks->SetValue(id, 0x0123, 2, err)) << err;
    const LockEntry* got = locks->Find(id);
    EXPECT_EQ(got->size, 2);
    // A forced value protects exactly its own width, so len has to follow or the protection
    // span and the value would disagree.
    EXPECT_EQ(got->len, 2u);
    EXPECT_EQ(got->Span(), 2u);
}

TEST(MemoryLocksTest, SetValueRejectsAValueTooWideForItsSize)
{
    ScopedLocks locks("locks_setvalue_toobig.json");
    LockEntry e;
    e.mode = LockMode::Set;
    e.anchor.kind = AnchorKind::Absolute;
    e.anchor.addr = 0x0200;
    e.size = 1;
    e.value = 0x05;
    const std::string id = locks->Add(e);

    const uint64_t before = locks->Revision();
    std::string err;
    // Truncating silently would force a value the user never asked for, forever.
    EXPECT_FALSE(locks->SetValue(id, 0x1234, 1, err));
    EXPECT_NE(err.find("does not fit"), std::string::npos);
    EXPECT_EQ(locks->Find(id)->value, 0x05u) << "a rejected value must not be applied";
    EXPECT_EQ(locks->Revision(), before) << "a rejected change must not bump the revision";
}

TEST(MemoryLocksTest, SetValueRefusesBlockAndPatchEntriesAndUnknownIds)
{
    ScopedLocks locks("locks_setvalue_mode.json");
    LockEntry block;
    block.mode = LockMode::Block;
    block.anchor.kind = AnchorKind::Absolute;
    block.anchor.addr = 0x0200;
    block.len = 4;
    const std::string blockId = locks->Add(block);

    std::string err;
    // Only a Set lock forces a value; silently converting a Block lock would change what the
    // user asked for into something that writes to memory.
    EXPECT_FALSE(locks->SetValue(blockId, 0x63, 1, err));
    EXPECT_NE(err.find("block"), std::string::npos);
    EXPECT_EQ(locks->Find(blockId)->len, 4u);

    err.clear();
    EXPECT_FALSE(locks->SetValue("nosuchid", 0x63, 1, err));
    EXPECT_FALSE(err.empty());
}

TEST(MemoryLocksTest, SetValueRejectsAWidthThatIsNotOneTwoOrFour)
{
    ScopedLocks locks("locks_setvalue_badsize.json");
    LockEntry e;
    e.mode = LockMode::Set;
    e.anchor.kind = AnchorKind::Absolute;
    e.anchor.addr = 0x0200;
    e.size = 1;
    const std::string id = locks->Add(e);

    std::string err;
    EXPECT_FALSE(locks->SetValue(id, 0x01, 3, err));
    EXPECT_NE(err.find("size"), std::string::npos);
}

TEST(MemoryLocksTest, SetValueReachesTheCoreThroughBuildProtections)
{
    ScopedLocks locks("locks_setvalue_core.json");
    FakeMemory mem;
    LockContext ctx;
    ctx.mem = &mem.Mem();

    LockEntry e;
    e.mode = LockMode::Set;
    e.anchor.kind = AnchorKind::Absolute;
    e.anchor.addr = 0x0200;
    e.size = 1;
    e.value = 0x05;
    e.len = 1;
    const std::string id = locks->Add(e);
    std::string err;
    ASSERT_TRUE(locks->SetEnabled(id, true, ctx, err)) << err;

    ASSERT_TRUE(locks->SetValue(id, 0x63, 1, err)) << err;

    // What the core is handed is the whole point - an updated entry that never makes it into
    // BuildProtections() would look right in the UI and do nothing to the machine.
    const auto specs = locks->BuildProtections();
    ASSERT_EQ(specs.size(), 1u);
    EXPECT_EQ(specs[0].addr, 0x0200u);
    EXPECT_EQ(specs[0].value, 0x63u);
    EXPECT_EQ(specs[0].size, 1);
    EXPECT_EQ(specs[0].mode, 1) << "1 = ADM_PROTECT_MODE_SET";
}
