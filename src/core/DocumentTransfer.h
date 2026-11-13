#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "HostDirectoryDocument.h"

namespace adm::core {

struct DiskDocument;
struct FileNode;

/**
 * @brief What a transfer needs from the application around it.
 *
 * Finding the source document by id, reporting a failure (which means raising the error modal)
 * and telling the rest of the app a document changed are all Application's business.
 */
struct DocumentTransferOps
{
    std::function<DiskDocument*(uint32_t docId)> findDocument;
    std::function<void(const std::string& message)> reportError;
    std::function<void(DiskDocument* doc, bool preserveNavigation)> updateDocumentViews;
    std::function<void(DiskDocument* doc)> updateHostDirectoryDocument;
    // The current file tree of a host-directory document — owned by its FileListView on the
    // ui side, so it reaches core through this hook rather than a ../ui include.
    std::function<FileNode(DiskDocument* doc)> getHostDirectoryTree;
};

/**
 * @brief Copying and moving files between two open documents.
 *
 * Drag-and-drop across document windows, and paste. Four kinds of endpoint have to interoperate
 * - an ADF volume or a host directory, on either side - and a directory dragged across brings
 * its whole subtree with it, so this is more than a byte copy: names have to be made unique
 * against the destination, free space has to be checked up front against what the subtree will
 * actually need, and a partial failure has to leave something sensible behind.
 *
 * Extracted from Application.
 */
class DocumentTransfer
{
public:
    void Init(DocumentTransferOps ops);

    /// Move or copy @p entries from the document with @p sourceDocId into @p destParent.
    void TransferEntries(uint32_t sourceDocId, const std::vector<uint32_t>& entries,
                         DiskDocument* destDoc, uint32_t destParent, bool isCopy);

    /// Paste @p sectors from the document with @p sourceDocId. Always a copy.
    void PasteFiles(uint32_t sourceDocId, const std::vector<uint32_t>& sectors,
                    DiskDocument* destDoc, uint32_t destParentSector);

private:
    // A name that does not clash with anything already in the destination directory, by
    // appending a counter the way a desktop file manager would.
    std::string MakeUniqueChildName(DiskDocument* destDoc, uint32_t destParentSector,
                                    const std::string& name);

    /** @brief Recursively copy a source node into destParentSector as newName. */
    bool CopyNodeRecursive(DiskDocument* srcDoc, const FileNode& srcNode, DiskDocument* destDoc,
                           uint32_t destParentSector, const std::string& newName,
                           std::optional<DocumentEntryId> destHostParent = std::nullopt);

    // Blocks the destination will need for @p node and everything under it, so a paste that
    // cannot fit is refused before it has half-written anything.
    uint32_t RequiredBlocksForNode(DiskDocument* destDoc, const FileNode& node);

    DocumentTransferOps m_ops;
};

} // namespace adm::core
