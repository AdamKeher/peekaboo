#include "ValidationSuite.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace adm::validation {
namespace {

std::string EscapeXml(const std::string& value)
{
    std::string out;
    for (char c : value) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else if (c == '\"')
            out += "&quot;";
        else
            out += c;
    }
    return out;
}

nlohmann::json FailureJson(const ComparisonFailure& failure)
{
    return {{"layer", SemanticComparator::LayerName(failure.layer)},
            {"path", failure.path},
            {"message", failure.message},
            {"expected", failure.expected},
            {"actual", failure.actual}};
}

nlohmann::json ResultJson(const ValidationRunResult& result)
{
    nlohmann::json cases = nlohmann::json::array();
    std::set<std::string> skippedCapabilities;
    for (const auto& item : result.cases) {
        nlohmann::json failures = nlohmann::json::array();
        for (const auto& failure : item.failures)
            failures.push_back(FailureJson(failure));
        cases.push_back({{"id", item.id},
                         {"name", item.name},
                         {"phase", item.phase},
                         {"status", CaseStatusName(item.status)},
                         {"reason", item.reason},
                         {"snapshotHash", item.snapshotHash},
                         {"expected", item.expected},
                         {"raw", item.raw},
                         {"displayed", item.displayed},
                         {"failures", failures}});
        if (item.status == ValidationCaseResult::Status::Skip &&
            item.reason.starts_with("missing capabilities: ")) {
            std::istringstream stream(item.reason.substr(22));
            std::string capability;
            while (std::getline(stream, capability, ',')) {
                if (!capability.empty() && capability.front() == ' ')
                    capability.erase(capability.begin());
                skippedCapabilities.insert(capability);
            }
        }
    }
    return {{"profile", result.profile},
            {"coreVersion", result.coreVersion},
            {"coreSha256", result.coreSha256},
            {"romHash", result.romHash},
            {"buildId", result.buildId},
            {"summary",
             {{"passed", result.PassedCount()},
              {"failed", result.FailedCount()},
              {"skipped", result.SkippedCount()}}},
            {"skippedCapabilities", skippedCapabilities},
            {"cases", cases}};
}

} // namespace

ExpectedManifest ExpectedManifest::Load(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open expected manifest: " + path.string());
    nlohmann::json json;
    input >> json;
    return Parse(json);
}

ExpectedManifest ExpectedManifest::Parse(const nlohmann::json& json)
{
    ExpectedManifest out;
    out.schemaVersion = json.at("schemaVersion").get<uint32_t>();
    if (out.schemaVersion != 1)
        throw std::runtime_error("unsupported expected manifest schema");
    out.buildId = json.at("buildId").get<uint32_t>();
    out.target = json.value("target", std::string());
    out.profiles = json.value("profiles", std::vector<std::string>{});
    std::set<uint16_t> ids;
    for (const auto& scenario : json.at("scenarios")) {
        ValidationScenario parsed;
        parsed.id = scenario.at("id").get<uint16_t>();
        parsed.phase = scenario.at("phase").get<int>();
        parsed.name = scenario.at("name").get<std::string>();
        parsed.capabilities = scenario.value("capabilities", std::set<std::string>{});
        if (!ids.insert(parsed.id).second)
            throw std::runtime_error("duplicate scenario id " + std::to_string(parsed.id));
        std::set<uint16_t> checkpoints;
        for (const auto& checkpoint : scenario.at("checkpoints")) {
            ValidationCheckpoint cp;
            cp.id = checkpoint.at("id").get<uint16_t>();
            cp.name = checkpoint.at("name").get<std::string>();
            cp.symbol = checkpoint.value("symbol", std::string());
            cp.expectations = checkpoint.at("expectations");
            if (!checkpoints.insert(cp.id).second)
                throw std::runtime_error("duplicate checkpoint id in " + parsed.name);
            parsed.checkpoints.push_back(std::move(cp));
        }
        out.scenarios.push_back(std::move(parsed));
    }
    return out;
}

bool ValidationRunResult::Passed() const { return FailedCount() == 0; }

