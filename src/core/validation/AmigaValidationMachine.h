#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "ElfSymbolResolver.h"
#include "ValidationSuite.h"

namespace adm {
class AmigaEmulator;
}

namespace adm::validation {

inline constexpr uint32_t kResolvedMailboxAddress = 0xffffffffU;

struct AmigaValidationOptions
{
    std::filesystem::path corePath;
    std::filesystem::path systemDirectory;
    std::filesystem::path executablePath;
    std::filesystem::path elfPath;
    std::string profile = "a500-pal";
};

class AmigaValidationMachine final : public IValidationMachine
{
public:
    explicit AmigaValidationMachine(AmigaValidationOptions options);
    ~AmigaValidationMachine() override;

    bool Initialize(std::string& error);

    bool ReadMemory(uint32_t address, std::span<uint8_t> output) override;
    bool WriteMemory(uint32_t address, std::span<const uint8_t> input) override;
    bool PrepareCheckpoint(const std::string& symbol, std::string& error) override;
    bool CheckpointReached(std::string& error) override;
    void ReleaseCheckpoint() override;
    void StepFrame() override;
    nlohmann::json CaptureRaw(uint16_t scenario, uint16_t checkpoint) override;
    nlohmann::json CaptureDisplayed(uint16_t scenario, uint16_t checkpoint) override;
    std::set<std::string> Capabilities() const override;
    bool SaveState(std::vector<uint8_t>& output) override;
    bool LoadState(std::span<const uint8_t> input) override;
    void Reset() override;

    [[nodiscard]] const std::string& CoreVersion() const;
    [[nodiscard]] const std::string& LastError() const { return m_lastError; }
    [[nodiscard]] std::string RelocationDiagnostic() const;
    [[nodiscard]] std::optional<uint32_t> ResolveSymbol(const std::string& name) const;
    [[nodiscard]] std::string NormalizeAddress(uint32_t address) const;

private:
    std::vector<RuntimeSegment> RuntimeSegments() const;
    uint32_t ResolveAddress(uint32_t address) const;
    bool Launch(std::string& error);
    nlohmann::json CaptureCpu(bool displayed) const;
    nlohmann::json CaptureCopper(bool displayed) const;
    nlohmann::json CaptureBlitter(bool displayed) const;
    nlohmann::json CaptureTimeline(bool displayed) const;
    nlohmann::json CaptureCia(bool displayed) const;

    AmigaValidationOptions m_options;
    std::unique_ptr<AmigaEmulator> m_emulator;
    ElfSymbolResolver m_symbols;
    std::optional<uint32_t> m_armedAddress;
    bool m_breakHit = false;
    int m_breakStatus = 0;
    uint32_t m_breakStopPc = 0;
    uint32_t m_breakExecuted = 0;
    bool m_firstReset = true;
    std::string m_lastError;
};

} // namespace adm::validation
