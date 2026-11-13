#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adm {
namespace core {

// Forward declarations
class AdfHandler;

/**
 * @brief Severity level for validation issues.
 */
enum class ValidationSeverity {
    Success, // Test passed successfully
    Info,    // Informational only
    Warning, // Potential issue, disk may still work
    Error    // Definite problem, disk may be corrupted
};

/**
 * @brief Category of validation issue.
 */
enum class ValidationCategory {
    BootBlock,
    RootBlock,
    Bitmap,
    FileHeader,
    DataBlock,
    DirectoryHash,
    BlockChain,
    Orphan,
    CrossLink
};

/**
 * @brief A single validation issue found on the disk.
 */
struct ValidationIssue
{
    ValidationSeverity severity = ValidationSeverity::Info;
    ValidationCategory category = ValidationCategory::BootBlock;
    uint32_t blockNumber = 0;
    std::string message;
    std::string details;
    bool isRepairable = false;
};

/**
 * @brief Result of disk validation.
 */
struct ValidationResult
{
    bool completed = false;
    int successCount = 0;
    int infoCount = 0;
    int warningCount = 0;
    int errorCount = 0;
    std::vector<ValidationIssue> issues;

    bool HasErrors() const { return errorCount > 0; }
    bool HasWarnings() const { return warningCount > 0; }
    bool IsClean() const { return errorCount == 0 && warningCount == 0; }
};

/**
 * @brief Validates Amiga disk filesystem integrity.
 *
 * Checks for:
 * - Boot block checksum validity
 * - Root block checksum and structure
 * - Bitmap consistency (allocated vs actual usage)
 * - File header chain integrity
 * - Data block chain integrity
 * - Directory hash table validity
 * - Orphaned blocks (allocated but unreferenced)
 * - Cross-linked blocks (referenced by multiple files)
 * - Invalid block pointers
 */
class DiskValidator
{
public:
    /**
     * @brief Validate a mounted disk.
     * @param volume The AdfHandler to validate.
     * @return ValidationResult containing all issues found.
     */
    static ValidationResult Validate(const AdfHandler& volume);

    /**
     * @brief Get a human-readable name for a severity level.
     */
    static std::string GetSeverityName(ValidationSeverity severity);

    /**
     * @brief Get a human-readable name for a category.
     */
    static std::string GetCategoryName(ValidationCategory category);

private:
    static void ValidateBootBlock(const AdfHandler& volume, ValidationResult& result);
    static void ValidateRootBlock(const AdfHandler& volume, ValidationResult& result);
    static void ValidateBitmap(const AdfHandler& volume, ValidationResult& result);
    static void ValidateFileStructure(const AdfHandler& volume, ValidationResult& result);

    static void AddIssue(ValidationResult& result, ValidationSeverity severity,
                         ValidationCategory category, uint32_t block, const std::string& message,
                         const std::string& details = "", bool repairable = false);

    // Helper: read big-endian uint32 from data
    static uint32_t ReadU32(const std::vector<uint8_t>& data, size_t offset);

    // Helper: read big-endian int32 from data
    static int32_t ReadS32(const std::vector<uint8_t>& data, size_t offset);
};

} // namespace core
} // namespace adm
