#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace adm::validation {

constexpr uint32_t kPtvMagic = 0x50545631; // "PTV1"
constexpr uint16_t kPtvProtocolVersion = 1;
constexpr size_t kPtvMailboxSize = 44;

enum class PtvCommand : uint16_t {
    None = 0,
    Reset = 1,
    Select = 2,
    Run = 3,
    Continue = 4,
    Exit = 5
};
enum class PtvStatus : uint16_t {
    Booting = 0,
    Ready = 1,
    Running = 2,
    Pass = 3,
    GuestFail = 4,
    Done = 5
};

struct PtvMailbox
{
    uint32_t magic = kPtvMagic;
    uint16_t protocol = kPtvProtocolVersion;
    uint16_t size = static_cast<uint16_t>(kPtvMailboxSize);
    uint32_t buildId = 0;
    uint32_t commandSeq = 0;
    PtvCommand command = PtvCommand::None;
    uint16_t scenario = 0;
    uint32_t checkpointSeq = 0;
    uint16_t checkpoint = 0;
    PtvStatus status = PtvStatus::Booting;
    uint32_t guestFrame = 0;
    uint32_t payloadAddr = 0;
    uint32_t payloadSize = 0;
    uint32_t payloadCrc32 = 0;
};

struct MailboxDecodeResult
{
    bool ok = false;
    PtvMailbox mailbox;
    std::string error;
};

MailboxDecodeResult DecodeMailbox(std::span<const uint8_t> bytes);
std::array<uint8_t, kPtvMailboxSize> EncodeMailbox(const PtvMailbox& mailbox);
uint32_t Crc32(std::span<const uint8_t> bytes);

} // namespace adm::validation
