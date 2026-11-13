#include "Horo/Application/PlatformServicesProjectCook.h"
#include "Horo/Application/PlatformServicesProjectSources.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Platform/ExternalProcess.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    using Json = nlohmann::json;

    /** @brief Turns protocol assertions into a nonzero executable test result. */
    void Require(const bool condition, const std::string_view message) {
        if (!condition)
            throw std::runtime_error{std::string{message}};
    }

    /** @brief Keeps UTF-8 argv and result paths portable across the native process boundary. */
    std::string Utf8Path(const std::filesystem::path &path) {
        const auto value = path.generic_u8string();
        return {value.begin(), value.end()};
    }

    /** @brief Owns only a freshly created test directory and removes it on every terminal path. */
    class TemporaryDirectory final {
    public:
        TemporaryDirectory() {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() / std::filesystem::path{u8"horo cli ü "};
            path_ += std::to_string(suffix);
            Require(std::filesystem::create_directory(path_), "unique test directory could not be created");
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        TemporaryDirectory(const TemporaryDirectory &) = delete;
        TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

        const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    /** @brief Executes only the CMake-supplied binary with argument arrays and bounded drained output. */
    class ProcessFixture final {
    public:
        explicit ProcessFixture(std::string executable) : executable_(std::move(executable)) {
            Require(std::filesystem::is_regular_file(std::filesystem::path{std::u8string{executable_.begin(), executable_.end()}}),
                    "CMake CLI executable is missing");
        }

        std::string Run(std::vector<std::string> arguments, const int expected) {
            std::string output;
            std::string diagnostics;
            bool truncated = false;
            Horo::ExternalProcessRequest request{.executable = executable_,
                                                 .arguments = std::move(arguments),
                                                 .timeout = std::chrono::seconds{30},
                                                 .maximumLineBytes = 1024 * 1024,
                                                 .onOutput =
                                                     [&output, &diagnostics, &truncated](Horo::ProcessOutputLine line) {
                truncated = truncated || line.truncated;
                auto &stream = line.stream == Horo::ProcessOutputStream::StandardOutput ? output : diagnostics;
                stream += line.text + '\n';
            },
                                                 .maximumOutputBytes = 4 * 1024 * 1024};
            const auto result = runner_.Run(request, {});
            Require(!truncated, "protocol line exceeded the process test bound");
            Require(result.HasValue(), "native process launch or drain failed");
            Require(result.Value().reason == Horo::ProcessTerminationReason::Exited, "CLI timed out or was cancelled");
            Require(result.Value().exitCode == expected, "unexpected CLI exit: " + diagnostics + output);
            Require(output.find('\r') == std::string::npos, "machine output contains terminal carriage returns");
            Require(output.find('\x1b') == std::string::npos, "machine output contains terminal decoration");
            return output;
        }

    private:
        std::string executable_;
        Horo::NativeExternalProcessRunner runner_;
    };

    /** @brief Validates required version-one framing while allowing optional compatible fields. */
    void Envelope(const Json &value) {
        Require(value.is_object(), "terminal result must be an object");
        Require(value.at("schemaVersion") == 1, "schema version changed");
        Require(value.at("command").is_string(), "missing command identity");
        Require(value.at("invocationId").get<std::string>().starts_with("cli-"), "missing invocation identity");
        const auto status = value.at("status").get<std::string>();
        Require(status == "succeeded" || status == "failed" || status == "partial_failure", "unknown terminal status");
        Require(value.at("outputSchema").at("id").is_string(), "missing command output schema identity");
        Require(value.at("exitCode").is_number_integer(), "missing stable exit category");
        Require(value.at("diagnostics").is_array(), "diagnostics must be structured");
        Require(value.at("metadata").is_object(), "metadata must be structured");
        Require(value.at("outputSchema").at("version") == 1, "command output schema changed");
        const auto &result = value.at("result");
        const auto &error = value.at("error");
        Require(result.is_null() || result.is_object(), "result must be structured");
        Require(error.is_null() || error.is_object(), "error must be structured");
    }

    /** @brief Checks canonical success and original usage/validation identities through the real bootstrap. */
    void CheckJson(ProcessFixture &process, std::vector<std::string> arguments, const int expected) {
        const auto record = Json::parse(process.Run(std::move(arguments), expected));
        Envelope(record);
        Require(record.at("exitCode") == expected, "envelope and process exit differ");
        if (expected == 0) {
            Require(record.at("status") == "succeeded", "successful command failed");
            Require(record.at("error").is_null(), "success contains an error");
            return;
        }
        Require(record.at("status") == "failed", "error command succeeded");
        Require(!record.at("error").at("code").get<std::string>().empty(), "canonical error code was lost");
        if (expected == 2)
            Require(record.at("command") == "cli.invocation", "usage failure was not admitted at the host boundary");
        if (expected == 3) {
            Require(record.at("error").at("domain") == "horo.foundation.observability", "bundle error domain was translated away");
            Require(record.at("error").at("code") == "observability.bundle.invalid_request", "bundle error code was translated away");
        }
    }

    /** @brief Exercises one-document success/failure framing without relying on helper-only registrations. */
    void JsonCases(ProcessFixture &process) {
        CheckJson(process, {"observability", "smoke", "--output", "json"}, 0);
        CheckJson(process, {"--emit-observability-smoke", "--output=json"}, 0);
        CheckJson(process, {"--help", "--output", "json"}, 0);
        CheckJson(process, {"host", "inspect", "--unknown", "--output", "json"}, 2);
        CheckJson(process, {"missing", "command", "--output", "json"}, 2);
        CheckJson(process, {"diagnostics", "bundle", "--output-path", "relative.zip", "--output", "json"}, 3);
    }

    /** @brief Parses each declared JSONL record separately so log contamination fails the contract. */
    std::vector<Json> Records(const std::string &output) {
        std::vector<Json> records;
        std::size_t begin = 0;
        while (begin < output.size()) {
            const auto end = output.find('\n', begin);
            Require(end != std::string::npos, "JSONL record was not terminated");
            records.push_back(Json::parse(output.substr(begin, end - begin)));
            begin = end + 1;
        }
        return records;
    }

    /** @brief Verifies declared progress and one correlated summary for a real ZIP in Unicode/space paths. */
    void JsonLinesCases(ProcessFixture &process) {
        TemporaryDirectory directory;
        const auto destination = directory.Path() / "diagnostics bundle.zip";
        const auto records = Records(process.Run({"--diagnostic-bundle", Utf8Path(destination), "--output", "jsonl"}, 0));
        Require(std::filesystem::is_regular_file(destination), "diagnostic ZIP was not produced");
        Require(records.size() == 2, "expected one progress record and one terminal summary");
        Require(records.front().at("type") == "progress", "progress was not declared");
        Require(records.front().at("recordVersion") == 1, "progress record version changed");
        const auto &terminal = records.back();
        Envelope(terminal);
        Require(terminal.at("type") == "result", "terminal summary is missing");
        Require(terminal.at("recordVersion") == 1, "terminal record version changed");
        Require(terminal.at("result").at("outputPath") == Utf8Path(destination), "UTF-8 path changed across process boundaries");
        Require(records.front().at("invocationId") == terminal.at("invocationId"), "progress correlation changed");
        const auto failure = Records(process.Run({"host", "inspect", "--unknown", "--output", "jsonl"}, 2));
        Require(failure.size() == 1, "JSONL admission failure emitted extra records");
        Envelope(failure.front());
        Require(failure.front().at("type") == "result", "JSONL admission failure lacks its summary");
    }

    /** @brief Exercises the actual CLI registration and shared validation with on-disk sources, not an adapter-only fake. */
    void PlatformServicesCases(ProcessFixture &process) {
        using namespace Horo;
        using namespace Horo::Application;
        using namespace Horo::PlatformServices;
        TemporaryDirectory directory;
        const auto root = std::filesystem::canonical(directory.Path());
        Envelope(Json::parse(process.Run({"platform-services", "validate", "--output", "json"}, 2)));
        Envelope(Json::parse(process.Run({"platform-services", "validate", "--project", "relative-project", "--output", "json"}, 2)));
        const auto *release = BuiltInReleaseCompatibilityRegistry().Find(CurrentEngineReleaseVersion());
        Require(release != nullptr, "current release contract is missing");
        Json metadata{{"horoVersion", FormatHoroVersion(release->release.value)},
                      {"persistentContract", FormatPersistentContractHash(release->persistentContract)},
                      {"projectId", "platform-cli-contract"},
                      {"name", "Platform CLI"},
                      {"projectVersion", "1.0.0"},
                      {"createdAt", "2026-11-11T00:00:00Z"},
                      {"settings", {{"renderBackend", "null"}}}};
        PlatformServicesIdSalt salt;
        salt.bytes.front() = std::byte{1};
        auto initial = PreparePlatformServicesInitialDocuments(metadata.dump(), salt);
        Require(initial.HasValue(), "initial platform documents failed");
        const auto writeAt = [&](const std::filesystem::path &base, const std::string_view relative, const std::string_view text) {
            const auto path = base / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.close();
            Require(!stream.fail(), "platform fixture write failed");
        };
        const auto write = [&](const std::string_view relative, const std::string_view text) {
            writeAt(root, relative, text);
        };
        const auto &docs = initial.Value();
        const auto materialize = [&](const std::filesystem::path &base) {
            writeAt(base, ".horo/project.json", docs.projectSource);
            writeAt(base, PlatformServicesDocumentPaths::Ledger, docs.ledgerSource);
            writeAt(base, PlatformServicesDocumentPaths::Achievements, docs.achievementSource);
            writeAt(base, PlatformServicesDocumentPaths::Stats, docs.statSource);
            writeAt(base, PlatformServicesDocumentPaths::Leaderboards, docs.leaderboardSource);
            writeAt(base, PlatformServicesDocumentPaths::Presence, docs.presenceSource);
        };
        materialize(root);
        const auto admission = InspectProjectCompatibility(root);
        const auto expected =
            CapturePlatformServicesProjectSources(root, admission, {.profile = PlatformServicesHostProfile::HeadlessServer});
        Require(expected.HasValue(), "shared headless validation failed");
        const auto run = [&](const int exit) {
            auto result =
                Json::parse(process.Run({"platform-services", "validate", "--project", Utf8Path(root), "--output", "json"}, exit));
            Envelope(result);
            return result;
        };
        const auto valid = run(0);
        Require(valid.at("outputSchema").at("id") == "horo.platform-services.validation", "platform command was not registered");
        Require(valid.at("result").at("validated") == true, "platform validation was not executed");
        Require(valid.at("result").at("sourceFingerprint") == FormatSha256(expected.Value().SourceFingerprint()),
                "CLI captured a different source generation");
        Require(!std::filesystem::exists(root / ".horo/local"), "read-only validation created local state");
        const auto runCook = [&](const std::filesystem::path &base, const int exit) {
            auto result = Json::parse(process.Run({"platform-services", "cook", "--project", Utf8Path(base), "--output", "json"}, exit));
            Envelope(result);
            return result;
        };
        const auto readPair = [&](const std::filesystem::path &base) {
            auto pair = Assets::CaptureAssetSource(base, base / PlatformServicesProjectCookPath,
                                                   PlatformProviderManifestCookGenerationMaximumBytes);
            Require(pair.HasValue() && pair.Value().identity.exists, "committed paired output is missing");
            Require(DecodePlatformProviderManifestCookGeneration(pair.Value().bytes, pair.Value().identity.digest).HasValue(),
                    "published pair is malformed");
            return std::move(pair).Value();
        };
        const auto firstCook = runCook(root, 0);
        Require(firstCook.at("outputSchema").at("id") == "horo.platform-services.cook", "production cook command was not registered");
        Require(firstCook.at("result").at("committed") == true && firstCook.at("result").at("durabilityConfirmed") == true,
                "cook did not confirm durable publication");
        const auto firstPair = readPair(root);
        Require(firstCook.at("result").at("generationFingerprint") == FormatSha256(firstPair.identity.digest),
                "CLI cook receipt disagrees with the paired file");
        auto changedPolicy = Json::parse(docs.projectSource);
        changedPolicy["settings"]["platformServices"]["services"][0] = 1;
        write(".horo/project.json", changedPolicy.dump());
        const auto incrementalCook = runCook(root, 0);
        const auto incrementalPair = readPair(root);
        Require(incrementalPair.bytes != firstPair.bytes, "semantic policy edit did not invalidate the published pair");
        TemporaryDirectory clean;
        const auto cleanRoot = std::filesystem::canonical(clean.Path());
        materialize(cleanRoot);
        writeAt(cleanRoot, ".horo/project.json", changedPolicy.dump());
        const auto cleanCook = runCook(cleanRoot, 0);
        Require(readPair(cleanRoot).bytes == incrementalPair.bytes, "clean and incremental production cooks diverged");
        Require(cleanCook.at("result").at("generationFingerprint") == incrementalCook.at("result").at("generationFingerprint"),
                "clean and incremental cook receipts diverged");
        write(".horo/project.json", docs.projectSource);
        auto skewedStats = Json::parse(docs.statSource);
        skewedStats["schemaVersion"] = 99;
        for (const auto &text : {std::string{"{"}, std::string{"{\"schemaVersion\":1,\"schemaVersion\":1}"}, skewedStats.dump(),
                                 std::string(4U * 1024U * 1024U + 1, ' ')}) {
            write(PlatformServicesDocumentPaths::Stats, text);
            const auto failure = run(3);
            const auto shared = CapturePlatformServicesProjectSources(root, InspectProjectCompatibility(root),
                                                                      {.profile = PlatformServicesHostProfile::HeadlessServer});
            Require(shared.HasError(), "hostile stats source was accepted by shared validation");
            Require(failure.at("error").at("code") == shared.ErrorValue().code.Value(), "CLI changed the original validator identity");
            const auto cookFailure = runCook(root, 3);
            Require(cookFailure.at("error").at("code") == shared.ErrorValue().code.Value(), "cook changed the shared validator identity");
            Require(readPair(root).bytes == incrementalPair.bytes, "failed cook replaced the previous pair");
        }
        write(PlatformServicesDocumentPaths::Stats, docs.statSource);
        Require(std::filesystem::remove(root / PlatformServicesDocumentPaths::Presence), "presence fixture was not removed");
        const auto missing = run(5);
        Require(missing.at("error").at("code") == Assets::AssetSourceErrors::ReadFailed.code.Value(),
                "missing authority was not preserved");
        Require(!std::filesystem::exists(root / PlatformServicesDocumentPaths::Presence), "CLI repaired missing source");
        Require(runCook(root, 5).at("error").at("code") == Assets::AssetSourceErrors::ReadFailed.code.Value(),
                "cook repaired missing authority");
        Require(readPair(root).bytes == incrementalPair.bytes, "missing-source cook changed the published pair");
        write(PlatformServicesDocumentPaths::Presence, docs.presenceSource);
        auto exact = Json::parse(docs.projectSource);
        exact["settings"]["platformServices"]["provider"] = {{"mode", "exact"}, {"key", "unavailable.fixture"}};
        write(".horo/project.json", exact.dump());
        const auto unsupported = run(4);
        Require(unsupported.at("error").at("code") == PlatformProjectConfigurationErrors::ProviderNotFound.code.Value(),
                "exact provider selection fell back");
        Require(runCook(root, 4).at("error").at("code") == PlatformProjectConfigurationErrors::ProviderNotFound.code.Value(),
                "cook selected an implicit provider fallback");
        Require(readPair(root).bytes == incrementalPair.bytes, "unavailable-provider cook changed the published pair");
        auto legacy = Json::parse(docs.projectSource);
        const auto *legacyRelease = BuiltInReleaseCompatibilityRegistry().Find({ParseHoroVersion("0.3.0").Value()});
        Require(legacyRelease != nullptr, "legacy release fixture is missing");
        legacy["horoVersion"] = "0.3.0";
        legacy["persistentContract"] = FormatPersistentContractHash(legacyRelease->persistentContract);
        write(".horo/project.json", legacy.dump());
        const auto migrationRequired = run(6);
        Require(migrationRequired.at("status") == "failed", "CLI activated an unmigrated root");
        Require(migrationRequired.at("error").at("code") == Assets::AssetSourceErrors::Authority.code.Value(),
                "unmigrated input bypassed shared admission");
        Require(runCook(root, 6).at("status") == "failed", "cook bypassed required migration");
        write(".horo/project.json", docs.projectSource);
        Require(run(0).at("result").at("sourceFingerprint") == valid.at("result").at("sourceFingerprint"),
                "reload changed source generation");
        {
            NativeDurableFileSystem files;
            Editor::ProjectMutationCoordinator mutations{files};
            auto held = mutations.TryAcquire({root, Editor::ProjectMutationOwner::Migration, "production-cook-busy-regression"});
            Require(held.HasValue(), "test could not hold the real project mutation lease");
            const auto busy = runCook(root, 5);
            const auto domain = Editor::ProjectMutationErrorDomain();
            Require(busy.at("error").at("domain") == domain.id.Value(), "cook bypassed the shared project writer lease");
            Require(readPair(root).bytes == incrementalPair.bytes, "busy cook modified the previous publication");
        }
        Require(runCook(root, 0).at("result").at("generationFingerprint") == firstCook.at("result").at("generationFingerprint"),
                "restored authored policy did not reproduce the first cook");
        Require(readPair(root).bytes == firstPair.bytes, "restored policy produced different paired bytes");
    }
}  // namespace

/** @brief Runs the bounded native production protocol contract supplied by CTest. */
int main(const int argc, char **argv) {
    try {
        Require(argc == 2, "expected exactly the CMake-supplied CLI binary");
        ProcessFixture process{argv[1]};
        JsonCases(process);
        JsonLinesCases(process);
        PlatformServicesCases(process);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
