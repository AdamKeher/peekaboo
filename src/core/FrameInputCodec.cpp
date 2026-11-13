#include "FrameInputCodec.h"

namespace adm::core {

void WriteFrameInput(std::vector<uint8_t>& out, const FrameInput& in)
{
    PutLE<uint16_t>(out, in.joypad[0]);
    PutLE<uint16_t>(out, in.joypad[1]);
    PutLE<uint16_t>(out, static_cast<uint16_t>(in.mouseDeltaX));
    PutLE<uint16_t>(out, static_cast<uint16_t>(in.mouseDeltaY));
    PutLE<uint8_t>(out, in.mouseButtons);

    PutLE<uint32_t>(out, static_cast<uint32_t>(in.keysDown.size()));
    for (uint16_t k : in.keysDown)
        PutLE<uint16_t>(out, k);

    PutLE<uint32_t>(out, static_cast<uint32_t>(in.keyEvents.size()));
    for (const auto& ev : in.keyEvents) {
        PutLE<uint16_t>(out, ev.key);
        PutLE<uint8_t>(out, ev.down ? 1u : 0u);
    }
}

bool ReadFrameInput(ByteReader& r, FrameInput& out)
{
    out.joypad[0] = r.Get<uint16_t>();
    out.joypad[1] = r.Get<uint16_t>();
    out.mouseDeltaX = static_cast<int16_t>(r.Get<uint16_t>());
    out.mouseDeltaY = static_cast<int16_t>(r.Get<uint16_t>());
    out.mouseButtons = r.Get<uint8_t>();

    const uint32_t keys = r.Get<uint32_t>();
    if (!r.Ok() || keys > r.Remaining() / sizeof(uint16_t))
        return false;
    out.keysDown.clear();
    out.keysDown.reserve(keys);
    for (uint32_t k = 0; k < keys; ++k)
        out.keysDown.push_back(r.Get<uint16_t>());

    const uint32_t events = r.Get<uint32_t>();
    if (!r.Ok() || events > r.Remaining() / 3)
        return false;
    out.keyEvents.clear();
    out.keyEvents.reserve(events);
    for (uint32_t k = 0; k < events; ++k) {
        FrameInput::KeyEvent ev;
        ev.key = r.Get<uint16_t>();
        ev.down = r.Get<uint8_t>() != 0;
        out.keyEvents.push_back(ev);
    }

    return r.Ok();
}

} // namespace adm::core