size_t ValidationRunResult::PassedCount() const
{
    return std::count_if(cases.begin(), cases.end(), [](const auto& value) {
        return value.status == ValidationCaseResult::Status::Pass;
    });
}

size_t ValidationRunResult::FailedCount() const
{
    return std::count_if(cases.begin(), cases.end(), [](const auto& value) {
        return value.status == ValidationCaseResult::Status::Fail;
    });
}

size_t ValidationRunResult::SkippedCount() const
{
    return std::count_if(cases.begin(), cases.end(), [](const auto& value) {
        return value.status == ValidationCaseResult::Status::Skip;
    });
}

CheckpointRunner::CheckpointRunner(IValidationMachine& machine, CheckpointRunnerOptions options)
    : m_machine(machine), m_options(options)
{
}

bool CheckpointRunner::ReadMailbox(PtvMailbox& mailbox, std::string& error)
{
    std::array<uint8_t, kPtvMailboxSize> bytes{};
    if (!m_machine.ReadMemory(m_options.mailboxAddress, bytes)) {
        error = "mailbox address is not mapped RAM";
        return false;
    }
    const auto decoded = DecodeMailbox(bytes);
    if (!decoded.ok) {
        error = decoded.error;
        return false;
    }
    mailbox = decoded.mailbox;
    if (mailbox.buildId != m_options.expectedBuildId) {
        error = "target build ID does not match expected manifest";
        return false;
    }
    return true;
}

bool CheckpointRunner::WaitForReady(std::string& error)
{
    std::string lastError;
    for (uint32_t frame = 0; frame < m_options.frameBudget; ++frame) {
        PtvMailbox mailbox;
        if (ReadMailbox(mailbox, error) && mailbox.status == PtvStatus::Ready &&
            mailbox.command == PtvCommand::None) {
            m_lastCommandSeq = mailbox.commandSeq;
            m_lastCheckpointSeq = mailbox.checkpointSeq;
            return true;
        }
        lastError = error;
        m_machine.StepFrame();
    }
    error = "READY timed out after " + std::to_string(m_options.frameBudget) + " frames";
    if (!lastError.empty())
        error += " (last observation: " + lastError + ")";
    return false;
}

bool CheckpointRunner::WriteCommand(PtvCommand command, uint16_t scenario, std::string& error)
{
    PtvMailbox mailbox;
    if (!ReadMailbox(mailbox, error))
        return false;
    mailbox.commandSeq = std::max(mailbox.commandSeq, m_lastCommandSeq) + 1;
    mailbox.command = command;
    mailbox.scenario = scenario;
    auto bytes = EncodeMailbox(mailbox);
    if (!m_machine.WriteMemory(m_options.mailboxAddress, bytes)) {
        error = "cannot write mailbox command";
        return false;
    }
    m_lastCommandSeq = mailbox.commandSeq;
    return true;
}

bool CheckpointRunner::RunToCheckpoint(uint16_t scenario, const ValidationCheckpoint& checkpoint,
                                       PtvMailbox& mailbox, std::string& error)
{
    if (!m_machine.PrepareCheckpoint(checkpoint.symbol, error))
        return false;
    if (!WriteCommand(PtvCommand::Run, scenario, error))
        return false;
    for (uint32_t frame = 0; frame < m_options.frameBudget; ++frame) {
        m_machine.StepFrame();
        if (!ReadMailbox(mailbox, error))
            continue;
        if (mailbox.checkpointSeq <= m_lastCheckpointSeq)
            continue;
        if (mailbox.scenario != scenario || mailbox.checkpoint != checkpoint.id) {
            error = "guest published an unexpected checkpoint";
            return false;
        }
        if (mailbox.status == PtvStatus::GuestFail) {
            error = "guest reported failure";
            return false;
        }
        if (!m_machine.CheckpointReached(error))
            return false;
        m_lastCheckpointSeq = mailbox.checkpointSeq;
        return ValidatePayload(mailbox, error);
    }
    error = "checkpoint timed out after " + std::to_string(m_options.frameBudget) + " frames";
    return false;
}

