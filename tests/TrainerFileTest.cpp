// Tests for trainer file I/O: our own peekaboo-trainer JSON, and the one-way RetroArch .cht
// importer.
//
// What matters here is not that JSON round-trips. It is that a trainer arriving from somewhere
// else cannot quietly become live: every import path clears `enabled`, and identity matching
// distinguishes "this exact release" from "some release of this title" so the UI can say which
// it got. The anchors and guard bytes that back that promise are covered by MemoryLocksTest;
// this file covers the container around them.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../src/core/MemoryLocks.h"
#include "../src/core/TrainerFile.h"

using namespace adm::core;

namespace {

// A file that deletes itself, so a failing assertion never leaves litter behind for the next
// run to trip over.
class ScopedFile
{
public:
    explicit ScopedFile(std::string path) : m_path(std::move(path)) { std::remove(m_path.c_str()); }
    ~ScopedFile() { std::remove(m_path.c_str()); }

    const std::string& Path() const { return m_path; }

    void Write(const std::string& contents) const
    {
        std::ofstream f(m_path);
        f << contents;
    }

private:
    std::string m_path;
};

// Same, for a directory of trainer files.
class ScopedDir
{
public:
    explicit ScopedDir(std::string path) : m_path(std::move(path))
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
    }
    ~ScopedDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    const std::string& Path() const { return m_path; }

    void WriteFile(const std::string& name, const std::string& contents) const
    {
        std::ofstream f((std::filesystem::path(m_path) / name).string());
        f << contents;
    }

private:
    std::string m_path;
};

LockEntry BlockEntry(uint32_t addr, uint32_t len, std::string desc)
{
    LockEntry e;
    e.desc = std::move(desc);
    e.mode = LockMode::Block;
    e.anchor.kind = AnchorKind::Absolute;
    e.anchor.addr = addr;
    e.len = len;
    return e;
}

} // namespace

// --- native JSON round trip ---------------------------------------------------------------

