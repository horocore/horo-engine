#include "../hosts/HostErrorFixture.h"
#include "Horo/Cli/CliErrors.h"
#include "Horo/Cli/CliOutputPresenter.h"
#include "Horo/Foundation/Diagnostics/DiagnosticBundle.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <sstream>

namespace Horo::Cli {
    namespace {
        using Json = nlohmann::json;

        /** @brief Uses the same application fixture as GUI/MCP, extended with exact CLI-owned errors. */
        Hosts::ErrorTranslator Translator() {
            const ModuleDescriptor module{.id = {"horo.cli"}, .version = {1, 0, 0}, .errorDomains = {CliErrors::ErrorDomain()}};
            auto registry = ExtendErrorCodeRegistry(HostErrorFixture::Registry(), std::span{&module, 1});
            REQUIRE(registry.HasValue());
            REQUIRE(registry.Value().Resolve(CliErrors::DispatchRegistrationInvalid.domain, CliErrors::DispatchRegistrationInvalid.code) !=
                    nullptr);
            const auto application = HostErrorFixture::Mappings();
            std::vector<Hosts::ErrorMapping> mappings(application.begin(), application.end());
            mappings.push_back({CliErrors::HostFailure.domain, CliErrors::HostFailure.code, Hosts::ExitCategory::Invariant});
            mappings.push_back(
                {CliErrors::ExecutionContextInvalid.domain, CliErrors::ExecutionContextInvalid.code, Hosts::ExitCategory::Invariant});
            mappings.push_back({CliErrors::ParseFailed.domain, CliErrors::ParseFailed.code, Hosts::ExitCategory::Usage});
            auto translator = Hosts::ErrorTranslator::Create(std::move(registry).Value(), mappings);
            REQUIRE(translator);
            return std::move(*translator);
        }

        /** @brief Accepted metadata with a closed, version-one progress record declaration. */
        CliCommandDescriptor Descriptor() {
            return {.path = {{"project", "validate"}},
                    .summary = "Validate a project.",
                    .output = {.id = "horo.cli.validation",
                               .version = 1,
                               .formats = CliOutputFormat::Human | CliOutputFormat::Json | CliOutputFormat::JsonLines,
                               .progressRecords = true},
                    .hosts = CliHostAvailability::HoroEngine,
                    .contractVersion = {1, 0, 0},
                    .ownerId = "horo.project"};
        }

        /** @brief Checks the required result-envelope schema independent of optional fields. */
        void RequireEnvelope(const Json &value) {
            REQUIRE(value.is_object());
            CHECK(value.at("schemaVersion") == 1);
            CHECK(value.at("command").is_string());
            CHECK(value.at("invocationId").is_string());
            CHECK(value.at("status").is_string());
            CHECK(value.at("exitCode").is_number_integer());
            CHECK(value.at("diagnostics").is_array());
            CHECK(value.at("metadata").is_object());
            CHECK(value.at("outputSchema").at("id").is_string());
            CHECK(value.at("outputSchema").at("version") == 1);
            CHECK((value.at("result").is_object() || value.at("result").is_null()));
            CHECK((value.at("error").is_object() || value.at("error").is_null()));
        }

        /** @brief Fully correlated owned command success. */
        CliTerminalResult Success() {
            return CliTerminalResult::Success({.invocation = {31},
                                               .operation = {{9}},
                                               .job = {{7}},
                                               .project = CliSafeProjectContext{"safe-project"}},
                                              {.fields = {{"valid", true},
                                                          {"count", std::int64_t{2}},
                                                          {"ratio", 0.5},
                                                          {"text", std::string{"line\n\x1b[31m"}}}});
        }
    }  // namespace

