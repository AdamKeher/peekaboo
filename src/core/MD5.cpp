#include "MD5.h"
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace adm::core {

// Constants for MD5Transform routine.
#define S11 7
#define S12 12
#define S13 17
#define S14 22
#define S21 5
#define S22 9
#define S23 14
#define S24 20
#define S31 4
#define S32 11
#define S33 16
#define S34 23
#define S41 6
#define S42 10
#define S43 15
#define S44 21

static uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (((x) & (y)) | ((~x) & (z))); }
static uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (((x) & (z)) | ((y) & (~z))); }
static uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return ((x) ^ (y) ^ (z)); }
static uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return ((y) ^ ((x) | (~z))); }

static uint32_t RotateLeft(uint32_t x, int n) { return (((x) << (n)) | ((x) >> (32 - (n)))); }

static void FF(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac)
{
    (a) += F((b), (c), (d)) + (x) + (ac);
    (a) = RotateLeft((a), (s));
    (a) += (b);
}
static void GG(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac)
{
    (a) += G((b), (c), (d)) + (x) + (ac);
    (a) = RotateLeft((a), (s));
    (a) += (b);
}
static void HH(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac)
{
    (a) += H((b), (c), (d)) + (x) + (ac);
    (a) = RotateLeft((a), (s));
    (a) += (b);
}
static void II(uint32_t& a, uint32_t b, uint32_t c, uint32_t d, uint32_t x, uint32_t s, uint32_t ac)
{
    (a) += I((b), (c), (d)) + (x) + (ac);
    (a) = RotateLeft((a), (s));
    (a) += (b);
}

MD5::MD5()
{
    m_count[0] = m_count[1] = 0;
    m_state[0] = 0x67452301;
    m_state[1] = 0xefcdab89;
    m_state[2] = 0x98badcfe;
    m_state[3] = 0x10325476;
    m_finished = false;
}

void MD5::Update(const void* data, size_t len)
{
    const uint8_t* input = static_cast<const uint8_t*>(data);
    uint32_t i, index, partLen;

    m_finished = false;

    // Compute number of bytes mod 64
    index = (uint32_t)((m_count[0] >> 3) & 0x3F);

    // Update number of bits
    if ((m_count[0] += ((uint32_t)len << 3)) < ((uint32_t)len << 3)) {
        m_count[1]++;
    }
    m_count[1] += ((uint32_t)len >> 29);

    partLen = 64 - index;

    // Transform as many times as possible
    if (len >= partLen) {
        std::memcpy(&m_buffer[index], input, partLen);
        Transform(m_buffer);

        for (i = partLen; i + 63 < len; i += 64) {
            Transform(&input[i]);
        }
        index = 0;
    } else {
        i = 0;
    }

    // Buffer remaining input
    std::memcpy(&m_buffer[index], &input[i], len - i);
}

void MD5::Final()
{
    if (m_finished)
        return;

    uint8_t bits[8];
    uint32_t index, padLen;
    static uint8_t PADDING[64] = {0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    // Save number of bits
    Encode(m_count, bits, 8);

    // Pad out to 56 mod 64
    index = (uint32_t)((m_count[0] >> 3) & 0x3f);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    Update(PADDING, padLen);

    // Append length (before padding)
    Update(bits, 8);

    // Store state in digest
    Encode(m_state, m_digest, 16);

    // Zeroize sensitive information
    std::memset(m_buffer, 0, sizeof(m_buffer));
    std::memset(m_count, 0, sizeof(m_count));

    m_finished = true;
}

std::string MD5::HexDigest() const
{
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        ss << std::setw(2) << (int)m_digest[i];
    }
    return ss.str();
}

std::string MD5::Compute(const std::string& data)
{
    MD5 md5;
    md5.Update(data.c_str(), data.length());
    md5.Final();
    return md5.HexDigest();
}

std::string MD5::Compute(const std::vector<uint8_t>& data)
{
    MD5 md5;
    md5.Update(data.data(), data.size());
    md5.Final();
    return md5.HexDigest();
}

std::string MD5::ComputeFile(const std::string& filePath)
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return "";

    MD5 md5;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        md5.Update(buffer, static_cast<size_t>(file.gcount()));
        if (!file)
            break;
    }
    md5.Final();
    return md5.HexDigest();
}

