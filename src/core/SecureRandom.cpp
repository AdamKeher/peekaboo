#include "SecureRandom.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>
#elif defined(__APPLE__)
#include <cstdlib>
#else
#include <fstream>
#endif

namespace adm {
namespace core {

bool SecureRandomBytes(uint8_t* out, size_t count)
{
    if (count == 0)
        return true;
#ifdef _WIN32
    const NTSTATUS status =
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out), static_cast<ULONG>(count),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0;
#elif defined(__APPLE__)
    arc4random_buf(out, count);
    return true;
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom)
        return false;
    urandom.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(count));
    return urandom.gcount() == static_cast<std::streamsize>(count);
#endif
}

std::string SecureRandomHex(size_t byteCount)
{
    static const char* hex = "0123456789abcdef";
    std::string bytes(byteCount, '\0');
    if (!SecureRandomBytes(reinterpret_cast<uint8_t*>(bytes.data()), byteCount))
        return {};
    std::string out;
    out.reserve(byteCount * 2);
    for (unsigned char b : bytes) {
        out += hex[b >> 4];
        out += hex[b & 0x0F];
    }
    return out;
}

} // namespace core
} // namespace adm
