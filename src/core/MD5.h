#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm::core {

class MD5
{
public:
    MD5();
    void Update(const void* data, size_t len);
    void Final();
    std::string HexDigest() const;
    static std::string Compute(const std::string& data);
    static std::string Compute(const std::vector<uint8_t>& data);
    static std::string ComputeFile(const std::string& filePath);

private:
    void Transform(const uint8_t block[64]);
    void Encode(const uint32_t* input, uint8_t* output, size_t length);
    void Decode(const uint8_t* input, uint32_t* output, size_t length);

    uint32_t m_state[4];
    uint32_t m_count[2];
    uint8_t m_buffer[64];
    uint8_t m_digest[16];
    bool m_finished = false;
};

} // namespace adm::core
