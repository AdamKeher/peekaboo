#include "UaeSavestate.h"
#include "BigEndian.h"

#include <cstring>
#include <map>
#include <string_view>

namespace adm::core {

namespace {

uint32_t ReadU32(const uint8_t* p) { return be::U32(p); }

uint16_t ReadU16(const uint8_t* p) { return be::U16(p); }

// Sequential big-endian reader over one chunk payload. Reads past the end yield zero and
// latch `ok` false, so a truncated chunk is detected once at the end rather than guarded
// at every field.
class Reader
{
public:
    Reader(const uint8_t* data, size_t len) : m_p(data), m_end(data + len) {}

    uint16_t U16()
    {
        if (m_end - m_p < 2) {
            m_ok = false;
            return 0;
        }
        const uint16_t v = ReadU16(m_p);
        m_p += 2;
        return v;
    }

    uint32_t U32()
    {
        if (m_end - m_p < 4) {
            m_ok = false;
            return 0;
        }
        const uint32_t v = ReadU32(m_p);
        m_p += 4;
        return v;
    }

    uint8_t U8()
    {
        if (m_end - m_p < 1) {
            m_ok = false;
            return 0;
        }
        return *m_p++;
    }

    bool Ok() const { return m_ok; }

private:
    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_ok = true;
};

// One located chunk: name and payload. Payload is empty for chunks we cannot read
// (compressed, or the zero-length END marker).
struct Chunk
{
    std::string_view name;
    const uint8_t* data = nullptr;
    size_t len = 0;
};

/**
 * Walks the ASF chunk list.
 *
 * UAE writes each chunk (savestate.c:save_chunk) as a 4-byte name, a big-endian total
 * length of payload + 12, a 4-byte flags word, the payload, then 1-4 bytes of alignment
 * padding. A total of 12 or less means the reader stops after the length without ever
 * consuming a flags word — that is how the 8-byte `END ` marker is encoded — so it is
 * matched exactly rather than treated as an empty payload of the normal shape.
 *
 * Compressed chunks (flags bit 0) prefix the payload with its uncompressed length. We do
 * not inflate them: none of the chunks we want are compressed, and PUAE writes every
 * compressible one (the RAM images) after the chipset chunks. They are stepped over with
 * the same arithmetic UAE uses so the walk stays aligned regardless.
 */
template <typename Fn> void ForEachChunk(const uint8_t* data, size_t size, Fn&& fn)
{
    size_t pos = 0;
    while (pos + 8 <= size) {
        const std::string_view name(reinterpret_cast<const char*>(data + pos), 4);
        const uint32_t total = ReadU32(data + pos + 4);
        pos += 8;

        if (name == "END ")
            return;
        if (total <= 12)
            continue; // header-only chunk: no flags word, no payload, no padding

        if (pos + 4 > size)
            return;
        const uint32_t flags = ReadU32(data + pos);
        pos += 4;

        // `body` is everything after the flags word; for a compressed chunk its first 4
        // bytes are the uncompressed length and the alignment is computed from what
        // remains, exactly as restore_chunk() does.
        const size_t body = total - 12;
        size_t payload = body;
        const uint8_t* payloadStart = data + pos;
        const bool compressed = (flags & 1) != 0;
        if (compressed) {
            if (body < 4)
                return;
            payload = body - 4;
            payloadStart += 4;
        }
        if (pos + body > size)
            return;

        if (!fn(Chunk{name, compressed ? nullptr : payloadStart, compressed ? 0 : payload}))
            return;

        pos += body;
        pos += 4 - (payload & 3); // UAE always writes 1-4 padding bytes, never 0
    }
}

// One contiguous run of registers in the CHIP chunk. save_custom() emits the DFF000 map in
// strictly ascending offset order, with pointer registers as longwords, so the whole
// layout collapses to a handful of runs. The two gaps (the audio block at $0A0-$0DE and
// the sprite block at $120-$17E) are the ones save_custom(..., full = 0) skips — PUAE's
// savestate always passes full = 0, and those registers come back from the AUDx and SPRx
// chunks instead.
struct RegisterRun
{
    uint16_t first; // first DFF000-relative offset in the run
    uint16_t last;  // last offset, inclusive
    uint8_t width;  // 2 = one word per register, 4 = a longword pair
};

constexpr RegisterRun kChipRuns[] = {
    {0x000, 0x01E, 2}, // BLTDDAT..INTREQR (the read-side mirror block)
    {0x020, 0x020, 4}, // DSKPT
    {0x024, 0x046, 2}, // DSKLEN..BLTALWM
    {0x048, 0x054, 4}, // BLTCPT, BLTBPT, BLTAPT, BLTDPT
    {0x058, 0x07E, 2}, // BLTSIZE..DSKSYNC
    {0x080, 0x084, 4}, // COP1LC, COP2LC
    {0x088, 0x09E, 2}, // ..DIWSTRT/DIWSTOP/DDFSTRT/DDFSTOP/DMACON/CLXCON/INTENA/INTREQ/ADKCON
    {0x0E0, 0x0FC, 4}, // BPL1PT..BPL8PT
    {0x100, 0x11E, 2}, // BPLCON0..BPL8DAT
    {0x180, 0x1FE, 2}, // COLOR00..the FMODE/last-value tail
};

void AppendWord(std::vector<SavestateRegisterWord>& out, uint16_t offset, uint16_t value)
{
    out.push_back({offset, value});
}

// A 32-bit pointer register occupies two consecutive word slots: the high half at the even
// offset, the low half two bytes up. That is the layout GetCustomRegisters() reassembles
// (and why SwapPtrWords() exists), so writing it any other way would break every pointer
// the debugger shows.
void AppendLong(std::vector<SavestateRegisterWord>& out, uint16_t offset, uint32_t value)
{
    AppendWord(out, offset, static_cast<uint16_t>(value >> 16));
    AppendWord(out, static_cast<uint16_t>(offset + 2), static_cast<uint16_t>(value & 0xFFFF));
}

bool ParseChipChunk(const Chunk& c, std::vector<SavestateRegisterWord>& out)
{
    Reader r(c.data, c.len);
    r.U32(); // currprefs.chipset_mask, not a register

    for (const auto& run : kChipRuns) {
        for (uint16_t off = run.first; off <= run.last;
             off = static_cast<uint16_t>(off + run.width))
            if (run.width == 4)
                AppendLong(out, off, r.U32());
            else
                AppendWord(out, off, r.U16());
    }
    return r.Ok();
}

// AUDx: state, volume, intreq2 and a flags byte, then the programmed length/period/data and
// the location pointer. `wlen` rather than `len` is the value software wrote — `len` is the
// live countdown. The trailing pointer, event time and DMA-request position are running
// state with no register of their own.
bool ParseAudioChunk(const Chunk& c, int channel, std::vector<SavestateRegisterWord>& out)
{
    const uint16_t base = static_cast<uint16_t>(0x0A0 + channel * 0x10);
    Reader r(c.data, c.len);

    r.U8(); // state
    const uint8_t vol = r.U8();
    r.U8();  // intreq2
    r.U8();  // dr/dsr flags
    r.U16(); // len (live counter)
    const uint16_t wlen = r.U16();
    const uint16_t per = r.U16();
    const uint16_t dat = r.U16();
    const uint32_t lc = r.U32();

    AppendLong(out, base, lc);                                // AUDxLCH / AUDxLCL
    AppendWord(out, static_cast<uint16_t>(base + 0x4), wlen); // AUDxLEN
    AppendWord(out, static_cast<uint16_t>(base + 0x6), per);  // AUDxPER
    AppendWord(out, static_cast<uint16_t>(base + 0x8), vol);  // AUDxVOL
    AppendWord(out, static_cast<uint16_t>(base + 0xA), dat);  // AUDxDAT
    return r.Ok();
}

// SPRx: the pointer, then POS/CTL and the first of the four buffered data words. Only
// data[0]/datb[0] correspond to the SPRxDATA/SPRxDATB registers; the rest are the AGA
// fetch pipeline.
bool ParseSpriteChunk(const Chunk& c, int sprite, std::vector<SavestateRegisterWord>& out)
{
    Reader r(c.data, c.len);
    const uint32_t pt = r.U32();
    const uint16_t pos = r.U16();
    const uint16_t ctl = r.U16();
    const uint16_t data = r.U16();
    const uint16_t datb = r.U16();

    AppendLong(out, static_cast<uint16_t>(0x120 + sprite * 4), pt);
    const uint16_t base = static_cast<uint16_t>(0x140 + sprite * 8);
    AppendWord(out, base, pos);
    AppendWord(out, static_cast<uint16_t>(base + 2), ctl);
    AppendWord(out, static_cast<uint16_t>(base + 4), data);
    AppendWord(out, static_cast<uint16_t>(base + 6), datb);
    return r.Ok();
}

} // namespace

std::vector<SavestateRegisterWord> ExtractCustomRegisters(const uint8_t* data, size_t size)
{
    std::vector<SavestateRegisterWord> out;
    if (!data || size < 8)
        return out;

    // The first chunk of an AmigaStateFile is always "ASF ". Anything else is not a
    // savestate and walking it would just be reading noise as lengths.
    if (std::string_view(reinterpret_cast<const char*>(data), 4) != "ASF ")
        return out;

    bool sawChip = false;
    bool truncated = false;

    ForEachChunk(data, size, [&](const Chunk& c) {
        if (!c.data)
            return true; // compressed or empty: nothing to read, keep walking

        if (c.name == "CHIP") {
            sawChip = true;
            if (!ParseChipChunk(c, out))
                truncated = true;
        } else if (c.name.substr(0, 3) == "AUD" && c.name[3] >= '0' && c.name[3] <= '3') {
            if (!ParseAudioChunk(c, c.name[3] - '0', out))
                truncated = true;
        } else if (c.name.substr(0, 3) == "SPR" && c.name[3] >= '0' && c.name[3] <= '7') {
            if (!ParseSpriteChunk(c, c.name[3] - '0', out))
                truncated = true;
        }
        return !truncated;
    });

    // The CHIP chunk carries nearly everything; without it there is no useful rebuild, and
    // a half-populated shadow would be a worse lie than the stale one it replaced.
    if (!sawChip || truncated)
        out.clear();
    return out;
}

std::vector<SavestateRamRegion> ExtractRamRegions(const uint8_t* data, size_t size)
{
    std::vector<SavestateRamRegion> out;
    if (!data || size < 4)
        return out;
    if (std::string_view(reinterpret_cast<const char*>(data), 4) != "ASF ")
        return out;

    std::map<std::string, int> boards; // FRAM/ZRAM repeat, one chunk per RAM board

    ForEachChunk(data, size, [&](const Chunk& c) {
        if (!c.data || c.len == 0)
            return true; // compressed or empty: nothing addressable here

        const bool isRam = c.name == "CRAM" || c.name == "BRAM" || c.name == "FRAM" ||
                           c.name == "ZRAM" || c.name == "A3K1" || c.name == "A3K2";
        if (!isRam)
            return true;

        SavestateRamRegion r;
        r.chunk = std::string(c.name);
        r.blobOffset = static_cast<size_t>(c.data - data);
        r.size = c.len;
        r.board = boards[r.chunk]++;
        out.push_back(std::move(r));
        return true;
    });

    return out;
}

} // namespace adm::core
