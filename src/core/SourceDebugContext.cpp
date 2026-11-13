#include "SourceDebugContext.h"

#include "DebugFrame.h"
#include "ElfHunkMap.h"

namespace adm::core {

bool SourceDebugContext::CanUnwind() const
{
    return frames != nullptr && frames->IsLoaded() && (hunkMap != nullptr || !segments.empty());
}

std::optional<uint32_t> SourceDebugContext::ToLink(uint32_t runtimeAddr) const
{
    if (hunkMap && hunkMap->IsRelocated()) {
        return hunkMap->ToLink(runtimeAddr);
    }
    // Without the hunk map the only thing known is where the code hunk landed. That is
    // the assumption the frame-scrubber's function bands already make, and it holds for
    // the .text-relative addresses everything here cares about.
    if (segments.empty()) {
        return std::nullopt;
    }
    const AmigaSegment& code = segments.front();
    if (runtimeAddr < code.base || runtimeAddr >= code.base + code.size) {
        return std::nullopt;
    }
    return runtimeAddr - code.base;
}

std::optional<uint32_t> SourceDebugContext::ToRuntime(uint32_t linkAddr) const
{
    if (hunkMap && hunkMap->IsRelocated()) {
        return hunkMap->ToRuntime(linkAddr);
    }
    if (segments.empty()) {
        return std::nullopt;
    }
    const AmigaSegment& code = segments.front();
    if (linkAddr >= code.size) {
        return std::nullopt;
    }
    return code.base + linkAddr;
}

} // namespace adm::core
