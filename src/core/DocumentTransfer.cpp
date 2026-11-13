#include "DocumentTransfer.h"

#include <algorithm>
#include <string>
#include <vector>

#include "AdfWrapper.h"
#include "DiskDocument.h"
#include "Logger.h"

namespace adm::core {

void DocumentTransfer::Init(DocumentTransferOps ops) { m_ops = std::move(ops); }

void DocumentTransfer::TransferEntries(uint32_t sourceDocId, const std::vector<uint32_t>& entries,
                                       DiskDocument* destDoc, uint32_t destParent, bool isCopy)
{
    DiskDocument* sourceDoc = m_ops.findDocument(sourceDocId);
    if (!sourceDoc)
        return;
    if (sourceDoc == destDoc && destDoc->kind == core::DocumentKind::DiskImage && isCopy) {
        PasteFiles(sourceDocId, entries, destDoc, destParent);
        return;
    }
    if (destDoc->kind == core::DocumentKind::HostDirectory &&
        (!destDoc->hostDirectory || destDoc->hostDirectory->IsReadOnly())) {
        Logger::Warn("Transfer blocked: destination host directory is read-only");
        return;
    }

    std::function<const FileNode*(const FileNode&, uint32_t)> findNode =
        [&findNode](const FileNode& node, uint32_t id) -> const FileNode* {
        if (static_cast<uint32_t>(node.startSector) == id)
            return &node;
        for (const auto& child : node.children) {
            if (auto* found = findNode(child, id))
                return found;
        }
        return nullptr;
    };
    const FileNode sourceTree = sourceDoc->kind == core::DocumentKind::HostDirectory
                                    ? m_ops.getHostDirectoryTree(sourceDoc)
                                    : sourceDoc->volume.GetFileTree();
    std::vector<const FileNode*> nodes;
    for (uint32_t entry : entries) {
        if (const auto* node = findNode(sourceTree, entry))
            nodes.push_back(node);
    }
    if (nodes.empty())
        return;

    if (destDoc->kind == core::DocumentKind::DiskImage) {
        uint32_t required = 0;
        for (const auto* node : nodes)
            required += RequiredBlocksForNode(destDoc, *node);
        const uint32_t available = destDoc->volume.GetFreeBlocks();
        if (required > available) {
            m_ops.reportError(AdfHandler::InsufficientSpaceMessage(isCopy ? "Copy" : "Move",
                                                                   required, available));
            return;
        }
    }

    std::vector<const FileNode*> copied;
    for (const auto* node : nodes) {
        const std::string destinationName = MakeUniqueChildName(destDoc, destParent, node->name);
        if (CopyNodeRecursive(sourceDoc, *node, destDoc, destParent, destinationName))
            copied.push_back(node);
    }
    if (copied.empty())
        return;

    std::string error;
    if (!isCopy) {
        for (const auto* node : copied) {
            if (sourceDoc->kind == core::DocumentKind::HostDirectory) {
                if (!sourceDoc->hostDirectory->Remove(node->entryId, error))
                    Logger::Error("Transfer: unable to remove '{}': {}", node->name, error);
            } else if (!sourceDoc->volume.DeleteFile(node->parentSector, node->name)) {
                Logger::Error("Transfer: unable to remove '{}' from source ADF", node->name);
            }
        }
    }

    auto refresh = [this](DiskDocument* document) {
        if (document->kind == core::DocumentKind::HostDirectory) {
            std::string refreshError;
            document->hostDirectory->RefreshNow(refreshError);
            m_ops.updateHostDirectoryDocument(document);
        } else {
            m_ops.updateDocumentViews(document, false);
        }
    };
    refresh(destDoc);
    if (!isCopy)
        refresh(sourceDoc);
    Logger::Info("{} {} item(s) between documents", isCopy ? "Copied" : "Moved", copied.size());
}

std::string DocumentTransfer::MakeUniqueChildName(DiskDocument* destDoc, uint32_t destParentSector,
                                                  const std::string& name)
{
    const bool hostDestination = destDoc->kind == core::DocumentKind::HostDirectory;
    constexpr size_t kAmigaMaxNameLen = 30;

    auto nameExists = [&](const std::string& candidate) {
        if (!hostDestination)
            return destDoc->volume.FindFile(destParentSector, candidate) != 0;
        const auto parent = destDoc->hostEntryIds.find(destParentSector);
        if (!destDoc->hostDirectory || parent == destDoc->hostEntryIds.end())
            return true;
        auto foldAscii = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        };
        const std::string foldedCandidate = foldAscii(candidate);
        for (const auto& entry : destDoc->hostDirectory->GetScan().entries) {
            if (entry.parentId == parent->second && foldAscii(entry.name) == foldedCandidate)
                return true;
        }
        return false;
    };

