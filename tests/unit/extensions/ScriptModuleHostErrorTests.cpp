#include "CliHostErrorTranslation.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Application::Internal {
    TEST_CASE("Real host composition admits canonical module errors with capability cancellation and timeout categories",
              "[extensions][script][cli][errors]") {
        namespace E = Extensions::ExtensionErrors;
        using enum Hosts::ExitCategory;
        auto translator = MakeCliHostErrorTranslator();
        REQUIRE(translator);
        const std::array cases{std::pair{&E::ScriptInvocationUnavailable, Capability},
                               std::pair{&E::ScriptInvocationContextRevoked, Cancelled}, std::pair{&E::ScriptInvocationTimeout, Timeout}};
        for (const auto &[descriptor, category] : cases) {
            auto result = translator->Translate(MakeError(*descriptor));
            REQUIRE(result);
            CHECK(result->Category() == category);
            CHECK(result->Failure().code.Value() == descriptor->code.Value());
            CHECK(result->Failure().domain.Value() == descriptor->domain.Value());
        }
    }

    TEST_CASE("Real module error translation preserves typed causes and trusted diagnostics while rejecting legacy identities",
              "[extensions][script][cli][errors]") {
        namespace E = Extensions::ExtensionErrors;
        auto translator = MakeCliHostErrorTranslator();
        REQUIRE(translator);
        auto error = WrapError(E::ScriptInvocationUnavailable, MakeError(E::ScriptExportDescriptorIncompatible, "PRIVATE candidate detail"),
                               "PRIVATE owner detail");
        error.diagnostics.push_back({DiagnosticCode{E::ScriptInvocationUnavailable.code.Value()},
                                     DiagnosticSeverity::Error,
                                     "PRIVATE diagnostic",
                                     {"private/module.script", 7, 9},
                                     "horo.reference"});
        auto trusted = translator->Translate(error, Hosts::ErrorDetail::TrustedLocal);
        REQUIRE(trusted);
        CHECK(trusted->Failure().message == error.message);
        REQUIRE(trusted->Failure().cause.Get());
        CHECK(trusted->Failure().cause.Get()->code.Value() == E::ScriptExportDescriptorIncompatible.code.Value());
        CHECK(trusted->Failure().cause.Get()->domain.Value() == E::ScriptExportDescriptorIncompatible.domain.Value());
        CHECK(trusted->Failure().cause.Get()->message == "PRIVATE candidate detail");
        REQUIRE(trusted->Failure().diagnostics.size() == 1);
        CHECK(trusted->Failure().diagnostics.front().message == "PRIVATE diagnostic");
        CHECK(trusted->Failure().diagnostics.front().location.source == "private/module.script");
        CHECK(trusted->Failure().diagnostics.front().location.line == 7);
        CHECK(trusted->Failure().diagnostics.front().location.column == 9);
        CHECK(trusted->Failure().diagnostics.front().path == "horo.reference");
        auto publicResult = translator->Translate(error);
        REQUIRE(publicResult);
        CHECK(publicResult->Json().find("PRIVATE") == std::string::npos);
        REQUIRE(publicResult->Failure().cause.Get());
        CHECK(publicResult->Failure().cause.Get()->code.Value() == E::ScriptExportDescriptorIncompatible.code.Value());
        error.code = ErrorCode{"script_invocation_unavailable"};
        CHECK_FALSE(translator->Translate(error));
    }
}  // namespace Horo::Application::Internal
