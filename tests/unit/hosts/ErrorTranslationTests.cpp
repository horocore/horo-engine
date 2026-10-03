#include "HostErrorFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace HostErrorFixture;

TEST_CASE("All hosts preserve the same application error across stable presentation categories", "[hosts][errors][headless]") {
    auto translator = ErrorTranslator::Create(Registry(), Mappings());
    REQUIRE(translator);
    for (std::size_t index = 0; index < Descriptors.size(); ++index) {
        const Error original = Failure(index);
        const auto translated = translator->Translate(original);
        REQUIRE(translated);
        const auto payload = nlohmann::json::parse(translated->Json());
        const auto cli = TranslateCliError(*translated);
        const auto mcp = nlohmann::json::parse(TranslateMcpError(*translated));
        const auto gui = TranslateGuiError(*translated, GuiErrorSurface::Inline);
        REQUIRE(nlohmann::json::parse(cli.json)["error"] == payload);
        REQUIRE(mcp["data"] == payload);
        REQUIRE(mcp["code"] == -32000);
        REQUIRE(nlohmann::json::parse(gui.detail.Json()) == payload);
        REQUIRE(cli.exitCode == static_cast<int>(Categories[index]));
        REQUIRE(gui.surface == (index == 7 ? GuiErrorSurface::FatalDialog : GuiErrorSurface::Inline));
        REQUIRE(payload["domain"] == original.domain.Value());
        REQUIRE(payload["code"] == original.code.Value());
        REQUIRE(payload["message"] == Descriptors[index].summary);
        REQUIRE(payload["metadata"].is_object());
        REQUIRE(payload["diagnostics"].size() == 2);
        REQUIRE(payload["diagnostics"][0]["severity"] == "warning");
        REQUIRE(payload["diagnostics"][0]["message"] == DiagnosticDescriptors[0].summary);
        REQUIRE(payload["diagnostics"][1]["severity"] == "note");
        REQUIRE(payload["cause"]["code"] == Descriptors[3].code.Value());
        REQUIRE(payload["cause"]["cause"].is_null());
        REQUIRE(translated->Remediation() == Descriptors[index].remediationHint);
        REQUIRE(translated->Retryable() == Descriptors[index].retryable);
        REQUIRE(translated->UserActionable() == Descriptors[index].userActionable);
        REQUIRE(translated->Json().find("Private") == std::string::npos);
        REQUIRE(cli.stderrText.find("Private") == std::string::npos);
        REQUIRE(original.message == "Private credential and /home/private/project");
        REQUIRE(original.diagnostics[0].location.line == 4);
    }
}

TEST_CASE("Trusted local details preserve operation text and typed diagnostic locations", "[hosts][errors][headless]") {
    const auto translator = ErrorTranslator::Create(Registry(), Mappings());
    REQUIRE(translator);
    const auto translated = translator->Translate(Failure(3), ErrorDetail::TrustedLocal);
    REQUIRE(translated);
    const auto payload = nlohmann::json::parse(translated->Json());
    REQUIRE(payload["message"] == "Private credential and /home/private/project");
    REQUIRE(payload["diagnostics"][0]["location"] == nlohmann::json{{"source", "private/project.json"}, {"line", 4}, {"column", 7}});
    REQUIRE(payload["diagnostics"][0]["path"] == "entities[0].id");
    REQUIRE(payload["cause"]["message"] == "Private cause detail");
    const auto cli = TranslateCliError(*translated);
    REQUIRE(cli.stderrText.ends_with("Private credential and /home/private/project\n"));
    for (const auto surface : {GuiErrorSurface::Notification, GuiErrorSurface::Workflow, GuiErrorSurface::FatalDialog})
        REQUIRE(TranslateGuiError(*translated, surface).surface == surface);
}

TEST_CASE("Host mappings reject duplicates invalid categories and undeclared exact identities", "[hosts][errors][headless]") {
    auto mappings = Mappings();
    SECTION("duplicate") {
        mappings[1] = mappings[0];
    }
    SECTION("invalid category") {
        mappings[0].category = static_cast<ExitCategory>(1);
    }
    SECTION("unknown code") {
        mappings[0].code = ErrorCode{"project.translation.unknown"};
    }
    SECTION("foreign domain") {
        mappings[0].domain = ErrorDomainId{"project.other"};
    }
    REQUIRE_FALSE(ErrorTranslator::Create(Registry(), mappings));
}

