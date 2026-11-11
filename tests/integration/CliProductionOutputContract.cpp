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
        RejectedBundleCases(process);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
