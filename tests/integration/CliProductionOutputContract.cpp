#include "Horo/Application/PlatformServicesProjectCook.h"
#include "Horo/Application/PlatformServicesProjectSources.h"
#include "Horo/Editor/ProjectMutation.h"
#include "CliOutputContractFixture.h"
#include "Horo/Platform/ExternalProcess.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
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
                                                 .environment = Environment(),
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
            Require(output.find("fake-cli-secret-731") == std::string::npos, "stdout disclosed a synthetic credential");
            Require(diagnostics.find("fake-cli-secret-731") == std::string::npos, "stderr disclosed a synthetic credential");
            return output;
        }

    private:
        /** @brief Keeps real user credentials and pre-existing logs outside the privacy fixture's child environment. */
        Horo::ProcessEnvironment Environment() const {
            const auto directory = Utf8Path(home_.Path());
            Horo::ProcessEnvironment environment{.base = Horo::ProcessEnvironmentBase::Replace,
                                                 .set = {{"HOME", directory},
                                                         {"USERPROFILE", directory},
                                                         {"TEMP", directory},
                                                         {"TMP", directory},
                                                         {"HORO_CLI_FAKE_CREDENTIAL", "fake-cli-secret-731"}}};
#if defined(_WIN32)
            if (const auto *root = std::getenv("SystemRoot"))
                environment.set.push_back({"SystemRoot", root});
#endif
            return environment;
        }

        std::string executable_;
        Horo::NativeExternalProcessRunner runner_;
        TemporaryDirectory home_;
    };

    /** @brief Validates required version-one framing while allowing optional compatible fields. */
    void Envelope(const Json &value) {
        Horo::Tests::CliContract::ValidateEnvelope(value);
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

    /** @brief Requires a versioned fixture for every command exposed by production discovery. */
    void CheckCommandInventory(ProcessFixture &process, const Json &fixture) {
        const auto help = Json::parse(process.Run({"host", "help", "--output", "json"}, 0));
        Envelope(help);
        std::set<std::string> actualCommands;
        std::istringstream lines{help.at("result").at("help").get<std::string>()};
        std::string line;
        while (std::getline(lines, line)) {
            if (line.starts_with("  ")) {
                auto command = line.substr(2, line.find("  ", 2) - 2);
                std::ranges::replace(command, ' ', '.');
                actualCommands.insert(std::move(command));
            }
        }
        std::set<std::string> expectedCommands;
        for (const auto &contract : fixture.at("commandSchemas")) {
            if (contract.at("command") != "cli.invocation")
                expectedCommands.insert(contract.at("command").get<std::string>());
        }
        Require(actualCommands == expectedCommands, "public command inventory lacks a versioned output fixture");
    }

    /** @brief Exercises one-document success/failure framing without relying on helper-only registrations. */
    void JsonCases(ProcessFixture &process) {
        const auto &fixture = Horo::Tests::CliContract::Fixture();
        CheckCommandInventory(process, fixture);
        for (const auto &test : fixture.at("successCases")) {
            for (const auto &mode : {std::string{"json"}, std::string{"jsonl"}}) {
                auto arguments = test.at("arguments").get<std::vector<std::string>>();
                arguments.insert(arguments.end(), {"--non-interactive", "--output", mode});
                const auto envelope = Json::parse(process.Run(std::move(arguments), 0));
                Envelope(envelope);
                Require(envelope.at("schemaVersion") == fixture.at("schemaVersion"), "fixture schema version changed");
                if (mode == "jsonl")
                    Require(envelope.at("recordVersion") == fixture.at("recordVersion"), "fixture record version changed");
                Require(envelope.at("command") == test.at("command"), "fixture command identity changed");
                Require(envelope.at("outputSchema").at("id") == test.at("schema"), "fixture output schema changed");
                Require(envelope.at("result") == test.at("result"), "fixture result changed");
            }
        }
        CheckJson(process, {"observability", "smoke", "--output", "json"}, 0);
        CheckJson(process, {"--emit-observability-smoke", "--output=json"}, 0);
        CheckJson(process, {"--help", "--output", "json"}, 0);
        CheckJson(process, {"host", "inspect", "--unknown", "--output", "json"}, 2);
        CheckJson(process, {"missing", "command", "--output", "json"}, 2);
        CheckJson(process, {"diagnostics", "bundle", "--output-path", "relative.zip", "--output", "json"}, 3);
        for (const char *mode : {"human", "json", "jsonl"})
            static_cast<void>(process.Run({"host", "inspect", "--fake-secret=fake-cli-secret-731", "--output", mode}, 2));
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

    /** @brief Inspects decompressed entries so compression cannot hide an environment disclosure. */
    void CheckBundlePrivacy(const std::filesystem::path &destination) {
        Require(std::filesystem::file_size(destination) <= 64U * 1024U * 1024U, "test bundle exceeded its input bound");
        std::ifstream bundle(destination, std::ios::binary);
        const std::string archive{std::istreambuf_iterator<char>{bundle}, std::istreambuf_iterator<char>{}};
        mz_zip_archive zip{};
        Require(mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0) != 0, "bundle is not a readable ZIP");

        /** @brief Closes the reader when a protocol assertion unwinds. */
        struct ReaderGuard {
            explicit ReaderGuard(mz_zip_archive &archive) : zip(archive) {}

            ReaderGuard(const ReaderGuard &) = delete;
            ReaderGuard &operator=(const ReaderGuard &) = delete;
            mz_zip_archive &zip;

            ~ReaderGuard() {
                mz_zip_reader_end(&zip);
            }
        } guard{zip};

        for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); ++index) {
            mz_zip_archive_file_stat entry{};
            Require(mz_zip_reader_file_stat(&zip, index, &entry) != 0, "bundle entry metadata is invalid");
            Require(entry.m_uncomp_size <= 64U * 1024U * 1024U, "bundle entry exceeded the test bound");
            std::string contents(static_cast<std::size_t>(entry.m_uncomp_size), '\0');
            Require(mz_zip_reader_extract_to_mem(&zip, index, contents.data(), contents.size(), 0) != 0, "bundle entry is unreadable");
            Require(contents.find("fake-cli-secret-731") == std::string::npos, "bundle disclosed an environment credential");
        }
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
        CheckBundlePrivacy(destination);
        const auto failure = Records(process.Run({"host", "inspect", "--unknown", "--output", "jsonl"}, 2));
        Require(failure.size() == 1, "JSONL admission failure emitted extra records");
        Envelope(failure.front());
        Require(failure.front().at("type") == "result", "JSONL admission failure lacks its summary");
    }

    namespace PlatformProtocol {
        using namespace Horo;
        using namespace Horo::Application;
        using namespace Horo::PlatformServices;

        /** @brief Owns real project bytes and observed publication receipts across production-process scenarios. */
        struct Fixture final {
            explicit Fixture(ProcessFixture &value) : process(value), root(std::filesystem::canonical(directory.Path())), docs(Initial()) {
                Materialize(root);
            }

            ProcessFixture &process;
            TemporaryDirectory directory;
            std::filesystem::path root;
            PlatformServicesInitialDocuments docs;
            Json valid;
            Json firstCook;
            Assets::AssetSourceCapture firstPair;
            Assets::AssetSourceCapture incrementalPair;

            /** @brief Constructs explicit current-version authority without exercising the command under test. */
            static PlatformServicesInitialDocuments Initial() {
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
                return std::move(initial).Value();
            }

            /** @brief Writes fixture input only, preserving literal Unicode project paths. */
            static void WriteAt(const std::filesystem::path &base, std::string_view relative, std::string_view text) {
                const auto path = base / relative;
                std::filesystem::create_directories(path.parent_path());
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream.write(text.data(), static_cast<std::streamsize>(text.size()));
                stream.close();
                Require(!stream.fail(), "platform fixture write failed");
            }

            /** @brief Replaces one authored fixture source in the primary root. */
            void Write(std::string_view path, std::string_view text) {
                WriteAt(root, path, text);
            }

            /** @brief Materializes all six initial documents in an independent clean root. */
            void Materialize(const std::filesystem::path &base) const {
                WriteAt(base, ".horo/project.json", docs.projectSource);
                WriteAt(base, PlatformServicesDocumentPaths::Ledger, docs.ledgerSource);
                WriteAt(base, PlatformServicesDocumentPaths::Achievements, docs.achievementSource);
                WriteAt(base, PlatformServicesDocumentPaths::Stats, docs.statSource);
                WriteAt(base, PlatformServicesDocumentPaths::Leaderboards, docs.leaderboardSource);
                WriteAt(base, PlatformServicesDocumentPaths::Presence, docs.presenceSource);
            }

            /** @brief Invokes the actual read-only CLI protocol against the primary project. */
            Json Validate(int exit) {
                auto result =
                    Json::parse(process.Run({"platform-services", "validate", "--project", Utf8Path(root), "--output", "json"}, exit));
                Envelope(result);
                return result;
            }

            /** @brief Invokes actual command registration, authority admission, cooking and publication. */
            Json Cook(const std::filesystem::path &base, int exit) {
                auto result =
                    Json::parse(process.Run({"platform-services", "cook", "--project", Utf8Path(base), "--output", "json"}, exit));
                Envelope(result);
                return result;
            }

            /** @brief Reads and validates the complete committed pair without consulting staging files. */
            static Assets::AssetSourceCapture Pair(const std::filesystem::path &base) {
                auto pair = Assets::CaptureAssetSource(base, base / PlatformServicesProjectCookPath,
                                                       PlatformProviderManifestCookGenerationMaximumBytes);
                Require(pair.HasValue() && pair.Value().identity.exists, "committed paired output is missing");
                Require(DecodePlatformProviderManifestCookGeneration(pair.Value().bytes, pair.Value().identity.digest).HasValue(),
                        "published pair is malformed");
                return std::move(pair).Value();
            }
        };

        /** @brief Compares production registration and capture with the shared immutable headless validator. */
        void Validation(Fixture &fixture) {
            Envelope(Json::parse(fixture.process.Run({"platform-services", "validate", "--output", "json"}, 2)));
            Envelope(Json::parse(
                fixture.process.Run({"platform-services", "validate", "--project", "relative-project", "--output", "json"}, 2)));
            const auto expected = CapturePlatformServicesProjectSources(fixture.root, InspectProjectCompatibility(fixture.root),
                                                                        {.profile = PlatformServicesHostProfile::HeadlessServer});
            Require(expected.HasValue(), "shared headless validation failed");
            fixture.valid = fixture.Validate(0);
            Require(fixture.valid.at("outputSchema").at("id") == "horo.platform-services.validation",
                    "platform command was not registered");
            Require(fixture.valid.at("result").at("validated") == true, "platform validation was not executed");
            Require(fixture.valid.at("result").at("sourceFingerprint") == FormatSha256(expected.Value().SourceFingerprint()),
                    "CLI captured a different source generation");
            Require(!std::filesystem::exists(fixture.root / ".horo/local"), "read-only validation created local state");
        }

        /** @brief Requires semantic edits and independent clean roots to converge through real command processes. */
        void Convergence(Fixture &fixture) {
            fixture.firstCook = fixture.Cook(fixture.root, 0);
            Require(fixture.firstCook.at("outputSchema").at("id") == "horo.platform-services.cook",
                    "production cook command was not registered");
            Require(fixture.firstCook.at("result").at("committed") == true &&
                        fixture.firstCook.at("result").at("durabilityConfirmed") == true,
                    "cook did not confirm durable publication");
            fixture.firstPair = Fixture::Pair(fixture.root);
            Require(fixture.firstCook.at("result").at("generationFingerprint") == FormatSha256(fixture.firstPair.identity.digest),
                    "CLI cook receipt disagrees with the paired file");
            auto changedPolicy = Json::parse(fixture.docs.projectSource);
            changedPolicy["settings"]["platformServices"]["services"][0] = 1;
            fixture.Write(".horo/project.json", changedPolicy.dump());
            const auto incrementalCook = fixture.Cook(fixture.root, 0);
            fixture.incrementalPair = Fixture::Pair(fixture.root);
            Require(fixture.incrementalPair.bytes != fixture.firstPair.bytes, "semantic policy edit did not invalidate the published pair");
            TemporaryDirectory clean;
            const auto cleanRoot = std::filesystem::canonical(clean.Path());
            fixture.Materialize(cleanRoot);
            Fixture::WriteAt(cleanRoot, ".horo/project.json", changedPolicy.dump());
            const auto cleanCook = fixture.Cook(cleanRoot, 0);
            Require(Fixture::Pair(cleanRoot).bytes == fixture.incrementalPair.bytes, "clean and incremental production cooks diverged");
            Require(cleanCook.at("result").at("generationFingerprint") == incrementalCook.at("result").at("generationFingerprint"),
                    "clean and incremental cook receipts diverged");
            fixture.Write(".horo/project.json", fixture.docs.projectSource);
        }

        /** @brief Requires malformed, duplicate, oversized and version-skewed inputs to preserve the previous pair. */
        void Malformed(Fixture &fixture) {
            auto skewedStats = Json::parse(fixture.docs.statSource);
            skewedStats["schemaVersion"] = 99;
            for (const auto &text : {std::string{"{"}, std::string{"{\"schemaVersion\":1,\"schemaVersion\":1}"}, skewedStats.dump(),
                                     std::string(4U * 1024U * 1024U + 1, ' ')}) {
                fixture.Write(PlatformServicesDocumentPaths::Stats, text);
                const auto failure = fixture.Validate(3);
                const auto shared = CapturePlatformServicesProjectSources(fixture.root, InspectProjectCompatibility(fixture.root),
                                                                          {.profile = PlatformServicesHostProfile::HeadlessServer});
                Require(shared.HasError(), "hostile stats source was accepted by shared validation");
                Require(failure.at("error").at("code") == shared.ErrorValue().code.Value(), "CLI changed the original validator identity");
                Require(fixture.Cook(fixture.root, 3).at("error").at("code") == shared.ErrorValue().code.Value(),
                        "cook changed the shared validator identity");
                Require(Fixture::Pair(fixture.root).bytes == fixture.incrementalPair.bytes, "failed cook replaced the previous pair");
            }
            fixture.Write(PlatformServicesDocumentPaths::Stats, fixture.docs.statSource);
        }

        /** @brief Refuses missing authority and unavailable exact providers without repair or implicit fallback. */
        void Unavailable(Fixture &fixture) {
            Require(std::filesystem::remove(fixture.root / PlatformServicesDocumentPaths::Presence), "presence fixture was not removed");
            Require(fixture.Validate(5).at("error").at("code") == Assets::AssetSourceErrors::ReadFailed.code.Value(),
                    "missing authority was not preserved");
            Require(!std::filesystem::exists(fixture.root / PlatformServicesDocumentPaths::Presence), "CLI repaired missing source");
            Require(fixture.Cook(fixture.root, 5).at("error").at("code") == Assets::AssetSourceErrors::ReadFailed.code.Value(),
                    "cook repaired missing authority");
            Require(Fixture::Pair(fixture.root).bytes == fixture.incrementalPair.bytes, "missing-source cook changed the published pair");
            fixture.Write(PlatformServicesDocumentPaths::Presence, fixture.docs.presenceSource);
            auto exact = Json::parse(fixture.docs.projectSource);
            exact["settings"]["platformServices"]["provider"] = {{"mode", "exact"}, {"key", "unavailable.fixture"}};
            fixture.Write(".horo/project.json", exact.dump());
            Require(fixture.Validate(4).at("error").at("code") == PlatformProjectConfigurationErrors::ProviderNotFound.code.Value(),
                    "exact provider selection fell back");
            Require(fixture.Cook(fixture.root, 4).at("error").at("code") ==
                        PlatformProjectConfigurationErrors::ProviderNotFound.code.Value(),
                    "cook selected an implicit provider fallback");
            Require(Fixture::Pair(fixture.root).bytes == fixture.incrementalPair.bytes,
                    "unavailable-provider cook changed the published pair");
        }

        /** @brief Prevents ordinary commands from activating an unmigrated root and checks restoration of source identity. */
        void Migration(Fixture &fixture) {
            auto legacy = Json::parse(fixture.docs.projectSource);
            const auto *release = BuiltInReleaseCompatibilityRegistry().Find({ParseHoroVersion("0.3.0").Value()});
            Require(release != nullptr, "legacy release fixture is missing");
            legacy["horoVersion"] = "0.3.0";
            legacy["persistentContract"] = FormatPersistentContractHash(release->persistentContract);
            fixture.Write(".horo/project.json", legacy.dump());
            const auto required = fixture.Validate(6);
            Require(required.at("status") == "failed", "CLI activated an unmigrated root");
            Require(required.at("error").at("code") == Assets::AssetSourceErrors::Authority.code.Value(),
                    "unmigrated input bypassed shared admission");
            Require(fixture.Cook(fixture.root, 6).at("status") == "failed", "cook bypassed required migration");
            fixture.Write(".horo/project.json", fixture.docs.projectSource);
            Require(fixture.Validate(0).at("result").at("sourceFingerprint") == fixture.valid.at("result").at("sourceFingerprint"),
                    "reload changed source generation");
        }

        /** @brief Holds the genuine shared mutation lease in the parent process, then proves deterministic retry. */
        void LeaseAndRetry(Fixture &fixture) {
            {
                NativeDurableFileSystem files;
                Editor::ProjectMutationCoordinator mutations{files};
                auto held =
                    mutations.TryAcquire({fixture.root, Editor::ProjectMutationOwner::Migration, "production-cook-busy-regression"});
                Require(held.HasValue(), "test could not hold the real project mutation lease");
                const auto busy = fixture.Cook(fixture.root, 5);
                const auto domain = Editor::ProjectMutationErrorDomain();
                Require(busy.at("error").at("domain") == domain.id.Value(), "cook bypassed the shared project writer lease");
                Require(Fixture::Pair(fixture.root).bytes == fixture.incrementalPair.bytes, "busy cook modified the previous publication");
            }
            Require(fixture.Cook(fixture.root, 0).at("result").at("generationFingerprint") ==
                        fixture.firstCook.at("result").at("generationFingerprint"),
                    "restored authored policy did not reproduce the first cook");
            Require(Fixture::Pair(fixture.root).bytes == fixture.firstPair.bytes, "restored policy produced different paired bytes");
        }
    }  // namespace PlatformProtocol

    /** @brief Exercises actual registration, validation, publication, hostile input and writer lifecycle in protocol order. */
    void PlatformServicesCases(ProcessFixture &process) {
        PlatformProtocol::Fixture fixture{process};
        PlatformProtocol::Validation(fixture);
        PlatformProtocol::Convergence(fixture);
        PlatformProtocol::Malformed(fixture);
        PlatformProtocol::Unavailable(fixture);
        PlatformProtocol::Migration(fixture);
        PlatformProtocol::LeaseAndRetry(fixture);
    }

    /** @brief Checks rejected exports emit no collection progress and preserve occupied destinations. */
    void RejectedBundleCases(ProcessFixture &process) {
        TemporaryDirectory directory;
        const auto destination = directory.Path() / "occupied.zip";
        std::ofstream(destination) << "preserve";
        for (const auto &path : {std::string{"relative.zip"}, Utf8Path(destination)}) {
            const auto records =
                Records(process.Run({"diagnostics", "bundle", "--output-path", path, "--non-interactive", "--output", "jsonl"}, 3));
            Require(records.size() == 1, "rejected bundle started collection progress");
            Envelope(records.front());
            Require(records.front().at("error").at("code") == "observability.bundle.invalid_request", "bundle error changed");
        }
        std::ifstream contents(destination);
        std::string text;
        contents >> text;
        Require(text == "preserve", "rejected bundle overwrote existing data");
        Require(!std::filesystem::exists(directory.Path() / "occupied.zip.tmp"), "rejected bundle created temporary output");
        const auto missing = Records(process.Run({"diagnostics", "bundle", "--non-interactive", "--output", "jsonl"}, 2));
        Require(missing.size() == 1, "missing explicit destination emitted progress");
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
        RejectedBundleCases(process);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
