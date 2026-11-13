#pragma once

#include <string>

namespace adm {
namespace core {

// A remote-release load request, produced by the Peekaboo HTTP server (/api/load, fed by the
// Chrome extension) and consumed by Application.
struct PeekabooLoadEvent
{
    int releaseId = 0;
    std::string releaseName; // optional — may be absent from older server versions
    std::string groupName;   // optional — may be absent from older server versions
    std::string source;      // "demozoo" | "pouet" | "janeway" | "" — origin site
    std::string fileUrl;
    std::string filename; // derived from fileUrl (last path component)
    std::string fileType; // "diskImage" | "executable"; defaults to "diskImage"
};

} // namespace core
} // namespace adm
