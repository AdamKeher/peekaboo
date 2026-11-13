#pragma once

#include <string>

namespace adm::core {

// Console-facing scenario control: SCEN REC / STOP / PLAY / STATUS. Shaped like
// AudioRecordRequest next door - one request struct, one result, one host hook - so the
// interpreter stays free of anything that knows what an emulator is.
struct ScenarioRequest
{
    enum class Verb {
        Status,
        Record,
        Stop,
        Play,
    };

    Verb verb = Verb::Status;
    std::string path;
    // Record from a save-state seed (the default) or as a cold boot. A cold-boot recording
    // only replays correctly from the machine's own start, so it is for scenarios that are
    // going to be replayed from a fresh boot of the same media.
    bool coldBoot = false;
    std::string notes;
};

struct ScenarioResult
{
    bool ok = false;
    std::string message;
};

} // namespace adm::core
