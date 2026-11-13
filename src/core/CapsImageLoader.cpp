#include "CapsImageLoader.h"

#include "Logger.h"

extern "C" {
#include "CapsAPI.h"
}

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace adm::core {

namespace {

// capsimg entry point signatures we use (subset of CapsLib.h).
typedef SDWORD (*CAPSInit_t)();
typedef SDWORD (*CAPSExit_t)();
typedef SDWORD (*CAPSAddImage_t)();
typedef SDWORD (*CAPSRemImage_t)(SDWORD);
typedef SDWORD (*CAPSLockImageMemory_t)(SDWORD, PUBYTE, UDWORD, UDWORD);
typedef SDWORD (*CAPSUnlockImage_t)(SDWORD);
typedef SDWORD (*CAPSGetImageInfo_t)(PCAPSIMAGEINFO, SDWORD);
typedef SDWORD (*CAPSLockTrack_t)(PVOID, SDWORD, UDWORD, UDWORD, UDWORD);
typedef SDWORD (*CAPSUnlockTrack_t)(SDWORD, UDWORD, UDWORD);

// Candidate plugin locations / names, tried in order. The bundled copy lives in
// emulation/ next to the PuAE core; bare names fall back to the OS search path.
std::vector<std::string> CandidatePaths()
{
#ifdef _WIN32
    return {"./emulation/capsimg.dll", "capsimg.dll"};
#elif defined(__APPLE__)
    // capsimg is commonly distributed as capsimg.so even on macOS (RetroArch
    // naming), so accept both extensions.
    return {"./emulation/capsimg.dylib", "./emulation/capsimg.so", "capsimg.dylib",
            "libcapsimg.dylib", "capsimg.so"};
#else
    return {"./emulation/capsimg.so", "capsimg.so", "libcapsimg.so", "libcapsimg.so.5"};
#endif
}

void* OpenLib(const std::string& path)
{
#ifdef _WIN32
    return (void*)LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
}

void* Sym(void* handle, const char* name)
{
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

void CloseLib(void* handle)
{
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

} // namespace

CapsImageLoader& CapsImageLoader::Instance()
{
    static CapsImageLoader instance;
    return instance;
}

CapsImageLoader::~CapsImageLoader()
{
    if (m_initialised && m_capsExit)
        reinterpret_cast<CAPSExit_t>(m_capsExit)();
    if (m_handle)
        CloseLib(m_handle);
}

bool CapsImageLoader::EnsureLoaded()
{
    if (m_triedLoad)
        return m_handle != nullptr && m_initialised;
    m_triedLoad = true;

    std::string triedList;
    for (const auto& path : CandidatePaths()) {
        if (!triedList.empty())
            triedList += ", ";
        triedList += path;
        m_handle = OpenLib(path);
        if (m_handle) {
            Logger::Info("capsimg: loaded plugin from '{}'", path);
            break;
        }
    }

    if (!m_handle) {
        m_lastError = "capsimg plugin not found (tried: " + triedList +
                      "). Place capsimg.dll / capsimg.so / capsimg.dylib in the "
                      "emulation/ folder to open IPF files.";
        Logger::Warn("{}", m_lastError);
        return false;
    }

    m_capsInit = Sym(m_handle, "CAPSInit");
    m_capsExit = Sym(m_handle, "CAPSExit");
    m_capsAddImage = Sym(m_handle, "CAPSAddImage");
    m_capsRemImage = Sym(m_handle, "CAPSRemImage");
    m_capsLockImageMemory = Sym(m_handle, "CAPSLockImageMemory");
    m_capsUnlockImage = Sym(m_handle, "CAPSUnlockImage");
    m_capsGetImageInfo = Sym(m_handle, "CAPSGetImageInfo");
    m_capsLockTrack = Sym(m_handle, "CAPSLockTrack");
    m_capsUnlockTrack = Sym(m_handle, "CAPSUnlockTrack");

    if (!m_capsInit || !m_capsExit || !m_capsAddImage || !m_capsRemImage ||
        !m_capsLockImageMemory || !m_capsUnlockImage || !m_capsGetImageInfo || !m_capsLockTrack ||
        !m_capsUnlockTrack) {
        m_lastError = "capsimg plugin is missing required symbols; it may be an "
                      "incompatible version.";
        Logger::Warn("{}", m_lastError);
        CloseLib(m_handle);
        m_handle = nullptr;
        return false;
    }

    if (reinterpret_cast<CAPSInit_t>(m_capsInit)() != imgeOk) {
        m_lastError = "capsimg CAPSInit() failed.";
        Logger::Warn("{}", m_lastError);
        CloseLib(m_handle);
        m_handle = nullptr;
        return false;
    }

    m_initialised = true;
    m_lastError.clear();
    return true;
}

bool CapsImageLoader::Available() { return EnsureLoaded(); }

std::optional<CapsImageLoader::DecodedImage>
CapsImageLoader::Decode(const std::vector<uint8_t>& ipfBytes)
{
    if (!EnsureLoaded())
        return std::nullopt;

    const SDWORD id = reinterpret_cast<CAPSAddImage_t>(m_capsAddImage)();
    if (id < 0) {
        m_lastError = "capsimg CAPSAddImage() failed.";
        Logger::Error("{}", m_lastError);
        return std::nullopt;
    }

    auto cleanup = [&]() { reinterpret_cast<CAPSRemImage_t>(m_capsRemImage)(id); };

    // flag 0 => capsimg copies the buffer internally (no DI_LOCK_MEMREF), so we
    // need not keep ipfBytes alive past this call.
    const SDWORD locked = reinterpret_cast<CAPSLockImageMemory_t>(m_capsLockImageMemory)(
        id, const_cast<PUBYTE>(ipfBytes.data()), static_cast<UDWORD>(ipfBytes.size()), 0);
    if (locked != imgeOk) {
        m_lastError =
            "capsimg failed to lock the IPF image (error " + std::to_string(locked) + ").";
        Logger::Error("{}", m_lastError);
        cleanup();
        return std::nullopt;
    }

    CapsImageInfo ii{};
    if (reinterpret_cast<CAPSGetImageInfo_t>(m_capsGetImageInfo)(&ii, id) != imgeOk) {
        m_lastError = "capsimg CAPSGetImageInfo() failed.";
        Logger::Error("{}", m_lastError);
        reinterpret_cast<CAPSUnlockImage_t>(m_capsUnlockImage)(id);
        cleanup();
        return std::nullopt;
    }

    DecodedImage out;
    out.minCylinder = ii.mincylinder;
    out.maxCylinder = ii.maxcylinder;
    out.minHead = ii.minhead;
    out.maxHead = ii.maxhead;

    // Index-aligned MFM with density generation; DI_LOCK_TYPE makes capsimg
    // honour the CapsTrackInfoT2 layout we pass in.
    const UDWORD lockFlags = DI_LOCK_INDEX | DI_LOCK_DENVAR | DI_LOCK_DENAUTO | DI_LOCK_DENNOISE |
                             DI_LOCK_NOISE | DI_LOCK_TYPE;

    auto lockTrack = reinterpret_cast<CAPSLockTrack_t>(m_capsLockTrack);
    auto unlockTrack = reinterpret_cast<CAPSUnlockTrack_t>(m_capsUnlockTrack);

    for (uint32_t cyl = ii.mincylinder; cyl <= ii.maxcylinder; ++cyl) {
        for (uint32_t head = ii.minhead; head <= ii.maxhead; ++head) {
            CapsTrackInfoT2 ti{};
            ti.type = 2; // request the T2 structure layout
            if (lockTrack(&ti, id, cyl, head, lockFlags) != imgeOk)
                continue;

            RawTrack t;
            t.cylinder = cyl;
            t.head = head;
            t.lengthBits = ti.tracklen * 8; // tracklen is bytes (no DI_LOCK_TRKBIT)
            t.weakCount = ti.weakcnt;
            t.trackType = ti.type & CTIT_MASK_TYPE;
            if (ti.trackbuf && ti.tracklen)
                t.mfm.assign(ti.trackbuf, ti.trackbuf + ti.tracklen);
            out.tracks.push_back(std::move(t));

            unlockTrack(id, cyl, head);
        }
    }

    reinterpret_cast<CAPSUnlockImage_t>(m_capsUnlockImage)(id);
    cleanup();

    if (out.tracks.empty()) {
        m_lastError = "capsimg decoded no tracks from the IPF image.";
        Logger::Error("{}", m_lastError);
        return std::nullopt;
    }

    m_lastError.clear();
    return out;
}

} // namespace adm::core
