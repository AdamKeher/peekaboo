#include "PtvProtocol.h"

#include <limits>

namespace adm::validation {
namespace {

uint16_t Read16(std::span<const uint8_t> data, size_t offset)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
}

uint32_t Read32(std::span<const uint8_t> data, size_t offset)
{
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
}

void Write16(std::span<uint8_t> data, size_t offset, uint16_t value)
{
    data[offset] = static_cast<uint8_t>(value >> 8);
    data[offset + 1] = static_cast<uint8_t>(value);
}

void Write32(std::span<uint8_t> data, size_t offset, uint32_t value)
{
    data[offset] = static_cast<uint8_t>(value >> 24);
    data[offset + 1] = static_cast<uint8_t>(value >> 16);
    data[offset + 2] = static_cast<uint8_t>(value >> 8);
    data[offset + 3] = static_cast<uint8_t>(value);
}

} // namespace

MailboxDecodeResult DecodeMailbox(std::span<const uint8_t> bytes)
{
    MailboxDecodeResult result;
    if (bytes.size() < kPtvMailboxSize) {
        result.error = "mailbox is truncated";
        return result;
    }

    auto& out = result.mailbox;
    out.magic = Read32(bytes, 0);
    out.protocol = Read16(bytes, 4);
    out.size = Read16(bytes, 6);
    out.buildId = Read32(bytes, 8);
    out.commandSeq = Read32(bytes, 12);
    out.command = static_cast<PtvCommand>(Read16(bytes, 16));
    out.scenario = Read16(bytes, 18);
    out.checkpointSeq = Read32(bytes, 20);
    out.checkpoint = Read16(bytes, 24);
    out.status = static_cast<PtvStatus>(Read16(bytes, 26));
    out.guestFrame = Read32(bytes, 28);
    out.payloadAddr = Read32(bytes, 32);
    out.payloadSize = Read32(bytes, 36);

    out.payloadCrc32 = Read32(bytes, 40);

    if (out.magic != kPtvMagic) {
        result.error = "mailbox magic is not PTV1";
        return result;
    }
    if (out.protocol != kPtvProtocolVersion) {
        result.error = "unsupported mailbox protocol " + std::to_string(out.protocol);
        return result;
    }
    if (out.size != kPtvMailboxSize) {
        result.error = "unsupported mailbox size " + std::to_string(out.size);
        return result;
    }
    if (out.payloadSize > std::numeric_limits<uint32_t>::max() - out.payloadAddr) {
        result.error = "payload address range overflows";
        return result;
    }
    result.ok = true;
    return result;
}

std::array<uint8_t, kPtvMailboxSize> EncodeMailbox(const PtvMailbox& mailbox)
{
    std::array<uint8_t, kPtvMailboxSize> out{};
    Write32(out, 0, mailbox.magic);
    Write16(out, 4, mailbox.protocol);
    Write16(out, 6, mailbox.size);
    Write32(out, 8, mailbox.buildId);
    Write32(out, 12, mailbox.commandSeq);
    Write16(out, 16, static_cast<uint16_t>(mailbox.command));
    Write16(out, 18, mailbox.scenario);
    Write32(out, 20, mailbox.checkpointSeq);
    Write16(out, 24, mailbox.checkpoint);
    Write16(out, 26, static_cast<uint16_t>(mailbox.status));
    Write32(out, 28, mailbox.guestFrame);
    Write32(out, 32, mailbox.payloadAddr);
    Write32(out, 36, mailbox.payloadSize);
    Write32(out, 40, mailbox.payloadCrc32);
    return out;
}

uint32_t Crc32(std::span<const uint8_t> bytes)
{
    uint32_t crc = 0xffffffffU;
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

} // namespace adm::validation
