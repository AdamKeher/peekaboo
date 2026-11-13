#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace adm::core {

/**
 * @brief One node of an AmigaDOS segment list (a loaded hunk).
 *
 * `base` is the runtime address where the segment's code/data actually lives
 * after AmigaDOS relocated it; `size` is the usable byte length. To turn a
 * runtime program counter into the link-time address stored in the ELF, the
 * caller subtracts the matching segment base (for a single-.text bartman build
 * that is simply ProgramBase()).
 */
struct AmigaSegment
{
    uint32_t base = 0; // runtime start of code/data (segment node + 4)
    uint32_t size = 0; // usable length in bytes (AmigaDOS allocation length, minus 8)
};

/**
 * @brief Recovers the running program's load address(es) by walking AmigaDOS
 *        structures in emulated memory.
 *
 * This is the Stage 2 relocation step. Unlike Bartman's tool — which injects the
 * program with its own loader and therefore knows the seglist directly — PuAE
 * runs LoadSeg as emulated 68k code inside Kickstart, so there is no C hook to
 * capture. Instead we introspect ExecBase -> ThisTask -> pr_CLI -> cli_Module
 * (falling back to pr_SegList) and follow the BPTR chain, exactly as UAE's own
 * debugger does. No PuAE patch or DLL rebuild is required.
 *
 * The class is deliberately decoupled from the emulator: it reads through
 * caller-supplied big-endian memory accessors, so it can be unit-tested against
 * a synthetic memory image.
 */
class SegListResolver
{
public:
    // Returns the 32-bit big-endian value at `addr`; sets ok=false if unmapped.
    using ReadLongFn = std::function<uint32_t(uint32_t addr, bool& ok)>;
    // Returns the byte at `addr`; sets ok=false if unmapped.
    using ReadByteFn = std::function<uint8_t(uint32_t addr, bool& ok)>;

    /**
     * @brief Walk the currently-running process's segment list.
     * @return Segments in load order (index 0 = primary code hunk), or empty if
     *         no AmigaDOS process is running / the structures look invalid.
     */
    static std::vector<AmigaSegment> ResolveActiveProcess(const ReadLongFn& readLong,
                                                          const ReadByteFn& readByte);

    /**
     * @brief Base address of the first (code) segment of the running process.
     * @return The base, or std::nullopt if no process/segment was found.
     *
     * For a single-code-hunk program: linkAddr = runtimePc - *ProgramBase().
     */
    static std::optional<uint32_t> ProgramBase(const ReadLongFn& readLong,
                                               const ReadByteFn& readByte);

    /**
     * @brief Find the running process's segment that contains `pc`.
     * @return The segment, or std::nullopt if pc is outside every segment.
     */
    static std::optional<AmigaSegment>
    FindSegmentContaining(uint32_t pc, const ReadLongFn& readLong, const ReadByteFn& readByte);

private:
    // Guard against corrupt/circular BPTR chains.
    static constexpr int kMaxSegments = 256;
};

} // namespace adm::core
