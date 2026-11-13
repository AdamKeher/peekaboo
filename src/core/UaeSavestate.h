#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace adm::core {

// One custom-chip register word recovered from a savestate, keyed by its DFF000-relative
// offset (always even, 0x000-0x1FE).
struct SavestateRegisterWord
{
    uint16_t offset = 0;
    uint16_t value = 0;
};

/**
 * @brief Recovers the custom-chip register file held in a UAE savestate blob.
 *
 * PUAE's `custom_storage[]` — the array `AmigaEmulator::GetCustomRegisters()` reads
 * write-only registers from — is a *write shadow*: `custom_wput_1()` fills it as the
 * emulated program writes registers, and `custom_reset()` clears it. It is not part of
 * the savestate, and `restore_custom()` repopulates UAE's real chipset variables without
 * ever writing back into it. So after a state restore the shadow reads stale or zero and
 * the machine looks like it lost its chipset when in fact only our view of it is wrong.
 *
 * This reads the register values back out of the savestate itself so the shadow can be
 * rebuilt, which keeps the fix entirely host-side: no PUAE patch, no core rebuild.
 *
 * Covers the `CHIP` chunk (everything except the audio and sprite blocks, which
 * `save_custom(..., full = 0)` omits), the `AUD0`-`AUD3` chunks and the `SPR0`-`SPR7`
 * chunks. Registers that hardware only ever reads (DMACONR, VPOSR, JOYxDAT, DENISEID,
 * HHPOSR, ...) are reported too, at their read-side offsets, because a caller rebuilding
 * the shadow wants the whole picture; the shadow itself is indexed by write offset, so
 * they land harmlessly and are overwritten from live reads.
 *
 * Returns an empty vector if the blob is not an AmigaStateFile or the chunk is missing or
 * truncated — a partial rebuild would be worse than none.
 */
std::vector<SavestateRegisterWord> ExtractCustomRegisters(const uint8_t* data, size_t size);

// One RAM image located inside a savestate blob.
struct SavestateRamRegion
{
    std::string chunk;     // ASF chunk name: "CRAM", "BRAM", "FRAM", "ZRAM", ...
    size_t blobOffset = 0; // byte offset of the image within the savestate blob
    size_t size = 0;
    int board = 0; // index among same-named chunks (FRAM/ZRAM can repeat per RAM board)
};

/**
 * @brief Locates the RAM images inside a savestate blob.
 *
 * Needed to turn a byte-level difference between two recorded frames back into an Amiga
 * address. This works only because the libretro build writes save states uncompressed:
 * savestate.c sets `comp = 0` under __LIBRETRO__, so a RAM chunk's payload is the raw bank
 * contents and blobOffset + n is simply the byte at bank address n. A desktop UAE state
 * (comp = 1) would zlib each RAM chunk and none of this would hold.
 *
 * The caller supplies the base address for each chunk - the blob does not record where a
 * bank lives, and the running machine already knows.
 *
 * Returns an empty vector if the blob is not an AmigaStateFile.
 */
std::vector<SavestateRamRegion> ExtractRamRegions(const uint8_t* data, size_t size);

} // namespace adm::core