    if (!nameExists(name)) {
        return name;
    }

    // Split base / extension so the suffix is inserted before a trailing ".ext".
    std::string base = name;
    std::string ext;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        base = name.substr(0, dot);
        ext = name.substr(dot);
    }

    for (int n = 1; n < 100000; ++n) {
        std::string suffix = " (" + std::to_string(n) + ")";
        // Truncate the base if needed so the whole name fits within the limit.
        std::string candidateBase = base;
        if (!hostDestination) {
            const size_t reserved = suffix.size() + ext.size();
            const size_t maxBase =
                (kAmigaMaxNameLen > reserved) ? (kAmigaMaxNameLen - reserved) : 0;
            if (candidateBase.size() > maxBase)
                candidateBase = candidateBase.substr(0, maxBase);
        }
        std::string candidate = candidateBase + suffix + ext;
        if (!nameExists(candidate)) {
            return candidate;
        }
    }
    return name; // Should never happen in practice.
}

bool DocumentTransfer::CopyNodeRecursive(DiskDocument* srcDoc, const FileNode& srcNode,
                                         DiskDocument* destDoc, uint32_t destParentSector,
                                         const std::string& newName,
                                         std::optional<DocumentEntryId> destHostParent)
{
    if (destDoc->kind == core::DocumentKind::HostDirectory) {
        if (!destDoc->hostDirectory || destDoc->hostDirectory->IsReadOnly())
            return false;
        if (!destHostParent) {
            auto parent = destDoc->hostEntryIds.find(destParentSector);
            if (parent == destDoc->hostEntryIds.end())
                return false;
            destHostParent = parent->second;
        }
        std::string error;
        if (srcNode.isDir) {
            if (!destDoc->hostDirectory->CreateDirectory(*destHostParent, newName, error)) {
                Logger::Error("Transfer: failed to create host directory '{}': {}", newName, error);
                return false;
            }
            const auto relative = std::filesystem::path(destHostParent->value) / newName;
            auto childParent = core::DocumentEntryId::RelativePath(relative, error);
            if (!childParent)
                return false;
            bool ok = true;
            for (const auto& child : srcNode.children) {
                if (!CopyNodeRecursive(srcDoc, child, destDoc, 0, child.name, childParent))
                    ok = false;
            }
            return ok;
        }

        std::vector<uint8_t> data;
        if (srcDoc->kind == core::DocumentKind::HostDirectory) {
            if (!srcDoc->hostDirectory ||
                !srcDoc->hostDirectory->ReadFile(srcNode.entryId, data, error)) {
                Logger::Error("Transfer: failed to read host file '{}': {}", srcNode.name, error);
                return false;
            }
        } else {
            const std::string content = srcDoc->volume.ReadFileContent(srcNode.startSector);
            if (content.empty() && srcNode.size > 0)
                return false;
            data.assign(content.begin(), content.end());
        }
        if (!destDoc->hostDirectory->CreateHostFile(*destHostParent, newName, data, error)) {
            Logger::Error("Transfer: failed to create host file '{}': {}", newName, error);
            return false;
        }
        return true;
    }

    if (srcNode.isDir) {
        if (!destDoc->volume.CreateDirectory(destParentSector, newName)) {
            Logger::Error("Paste: failed to create directory '{}'", newName);
            return false;
        }
        uint32_t newDirSector = destDoc->volume.FindFile(destParentSector, newName);
        if (newDirSector == 0) {
            Logger::Error("Paste: created directory '{}' but could not locate it", newName);
            return false;
        }
        bool ok = true;
        for (const auto& child : srcNode.children) {
            // Children go into a freshly created directory, so their original
            // names are guaranteed unique there.
            if (!CopyNodeRecursive(srcDoc, child, destDoc, newDirSector, child.name)) {
                ok = false;
            }
        }
        return ok;
    }

    std::vector<uint8_t> data;
    if (srcDoc->kind == core::DocumentKind::HostDirectory) {
        std::string error;
        if (!srcDoc->hostDirectory ||
            !srcDoc->hostDirectory->ReadFile(srcNode.entryId, data, error)) {
            Logger::Error("Transfer: failed to read host file '{}': {}", srcNode.name, error);
            return false;
        }
    } else {
        const std::string content = srcDoc->volume.ReadFileContent(srcNode.startSector);
        if (content.empty() && srcNode.size > 0) {
            Logger::Error("Paste: failed to read '{}' from source", srcNode.name);
            return false;
        }
        data.assign(content.begin(), content.end());
    }
    if (!destDoc->volume.ImportFile(destParentSector, newName, data)) {
        Logger::Error("Paste: failed to write '{}' to destination", newName);
        return false;
    }
    return true;
}