    TEST_CASE("JSON CLI presentation is one document and preserves typed values and correlation") {
        std::ostringstream output, diagnostics;
        CliOutputPresenter presenter(Descriptor(), {31}, Translator(), output, diagnostics, CliProgressOutputMode::Json, true);
        REQUIRE(presenter.Progress({"validate", 0.5F, "Working"}).HasValue());
        CHECK(presenter.Complete(Success()).exitCode == 0);
        const std::string first = output.str();
        CHECK(presenter.Complete(Success()).exitCode == 0);
        CHECK(output.str() == first);
        CHECK(diagnostics.str().empty());
        CHECK(first.find('\x1b') == std::string::npos);
        const auto envelope = Json::parse(first);
        RequireEnvelope(envelope);
        CHECK(envelope["result"]["count"] == 2);
        CHECK(envelope["result"]["valid"] == true);
        CHECK(envelope["metadata"]["operationId"] == 9);
        CHECK(envelope["metadata"]["jobId"] == 7);
        CHECK(envelope["metadata"]["projectId"] == "safe-project");
        CHECK(envelope["error"].is_null());
    }

    TEST_CASE("JSON CLI failures preserve shared GUI and MCP meaning and every stable exit category") {
        for (std::size_t index = 0; index < HostErrorFixture::Descriptors.size(); ++index) {
            std::ostringstream output, diagnostics;
            auto translator = Translator();
            const auto error = HostErrorFixture::Failure(index);
            const auto translated = translator.Translate(error);
            REQUIRE(translated);
            const auto gui = Hosts::TranslateGuiError(*translated, Hosts::GuiErrorSurface::Inline);
            const auto mcp = Json::parse(Hosts::TranslateMcpError(*translated));
            CliOutputPresenter presenter(Descriptor(), {31}, std::move(translator), output, diagnostics, CliProgressOutputMode::Json,
                                         false);
            CHECK(presenter.Complete(CliTerminalResult::Failure({.invocation = {31}}, error)).exitCode ==
                  static_cast<int>(HostErrorFixture::Categories[index]));
            const auto envelope = Json::parse(output.str());
            RequireEnvelope(envelope);
            CHECK(envelope["error"] == Json::parse(gui.detail.Json()));
            CHECK(envelope["error"] == mcp["data"]);
            CHECK(envelope["result"].is_null());
            CHECK(envelope["status"] == "failed");
            CHECK(output.str().find("Private") == std::string::npos);
            CHECK(diagnostics.str().empty());
        }
    }

    TEST_CASE("JSONL CLI progress is declared correlated and followed by exactly one terminal summary") {
        std::ostringstream output, diagnostics;
        CliOutputPresenter presenter(Descriptor(), {31}, Translator(), output, diagnostics, CliProgressOutputMode::JsonLines, true);
        REQUIRE(presenter.Progress({"validate", 0.25F, "First\n\x1b"}).HasValue());
        REQUIRE(presenter.Progress({"validate", 1.0F, "Done"}).HasValue());
        CHECK(presenter.Complete(Success()).exitCode == 0);
        std::istringstream lines(output.str());
        std::string line;
        int count = 0;
        while (std::getline(lines, line)) {
            const auto record = Json::parse(line);
            ++count;
            CHECK(record["schemaVersion"] == 1);
            CHECK(record["recordVersion"] == 1);
            CHECK(record["invocationId"] == "cli-31");
            if (record["type"] == "result") {
                CHECK(count == 3);
                RequireEnvelope(record);
            } else
                CHECK(record["type"] == "progress");
        }
        CHECK(count == 3);
        CHECK(diagnostics.str().empty());
        CHECK(output.str().find('\x1b') == std::string::npos);
        const auto first = output.str();
        CHECK(presenter.Progress({"late", 1, "Late"}).HasError());
        CHECK(output.str() == first);
    }

    TEST_CASE("Machine CLI schemas do not depend on TTY detection") {
        for (const auto mode : {CliProgressOutputMode::Json, CliProgressOutputMode::JsonLines}) {
            std::ostringstream outputA, outputB, diagnosticsA, diagnosticsB;
            CliOutputPresenter tty(Descriptor(), {31}, Translator(), outputA, diagnosticsA, mode, true);
            CliOutputPresenter pipe(Descriptor(), {31}, Translator(), outputB, diagnosticsB, mode, false);
            REQUIRE(tty.Progress({"work", 0.5F, "Working"}).HasValue());
            REQUIRE(pipe.Progress({"work", 0.5F, "Working"}).HasValue());
            CHECK(tty.Complete(Success()).exitCode == pipe.Complete(Success()).exitCode);
            CHECK(outputA.str() == outputB.str());
            CHECK(diagnosticsA.str().empty());
            CHECK(diagnosticsB.str().empty());
        }
    }

