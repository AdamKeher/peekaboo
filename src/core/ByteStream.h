#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace adm::core {

// Little-endian byte plumbing shared by the on-disk formats that grew out of the frame
// timeline: the frame-history stream and the scenario files that replay against it. Lifted
// verbatim out of FrameHistory.cpp's anonymous namespace when the second reader appeared —
// the encoding is unchanged, and FrameHistory's round-trip tests are what pin that.

inline void PutVarint(std::vector<uint8_t>& out, uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v) | 0x80);
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

inline bool GetVarint(const uint8_t*& p, const uint8_t* end, uint64_t& out)
{
    uint64_t v = 0;
    int shift = 0;
    while (p < end) {
        const uint8_t b = *p++;
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            out = v;
            return true;
        }
        shift += 7;
        if (shift > 63)
            return false;
    }
    return false;
}

template <typename T> void PutLE(std::vector<uint8_t>& out, T v)
{
    for (size_t i = 0; i < sizeof(T); ++i)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline void PutBytes(std::vector<uint8_t>& out, const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

// Sequential reader over a serialized stream. Reads past the end latch `ok` false, so a
// truncated stream is caught once at the end rather than guarded at every field.
class ByteReader
{
public:
    ByteReader(const uint8_t* p, size_t n) : m_p(p), m_end(p + n) {}

    template <typename T> T Get()
    {
        T v = 0;
        if (static_cast<size_t>(m_end - m_p) < sizeof(T)) {
            m_ok = false;
            return 0;
        }
        for (size_t i = 0; i < sizeof(T); ++i)
            v |= static_cast<T>(static_cast<T>(m_p[i]) << (8 * i));
        m_p += sizeof(T);
        return v;
    }

    // Borrows `n` bytes in place; the caller copies out of them.
    const uint8_t* Take(size_t n)
    {
        if (static_cast<size_t>(m_end - m_p) < n) {
            m_ok = false;
            return nullptr;
        }
        const uint8_t* p = m_p;
        m_p += n;
        return p;
    }

    bool Ok() const { return m_ok; }
    size_t Remaining() const { return static_cast<size_t>(m_end - m_p); }

private:
    const uint8_t* m_p;
    const uint8_t* m_end;
    bool m_ok = true;
};

} // namespace adm::core
