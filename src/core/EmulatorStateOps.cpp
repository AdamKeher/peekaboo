#include "EmulatorStateOps.h"

#include "AmigaEmulator.h"

namespace adm::core {

namespace {

StateOpResult Fail(std::string error)
{
    StateOpResult r;
    r.error = std::move(error);
    return r;
}

// Both operations need a live machine and a name the store can turn into a file.
std::string CheckPreconditions(AmigaEmulator& emu, const std::string& name)
{
    if (!EmulatorStateStore::IsValidName(name))
        return "name must be 1-64 characters of letters, digits, '-', '_' or '.'";
    if (!emu.IsGameLoaded())
        return "emulator not running";
    return {};
}

} // namespace

StateOpResult SaveEmulatorState(AmigaEmulator& emu, EmulatorStateStore& store,
                                const std::string& name, const std::string& document, bool persist)
{
    if (auto err = CheckPreconditions(emu, name); !err.empty())
        return Fail(std::move(err));

    std::vector<uint8_t> data = emu.SaveStateForRestore();
    if (data.empty())
        return Fail("core produced no snapshot (retro_serialize unsupported or failed)");

    std::string saveError;
    const bool wrote = store.Save(name, std::move(data), emu.GetTotalFrameCount(),
                                  emu.GetCoreVersion(), document, persist, saveError);

    StateOpResult r;
    r.ok = true;
    if (auto info = store.Info(name))
        r.info = *info;
    if (persist && !wrote)
        r.warning = "kept in memory only: " + saveError;
    return r;
}

StateOpResult LoadEmulatorState(AmigaEmulator& emu, EmulatorStateStore& store,
                                const std::string& name, const std::string& document)
{
    if (auto err = CheckPreconditions(emu, name); !err.empty())
        return Fail(std::move(err));

    EmulatorStateStore::StateInfo info;
    auto data = store.Load(name, info);
    if (!data)
        return Fail("no snapshot named '" + name + "'");

    // A snapshot is only meaningful for the core build and disk it was taken on; refusing
    // here gives a readable error instead of an unexplainably broken machine.
    const std::string coreVersion = emu.GetCoreVersion();
    if (!info.coreVersion.empty() && !coreVersion.empty() && info.coreVersion != coreVersion)
        return Fail("snapshot was taken on core " + info.coreVersion + ", this is " + coreVersion);
    if (!info.document.empty() && !document.empty() && info.document != document)
        return Fail("snapshot was taken with '" + info.document + "' mounted, this is '" +
                    document + "'");

    if (!emu.LoadState(*data))
        return Fail("core rejected the snapshot (size mismatch or incompatible state)");

    StateOpResult r;
    r.ok = true;
    r.info = info;
    r.info.bytes = data->size();
    return r;
}

} // namespace adm::core
