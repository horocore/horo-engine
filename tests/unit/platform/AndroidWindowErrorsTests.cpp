#include "AndroidWindowBridgeTestFixture.h"
#include "AndroidWindowErrors.h"
#include "Horo/Foundation/ErrorCodeRegistry.h"

#include <optional>
#include <string>
#include <string_view>

namespace {
    using namespace Horo;
    using namespace Horo::Tests::AndroidWindow;

    struct ExpectedWindowError {
        std::string_view code;
        std::string_view summary;
        std::string_view remediation;
        bool retryable;
    };

    constexpr std::array<ExpectedWindowError, 4> ExpectedErrors{{
        {"android_window.operation_in_progress", "An Android native window operation is already in progress.",
         "Return from native dispatch before calling this owner again.", true},
        {"android_window.unsupported_backend", "The fixed Android backend selection does not support native windows.",
         "Select an admitted Android composition before forwarding windows.", false},
        {"android_window.no_presentation", "This Android host selected no presentation.",
         "Keep headless lifecycle operation independent of native presentation.", false},
        {"android_window.acquire_failed", "The native Android window reference could not be acquired.",
         "Do not publish the candidate; retry only while its native callback borrow remains valid.", true},
    }};

    ModuleDescriptor DescribeWindowOwner() {
        return {.id = ModuleId{"horo.platform"}, .errorDomains = {AndroidWindowErrorDomain()}};
    }

    ErrorCodeRegistry MakeWindowRegistry() {
        const std::array modules{DescribeWindowOwner()};
        auto built = BuildErrorCodeRegistry(modules);
        REQUIRE(built.HasValue());
        return std::move(built).Value();
    }

    void CheckMetadata(const ErrorCodeRegistry &registry, const ExpectedWindowError &expected) {
        const ErrorDomainId domain{"horo.platform"};
        const ErrorCode code{std::string{expected.code}};
        const auto *descriptor = registry.Resolve(domain, code);
        REQUIRE(descriptor != nullptr);
        CHECK(descriptor->domain.Value() == domain.Value());
        CHECK(descriptor->code.Value() == expected.code);
        CHECK(descriptor->defaultSeverity == ErrorSeverity::Error);
        CHECK(descriptor->summary == expected.summary);
        CHECK(descriptor->remediationHint == expected.remediation);
        CHECK(descriptor->retryable == expected.retryable);
        CHECK_FALSE(descriptor->userActionable);
        CHECK_FALSE(descriptor->deprecatedBy.has_value());
        const auto *owner = registry.OwnerOf(domain, code);
        REQUIRE(owner != nullptr);
        CHECK(owner->value == "horo.platform");
    }

    void CheckRegisteredFailure(const ErrorCodeRegistry &registry, const Error &error, const ExpectedWindowError &expected) {
        CHECK(error.domain.Value() == "horo.platform");
        CHECK(error.code.Value() == expected.code);
        CHECK(error.severity == ErrorSeverity::Error);
        CHECK(error.message == expected.summary);
        CHECK(error.diagnostics.empty());
        CHECK(error.cause.Get() == nullptr);
        CheckMetadata(registry, expected);
        CHECK(registry.Resolve(error) == registry.Resolve(error.domain, error.code));
    }

    TEST_CASE("Android window metadata resolves before any native owner or activation", "[unit][platform][android][window][errors]") {
        auto first = AndroidWindowErrorDomain();
        const auto second = AndroidWindowErrorDomain();
        CHECK(first.id.Value() == "horo.platform");
        CHECK(first.descriptors.size() == ExpectedErrors.size());
        CHECK(first.descriptors == second.descriptors);
        first.descriptors.clear();
        CHECK(second.descriptors.size() == ExpectedErrors.size());
        const auto registry = MakeWindowRegistry();
        CHECK(registry.Size() == ExpectedErrors.size());
        CHECK(registry.DomainCount() == 1);
        for (const auto &expected : ExpectedErrors) {
            INFO(expected.code);
            CheckMetadata(registry, expected);
        }
    }

