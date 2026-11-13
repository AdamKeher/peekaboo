#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace adm::core {

// Reader for the ExoBlock a Crass-engine production's player publishes for its Exocet sync tracks
// (the crass repo's docs/exocet.md, "The live block") - on the disk that ships, not a separate
// build. A program may register it through the Bartman debug gateway as resource type 4, beside
// the PROFILE block's type 3; one that does not is found by its magic. It says where the demo's
// timeline is: the played row and fraction it is evaluating, every track's value this field, the
// arena its exocet.bin lives in (which a push writes), the events it has fired and when each cut
// was ready and made.
//
// Everything is big-endian in m68k gcc layout (2-byte alignment, 32-bit pointers). The registry
// outlives the program that filled it in, so every read is validated from scratch, and any
// inconsistency reports "not present" rather than showing whatever the memory now holds.

struct ExocetFire
{
    uint16_t track = 0; // EXO_* index
    uint16_t row = 0;   // the played row its key is on
    uint16_t frame = 0; // the song frame it was delivered in
};

struct ExocetCut
{
    uint16_t cut = 0; // the director's value: the part cut to
    uint16_t readyRow = 0, readyFrame = 0;
    uint16_t cutRow = 0, cutFrame = 0; // 0xFFFF until it happens
};

// A program that writes the demo's memory map, as the block's owner table names it (version 4).
struct ExocetOwner
{
    char owner = 0; // MemMapEntry.owner
    uint8_t r = 0, g = 0, b = 0;
    std::string name;
};

// The arena's exocet.bin, as far as its header says (it is Peekaboo's to replace on a push).
struct ExocetArenaHeader
{
    bool valid = false; // 'EXO1' and a version this reads
    uint16_t version = 0;
    uint16_t tracks = 0;
    uint32_t manifest = 0;
    uint32_t song = 0; // the first song's trigger digest it was keyed on
    uint32_t main = 0; // the main song's (0: one song): the rows from EXO_SPAN_MAIN are its
    uint16_t rows = 0;
    uint16_t keys = 0;
    uint16_t spans = 0;
    uint16_t frames = 0;
};

// A validated snapshot of the block. `present` false means every other field is at its default.
struct ExocetBlockState
{
    bool present = false;
    uint32_t blockAddress = 0;
    uint16_t version = 0;
    uint16_t tracks = 0;
    uint32_t manifest = 0; // EXO_MANIFEST of the build the program is

    uint16_t row = 0;  // played row being evaluated
    uint16_t frac = 0; // 65536ths into it
    uint16_t frame = 0;
    uint16_t clock = 0;   // 0 the music, 1 the fallback frame clock
    uint32_t enabled = 0; // bit g: group g is evaluated

    uint16_t generation = 0; // bumped by Peekaboo after a push
    uint16_t seen = 0;       // the generation the runtime has loaded
    uint16_t firedCount = 0; // events delivered, ever

    uint32_t valuesAddress = 0;
    std::vector<int32_t> values; // this field's value of every track

    uint32_t arenaAddress = 0;
    uint32_t arenaBytes = 0;
    ExocetArenaHeader arena;

    std::vector<ExocetFire> fired; // the ring's contents, oldest first
    std::vector<ExocetCut> cuts;   // the cuts that have been ready or made

    // Music injection (step 9 of the crass work order): zero where the build does not do it
    // (no kExocetHasMusic).
    uint16_t reseat = 0, reseatSeen = 0, reseatRow = 0;
    uint16_t reseatError = 0; // 0: the last reseat took; else the player's reason
    uint32_t musicAddress = 0, musicBytes = 0;

    // The disk's memory map (core/CrassMemMap.h), 0 where the build has none.
    uint32_t memmapAddress = 0;
    // What the demo is doing: the part on the screen and the
    // part the disk reads or unpacks for, each part << 8 | phase (0 none), and the parts' names,
    // 1.. in order (the table the block points at).
    uint16_t partRun = 0, partLoad = 0;
    std::vector<std::string> partNames;
    // kExocetHas*: the optional sections the demo fills in
    uint16_t flags = 0;
    // The frame rate the song's frames are counted in (50 PAL, 60 NTSC).
    uint16_t fps = 50;
    // The programs that write the memory map, by letter; empty without kExocetHasOwners.
    std::vector<ExocetOwner> owners;
    const ExocetOwner* Owner(char letter) const
    {
        for (const auto& o : owners)
            if (o.owner == letter)
                return &o;
        return nullptr;
    }