TEST(TrainerFileTest, RoundTripsMetadataAndEveryAnchorKind)
{
    ScopedFile file("trainerfile_roundtrip.json");

    TrainerDoc doc;
    doc.title = "Ruff'n'Tumble";
    doc.author = "somebody";
    doc.notes = "lives at the usual place";
    doc.identity.md5 = {"0123456789abcdef0123456789abcdef"};
    doc.identity.tosecTitleGroupId = "ruffntumble";
    doc.identity.kickstart = "1.3";
    doc.identity.chipRamKb = 512;
    doc.identity.fastRamKb = 0;

    // Absolute + Block.
    doc.entries.push_back(BlockEntry(0x00021000, 4, "block the lives counter"));

    // Seglist + Set, which is what survives a different memory configuration.
    {
        LockEntry e;
        e.desc = "99 lives";
        e.mode = LockMode::Set;
        e.anchor.kind = AnchorKind::Seglist;
        e.anchor.seg = 2;
        e.anchor.offset = 0x1A4C;
        e.size = 1;
        e.value = 0x63;
        e.len = 1;
        std::string err;
        e.verify = BytePattern::Parse("53 80", err).value();
        doc.entries.push_back(std::move(e));
    }

    // Signature + Patch, which is what survives a different build.
    {
        LockEntry e;
        e.desc = "NOP the damage routine";
        e.mode = LockMode::Patch;
        e.anchor.kind = AnchorKind::Signature;
        std::string err;
        e.anchor.pattern = BytePattern::Parse("53 80 4EB9 ?? ?? ?? ??", err).value();
        e.anchor.targetOffset = 2;
        e.bytes = {0x4E, 0x71};
        doc.entries.push_back(std::move(e));
    }

    // Operand, for a variable that has no signature of its own.
    {
        LockEntry e;
        e.desc = "the lives variable, found via the instruction that writes it";
        e.mode = LockMode::Block;
        e.anchor.kind = AnchorKind::Operand;
        std::string err;
        e.anchor.pattern = BytePattern::Parse("53 39 ?? ?? ?? ??", err).value();
        e.anchor.operandAt = 2;
        e.anchor.operandSize = 4;
        e.len = 1;
        doc.entries.push_back(std::move(e));
    }

    std::string err;
    ASSERT_TRUE(TrainerFile::Save(file.Path(), doc, err)) << err;

    auto loaded = TrainerFile::Load(file.Path(), err);
    ASSERT_TRUE(loaded.has_value()) << err;

    EXPECT_EQ(loaded->title, doc.title);
    EXPECT_EQ(loaded->author, doc.author);
    EXPECT_EQ(loaded->notes, doc.notes);
    EXPECT_EQ(loaded->identity.md5, doc.identity.md5);
    EXPECT_EQ(loaded->identity.tosecTitleGroupId, doc.identity.tosecTitleGroupId);
    EXPECT_EQ(loaded->identity.kickstart, doc.identity.kickstart);
    EXPECT_EQ(loaded->identity.chipRamKb, 512u);

    ASSERT_EQ(loaded->entries.size(), 4u);

    EXPECT_EQ(loaded->entries[0].mode, LockMode::Block);
    EXPECT_EQ(loaded->entries[0].anchor.kind, AnchorKind::Absolute);
    EXPECT_EQ(loaded->entries[0].anchor.addr, 0x00021000u);
    EXPECT_EQ(loaded->entries[0].len, 4u);

    EXPECT_EQ(loaded->entries[1].mode, LockMode::Set);
    EXPECT_EQ(loaded->entries[1].anchor.kind, AnchorKind::Seglist);
    EXPECT_EQ(loaded->entries[1].anchor.seg, 2);
    EXPECT_EQ(loaded->entries[1].anchor.offset, 0x1A4Cu);
    EXPECT_EQ(loaded->entries[1].value, 0x63u);
    EXPECT_EQ(loaded->entries[1].size, 1);
    // The guard bytes are the whole point of the format - losing them silently would turn a
    // safe entry into a blind poke.
    EXPECT_EQ(loaded->entries[1].verify.ToString(), "53 80");

    EXPECT_EQ(loaded->entries[2].anchor.kind, AnchorKind::Signature);
    EXPECT_EQ(loaded->entries[2].anchor.pattern.ToString(), "53 80 4E B9 ?? ?? ?? ??");
    EXPECT_EQ(loaded->entries[2].anchor.targetOffset, 2);
    EXPECT_EQ(loaded->entries[2].bytes, (std::vector<uint8_t>{0x4E, 0x71}));

    EXPECT_EQ(loaded->entries[3].anchor.kind, AnchorKind::Operand);
    EXPECT_EQ(loaded->entries[3].anchor.operandAt, 2);
    EXPECT_EQ(loaded->entries[3].anchor.operandSize, 4);
}

TEST(TrainerFileTest, LoadReportsMissingAndMalformedFiles)
{
    std::string err;
    EXPECT_FALSE(TrainerFile::Load("trainerfile_does_not_exist.json", err).has_value());
    EXPECT_FALSE(err.empty());

    ScopedFile bad("trainerfile_malformed.json");
    bad.Write("{ this is not json");
    err.clear();
    EXPECT_FALSE(TrainerFile::Load(bad.Path(), err).has_value());
    EXPECT_FALSE(err.empty());
}

TEST(TrainerFileTest, SaveReportsAnUnwritablePath)
{
    TrainerDoc doc;
    doc.entries.push_back(BlockEntry(0x1000, 1, "x"));
    std::string err;
    EXPECT_FALSE(TrainerFile::Save("no_such_dir_here/nested/deeper/t.json", doc, err));
    EXPECT_FALSE(err.empty());
}

// --- .cht import --------------------------------------------------------------------------