    TEST_CASE("Partial CLI failures retain successful fields and canonical per-item errors") {
        std::ostringstream output, diagnostics;
        CliOutputPresenter presenter(Descriptor(), {31}, Translator(), output, diagnostics, CliProgressOutputMode::Json, false);
        auto terminal = CliTerminalResult::Success({.invocation = {31}}, {.fields = {{"completed", std::int64_t{3}}},
                                                                          .partialFailures = {{"item-4", HostErrorFixture::Failure(3)},
                                                                                              {"item-5", HostErrorFixture::Failure(1)}}});
        CHECK(presenter.Complete(terminal).exitCode == 5);
        const auto envelope = Json::parse(output.str());
        RequireEnvelope(envelope);
        CHECK(envelope["status"] == "partial_failure");
        CHECK(envelope["result"]["completed"] == 3);
        CHECK(envelope["partialFailures"].size() == 2);
        CHECK(envelope["partialFailures"][0]["error"]["code"] == HostErrorFixture::Descriptors[3].code.Value());
    }

    TEST_CASE("Unexpected host and untranslatable errors retain structured failure and owned evidence") {
        for (const auto mode : {CliProgressOutputMode::Json, CliProgressOutputMode::JsonLines}) {
            for (const bool unknownIdentity : {false, true}) {
                std::ostringstream output, diagnostics;
                CliOutputPresenter presenter(Descriptor(), {31}, Translator(), output, diagnostics, mode, false);
                if (unknownIdentity) {
                    auto error = HostErrorFixture::Failure(3);
                    error.code = ErrorCode{"project.translation.unknown"};
                    const auto outcome = presenter.Complete(CliTerminalResult::Failure({.invocation = {31}}, error));
                    CHECK(outcome.exitCode == 1);
                    REQUIRE(outcome.rejectedError);
                    CHECK(outcome.rejectedError->code.Value() == error.code.Value());
                    CHECK(outcome.rejectedError->message == error.message);
                    CHECK(outcome.rejectedError->diagnostics.size() == 2);
                    REQUIRE(outcome.rejectedError->cause);
                } else
                    CHECK(presenter.HostFailure().exitCode == 1);
                const auto envelope = Json::parse(output.str());
                RequireEnvelope(envelope);
                CHECK(envelope["error"]["code"] == "cli.host_failure");
                CHECK(envelope["exitCode"] == 1);
                if (mode == CliProgressOutputMode::JsonLines) {
                    CHECK(envelope["type"] == "result");
                    CHECK(envelope["recordVersion"] == 1);
                }
                const auto document = output.str();
                CHECK(std::count(document.begin(), document.end(), '\n') == 1);
                CHECK(presenter.HostFailure().exitCode == 1);
                CHECK(output.str() == document);
                CHECK(diagnostics.str().empty());
            }
        }
    }

    TEST_CASE("Invalid CLI results and undeclared progress fail before malformed output") {
        auto descriptor = Descriptor();
        descriptor.output.progressRecords = false;
        std::ostringstream output, diagnostics;
        CliOutputPresenter presenter(descriptor, {31}, Translator(), output, diagnostics, CliProgressOutputMode::JsonLines, false);
        SECTION("undeclared progress") {
            CHECK(presenter.Progress({"work", 0.5F, "Working"}).HasError());
        }
        SECTION("duplicate result fields") {
            const auto result = CliTerminalResult::Success({.invocation = {31}}, {.fields = {{"value", true}, {"value", false}}});
            CHECK(presenter.Complete(result).exitCode == 10);
        }
        if (output.str().empty())
            CHECK(presenter.Complete(Success()).exitCode == 10);
        const auto envelope = Json::parse(output.str());
        RequireEnvelope(envelope);
        CHECK(envelope["status"] == "failed");
    }