uint32_t DocumentTransfer::RequiredBlocksForNode(DiskDocument* destDoc, const FileNode& node)
{
    if (node.isDir) {
        uint32_t total = 1; // directory header
        for (const auto& child : node.children) {
            total += RequiredBlocksForNode(destDoc, child);
        }
        return total;
    }
    return destDoc->volume.CalculateRequiredBlocks(static_cast<uint64_t>(node.size));
}

void DocumentTransfer::PasteFiles(uint32_t sourceDocId, const std::vector<uint32_t>& sectors,
                                  DiskDocument* destDoc, uint32_t destParentSector)
{
    if (!destDoc || sectors.empty()) {
        return;
    }

    DiskDocument* srcDoc = m_ops.findDocument(sourceDocId);
    if (!srcDoc) {
        Logger::Error("Paste: source document {} not found (was it closed?)", sourceDocId);
        return;
    }

    std::function<const FileNode*(const FileNode&, uint32_t)> findNode =
        [&findNode](const FileNode& node, uint32_t sector) -> const FileNode* {
        if (static_cast<uint32_t>(node.startSector) == sector) {
            return &node;
        }
        for (const auto& child : node.children) {
            if (auto* found = findNode(child, sector)) {
                return found;
            }
        }
        return nullptr;
    };

    // Snapshot the source tree once; reads use original sectors while we write
    // new blocks, so the snapshot stays valid even for same-disk pastes.
    FileNode srcTree = srcDoc->volume.GetFileTree();

    // Resolve every clipboard item up front so we can run a single space check
    // before writing anything (no partial pastes on a full disk).
    std::vector<const FileNode*> srcNodes;
    int failed = 0;
    for (uint32_t sector : sectors) {
        const FileNode* srcNode = findNode(srcTree, sector);
        if (!srcNode) {
            Logger::Error("Paste: source item for sector {} not found", sector);
            failed++;
            continue;
        }
        srcNodes.push_back(srcNode);
    }

    // Sum the blocks the paste will consume on the destination disk.
    uint32_t requiredBlocks = 0;
    for (const FileNode* n : srcNodes) {
        requiredBlocks += RequiredBlocksForNode(destDoc, *n);
    }
    uint32_t freeBlocks = destDoc->volume.GetFreeBlocks();
    if (requiredBlocks > freeBlocks) {
        m_ops.reportError(
            AdfHandler::InsufficientSpaceMessage("Paste", requiredBlocks, freeBlocks));
        Logger::Error("Paste cancelled: required {} blocks, only {} free", requiredBlocks,
                      freeBlocks);
        return;
    }

    int copied = 0;
    for (const FileNode* srcNode : srcNodes) {
        // Only the top-level item needs a clash-free name (e.g. when pasting back
        // into the source directory); its subtree keeps original names.
        std::string uniqueName = MakeUniqueChildName(destDoc, destParentSector, srcNode->name);
        if (CopyNodeRecursive(srcDoc, *srcNode, destDoc, destParentSector, uniqueName)) {
            copied++;
        } else {
            failed++;
        }
    }

    if (copied > 0) {
        Logger::Info("Pasted {} item(s)", copied);
        m_ops.updateDocumentViews(destDoc, false);
    }
    if (failed > 0) {
        Logger::Warn("Paste: {} item(s) failed", failed);
    }
}

} // namespace adm::core
