#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "PtvProtocol.h"
#include "SemanticComparator.h"

namespace adm::validation {

struct ValidationCheckpoint
{
    uint16_t id = 0;
    std::string name;
    std::string symbol;
    nlohmann::json expectations;
};

struct ValidationScenario
{
    uint16_t id = 0;
    int phase = 0;
    std::string name;
    std::set<std::string> capabilities;
    std::vector<ValidationCheckpoint> checkpoints;
};

struct ExpectedManifest
{
    uint32_t schemaVersion = 0;
    uint32_t buildId = 0;
    std::string target;
    std::vector<std::string> profiles;
    std::vector<ValidationScenario> scenarios;

    static ExpectedManifest Load(const std::filesystem::path& path);
    static ExpectedManifest Parse(const nlohmann::json& json);
};

struct ValidationCaseResult
{
    enum class Status { Pass, Fail, Skip };
    std::string id;
    std::string name;
    int phase = 0;
    Status status = Status::Fail;
    std::string reason;
    std::string snapshotHash;
    nlohmann::json expected;
    nlohmann::json raw;
    nlohmann::json displayed;
    std::vector<ComparisonFailure> failures;
};

struct ValidationRunResult
{
    std::string profile;
    std::string coreVersion;
    std::string coreSha256;
    std::string romHash;
    uint32_t buildId = 0;
    std::vector<ValidationCaseResult> cases;

    [[nodiscard]] bool Passed() const;
    [[nodiscard]] size_t PassedCount() const;
    [[nodiscard]] size_t FailedCount() const;
    [[nodiscard]] size_t SkippedCount() const;
};

class IValidationMachine
{
public:
    virtual ~IValidationMachine() = default;
    virtual bool ReadMemory(uint32_t address, std::span<uint8_t> output) = 0;
    virtual bool WriteMemory(uint32_t address, std::span<const uint8_t> input) = 0;
    virtual bool PrepareCheckpoint(const std::string& symbol, std::string& error) = 0;
    virtual bool CheckpointReached(std::string& error) = 0;
    virtual void ReleaseCheckpoint() = 0;
    virtual void StepFrame() = 0;
    virtual nlohmann::json CaptureRaw(uint16_t scenario, uint16_t checkpoint) = 0;
    virtual nlohmann::json CaptureDisplayed(uint16_t scenario, uint16_t checkpoint) = 0;
    virtual std::set<std::string> Capabilities() const = 0;
    virtual bool SaveState(std::vector<uint8_t>& output) = 0;
    virtual bool LoadState(std::span<const uint8_t> input) = 0;
    virtual void Reset() = 0;
};

struct CheckpointRunnerOptions
{
    uint32_t mailboxAddress = 0;
    uint32_t expectedBuildId = 0;
    uint32_t frameBudget = 600;
};

class CheckpointRunner
{
public:
    CheckpointRunner(IValidationMachine& machine, CheckpointRunnerOptions options);

    bool WaitForReady(std::string& error);
    bool RunToCheckpoint(uint16_t scenario, const ValidationCheckpoint& checkpoint,
                         PtvMailbox& mailbox, std::string& error);
    bool Continue(std::string& error);
    bool ValidatePayload(const PtvMailbox& mailbox, std::string& error);

private:
    bool ReadMailbox(PtvMailbox& mailbox, std::string& error);
    bool WriteCommand(PtvCommand command, uint16_t scenario, std::string& error);

    IValidationMachine& m_machine;
    CheckpointRunnerOptions m_options;
    uint32_t m_lastCommandSeq = 0;
    uint32_t m_lastCheckpointSeq = 0;
};

class ValidationOrchestrator
{
public:
    ValidationOrchestrator(IValidationMachine& machine, ExpectedManifest manifest,
                           CheckpointRunnerOptions options);

    ValidationRunResult Run(const std::string& profile, int minPhase = 1, int maxPhase = 5,
                            int repetitions = 1);

private:
    IValidationMachine& m_machine;
    ExpectedManifest m_manifest;
    CheckpointRunnerOptions m_options;
};

class ValidationReportWriter
{
public:
    static bool Write(const std::filesystem::path& directory, const ValidationRunResult& result,
                      std::string& error);
};

const char* CaseStatusName(ValidationCaseResult::Status status);

} // namespace adm::validation
