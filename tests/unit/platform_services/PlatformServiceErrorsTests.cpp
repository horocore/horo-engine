#include "Horo/Foundation/ErrorCodeRegistry.h"
#include "Horo/PlatformServices/PlatformOfflineQueueErrors.h"
#include "Horo/PlatformServices/PlatformOfflineQueueStorage.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "Horo/PlatformServices/PlatformServiceErrors.h"
#include "Horo/PlatformServices/PlatformServicesBackend.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::PlatformServices::Tests {
    TEST_CASE("Offline policy and storage share canonical registry descriptors", "[platform-services][offline][errors]") {
        namespace Storage = ::Horo::PlatformOfflineQueue::PlatformOfflineQueueErrors;
        CHECK(&OfflineQueueErrors::InvalidConfiguration == &Storage::InvalidConfiguration);
        CHECK(&OfflineQueueErrors::CapacityExceeded == &Storage::CapacityExceeded);
        CHECK(&OfflineQueueErrors::IdentityConflict == &Storage::IdentityConflict);
        ModuleDescriptor module{.id = ModuleId{"horo.platform.services"}, .version = {1, 0, 0}};
        module.errorDomains = {PlatformServiceErrorDomain(), PlatformOfflineErrorDomain()};
        const std::array modules{module};
        const auto registry = BuildErrorCodeRegistry(modules);
        REQUIRE(registry.HasValue());
        CHECK(registry.Value().DomainCount() == 2);
        const std::array required{&Storage::InvalidConfiguration,
                                  &Storage::InvalidPartition,
                                  &Storage::InvalidRecord,
                                  &Storage::UnsupportedVersion,
                                  &Storage::PayloadTooLarge,
                                  &Storage::CapacityExceeded,
                                  &Storage::Corrupt,
                                  &Storage::DurableUnavailable,
                                  &Storage::StorageUnknown,
                                  &Storage::IdentityConflict,
                                  &Storage::RevisionConflict,
                                  &OfflineQueueErrors::InvalidIntent,
                                  &OfflineQueueErrors::Stale,
                                  &OfflineQueueErrors::Expired,
                                  &OfflineQueueErrors::InvalidTransition,
                                  &OfflineQueueErrors::ClockMovedBackward,
                                  &OfflineQueueErrors::QueueUnavailable};
        for (const auto *descriptor : required) {
            const auto *resolved = registry.Value().Resolve(MakeError(*descriptor));
            REQUIRE(resolved != nullptr);
            CHECK(resolved->summary == descriptor->summary);
            CHECK(resolved->retryable == descriptor->retryable);
        }
    }

    TEST_CASE("Platform service errors register stable provider causes", "[platform-services][errors]") {
        ModuleDescriptor module{.id = ModuleId{"horo.platform.services"}, .version = {1, 0, 0}};
        module.errorDomains.push_back(PlatformServiceErrorDomain());
        const std::array modules{module};
        const auto built = BuildErrorCodeRegistry(modules);
        REQUIRE(built.HasValue());
        REQUIRE(built.Value().DomainCount() == 1);
        REQUIRE(built.Value().Size() == 11);
        for (const auto *descriptor : module.errorDomains.front().descriptors)
            REQUIRE(built.Value().Resolve(descriptor->domain, descriptor->code) != nullptr);

        const auto error = MakePlatformProviderError(PlatformProviderFailureCategory::RateLimited, 42, 7);
        REQUIRE(built.Value().Resolve(error) != nullptr);
        REQUIRE(error.cause.Get());
        REQUIRE(built.Value().Resolve(*error.cause.Get()) != nullptr);
        CHECK(PlatformProviderCategory(error) == PlatformProviderFailureCategory::RateLimited);
        CHECK(error.cause.Get()->diagnostics.front().message == "request=42; generation=7");
        CHECK(PlatformProviderCategory(MakePlatformProviderError(static_cast<PlatformProviderFailureCategory>(255), 42, 7)) ==
              PlatformProviderFailureCategory::Unknown);
    }

    TEST_CASE("Application translation keeps capability, cancellation and timeout distinct", "[platform-services][errors]") {
        CHECK(ClassifyPlatformServiceError(MakeError(BackendErrors::ServiceUnavailable)) ==
              PlatformServiceErrorKind::CapabilityUnavailable);
        CHECK(ClassifyPlatformServiceError(MakeError(FrontendErrors::NullProvider)) == PlatformServiceErrorKind::NullProvider);
        CHECK(ClassifyPlatformServiceError(MakeError(PlatformSessionErrors::NoSubject)) ==
              PlatformServiceErrorKind::AuthenticationRequired);
        CHECK(ClassifyPlatformServiceError(MakeError(PlatformSessionErrors::AccessDenied)) == PlatformServiceErrorKind::AccessDenied);
        CHECK(ClassifyPlatformServiceError(MakeError(RequestErrors::Cancelled)) == PlatformServiceErrorKind::Cancelled);
        CHECK(ClassifyPlatformServiceError(MakeError(RequestErrors::TimedOut)) == PlatformServiceErrorKind::TimedOut);
        CHECK(ClassifyPlatformServiceError(MakeError(FrontendErrors::InvalidRequest)) == PlatformServiceErrorKind::Other);
    }
}  // namespace Horo::PlatformServices::Tests
