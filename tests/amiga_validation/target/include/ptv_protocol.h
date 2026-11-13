#ifndef PTV_PROTOCOL_H
#define PTV_PROTOCOL_H

#include <stdint.h>

#define PTV_MAGIC    0x50545631UL
#define PTV_PROTOCOL 1

enum PtvCommand {
    PTV_COMMAND_NONE = 0,
    PTV_COMMAND_RESET = 1,
    PTV_COMMAND_SELECT = 2,
    PTV_COMMAND_RUN = 3,
    PTV_COMMAND_CONTINUE = 4,
    PTV_COMMAND_EXIT = 5
};

enum PtvStatus {
    PTV_STATUS_BOOTING = 0,
    PTV_STATUS_READY = 1,
    PTV_STATUS_RUNNING = 2,
    PTV_STATUS_PASS = 3,
    PTV_STATUS_GUEST_FAIL = 4,
    PTV_STATUS_DONE = 5
};

/* The m68k target is big-endian. Keep this layout packed and mirror PtvProtocol.h exactly. */
struct __attribute__((packed, aligned(2))) PtvMailbox
{
    uint32_t magic;
    uint16_t protocol;
    uint16_t size;
    uint32_t build_id;
    uint32_t command_seq;
    uint16_t command;
    uint16_t scenario;
    uint32_t checkpoint_seq;
    uint16_t checkpoint;
    uint16_t status;
    uint32_t guest_frame;
    uint32_t payload_addr;
    uint32_t payload_size;
    uint32_t payload_crc32;
};

#endif
