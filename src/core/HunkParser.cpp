#include "HunkParser.h"
#include "Logger.h"
#include "SafeReader.h"

#include <cstring>

namespace adm {
namespace core {

// Amiga hunk type constants
namespace HunkTypes {
constexpr uint32_t HUNK_UNIT = 0x3E7;
constexpr uint32_t HUNK_NAME = 0x3E8;
constexpr uint32_t HUNK_CODE = 0x3E9;
constexpr uint32_t HUNK_DATA = 0x3EA;
constexpr uint32_t HUNK_BSS = 0x3EB;
constexpr uint32_t HUNK_RELOC32 = 0x3EC;
constexpr uint32_t HUNK_RELOC16 = 0x3ED;
constexpr uint32_t HUNK_RELOC8 = 0x3EE;
constexpr uint32_t HUNK_EXT = 0x3EF;
constexpr uint32_t HUNK_SYMBOL = 0x3F0;
constexpr uint32_t HUNK_DEBUG = 0x3F1;
constexpr uint32_t HUNK_END = 0x3F2;
constexpr uint32_t HUNK_HEADER = 0x3F3;
constexpr uint32_t HUNK_OVERLAY = 0x3F5;
constexpr uint32_t HUNK_BREAK = 0x3F6;
constexpr uint32_t HUNK_DREL32 = 0x3F7;
constexpr uint32_t HUNK_DREL16 = 0x3F8;
constexpr uint32_t HUNK_DREL8 = 0x3F9;
constexpr uint32_t HUNK_LIB = 0x3FA;
constexpr uint32_t HUNK_INDEX = 0x3FB;
constexpr uint32_t HUNK_RELOC32SHORT = 0x3FC;
constexpr uint32_t HUNK_RELRELOC32 = 0x3FD;
constexpr uint32_t HUNK_ABSRELOC16 = 0x3FE;

// Memory flags
constexpr uint32_t HUNKF_CHIP = 0x40000000;
constexpr uint32_t HUNKF_FAST = 0x80000000;
constexpr uint32_t HUNKF_MEMFLAGS = 0xC0000000;
} // namespace HunkTypes

// SafeReader is now used instead of the old DataReader class

HunkType HunkParser::MapHunkType(uint32_t rawType)
{
    // Mask off memory flags
    uint32_t type = rawType & 0x0FFFFFFF;

    switch (type) {
        case HunkTypes::HUNK_HEADER:
            return HunkType::Header;
        case HunkTypes::HUNK_CODE:
            return HunkType::Code;
        case HunkTypes::HUNK_DATA:
            return HunkType::Data;
        case HunkTypes::HUNK_BSS:
            return HunkType::BSS;
        case HunkTypes::HUNK_RELOC32:
        case HunkTypes::HUNK_RELOC32SHORT:
            return HunkType::Reloc32;
        case HunkTypes::HUNK_RELOC16:
            return HunkType::Reloc16;
        case HunkTypes::HUNK_RELOC8:
            return HunkType::Reloc8;
        case HunkTypes::HUNK_EXT:
            return HunkType::External;
        case HunkTypes::HUNK_SYMBOL:
            return HunkType::Symbol;
        case HunkTypes::HUNK_DEBUG:
            return HunkType::Debug;
        case HunkTypes::HUNK_END:
            return HunkType::End;
        case HunkTypes::HUNK_OVERLAY:
            return HunkType::Overlay;
        case HunkTypes::HUNK_BREAK:
            return HunkType::Break;
        case HunkTypes::HUNK_UNIT:
            return HunkType::Unit;
        case HunkTypes::HUNK_NAME:
            return HunkType::Name;
        case HunkTypes::HUNK_LIB:
            return HunkType::Library;
        case HunkTypes::HUNK_INDEX:
            return HunkType::Index;
        case HunkTypes::HUNK_RELRELOC32:
        case HunkTypes::HUNK_DREL32:
            return HunkType::RelReloc32;
        case HunkTypes::HUNK_ABSRELOC16:
        case HunkTypes::HUNK_DREL16:
            return HunkType::AbsReloc16;
        default:
            return HunkType::Unknown;
    }
}

std::string HunkParser::GetHunkTypeName(HunkType type)
{
    switch (type) {
        case HunkType::Header:
            return "HUNK_HEADER";
        case HunkType::Code:
            return "HUNK_CODE";
        case HunkType::Data:
            return "HUNK_DATA";
        case HunkType::BSS:
            return "HUNK_BSS";
        case HunkType::Reloc32:
            return "HUNK_RELOC32";
        case HunkType::Reloc16:
            return "HUNK_RELOC16";
        case HunkType::Reloc8:
            return "HUNK_RELOC8";
        case HunkType::External:
            return "HUNK_EXT";
        case HunkType::Symbol:
            return "HUNK_SYMBOL";
        case HunkType::Debug:
            return "HUNK_DEBUG";
        case HunkType::End:
            return "HUNK_END";
        case HunkType::Overlay:
            return "HUNK_OVERLAY";
        case HunkType::Break:
            return "HUNK_BREAK";
        case HunkType::Unit:
            return "HUNK_UNIT";
        case HunkType::Name:
            return "HUNK_NAME";
        case HunkType::Library:
            return "HUNK_LIB";
        case HunkType::Index:
            return "HUNK_INDEX";
        case HunkType::RelReloc32:
            return "HUNK_RELRELOC32";
        case HunkType::AbsReloc16:
            return "HUNK_ABSRELOC16";
        default:
            return "UNKNOWN";
    }
}

std::string HunkParser::GetMemoryFlagsString(uint32_t flags)
{
    std::string result;

    if (flags & HunkTypes::HUNKF_CHIP) {
        result = "CHIP";
    }
    if (flags & HunkTypes::HUNKF_FAST) {
        if (!result.empty())
            result += "|";
        result += "FAST";
    }

    // Additional memory flags in bits 30-24
    uint32_t additionalFlags = (flags >> 24) & 0x3F;
    if (additionalFlags) {
        if (!result.empty())
            result += "|";
        result += "0x" + std::to_string(additionalFlags);
    }

    return result.empty() ? "ANY" : result;
}

bool HunkParser::IsAmigaExecutable(const std::vector<uint8_t>& data)
{
    if (data.size() < 4) {
        return false;
    }

    uint32_t magic = (static_cast<uint32_t>(data[0]) << 24) |
                     (static_cast<uint32_t>(data[1]) << 16) |
                     (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);

    return magic == HunkTypes::HUNK_HEADER || magic == HunkTypes::HUNK_UNIT ||
           magic == HunkTypes::HUNK_LIB;
}

ExecutableType HunkParser::GetExecutableType(const std::vector<uint8_t>& data)
{
    if (data.size() < 4) {
        return ExecutableType::Unknown;
    }

    uint32_t magic = (static_cast<uint32_t>(data[0]) << 24) |
                     (static_cast<uint32_t>(data[1]) << 16) |
                     (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);

    switch (magic) {
        case HunkTypes::HUNK_HEADER:
            return ExecutableType::Executable;
        case HunkTypes::HUNK_UNIT:
            return ExecutableType::Object;
        case HunkTypes::HUNK_LIB:
            return ExecutableType::Library;
        default:
            return ExecutableType::Unknown;
    }
}

HunkParseResult HunkParser::Parse(const std::vector<uint8_t>& data)
{
    HunkParseResult result;

    if (data.size() < 8) {
        result.errorMessage = "File too small to be a valid executable";
        return result;
    }

    SafeReader reader(data);

    try {
        // Read first hunk type
        uint32_t firstHunk = reader.ReadU32BE();
        (void)firstHunk;
        result.executableType = GetExecutableType(data);

        switch (result.executableType) {
            case ExecutableType::Executable:
                result.executableTypeName = "Executable";
                break;
            case ExecutableType::Object:
                result.executableTypeName = "Object File";
                break;
            case ExecutableType::Library:
                result.executableTypeName = "Library";
                break;
            default:
                result.errorMessage = "Not a recognized Amiga executable format";
                return result;
        }

        // Parse based on type
        if (result.executableType == ExecutableType::Executable) {
            // Parse HUNK_HEADER
            // Skip resident library names (null-terminated list of longword counts)
            while (!reader.AtEnd()) {
                uint32_t nameLen = reader.ReadU32BE();
                if (nameLen == 0) {
                    break;
                }
                reader.SkipLW(nameLen);
            }

            // Table size (number of hunks)
            uint32_t tableSize = reader.ReadU32BE();
            result.firstLoadHunk = reader.ReadU32BE();
            result.lastLoadHunk = reader.ReadU32BE();
            result.numHunks = result.lastLoadHunk - result.firstLoadHunk + 1;

            // Read hunk sizes
            std::vector<uint32_t> hunkSizes;
            std::vector<uint32_t> hunkMemFlags;

            for (uint32_t i = 0; i < tableSize; ++i) {
                uint32_t sizeAndFlags = reader.ReadU32BE();
                uint32_t size = (sizeAndFlags & 0x3FFFFFFF) * 4; // Size in bytes
                uint32_t memFlags = sizeAndFlags & HunkTypes::HUNKF_MEMFLAGS;
                hunkSizes.push_back(size);
                hunkMemFlags.push_back(memFlags);

                if (memFlags & HunkTypes::HUNKF_CHIP) {
                    result.hasChipMem = true;
                }
                if (memFlags & HunkTypes::HUNKF_FAST) {
                    result.hasFastMem = true;
                }
            }

            // Parse individual hunks
            int currentHunkIndex = 0;

            while (!reader.AtEnd() && reader.HasBytes(4)) {
                uint32_t hunkTypeRaw = reader.ReadU32BE();
                uint32_t hunkType = hunkTypeRaw & 0x0FFFFFFF;
                uint32_t memFlags = hunkTypeRaw & HunkTypes::HUNKF_MEMFLAGS;

                HunkInfo info;
                info.type = MapHunkType(hunkType);
                info.typeName = GetHunkTypeName(info.type);
                info.memoryFlags = memFlags;
                info.dataOffset = static_cast<uint32_t>(reader.GetPosition() - 4);
                info.index = currentHunkIndex;

                switch (hunkType) {
                    case HunkTypes::HUNK_CODE:
                    case HunkTypes::HUNK_DATA: {
                        uint32_t numLongwords = reader.ReadU32BE();
                        info.size = numLongwords * 4;
                        reader.SkipLW(numLongwords);

                        if (hunkType == HunkTypes::HUNK_CODE) {
                            result.totalCodeSize += info.size;
                        } else {
                            result.totalDataSize += info.size;
                        }

                        // Get allocated size from header if available
                        if (currentHunkIndex < static_cast<int>(hunkSizes.size())) {
                            info.allocSize = hunkSizes[currentHunkIndex];
                            info.memoryFlags |= hunkMemFlags[currentHunkIndex];
                        }
                        break;
                    }

                    case HunkTypes::HUNK_BSS: {
                        uint32_t numLongwords = reader.ReadU32BE();
                        info.size = numLongwords * 4;
                        info.allocSize = info.size;
                        result.totalBSSSize += info.size;

                        if (currentHunkIndex < static_cast<int>(hunkSizes.size())) {
                            info.memoryFlags |= hunkMemFlags[currentHunkIndex];
                        }
                        break;
                    }

                    case HunkTypes::HUNK_RELOC32:
                    case HunkTypes::HUNK_RELOC16:
                    case HunkTypes::HUNK_RELOC8:
                    case HunkTypes::HUNK_DREL32:
                    case HunkTypes::HUNK_DREL16:
                    case HunkTypes::HUNK_DREL8: {
                        while (!reader.AtEnd()) {
                            uint32_t numOffsets = reader.ReadU32BE();
                            if (numOffsets == 0) {
                                break;
                            }
                            reader.ReadU32BE(); // targetHunk
                            for (uint32_t i = 0; i < numOffsets; ++i) {
                                uint32_t offset = reader.ReadU32BE();
                                info.relocations.push_back(offset);
                                result.totalRelocations++;
                            }
                        }
                        break;
                    }

                    case HunkTypes::HUNK_RELOC32SHORT: {
                        while (!reader.AtEnd()) {
                            uint32_t numOffsets = reader.ReadU32BE();
                            if (numOffsets == 0) {
                                break;
                            }
                            reader.ReadU32BE(); // targetHunk
                            // Short relocations use 16-bit offsets
                            reader.Skip(static_cast<size_t>(numOffsets) * 2);
                            // Align to longword
                            if (numOffsets & 1) {
                                reader.Skip(2);
                            }
                            result.totalRelocations += numOffsets;
                        }
                        break;
                    }

                    case HunkTypes::HUNK_SYMBOL: {
                        while (!reader.AtEnd()) {
                            uint32_t nameLen = reader.ReadU32BE();
                            if (nameLen == 0) {
                                break;
                            }
                            std::string symName = reader.ReadStringLW(nameLen);
                            reader.ReadU32BE(); // Symbol value
                            info.symbols.push_back(symName);
                            result.totalSymbols++;
                        }
                        break;
                    }

                    case HunkTypes::HUNK_DEBUG: {
                        uint32_t numLongwords = reader.ReadU32BE();
                        info.size = numLongwords * 4;
                        reader.SkipLW(numLongwords);
                        break;
                    }

                    case HunkTypes::HUNK_END:
                        currentHunkIndex++;
                        break;

                    case HunkTypes::HUNK_NAME: {
                        uint32_t nameLen = reader.ReadU32BE();
                        reader.SkipLW(nameLen);
                        break;
                    }

                    case HunkTypes::HUNK_EXT: {
                        while (!reader.AtEnd()) {
                            uint32_t typeAndLen = reader.ReadU32BE();
                            if (typeAndLen == 0) {
                                break;
                            }
                            uint32_t len = typeAndLen & 0x00FFFFFF;
                            reader.SkipLW(len);
                            // Additional data depends on ext type
                            uint32_t extType = (typeAndLen >> 24) & 0xFF;
                            if (extType < 0x80) {
                                reader.ReadU32BE(); // Value
                            } else {
                                uint32_t numRefs = reader.ReadU32BE();
                                reader.SkipLW(numRefs);
                            }
                        }
                        break;
                    }

                    default:
                        // Unknown hunk type - try to skip if it has size
                        break;
                }

                // Only add significant hunks to the list
                if (info.type != HunkType::End && info.type != HunkType::Unknown) {
                    result.hunks.push_back(info);
                }
            }
        } else if (result.executableType == ExecutableType::Object ||
                   result.executableType == ExecutableType::Library) {
            // Basic parsing for object/library files

            HunkInfo headerInfo;
            headerInfo.type = result.executableType == ExecutableType::Object ? HunkType::Unit
                                                                              : HunkType::Library;
            headerInfo.typeName = GetHunkTypeName(headerInfo.type);
            headerInfo.dataOffset = 0;
            headerInfo.index = 0;
            result.hunks.push_back(headerInfo);

            // For libraries/objects, count hunks
            while (!reader.AtEnd() && reader.HasBytes(4)) {
                uint32_t hunkType = reader.ReadU32BE() & 0x0FFFFFFF;

                HunkInfo info;
                info.type = MapHunkType(hunkType);
                info.typeName = GetHunkTypeName(info.type);
                info.dataOffset = static_cast<uint32_t>(reader.GetPosition() - 4);
                info.index = static_cast<int>(result.hunks.size());

                // Skip based on hunk type
                switch (hunkType) {
                    case HunkTypes::HUNK_UNIT:
                    case HunkTypes::HUNK_NAME: {
                        uint32_t len = reader.ReadU32BE();
                        reader.SkipLW(len);
                        break;
                    }
                    case HunkTypes::HUNK_CODE:
                    case HunkTypes::HUNK_DATA:
                    case HunkTypes::HUNK_DEBUG: {
                        uint32_t len = reader.ReadU32BE();
                        info.size = len * 4;
                        reader.SkipLW(len);
                        if (hunkType == HunkTypes::HUNK_CODE) {
                            result.totalCodeSize += info.size;
                        } else if (hunkType == HunkTypes::HUNK_DATA) {
                            result.totalDataSize += info.size;
                        }
                        break;
                    }
                    case HunkTypes::HUNK_BSS: {
                        uint32_t len = reader.ReadU32BE();
                        info.size = len * 4;
                        result.totalBSSSize += info.size;
                        break;
                    }
                    case HunkTypes::HUNK_END:
                        result.numHunks++;
                        break;
                    default:
                        break;
                }

                if (info.type != HunkType::End && info.type != HunkType::Unknown) {
                    result.hunks.push_back(info);
                }
            }
        }

        result.success = true;
    } catch (const std::out_of_range&) {
        result.success = false;
        result.errorMessage = "Truncated or corrupted executable";
    }

    return result;
}

ResidentType HunkParser::DetectResidentType(const std::vector<uint8_t>& data)
{
    // RomTag/Resident structure:
    // Offset 0:  UWORD rt_MatchWord = 0x4AFC (RTC_MATCHWORD)
    // Offset 2:  struct Resident* rt_MatchTag (pointer to self)
    // Offset 6:  APTR rt_EndSkip
    // Offset 10: UBYTE rt_Flags
    // Offset 11: UBYTE rt_Version
    // Offset 12: UBYTE rt_Type (NT_LIBRARY=9, NT_DEVICE=3, etc)
    // Offset 13: BYTE rt_Pri
    // ...

    constexpr uint16_t RTC_MATCHWORD = 0x4AFC;

    // Scan for RomTag marker - it can appear anywhere in the code/data hunks
    // Usually within the first few KB of the executable
    size_t searchLimit = std::min(data.size(), static_cast<size_t>(32768));

    for (size_t i = 0; i + 14 < searchLimit; i += 2) { // Word-aligned search
        // Check for RTC_MATCHWORD (0x4AFC)
        uint16_t word = (static_cast<uint16_t>(data[i]) << 8) | static_cast<uint16_t>(data[i + 1]);

        if (word == RTC_MATCHWORD) {
            // Found potential RomTag, verify it's valid by checking structure
            // The rt_MatchTag pointer should point back to this location
            // (We can't fully verify without knowing the load address, but we can
            // check that the type byte is reasonable)

            // Get rt_Type at offset 12 from the match word
            if (i + 12 < data.size()) {
                uint8_t rtType = data[i + 12];

                // Valid node types are 0-15, with common ones being:
                // NT_DEVICE=3, NT_HANDLER=5, NT_RESOURCE=8, NT_LIBRARY=9
                switch (rtType) {
                    case 1:
                        return ResidentType::Task;
                    case 2:
                        return ResidentType::Interrupt;
                    case 3:
                        return ResidentType::Device;
                    case 4:
                        return ResidentType::MsgPort;
                    case 5:
                        return ResidentType::Handler;
                    case 6:
                        return ResidentType::SoftInt;
                    case 8:
                        return ResidentType::Resource;
                    case 9:
                        return ResidentType::Library;
                    case 10:
                        return ResidentType::Memory;
                    case 11:
                        return ResidentType::SoftLink;
                    case 15:
                        return ResidentType::Semaphore;
                    default:
                        if (rtType > 0 && rtType < 16) {
                            return ResidentType::Unknown;
                        }
                        // Not a valid type, keep searching
                        break;
                }
            }
        }
    }

    return ResidentType::None;
}

std::string HunkParser::GetResidentTypeName(ResidentType type)
{
    switch (type) {
        case ResidentType::None:
            return "";
        case ResidentType::Task:
            return "Task";
        case ResidentType::Interrupt:
            return "Interrupt";
        case ResidentType::Device:
            return "Device";
        case ResidentType::MsgPort:
            return "MsgPort";
        case ResidentType::Handler:
            return "Handler";
        case ResidentType::SoftInt:
            return "SoftInt";
        case ResidentType::Resource:
            return "Resource";
        case ResidentType::Library:
            return "Library";
        case ResidentType::Memory:
            return "Memory";
        case ResidentType::SoftLink:
            return "SoftLink";
        case ResidentType::Semaphore:
            return "Semaphore";
        case ResidentType::Unknown:
        default:
            return "Unknown";
    }
}

} // namespace core
} // namespace adm