    TEST_CASE("Human CLI progress remains separate from result output and strips control text") {
        std::ostringstream output, diagnostics;
        CliOutputPresenter presenter(Descriptor(), {31}, Translator(), output, diagnostics, CliProgressOutputMode::Human, false);
        REQUIRE(presenter.Progress({"work\x1b", 0.5F, "Working\n"}).HasValue());
        CHECK(output.str().empty());
        CHECK(presenter.Complete(Success()).exitCode == 0);
        CHECK(output.str().find("succeeded") != std::string::npos);
        CHECK(diagnostics.str().find("50%") != std::string::npos);
        CHECK(diagnostics.str().find('\x1b') == std::string::npos);
        CHECK(output.str().find('\x1b') == std::string::npos);
    }

    TEST_CASE("Every original parser diagnostic remains a canonical usage failure") {
        const std::array codes{"cli.option_value_invalid",      "cli.path_normalizer_unavailable",
                               "cli.path_normalization_failed", "cli.configuration_value_incompatible",
                               "cli.stdin_malformed",           "cli.option_source_conflict",
                               "cli.option_value_missing",      "cli.option_unknown",
                               "cli.option_required",           "cli.positional_required",
                               "cli.positional_unexpected"};
        for (const auto code : codes) {
            auto error = MakeError(CliErrors::ParseFailed);
            error.diagnostics.push_back({.code = DiagnosticCode{code},
                                         .severity = DiagnosticSeverity::Error,
                                         .message = "Original parser diagnostic",
                                         .location = {"argv", 1, 2}});
            auto translator = Translator();
            const auto translated = translator.Translate(error);
            REQUIRE(translated);
            const auto canonical = Json::parse(translated->Json());
            std::ostringstream output, diagnostics;
            CliOutputPresenter presenter(Descriptor(), {31}, std::move(translator), output, diagnostics, CliProgressOutputMode::Json,
                                         false);
            const auto outcome = presenter.Complete(CliTerminalResult::Failure({.invocation = {31}}, error));
            CHECK(outcome.exitCode == 2);
            CHECK_FALSE(outcome.rejectedError);
            const auto envelope = Json::parse(output.str());
            CHECK(envelope["error"] == canonical);
            CHECK(envelope["diagnostics"][0]["code"] == code);
        }
    }

    TEST_CASE("Diagnostic bundle metadata preserves original Foundation identities through cross-host translation") {
        const auto domain = Diagnostics::DiagnosticBundleErrorDomain();
        CHECK(domain.id.Value() == "horo.foundation.observability");
        REQUIRE(domain.descriptors.size() == 4);
        const ModuleDescriptor module{.id = {"horo.foundation"}, .version = {1, 0, 0}, .errorDomains = {domain}};
        auto registry = BuildErrorCodeRegistry(std::span{&module, 1});
        REQUIRE(registry.HasValue());
        const std::array
            mappings{Hosts::ErrorMapping{domain.id, ErrorCode{"observability.bundle.invalid_request"}, Hosts::ExitCategory::Validation},
                     Hosts::ErrorMapping{domain.id, ErrorCode{"observability.bundle.read_failed"}, Hosts::ExitCategory::Operation},
                     Hosts::ErrorMapping{domain.id, ErrorCode{"observability.bundle.write_failed"}, Hosts::ExitCategory::Operation},
                     Hosts::ErrorMapping{domain.id, ErrorCode{"observability.bundle.size_exceeded"}, Hosts::ExitCategory::Validation}};
        const auto translator = Hosts::ErrorTranslator::Create(std::move(registry).Value(), mappings);
        REQUIRE(translator);
        for (const auto *descriptor : domain.descriptors) {
            const auto translated = translator->Translate(MakeError(*descriptor));
            REQUIRE(translated);
            const auto gui = Hosts::TranslateGuiError(*translated, Hosts::GuiErrorSurface::Inline);
            const auto cli = Json::parse(Hosts::TranslateCliError(*translated).json);
            const auto mcp = Json::parse(Hosts::TranslateMcpError(*translated));
            CHECK(cli["error"] == Json::parse(gui.detail.Json()));
            CHECK(cli["error"] == mcp["data"]);
            CHECK(cli["error"]["domain"] == domain.id.Value());
            CHECK(cli["error"]["code"] == descriptor->code.Value());
        }
    }
}  // namespace Horo::Cli