bool CheckpointRunner::Continue(std::string& error)
{
    m_machine.ReleaseCheckpoint();
    if (!WriteCommand(PtvCommand::Continue, 0, error))
        return false;
    // CONTINUE is a request, not an instantaneous state change. Wait until the guest
    // consumes it and publishes READY before a skipped case or the next RUN can
    // overwrite the command word.
    return WaitForReady(error);
}

bool CheckpointRunner::ValidatePayload(const PtvMailbox& mailbox, std::string& error)
{
    if (mailbox.payloadSize == 0)
        return true;
    std::vector<uint8_t> payload(mailbox.payloadSize);
    if (!m_machine.ReadMemory(mailbox.payloadAddr, payload)) {
        error = "checkpoint payload is outside mapped RAM";
        return false;
    }
    if (Crc32(payload) != mailbox.payloadCrc32) {
        error = "checkpoint payload CRC32 differs";
        return false;
    }
    return true;
}

ValidationOrchestrator::ValidationOrchestrator(IValidationMachine& machine,
                                               ExpectedManifest manifest,
                                               CheckpointRunnerOptions options)
    : m_machine(machine), m_manifest(std::move(manifest)), m_options(options)
{
}

ValidationRunResult ValidationOrchestrator::Run(const std::string& profile, int minPhase,
                                                int maxPhase, int repetitions)
{
    ValidationRunResult result;
    result.profile = profile;
    result.buildId = m_manifest.buildId;
    const auto available = m_machine.Capabilities();
    SemanticComparator comparator;
    std::map<std::string, std::string> firstHashes;

    for (int repetition = 0; repetition < std::max(1, repetitions); ++repetition) {
        m_machine.Reset();
        CheckpointRunner runner(m_machine, m_options);
        std::string readyError;
        const bool ready = runner.WaitForReady(readyError);
        for (const auto& scenario : m_manifest.scenarios) {
            if (scenario.phase < minPhase || scenario.phase > maxPhase)
                continue;
            std::vector<std::string> missing;
            std::set_difference(scenario.capabilities.begin(), scenario.capabilities.end(),
                                available.begin(), available.end(), std::back_inserter(missing));
            for (const auto& checkpoint : scenario.checkpoints) {
                ValidationCaseResult item;
                item.id = "P" + std::to_string(scenario.phase) + "-" + std::to_string(scenario.id) +
                          "-" + std::to_string(checkpoint.id);
                item.name = scenario.name + " / " + checkpoint.name;
                item.phase = scenario.phase;
                item.expected = checkpoint.expectations;
                if (!missing.empty()) {
                    item.status = ValidationCaseResult::Status::Skip;
                    item.reason = "missing capabilities: ";
                    for (size_t index = 0; index < missing.size(); ++index) {
                        if (index != 0)
                            item.reason += ", ";
                        item.reason += missing[index];
                    }
                } else if (!ready) {
                    item.status = ValidationCaseResult::Status::Fail;
                    item.reason = readyError;
                } else {
                    PtvMailbox mailbox;
                    std::string error;
                    if (!runner.RunToCheckpoint(scenario.id, checkpoint, mailbox, error)) {
                        item.status = ValidationCaseResult::Status::Fail;
                        item.reason = error;
                    } else {
                        item.raw = m_machine.CaptureRaw(scenario.id, checkpoint.id);
                        item.displayed = m_machine.CaptureDisplayed(scenario.id, checkpoint.id);
                        const auto compared = comparator.CompareLayers(checkpoint.expectations,
                                                                       item.raw, item.displayed);
                        item.failures = compared.failures;
                        item.status = compared.Passed() ? ValidationCaseResult::Status::Pass
                                                        : ValidationCaseResult::Status::Fail;
                        item.snapshotHash = SemanticComparator::StableHash(item.displayed);
                        const std::string key = item.id;
                        if (auto found = firstHashes.find(key); found == firstHashes.end()) {
                            firstHashes[key] = item.snapshotHash;
                        } else if (found->second != item.snapshotHash) {
                            item.status = ValidationCaseResult::Status::Fail;
                            item.reason = "normalized snapshot is nondeterministic";
                        }
                        if (!runner.Continue(error)) {
                            item.status = ValidationCaseResult::Status::Fail;
                            item.reason = "failed to continue guest: " + error;
                        }
                    }
                }
                if (repetition == 0 || item.status == ValidationCaseResult::Status::Fail)
                    result.cases.push_back(std::move(item));
            }
        }
    }
    return result;
}