void MD5::Transform(const uint8_t block[64])
{
    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], x[16];
    Decode(block, x, 64);

    /* Round 1 */
    FF(a, b, c, d, x[0], S11, 0xd76aa478);  /* 1 */
    FF(d, a, b, c, x[1], S12, 0xe8c7b756);  /* 2 */
    FF(c, d, a, b, x[2], S13, 0x242070db);  /* 3 */
    FF(b, c, d, a, x[3], S14, 0xc1bdceee);  /* 4 */
    FF(a, b, c, d, x[4], S11, 0xf57c0faf);  /* 5 */
    FF(d, a, b, c, x[5], S12, 0x4787c62a);  /* 6 */
    FF(c, d, a, b, x[6], S13, 0xa8304613);  /* 7 */
    FF(b, c, d, a, x[7], S14, 0xfd469501);  /* 8 */
    FF(a, b, c, d, x[8], S11, 0x698098d8);  /* 9 */
    FF(d, a, b, c, x[9], S12, 0x8b44f7af);  /* 10 */
    FF(c, d, a, b, x[10], S13, 0xffff5bb1); /* 11 */
    FF(b, c, d, a, x[11], S14, 0x895cd7be); /* 12 */
    FF(a, b, c, d, x[12], S11, 0x6b901122); /* 13 */
    FF(d, a, b, c, x[13], S12, 0xfd987193); /* 14 */
    FF(c, d, a, b, x[14], S13, 0xa679438e); /* 15 */
    FF(b, c, d, a, x[15], S14, 0x49b40821); /* 16 */

    /* Round 2 */
    GG(a, b, c, d, x[1], S21, 0xf61e2562);  /* 17 */
    GG(d, a, b, c, x[6], S22, 0xc040b340);  /* 18 */
    GG(c, d, a, b, x[11], S23, 0x265e5a51); /* 19 */
    GG(b, c, d, a, x[0], S24, 0xe9b6c7aa);  /* 20 */
    GG(a, b, c, d, x[5], S21, 0xd62f105d);  /* 21 */
    GG(d, a, b, c, x[10], S22, 0x02441453); /* 22 */
    GG(c, d, a, b, x[15], S23, 0xd8a1e681); /* 23 */
    GG(b, c, d, a, x[4], S24, 0xe7d3fbc8);  /* 24 */
    GG(a, b, c, d, x[9], S21, 0x21e1cde6);  /* 25 */
    GG(d, a, b, c, x[14], S22, 0xc33707d6); /* 26 */
    GG(c, d, a, b, x[3], S23, 0xf4d50d87);  /* 27 */
    GG(b, c, d, a, x[8], S24, 0x455a14ed);  /* 28 */
    GG(a, b, c, d, x[13], S21, 0xa9e3e905); /* 29 */
    GG(d, a, b, c, x[2], S22, 0xfcefa3f8);  /* 30 */
    GG(c, d, a, b, x[7], S23, 0x676f02d9);  /* 31 */
    GG(b, c, d, a, x[12], S24, 0x8d2a4c8a); /* 32 */

    /* Round 3 */
    HH(a, b, c, d, x[5], S31, 0xfffa3942);  /* 33 */
    HH(d, a, b, c, x[8], S32, 0x8771f681);  /* 34 */
    HH(c, d, a, b, x[11], S33, 0x6d9d6122); /* 35 */
    HH(b, c, d, a, x[14], S34, 0xfde5380c); /* 36 */
    HH(a, b, c, d, x[1], S31, 0xa4beea44);  /* 37 */
    HH(d, a, b, c, x[4], S32, 0x4bdecfa9);  /* 38 */
    HH(c, d, a, b, x[7], S33, 0xf6bb4b60);  /* 39 */
    HH(b, c, d, a, x[10], S34, 0xbebfbc70); /* 40 */
    HH(a, b, c, d, x[13], S31, 0x289b7ec6); /* 41 */
    HH(d, a, b, c, x[0], S32, 0xeaa127fa);  /* 42 */
    HH(c, d, a, b, x[3], S33, 0xd4ef3085);  /* 43 */
    HH(b, c, d, a, x[6], S34, 0x04881d05);  /* 44 */
    HH(a, b, c, d, x[9], S31, 0xd9d4d039);  /* 45 */
    HH(d, a, b, c, x[12], S32, 0xe6db99e5); /* 46 */
    HH(c, d, a, b, x[15], S33, 0x1fa27cf8); /* 47 */
    HH(b, c, d, a, x[2], S34, 0xc4ac5665);  /* 48 */

    /* Round 4 */
    II(a, b, c, d, x[0], S41, 0xf4292244);  /* 49 */
    II(d, a, b, c, x[7], S42, 0x432aff97);  /* 50 */
    II(c, d, a, b, x[14], S43, 0xab9423a7); /* 51 */
    II(b, c, d, a, x[5], S44, 0xfc93a039);  /* 52 */
    II(a, b, c, d, x[12], S41, 0x655b59c3); /* 53 */
    II(d, a, b, c, x[3], S42, 0x8f0ccc92);  /* 54 */
    II(c, d, a, b, x[10], S43, 0xffeff47d); /* 55 */
    II(b, c, d, a, x[1], S44, 0x85845dd1);  /* 56 */
    II(a, b, c, d, x[8], S41, 0x6fa87e4f);  /* 57 */
    II(d, a, b, c, x[15], S42, 0xfe2ce6e0); /* 58 */
    II(c, d, a, b, x[6], S43, 0xa3014314);  /* 59 */
    II(b, c, d, a, x[13], S44, 0x4e0811a1); /* 60 */
    II(a, b, c, d, x[4], S41, 0xf7537e82);  /* 61 */
    II(d, a, b, c, x[11], S42, 0xbd3af235); /* 62 */
    II(c, d, a, b, x[2], S43, 0x2ad7d2bb);  /* 63 */
    II(b, c, d, a, x[9], S44, 0xeb86d391);  /* 64 */

    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;

    // Zeroize sensitive information.
    std::memset(x, 0, sizeof(x));
}

void MD5::Encode(const uint32_t* input, uint8_t* output, size_t length)
{
    for (size_t i = 0, j = 0; j < length; ++i, j += 4) {
        output[j] = (uint8_t)(input[i] & 0xff);
        output[j + 1] = (uint8_t)((input[i] >> 8) & 0xff);
        output[j + 2] = (uint8_t)((input[i] >> 16) & 0xff);
        output[j + 3] = (uint8_t)((input[i] >> 24) & 0xff);
    }
}

void MD5::Decode(const uint8_t* input, uint32_t* output, size_t length)
{
    for (size_t i = 0, j = 0; j < length; ++i, j += 4) {
        output[i] = ((uint32_t)input[j]) | (((uint32_t)input[j + 1]) << 8) |
                    (((uint32_t)input[j + 2]) << 16) | (((uint32_t)input[j + 3]) << 24);
    }
}

} // namespace adm::core