TEST(TrainerFileTest, ImportsChtSetToValueCheatsAtEachSearchSize)
{
    ScopedFile file("trainerfile_import.cht");
    file.Write(R"(cheats = 3
cheat0_desc = "Infinite lives"
cheat0_enable = true
cheat0_code = ""
cheat0_address = 4096
cheat0_value = 99
cheat0_cheat_type = 1
cheat0_memory_search_size = 3
cheat1_desc = "Infinite energy"
cheat1_address = 8192
cheat1_value = 4096
cheat1_cheat_type = 1
cheat1_memory_search_size = 4
cheat2_desc = "Score"
cheat2_address = 12288
cheat2_value = 1000000
cheat2_cheat_type = 1
cheat2_memory_search_size = 5
)");

    std::string err;
    int skipped = -1;
    auto doc = TrainerFile::ImportCht(file.Path(), err, &skipped);
    ASSERT_TRUE(doc.has_value()) << err;
    EXPECT_EQ(skipped, 0);
    ASSERT_EQ(doc->entries.size(), 3u);

    // memory_search_size is an enum, not a byte count: 3 = 8-bit, 4 = 16-bit, 5 = 32-bit.
    EXPECT_EQ(doc->entries[0].size, 1);
    EXPECT_EQ(doc->entries[1].size, 2);
    EXPECT_EQ(doc->entries[2].size, 4);

    EXPECT_EQ(doc->entries[0].desc, "Infinite lives");
    EXPECT_EQ(doc->entries[0].anchor.kind, AnchorKind::Absolute);
    EXPECT_EQ(doc->entries[0].anchor.addr, 4096u);
    EXPECT_EQ(doc->entries[0].value, 99u);
    EXPECT_EQ(doc->entries[0].mode, LockMode::Set);

    // `cheat0_enable = true` is deliberately ignored. An imported address carries no guard
    // bytes, so it is exactly as brittle as the .cht was and must be reviewed first.
    for (const auto& e : doc->entries)
        EXPECT_FALSE(e.enabled) << e.desc << " must arrive disabled";
}

TEST(TrainerFileTest, ExpandsChtRepeatCountIntoOneEntryPerStride)
{
    ScopedFile file("trainerfile_repeat.cht");
    file.Write(R"(cheats = 1
cheat0_desc = "All weapons"
cheat0_address = 256
cheat0_value = 1
cheat0_cheat_type = 1
cheat0_memory_search_size = 3
cheat0_repeat_count = 4
cheat0_repeat_add_to_address = 2
cheat0_repeat_add_to_value = 10
)");

    std::string err;
    auto doc = TrainerFile::ImportCht(file.Path(), err, nullptr);
    ASSERT_TRUE(doc.has_value()) << err;
    ASSERT_EQ(doc->entries.size(), 4u);

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(doc->entries[i].anchor.addr, 256u + static_cast<uint32_t>(i) * 2u);
        EXPECT_EQ(doc->entries[i].value, 1u + static_cast<uint32_t>(i) * 10u);
        // Numbered, so a four-way expansion is not four identically-named rows in the table.
        EXPECT_NE(doc->entries[i].desc.find("All weapons ["), std::string::npos);
    }
}

TEST(TrainerFileTest, CountsChtCheatsItCannotExpressInsteadOfDroppingThemSilently)
{
    ScopedFile file("trainerfile_skips.cht");
    file.Write(R"(cheats = 3
cheat0_desc = "Set lives"
cheat0_address = 4096
cheat0_value = 5
cheat0_cheat_type = 1
cheat0_memory_search_size = 3
cheat1_desc = "Increase score each frame"
cheat1_address = 8192
cheat1_value = 1
cheat1_cheat_type = 2
cheat1_memory_search_size = 3
cheat2_desc = "A nibble"
cheat2_address = 12288
cheat2_value = 1
cheat2_cheat_type = 1
cheat2_memory_search_size = 1
)");

    std::string err;
    int skipped = 0;
    auto doc = TrainerFile::ImportCht(file.Path(), err, &skipped);
    ASSERT_TRUE(doc.has_value()) << err;
    // cheat_type 2 (increase) and the sub-byte width have no equivalent here, and the user is
    // told how many went rather than left wondering why three became one.
    EXPECT_EQ(doc->entries.size(), 1u);
    EXPECT_EQ(skipped, 2);
}

TEST(TrainerFileTest, ImportRejectsFilesWithNothingUsable)
{
    ScopedFile notCht("trainerfile_not.cht");
    notCht.Write("this file has no key value pairs at all\n");
    std::string err;
    EXPECT_FALSE(TrainerFile::ImportCht(notCht.Path(), err, nullptr).has_value());
    EXPECT_NE(err.find(".cht"), std::string::npos);

    ScopedFile allSkipped("trainerfile_allskipped.cht");
    allSkipped.Write(R"(cheats = 1
cheat0_desc = "Increase"
cheat0_address = 1
cheat0_value = 1
cheat0_cheat_type = 2
cheat0_memory_search_size = 3
)");
    err.clear();
    int skipped = 0;
    EXPECT_FALSE(TrainerFile::ImportCht(allSkipped.Path(), err, &skipped).has_value());
    EXPECT_EQ(skipped, 1);
    EXPECT_FALSE(err.empty());
}

