#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/AmigaEmulator.h"
#include "core/validation/AmigaValidationMachine.h"
#include "core/validation/ValidationSuite.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

using namespace adm::validation;

nlohmann::json Materialize(const nlohmann::json& rule)
{
    if (rule.is_object() && rule.contains("mode")) {
        const std::string mode = rule.value("mode", "exact");
        if (mode == "range")
            return rule.at("min");
        if (mode == "image")
            return rule.at("hash");
        return rule.at("value");
    }
    if (rule.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (const auto& [key, child] : rule.items())
            out[key] = Materialize(child);
        return out;
    }
    if (rule.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& child : rule)
            out.push_back(Materialize(child));
        return out;
    }
    return rule;
}

class FixtureMachine final : public IValidationMachine
{
public:
    FixtureMachine(const ExpectedManifest& manifest, uint32_t mailboxAddress)
        : m_manifest(manifest), m_mailboxAddress(mailboxAddress), m_memory(1024 * 1024)
    {
        for (const auto& scenario : manifest.scenarios)
            for (const auto& checkpoint : scenario.checkpoints)
                m_expectations[{scenario.id, checkpoint.id}] = checkpoint.expectations;
        // This machine exercises lifecycle, manifests, reports and skip handling only. It must
        // never claim target/core capabilities and turn fixture-oracle data into a hardware pass.
        m_capabilities = {"fixture-oracle"};
        Reset();
    }

    bool ReadMemory(uint32_t address, std::span<uint8_t> output) override
    {
        if (address > m_memory.size() || output.size() > m_memory.size() - address)
            return false;
        std::copy_n(m_memory.data() + address, output.size(), output.data());
        return true;
    }
    bool WriteMemory(uint32_t address, std::span<const uint8_t> input) override
    {
        if (address > m_memory.size() || input.size() > m_memory.size() - address)
            return false;
        std::copy(input.begin(), input.end(), m_memory.begin() + address);
        return true;
    }
    bool PrepareCheckpoint(const std::string&, std::string&) override
    {
        m_checkpointReached = false;
        return true;
    }
    bool CheckpointReached(std::string&) override { return m_checkpointReached; }
    void ReleaseCheckpoint() override { m_checkpointReached = false; }
    void StepFrame() override
    {
        std::array<uint8_t, kPtvMailboxSize> bytes{};
        ReadMemory(m_mailboxAddress, bytes);
        auto decoded = DecodeMailbox(bytes);
        if (!decoded.ok)
            return;
        auto mailbox = decoded.mailbox;
        if (mailbox.command == PtvCommand::Run && mailbox.commandSeq != m_seenCommand) {
            m_seenCommand = mailbox.commandSeq;
            mailbox.checkpoint = CheckpointFor(mailbox.scenario);
            ++mailbox.checkpointSeq;
            ++mailbox.guestFrame;
            mailbox.status = PtvStatus::Pass;
            WriteMailbox(mailbox);
            m_checkpointReached = true;
        } else if (mailbox.command == PtvCommand::Continue && mailbox.commandSeq != m_seenCommand) {
            m_seenCommand = mailbox.commandSeq;
            mailbox.command = PtvCommand::None;
            mailbox.status = PtvStatus::Ready;
            WriteMailbox(mailbox);
        }
    }
    nlohmann::json CaptureRaw(uint16_t scenario, uint16_t checkpoint) override
    {
        return Materialize(m_expectations.at({scenario, checkpoint}));
    }
    nlohmann::json CaptureDisplayed(uint16_t scenario, uint16_t checkpoint) override
    {
        return CaptureRaw(scenario, checkpoint);
    }
    std::set<std::string> Capabilities() const override { return m_capabilities; }
    bool SaveState(std::vector<uint8_t>& output) override
    {
        output = m_memory;
        return true;
    }
    bool LoadState(std::span<const uint8_t> input) override
    {
        if (input.size() != m_memory.size())
            return false;
        std::copy(input.begin(), input.end(), m_memory.begin());
        return true;
    }
    void Reset() override
    {
        std::fill(m_memory.begin(), m_memory.end(), 0);
        m_seenCommand = 0;
        m_checkpointReached = false;
        PtvMailbox mailbox;
        mailbox.buildId = m_manifest.buildId;
        mailbox.status = PtvStatus::Ready;
        WriteMailbox(mailbox);
    }

private:
    uint16_t CheckpointFor(uint16_t scenario) const
    {
        auto found = std::find_if(m_manifest.scenarios.begin(), m_manifest.scenarios.end(),
                                  [&](const auto& value) { return value.id == scenario; });
        return found == m_manifest.scenarios.end() || found->checkpoints.empty()
                   ? 0
                   : found->checkpoints.front().id;
    }
    void WriteMailbox(const PtvMailbox& mailbox)
    {
        const auto bytes = EncodeMailbox(mailbox);
        WriteMemory(m_mailboxAddress, bytes);
    }

