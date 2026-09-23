#include "Horo/Foundation/Assertions.h"
#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Result.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace {
    static_assert(noexcept(Horo::Result<void>::Success()), "A successful void Result must not allocate or throw on renderer hot paths.");
    static_assert(std::is_same_v<
                  decltype(Horo::Log::Logger::Write(std::string_view{"test"}, Horo::Log::Level::Info, std::string_view{"record"})), void>);
    static_assert(std::is_same_v<decltype(Horo::Log::Logger::WriteEmergency(std::string_view{"test"}, Horo::Log::Level::Critical,
                                                                            std::string_view{"record"})),
                                 void>);

    Horo::ErrorCodeDescriptor Descriptor(const std::string_view domain, const std::string_view code, const std::string_view summary) {
        return {
            .domain = Horo::ErrorDomainId{std::string{domain}},
            .code = Horo::ErrorCode{std::string{code}},
            .defaultSeverity = Horo::ErrorSeverity::Error,
            .summary = summary,
        };
    }

    Horo::Result<int> ForwardAcrossModuleBoundary(Horo::Result<int> result) {
        return result;
    }

    TEST_CASE("Result Preserves Successful Value", "[unit][foundation]") {
        const Horo::Result<int> result = Horo::Result<int>::Success(42);
        REQUIRE((result.HasValue()));
        REQUIRE((!result.HasError()));
        REQUIRE((result.Value() == 42));
    }

    TEST_CASE("Assertion helpers follow the build policy", "[unit][foundation]") {
        // The counters deliberately verify that each helper evaluates its condition at most once.
        int assertionEvaluations = 0;
        HORO_ASSERT(++assertionEvaluations == 1);
#if defined(NDEBUG)
        REQUIRE_FALSE(Horo::AssertionPolicy::DebugAssertionsEnabled);
        REQUIRE((assertionEvaluations == 0));
#else
        REQUIRE(Horo::AssertionPolicy::DebugAssertionsEnabled);
        REQUIRE((assertionEvaluations == 1));
#endif

        int invariantEvaluations = 0;
        HORO_INVARIANT(++invariantEvaluations == 1);
        REQUIRE((invariantEvaluations == 1));
    }

    TEST_CASE("Emergency logging bypasses filters and carries no result", "[unit][foundation]") {
        const Horo::Log::Level previousLevel = Horo::Log::Logger::GetLevel();
        Horo::Log::Logger::SetLevel(Horo::Log::Level::Off);
        const Horo::Log::LoggerStatistics before = Horo::Log::Logger::Statistics();
        Horo::Log::Logger::WriteEmergency("foundation.tests.assertion", Horo::Log::Level::Critical, "emergency assertion probe");
        const Horo::Log::LoggerStatistics after = Horo::Log::Logger::Statistics();
        Horo::Log::Logger::SetLevel(previousLevel);

        REQUIRE((after.emergencyRecords == before.emergencyRecords + 1));
    }

    TEST_CASE("Error severities map exactly to diagnostic severities", "[unit][foundation]") {
        const auto matches = [](const Horo::ErrorSeverity error, const Horo::DiagnosticSeverity diagnostic) {
            const std::optional<Horo::DiagnosticSeverity> mapped = Horo::DiagnosticSeverityForError(error);
            return mapped.has_value() && *mapped == diagnostic;
        };

        REQUIRE(matches(Horo::ErrorSeverity::Info, Horo::DiagnosticSeverity::Note));
        REQUIRE(matches(Horo::ErrorSeverity::Warning, Horo::DiagnosticSeverity::Warning));
        REQUIRE(matches(Horo::ErrorSeverity::Error, Horo::DiagnosticSeverity::Error));
        REQUIRE(matches(Horo::ErrorSeverity::Critical, Horo::DiagnosticSeverity::Fatal));
        REQUIRE_FALSE(Horo::DiagnosticSeverityForError(static_cast<Horo::ErrorSeverity>(0xff)).has_value());
    }

    TEST_CASE("Result Preserves Typed Failure", "[unit][foundation]") {
        const Horo::Error error{
            .code = Horo::ErrorCode{"foundation.test.failed"},
            .domain = Horo::ErrorDomainId{"horo.foundation.test"},
            .severity = Horo::ErrorSeverity::Error,
            .message = "Expected test failure",
        };
        const Horo::Result<int> result = Horo::Result<int>::Failure(error);
        REQUIRE((!result.HasValue()));
        REQUIRE((result.HasError()));
        REQUIRE((result.ErrorValue().code.Value() == "foundation.test.failed"));
        REQUIRE((result.ErrorValue().domain.Value() == "horo.foundation.test"));
    }

    TEST_CASE("Result Void Models Success And Failure", "[unit][foundation]") {
        const Horo::Result<void> success = Horo::Result<void>::Success();
        REQUIRE((success.HasValue()));

        const Horo::Result<void> failure = Horo::Result<void>::Failure(Horo::Error{
            .code = Horo::ErrorCode{"foundation.test.void_failure"},
            .domain = Horo::ErrorDomainId{"horo.foundation.test"},
            .severity = Horo::ErrorSeverity::Warning,
            .message = "Expected void failure",
        });
        REQUIRE((failure.HasError()));
        REQUIRE((failure.ErrorValue().severity == Horo::ErrorSeverity::Warning));
    }

    TEST_CASE("Error Factory Uses Declared Descriptor Identity", "[unit][foundation]") {
        const Horo::ErrorCodeDescriptor descriptor{
            .domain = Horo::ErrorDomainId{"horo.foundation.test"},
            .code = Horo::ErrorCode{"foundation.test.declared_failure"},
            .defaultSeverity = Horo::ErrorSeverity::Warning,
            .summary = "Declared failure",
            .remediationHint = "Use a valid test input.",
            .retryable = false,
            .userActionable = true,
        };

        const Horo::Error error = Horo::MakeError(descriptor, "Specific failure context");
        REQUIRE((error.domain.Value() == "horo.foundation.test"));
        REQUIRE((error.code.Value() == "foundation.test.declared_failure"));
        REQUIRE((error.severity == Horo::ErrorSeverity::Warning));
        REQUIRE((error.message == "Specific failure context"));
        REQUIRE((!error.cause));

        const Horo::Error fallback = Horo::MakeError(descriptor);
        REQUIRE((fallback.message == "Declared failure"));
        REQUIRE((fallback.cause.Get() == nullptr));
    }

    TEST_CASE("Typed Error Cause Chain Survives Result Copies And Moves", "[unit][foundation]") {
        const Horo::ErrorCodeDescriptor importDescriptor = Descriptor("horo.asset", "asset.import.failed", "Asset import failed");
        const Horo::ErrorCodeDescriptor fileDescriptor = Descriptor("horo.platform", "platform.file.read_failed", "File read failed");
        const Horo::ErrorCodeDescriptor permissionDescriptor =
            Descriptor("horo.platform", "platform.file.permission_denied", "Permission denied");

        const Horo::Result<int> copied = [&] {
            Horo::Error leaf = Horo::MakeError(permissionDescriptor, "Source file is not readable");
            leaf.diagnostics.push_back({.code = Horo::DiagnosticCode{"platform.file.permission_denied"},
                                        .severity = Horo::DiagnosticSeverity::Error,
                                        .message = "Permission denied",
                                        .location = {.source = "assets/tree.fbx"}});
            const Horo::Error middle = Horo::WrapError(fileDescriptor, leaf, "Could not read import source");
            const Horo::Error outer = Horo::WrapError(importDescriptor, middle, "Could not import tree asset");
            REQUIRE((outer.cause.Get()->cause.Get() == middle.cause.Get()));
            return Horo::Result<int>::Failure(outer);
        }();
        const Horo::Error *const sharedMiddle = copied.ErrorValue().cause.Get();
        Horo::Result<int> moved = ForwardAcrossModuleBoundary(copied);
        const Horo::Error &roundTripped = moved.ErrorValue();

        REQUIRE((roundTripped.code.Value() == "asset.import.failed"));
        REQUIRE((roundTripped.cause.Get() != nullptr));
        REQUIRE((roundTripped.cause.Get()->code.Value() == "platform.file.read_failed"));
        REQUIRE((roundTripped.cause.Get()->cause.Get() != nullptr));
        REQUIRE((roundTripped.cause.Get()->cause.Get()->code.Value() == "platform.file.permission_denied"));
        REQUIRE((roundTripped.cause.Get()->cause.Get()->diagnostics.size() == 1));
        REQUIRE((roundTripped.cause.Get()->cause.Get()->diagnostics.front().location.source == "assets/tree.fbx"));
        REQUIRE((roundTripped.cause.Get() == sharedMiddle));
    }

    TEST_CASE("Error Cause Queries Use Typed Domain And Code Identity", "[unit][foundation]") {
        const Horo::ErrorCodeDescriptor outerDescriptor = Descriptor("horo.project", "project.open.failed", "Project open failed");
        const Horo::ErrorCodeDescriptor causeDescriptor =
            Descriptor("horo.configuration", "configuration.value_invalid", "Configuration value is invalid");
        const Horo::Error error = Horo::WrapError(outerDescriptor, Horo::MakeError(causeDescriptor));

        REQUIRE((error.cause));
        REQUIRE((Horo::ErrorChainContains(error, outerDescriptor.domain, outerDescriptor.code)));
        REQUIRE((Horo::ErrorChainContains(error, causeDescriptor.domain, causeDescriptor.code)));
        REQUIRE((!Horo::ErrorChainContains(error, outerDescriptor.domain, causeDescriptor.code)));
        REQUIRE((!Horo::ErrorChainContains(error, Horo::ErrorDomainId{"horo.unknown"}, causeDescriptor.code)));

        const Horo::Result<void> copied = Horo::Result<void>::Failure(error);
        REQUIRE((copied.ErrorValue().cause.Get() != nullptr));
        REQUIRE((copied.ErrorValue().cause.Get()->message == "Configuration value is invalid"));
    }

    TEST_CASE("Cause Construction Preserves Existing Error Payloads", "[unit][foundation]") {
        const Horo::ErrorCodeDescriptor outerDescriptor = Descriptor("horo.asset", "asset.load.failed", "Asset load failed");
        const Horo::ErrorCodeDescriptor causeDescriptor = Descriptor("horo.platform", "platform.file.missing", "Source file is missing");

        Horo::Error outer = Horo::MakeError(outerDescriptor);
        outer.diagnostics.push_back({.code = Horo::DiagnosticCode{"asset.load.failed"},
                                     .severity = Horo::DiagnosticSeverity::Error,
                                     .message = "Asset could not be loaded"});
        Horo::Error cause = Horo::MakeError(causeDescriptor);
        cause.severity = Horo::ErrorSeverity::Critical;

        const Horo::Error chained = Horo::WithCause(std::move(outer), std::move(cause));

        REQUIRE((chained.message == "Asset load failed"));
        REQUIRE((chained.diagnostics.size() == 1));
        REQUIRE((chained.cause.Get() != nullptr));
        REQUIRE((chained.cause.Get()->severity == Horo::ErrorSeverity::Critical));
        REQUIRE((chained.cause.Get()->message == "Source file is missing"));
    }

    TEST_CASE("Diagnostic Carries Stable Identity And Location", "[unit][foundation]") {
        const Horo::Diagnostic diagnostic{
            .code = Horo::DiagnosticCode{"foundation.test.invalid_value"},
            .severity = Horo::DiagnosticSeverity::Error,
            .message = "Value is invalid",
            .location = Horo::SourceLocation{.source = "settings.json", .line = 7, .column = 3},
        };
        REQUIRE((diagnostic.code.Value() == "foundation.test.invalid_value"));
        REQUIRE((diagnostic.location.source == "settings.json"));
        REQUIRE((diagnostic.location.line == 7));
    }
}  // namespace
