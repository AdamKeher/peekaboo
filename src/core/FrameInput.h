#pragma once

#include <cstdint>
#include <vector>

namespace adm::core {

// Host input resolved for one emulated frame, captured alongside the machine state so a
// recorded frame can be re-executed and land on the same result. Without it a replayed
// frame drifts the moment anything is driving the machine.
//
// This is the input as the core sees it after polling - joypad buttons already merged
// with injected ones, mouse deltas as they will be consumed - not the raw host events.
struct FrameInput
{
    struct KeyEvent
    {
        uint16_t key = 0; // AmigaEmulator::RetroKey
        bool down = false;
    };

    uint16_t joypad[2] = {}; // libretro joypad button bitmask, one per port
    int16_t mouseDeltaX = 0;
    int16_t mouseDeltaY = 0;
    uint8_t mouseButtons = 0;        // bit 0 left, bit 1 right, bit 2 middle
    std::vector<uint16_t> keysDown;  // keys held at the start of the frame
    std::vector<KeyEvent> keyEvents; // key edges delivered during the frame, in order
};

} // namespace adm::core