    TEST_CASE("Android window admission failures resolve through the platform metadata", "[unit][platform][android][window][errors]") {
        const auto registry = MakeWindowRegistry();
        Fixture fixture;
        SECTION("unsupported backend") {
            auto bridge = fixture.Bridge(AndroidWindowSelection::Unsupported);
            const auto result = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
            REQUIRE(result.HasError());
            CheckRegisteredFailure(registry, result.ErrorValue(), ExpectedErrors[1]);
        }
        SECTION("no presentation") {
            auto bridge = fixture.Bridge(AndroidWindowSelection::NoPresentation);
            const auto result = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
            REQUIRE(result.HasError());
            CheckRegisteredFailure(registry, result.ErrorValue(), ExpectedErrors[2]);
        }
        SECTION("native acquisition failed") {
            fixture.native.failAcquire = true;
            auto bridge = fixture.Bridge();
            const auto result = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
            REQUIRE(result.HasError());
            CheckRegisteredFailure(registry, result.ErrorValue(), ExpectedErrors[3]);
        }
        CHECK(fixture.native.acquireCount == 0);
        CHECK(fixture.native.releaseCount == 0);
        CHECK(fixture.State().queuedObservationCount == 0);
        CHECK_FALSE(fixture.State().hasWindow);
    }

    TEST_CASE("Android window reentry failure resolves through the platform metadata", "[unit][platform][android][window][errors]") {
        const auto registry = MakeWindowRegistry();
        Fixture fixture;
        auto bridge = fixture.Bridge();

        struct Probe {
            AndroidWindowBridge &bridge;
            std::optional<Error> error;
        } probe{bridge};

        fixture.native.hookContext = &probe;
        fixture.native.onAcquire = [](void *context) noexcept {
            auto &state = *static_cast<Probe *>(context);
            auto result = state.bridge.Drain();
            if (result.HasError())
                state.error = std::move(result).ErrorValue();
        };
        REQUIRE(bridge.OnWindowAvailable(fixture.activity, fixture.native.First()).HasValue());
        fixture.native.onAcquire = nullptr;
        REQUIRE(probe.error.has_value());
        CheckRegisteredFailure(registry, *probe.error, ExpectedErrors[0]);
        CHECK(fixture.native.acquireCount == 1);
        CHECK(fixture.native.releaseCount == 0);
        CHECK(fixture.State().queuedObservationCount == 1);
        CHECK_FALSE(fixture.State().hasWindow);
    }

    TEST_CASE("Android window metadata rejects duplicate pairs", "[unit][platform][android][window][errors]") {
        std::array modules{DescribeWindowOwner()};
        auto &domain = modules[0].errorDomains[0];
        REQUIRE(domain.descriptors.size() == ExpectedErrors.size());
        domain.descriptors.push_back(domain.descriptors.front());
        const auto rejected = BuildErrorCodeRegistry(modules);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().domain.Value() == "horo.foundation.errors");
        CHECK(rejected.ErrorValue().code.Value() == "foundation.error_registry.duplicate_code");
    }

    TEST_CASE("Android window metadata rejects competing equal and nested owners", "[unit][platform][android][window][errors]") {
        auto competitor = DescribeWindowOwner();
        competitor.id = ModuleId{"horo.platform.services"};
        SECTION("equal domain") {}
        SECTION("existing platform services nested domain") {
            competitor.errorDomains = {{.id = ErrorDomainId{"horo.platform.service"}}};
        }
        const std::array modules{DescribeWindowOwner(), competitor};
        const auto rejected = BuildErrorCodeRegistry(modules);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().domain.Value() == "horo.foundation.errors");
        CHECK(rejected.ErrorValue().code.Value() == "foundation.error_registry.domain_ownership_conflict");
    }

    TEST_CASE("Android window registry extension preserves retained snapshots", "[unit][platform][android][window][errors]") {
        const ErrorCodeRegistry empty;
        const std::array modules{DescribeWindowOwner()};
        auto extended = ExtendErrorCodeRegistry(empty, modules);
        REQUIRE(extended.HasValue());
        CHECK(empty.Empty());
        const auto retained = std::move(extended).Value();
        for (const auto &expected : ExpectedErrors)
            CheckMetadata(retained, expected);
        const auto rejected = ExtendErrorCodeRegistry(retained, modules);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "foundation.error_registry.invalid_namespace");
        CHECK(retained.Size() == ExpectedErrors.size());
        CHECK(retained.DomainCount() == 1);
        for (const auto &expected : ExpectedErrors)
            CheckMetadata(retained, expected);
        CHECK(empty.Empty());
    }
}  // namespace
