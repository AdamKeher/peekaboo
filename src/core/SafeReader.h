#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace adm::core {

/**
 * @brief Bounds-checked big-endian binary reader over a non-owning span.
 *
 * Throws std::out_of_range on any attempt to read past the end.
 */
class SafeReader
{
public:
    explicit SafeReader(std::span<const uint8_t> data) : m_data(data), m_pos(0) {}

    [[nodiscard]] uint32_t ReadU32BE()
    {
        EnsureBytes(4);
        uint32_t val = (static_cast<uint32_t>(m_data[m_pos]) << 24) |
                       (static_cast<uint32_t>(m_data[m_pos + 1]) << 16) |
                       (static_cast<uint32_t>(m_data[m_pos + 2]) << 8) |
                       static_cast<uint32_t>(m_data[m_pos + 3]);
        m_pos += 4;
        return val;
    }

    [[nodiscard]] uint16_t ReadU16BE()
    {
        EnsureBytes(2);
        uint16_t val =
            (static_cast<uint16_t>(m_data[m_pos]) << 8) | static_cast<uint16_t>(m_data[m_pos + 1]);
        m_pos += 2;
        return val;
    }

    [[nodiscard]] uint8_t ReadU8()
    {
        EnsureBytes(1);
        return m_data[m_pos++];
    }

    /**
     * @brief Read a string stored as longwords (4 bytes each).
     * @param longwords Number of longwords. Checked for overflow before multiplication.
     * @return The string (stops at first null byte within the region).
     */
    [[nodiscard]] std::string ReadStringLW(size_t longwords)
    {
        size_t bytes = CheckedMul(longwords, 4);
        EnsureBytes(bytes);
        std::string result;
        for (size_t i = 0; i < bytes && m_data[m_pos + i] != 0; ++i) {
            result += static_cast<char>(m_data[m_pos + i]);
        }
        m_pos += bytes;
        return result;
    }

    /**
     * @brief Read raw bytes into a new vector.
     */
    [[nodiscard]] std::span<const uint8_t> ReadBytes(size_t count)
    {
        EnsureBytes(count);
        auto result = m_data.subspan(m_pos, count);
        m_pos += count;
        return result;
    }

    void Skip(size_t bytes)
    {
        EnsureBytes(bytes);
        m_pos += bytes;
    }

    /**
     * @brief Skip longwords (checked for overflow in multiplication).
     */
    void SkipLW(size_t longwords)
    {
        size_t bytes = CheckedMul(longwords, 4);
        Skip(bytes);
    }

    [[nodiscard]] size_t GetPosition() const { return m_pos; }
    [[nodiscard]] size_t GetRemaining() const { return m_data.size() - m_pos; }
    // m_pos <= size() is an invariant, so the subtraction cannot underflow; the addition-based
    // form (m_pos + count) would wrap for file-derived counts near SIZE_MAX and pass the check.
    [[nodiscard]] bool HasBytes(size_t count) const { return count <= m_data.size() - m_pos; }
    [[nodiscard]] bool AtEnd() const { return m_pos >= m_data.size(); }

private:
    void EnsureBytes(size_t count) const
    {
        if (count > m_data.size() - m_pos) { // subtraction form: m_pos + count could wrap
            throw std::out_of_range("SafeReader: read past end of data");
        }
    }

    static size_t CheckedMul(size_t a, size_t b)
    {
        size_t result;
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_mul_overflow(a, b, &result)) {
            throw std::out_of_range("SafeReader: integer overflow in size calculation");
        }
#else
        if (b != 0 && a > SIZE_MAX / b) {
            throw std::out_of_range("SafeReader: integer overflow in size calculation");
        }
        result = a * b;
#endif
        return result;
    }

    std::span<const uint8_t> m_data;
    size_t m_pos;
};

} // namespace adm::core