    const ExpectedManifest& m_manifest;
    uint32_t m_mailboxAddress;
    std::vector<uint8_t> m_memory;
    uint32_t m_seenCommand = 0;
    bool m_checkpointReached = false;
    std::set<std::string> m_capabilities;
    std::map<std::pair<uint16_t, uint16_t>, nlohmann::json> m_expectations;
};

std::string Argument(int argc, char** argv, const std::string& name, const std::string& fallback)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (argv[i] == name)
            return argv[i + 1];
    return fallback;
}

bool HasFlag(int argc, char** argv, const std::string& name)
{
    for (int i = 1; i < argc; ++i)
        if (argv[i] == name)
            return true;
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto manifestPath =
            Argument(argc, argv, "--manifest", "tests/amiga_validation/fixtures/expected.json");
        const auto artifacts = Argument(argc, argv, "--artifacts", "validation-artifacts");
        const auto profile = Argument(argc, argv, "--profile", "a500-pal");
        const int minPhase = std::stoi(Argument(argc, argv, "--min-phase", "1"));
        const int maxPhase = std::stoi(Argument(argc, argv, "--max-phase", "5"));
        const int repetitions = std::stoi(Argument(argc, argv, "--repetitions", "1"));
        const uint32_t frameBudget =
            static_cast<uint32_t>(std::stoul(Argument(argc, argv, "--frame-budget", "600")));

        const auto manifest = ExpectedManifest::Load(manifestPath);
        if (std::find(manifest.profiles.begin(), manifest.profiles.end(), profile) ==
            manifest.profiles.end()) {
            std::cerr << "Unsupported profile: " << profile << "\n";
            return 2;
        }
        const bool real = HasFlag(argc, argv, "--real");
#ifdef _WIN32
        if (real) {
            // Automated validation must report native failures through its exit code; never
            // leave a modal Windows Error Reporting dialog behind in a headless test run.
            SetErrorMode(SetErrorMode(0) | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        }
#endif
        const uint32_t mailboxAddress = real ? kResolvedMailboxAddress : 0x1000;
        std::unique_ptr<IValidationMachine> machine;
        AmigaValidationMachine* realMachine = nullptr;
        if (real) {
            AmigaValidationOptions options;
            options.profile = profile;
            options.corePath =
                Argument(argc, argv, "--core", adm::AmigaEmulator::GetDefaultCorePath());
            options.systemDirectory = Argument(argc, argv, "--system-dir", "emulation");
            options.executablePath = Argument(
                argc, argv, "--target", "tests/amiga_validation/fixtures/peekaboo_validation.exe");
            options.elfPath = Argument(argc, argv, "--elf",
                                       "tests/amiga_validation/fixtures/peekaboo_validation.elf");
            auto concrete = std::make_unique<AmigaValidationMachine>(std::move(options));
            std::string initializeError;
            if (!concrete->Initialize(initializeError)) {
                std::cerr << initializeError << "\n";
                return 2;
            }
            realMachine = concrete.get();
            machine = std::move(concrete);
        } else {
            machine = std::make_unique<FixtureMachine>(manifest, mailboxAddress);
        }
        ValidationOrchestrator orchestrator(
            *machine, manifest, {mailboxAddress, manifest.buildId, real ? frameBudget : 10U});
        auto result = orchestrator.Run(profile, minPhase, maxPhase, repetitions);
        result.coreVersion = realMachine ? realMachine->CoreVersion() : "fixture-machine/1";
        result.coreSha256 = realMachine ? "unavailable" : "synthetic";
        result.romHash = realMachine ? "not-exposed-by-core" : "synthetic";

        std::string error;
        if (!ValidationReportWriter::Write(artifacts, result, error)) {
            std::cerr << error << "\n";
            return 2;
        }
        for (const auto& item : result.cases)
            std::cout << CaseStatusName(item.status) << " " << item.id << " " << item.name
                      << (item.reason.empty() ? "" : ": " + item.reason) << "\n";
        if (realMachine && result.FailedCount() != 0)
            std::cerr << "Relocation diagnostic: " << realMachine->RelocationDiagnostic() << "\n";
        std::cout << result.PassedCount() << " passed, " << result.FailedCount() << " failed, "
                  << result.SkippedCount() << " skipped\n";
        return result.Passed() ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 2;
    }
}