    // The push handshake is idle: the runtime has loaded what was last written.
    bool Settled() const { return generation == seen; }
};

using ExocetRead = std::function<bool(uint32_t addr, uint8_t* out, uint32_t len)>;

struct ExocetRam
{
    uint32_t base = 0;
    uint32_t size = 0;
};

inline constexpr uint32_t kExocetBlockMagic = 0x45584F42u; // "EXOB"
// Version 4 (engine/exocet.h EXO_BLOCK_VERSION), 180 bytes: `enabled` a long, and after the parts
// the flags, the frame rate and the owner table's address. No other version is read.
inline constexpr uint16_t kExocetBlockVersion = 4;
inline constexpr uint32_t kExocetBlockSize = 180;
// Why a reseat was refused, in the block's reseat_error (engine/exocet.h EXO_RESEAT_*)
enum ExocetReseat : uint16_t {
    kExocetReseatOk = 0,
    kExocetReseatFile,  // not a music.bin this replayer reads
    kExocetReseatSize,  // its blobs run past the buffer
    kExocetReseatArena, // its schedule wants a bigger chip arena
    kExocetReseatLoads, // more loads than the table has room for
    kExocetReseatCache, // the schedule would write outside the arena
    kExocetReseatRow,   // the song ends before the row
    kExocetReseatReasons
};
// ExoBlock flags (EXO_HAS_*)
inline constexpr uint16_t kExocetHasMusic = 0x0001;
inline constexpr uint16_t kExocetHasMemmap = 0x0002;
inline constexpr uint16_t kExocetHasParts = 0x0004;
inline constexpr uint16_t kExocetHasOwners = 0x0008;
// The phases of a part (engine/exocet.h EXO_PHASE_*): 1 load .. 5 hold.
inline constexpr int kExocetPhases = 5;
inline constexpr uint32_t kExocetBinMagic = 0x45584F31u; // "EXO1"
inline constexpr uint16_t kExocetBinVersion = 3; // 3: the main song's digest (2: a key's sub)
inline constexpr uint32_t kExocetBinHeader = 40; // its header's bytes

inline constexpr int kExocetFired = 16;
inline constexpr int kExocetMaxCuts = 32; // EXO_CUTS: 16, or 32 for a production with more
inline constexpr int kExocetMaxTracks = 255;
inline constexpr uint32_t kExocetMaxArena = 1024 * 1024;

// Offsets inside the block, for the push (which writes the arena and then `generation`).
inline constexpr uint32_t kExocetBlockGenerationOffset = 24;
inline constexpr uint32_t kExocetBlockReseatOffset = 144;    // then reseat_seen
inline constexpr uint32_t kExocetBlockReseatRowOffset = 148; // then reseat_error

// Reads and validates the ExoBlock at `blockAddr`. Returns false - leaving `out` at its default -
// on a bad magic or version, an implausible track count, a pointer outside `ram` or odd, an
// arena that cannot hold even a header, or a cut count past the runtime's.
bool ReadExocetBlock(const ExocetRead& read, const std::vector<ExocetRam>& ram, uint32_t blockAddr,
                     ExocetBlockState& out);

// A RAM bank's bytes as the emulator holds them - big-endian, Amiga order - for the scan below.
struct ExocetHostBank
{
    uint32_t base = 0;
    uint32_t size = 0;
    const uint8_t* bytes = nullptr;
};

// A block no program registered (a disk that ships registers nothing): the first "EXOB" on an
// even address in `banks` that ReadExocetBlock takes AND whose arena holds an exocet.bin. The
// magic is also an immediate in the code that writes it, and that code validates as nothing.
// Returns the block's address with `out` filled in, or 0 with `out` at its default.
uint32_t FindExocetBlock(const ExocetRead& read, const std::vector<ExocetRam>& ram,
                         const std::vector<ExocetHostBank>& banks, ExocetBlockState& out);

} // namespace adm::core