bool ValidationReportWriter::Write(const std::filesystem::path& directory,
                                   const ValidationRunResult& result, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = "cannot create artifact directory: " + ec.message();
        return false;
    }
    std::ofstream jsonFile(directory / "result.json");
    jsonFile << ResultJson(result).dump(2);
    std::ofstream junit(directory / "junit.xml");
    junit << "<testsuite tests=\"" << result.cases.size() << "\" failures=\""
          << result.FailedCount() << "\" skipped=\"" << result.SkippedCount() << "\">\n";
    for (const auto& item : result.cases) {
        junit << "  <testcase classname=\"phase" << item.phase << "\" name=\""
              << EscapeXml(item.id + " " + item.name) << "\">";
        if (item.status == ValidationCaseResult::Status::Skip)
            junit << "<skipped message=\"" << EscapeXml(item.reason) << "\"/>";
        else if (item.status == ValidationCaseResult::Status::Fail)
            junit << "<failure message=\"" << EscapeXml(item.reason) << "\"/>";
        junit << "</testcase>\n";
    }
    junit << "</testsuite>\n";

    std::ofstream html(directory / "report.html");
    html << "<!doctype html><meta charset=\"utf-8\"><title>Peekaboo validation</title>"
            "<style>body{font:14px sans-serif}table{border-collapse:collapse}td,th{padding:.4em;"
            "border:1px solid #bbb}.pass{color:#080}.fail{color:#b00}.skip{color:#777}</style>"
            "<h1>Peekaboo validation</h1><table><tr><th>ID</th><th>Phase</th><th>Status</th>"
            "<th>Test</th><th>Reason</th></tr>";
    for (const auto& item : result.cases)
        html << "<tr><td>" << EscapeXml(item.id) << "</td><td>" << item.phase << "</td><td class=\""
             << CaseStatusName(item.status) << "\">" << CaseStatusName(item.status) << "</td><td>"
             << EscapeXml(item.name) << "</td><td>" << EscapeXml(item.reason) << "</td></tr>";
    html << "</table><h2>Snapshots</h2>";
    for (const auto& item : result.cases) {
        html << "<details><summary>" << EscapeXml(item.id + " " + item.name)
             << "</summary><table><tr><th>Expected</th><th>Raw</th><th>Displayed</th></tr><tr>"
             << "<td><pre>" << EscapeXml(item.expected.dump(2)) << "</pre></td><td><pre>"
             << EscapeXml(item.raw.dump(2)) << "</pre></td><td><pre>"
             << EscapeXml(item.displayed.dump(2)) << "</pre></td></tr></table></details>";
    }

    for (const auto& item : result.cases) {
        if (item.status == ValidationCaseResult::Status::Pass)
            continue;
        const auto caseDir = directory / item.id;
        std::filesystem::create_directories(caseDir, ec);
        std::ofstream(caseDir / "expected.json") << item.expected.dump(2);
        std::ofstream(caseDir / "raw.json") << item.raw.dump(2);
        std::ofstream(caseDir / "displayed.json") << item.displayed.dump(2);
    }
    return jsonFile.good() && junit.good() && html.good();
}

const char* CaseStatusName(ValidationCaseResult::Status status)
{
    switch (status) {
        case ValidationCaseResult::Status::Pass:
            return "pass";
        case ValidationCaseResult::Status::Fail:
            return "fail";
        case ValidationCaseResult::Status::Skip:
            return "skip";
    }
    return "fail";
}

} // namespace adm::validation