TEST_CASE("Host translation fails closed without silently inventing or truncating failure meaning", "[hosts][errors][headless]") {
    const auto translator = ErrorTranslator::Create(Registry(), Mappings());
    REQUIRE(translator);
    Error error = Failure(3);
    SECTION("unknown outer") {
        error.code = ErrorCode{"project.translation.unknown"};
    }
    SECTION("unknown cause") {
        Error cause = MakeError(Descriptors[0]);
        cause.domain = ErrorDomainId{"project.other"};
        error = WithCause(std::move(error), std::move(cause));
    }
    SECTION("invalid severity") {
        error.severity = static_cast<ErrorSeverity>(255);
    }
    SECTION("raised severity") {
        error.severity = ErrorSeverity::Critical;
    }
    SECTION("downgraded fatal") {
        error = MakeError(Descriptors[7]);
        error.severity = ErrorSeverity::Error;
    }
    SECTION("invalid diagnostic severity") {
        error.diagnostics[0].severity = static_cast<DiagnosticSeverity>(255);
    }
    SECTION("empty diagnostic identity") {
        error.diagnostics[0].code = DiagnosticCode{};
    }
    SECTION("undeclared diagnostic identity") {
        error.diagnostics[0].code = DiagnosticCode{"project.translation.unknown"};
    }
    SECTION("raised diagnostic severity") {
        error.diagnostics[0].severity = DiagnosticSeverity::Fatal;
    }
    SECTION("downgraded fatal diagnostic") {
        error.diagnostics[0].code = DiagnosticCode{Descriptors[7].code.Value()};
        error.diagnostics[0].severity = DiagnosticSeverity::Error;
    }
    SECTION("too many diagnostics") {
        error.diagnostics.resize(65, error.diagnostics[0]);
    }
    SECTION("too many causes") {
        for (int count = 0; count < 16; ++count)
            error = WrapError(Descriptors[3], std::move(error));
    }
    REQUIRE_FALSE(translator->Translate(error));
}

TEST_CASE("Host translation validates disclosed text and admits exact finite bounds", "[hosts][errors][headless]") {
    const auto translator = ErrorTranslator::Create(Registry(), Mappings());
    REQUIRE(translator);
    Error error = Failure(3);
    SECTION("oversized message") {
        error.message.assign(4097, 'x');
    }
    SECTION("invalid UTF8") {
        error.message = std::string(1, static_cast<char>(0xFF));
    }
    SECTION("oversized source") {
        error.diagnostics[0].location.source.assign(4097, 'x');
    }
    SECTION("oversized diagnostic message") {
        error.diagnostics[0].message.assign(4097, 'x');
    }
    SECTION("oversized path") {
        error.diagnostics[0].path.assign(4097, 'x');
    }
    REQUIRE_FALSE(translator->Translate(error, ErrorDetail::TrustedLocal));
    REQUIRE(translator->Translate(error));
}

TEST_CASE("Translation owns snapshot and mappings and never parses diagnostic text", "[hosts][errors][headless]") {
    auto mappings = Mappings();
    const auto translator = ErrorTranslator::Create(Registry(), mappings);
    REQUIRE(translator);
    mappings[3].category = ExitCategory::Timeout;
    Error error = Failure(3);
    error.message = "timeout cancelled forbidden usage";
    const auto result = translator->Translate(error);
    REQUIRE(result);
    REQUIRE(result->Category() == ExitCategory::Operation);
    REQUIRE_FALSE(translator->Translate(error, static_cast<ErrorDetail>(255)));
    const auto partial = ErrorTranslator::Create(Registry(), std::span{mappings.data(), 1});
    REQUIRE(partial);
    REQUIRE_FALSE(partial->Translate(error));
    error.message.assign(4096, 'x');
    error.diagnostics.resize(64, error.diagnostics[0]);
    REQUIRE(translator->Translate(error, ErrorDetail::TrustedLocal));
    error = MakeError(Descriptors[3]);
    for (int count = 0; count < 15; ++count)
        error = WrapError(Descriptors[3], std::move(error));
    REQUIRE(translator->Translate(error));
    error = MakeError(Descriptors[3]);
    error.severity = ErrorSeverity::Warning;
    REQUIRE(nlohmann::json::parse(translator->Translate(error)->Json())["severity"] == "warning");
    error.severity = ErrorSeverity::Info;
    REQUIRE(nlohmann::json::parse(translator->Translate(error)->Json())["severity"] == "info");
    REQUIRE(nlohmann::json::parse(translator->Translate(error)->Json())["cause"].is_null());
    error.diagnostics.push_back({DiagnosticCode{Descriptors[7].code.Value()}, DiagnosticSeverity::Fatal, "Fatal finding", {}, {}});
    REQUIRE(nlohmann::json::parse(translator->Translate(error)->Json())["diagnostics"][0]["severity"] == "fatal");
}