// --- identity matching --------------------------------------------------------------------

TEST(TrainerFileTest, MatchesMd5IsCaseInsensitiveAndRejectsAnEmptyHash)
{
    TrainerDoc doc;
    doc.identity.md5 = {"aabbccddeeff00112233445566778899"};
    EXPECT_TRUE(TrainerFile::MatchesMd5(doc, "AABBCCDDEEFF00112233445566778899"));
    EXPECT_FALSE(TrainerFile::MatchesMd5(doc, "00000000000000000000000000000000"));
    EXPECT_FALSE(TrainerFile::MatchesMd5(doc, ""));
}

TEST(TrainerFileTest, FindForMediaPrefersTheHashNamedFileWithoutParsingAnything)
{
    ScopedDir dir("trainerfile_find_direct");
    const std::string md5 = "aabbccddeeff00112233445566778899";
    dir.WriteFile(md5 + ".json", R"({"version":1,"entries":[]})");

    TrainerMatch match = TrainerMatch::None;
    const std::string hit = TrainerFile::FindForMedia(dir.Path(), md5, "", match);
    EXPECT_FALSE(hit.empty());
    EXPECT_EQ(match, TrainerMatch::ExactMd5);
}

TEST(TrainerFileTest, FindForMediaPrefersAListedHashOverATitleGroup)
{
    ScopedDir dir("trainerfile_find_priority");
    const std::string md5 = "aabbccddeeff00112233445566778899";
    // Written first, and alphabetically first, so only the priority rule can decide this.
    dir.WriteFile("a_group.json",
                  R"({"version":1,"identity":{"tosecTitleGroupId":"thegame"},"entries":[]})");
    dir.WriteFile("z_exact.json", std::string(R"({"version":1,"identity":{"md5":[")") + md5 +
                                      R"("],"tosecTitleGroupId":"thegame"},"entries":[]})");

    TrainerMatch match = TrainerMatch::None;
    const std::string hit = TrainerFile::FindForMedia(dir.Path(), md5, "thegame", match);
    EXPECT_EQ(match, TrainerMatch::ExactMd5);
    EXPECT_NE(hit.find("z_exact.json"), std::string::npos);
}

TEST(TrainerFileTest, FindForMediaFallsBackToTheTitleGroupAndSaysSo)
{
    ScopedDir dir("trainerfile_find_group");
    dir.WriteFile("other_release.json",
                  R"({"version":1,"identity":{"md5":["ffffffffffffffffffffffffffffffff"],)"
                  R"("tosecTitleGroupId":"thegame"},"entries":[]})");

    TrainerMatch match = TrainerMatch::None;
    const std::string hit =
        TrainerFile::FindForMedia(dir.Path(), "aabbccddeeff00112233445566778899", "thegame", match);
    EXPECT_FALSE(hit.empty());
    // Reported as a weaker match, because nobody has verified this trainer against this crack.
    EXPECT_EQ(match, TrainerMatch::TitleGroup);
}

TEST(TrainerFileTest, FindForMediaReturnsNothingForAnUnknownDiskOrMissingDirectory)
{
    ScopedDir dir("trainerfile_find_none");
    dir.WriteFile("unrelated.json",
                  R"({"version":1,"identity":{"tosecTitleGroupId":"somethingelse"},"entries":[]})");

    TrainerMatch match = TrainerMatch::ExactMd5; // must be reset even on a miss
    EXPECT_TRUE(
        TrainerFile::FindForMedia(dir.Path(), "aabbccddeeff00112233445566778899", "thegame", match)
            .empty());
    EXPECT_EQ(match, TrainerMatch::None);

    match = TrainerMatch::ExactMd5;
    EXPECT_TRUE(
        TrainerFile::FindForMedia("trainerfile_no_such_directory", "aabb", "x", match).empty());
    EXPECT_EQ(match, TrainerMatch::None);
}
