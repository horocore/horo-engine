#include "Horo/XR/XRLoaderPreflight.h"
#include "support/AllocationProbe.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace Horo::XR {
    namespace {
        using Horo::Tests::RequireFailureIdentity;

        [[nodiscard]] XRLoaderPreflightRequest Request() {
            return {.attempt = Horo::Tests::IdentityValue<XRLoaderPreflightAttempt>(11),
                    .backend = Horo::Tests::IdentityValue<XRBackendId>(12),
                    .installRecord = Horo::Tests::IdentityValue<XRInstallRecordId>(13),
                    .productProfile = Horo::Tests::IdentityValue<XRProductProfileId>(14),
                    .admittedLoaderVersions = {{1, 0, 0}, {1, 2, 9}}};
        }

        [[nodiscard]] XRLoaderProbeEvidence AvailableEvidence() {
            return {.attempt = Horo::Tests::IdentityValue<XRLoaderPreflightAttempt>(11),
                    .loader = XRLoaderAvailability::Available,
                    .runtime = XRRuntimeAvailability::Available,
                    .system = XRSystemAvailability::Supported,
                    .loaderApiVersion = {1, 1, 3},
                    .runtimeGeneration = Horo::Tests::IdentityValue<XRRuntimeGeneration>(21),
                    .consumedProbeSteps = 3};
        }

        TEST_CASE("XR loader preflight owns exact successful selection evidence", "[unit][xr][loader]") {
            const auto request = Request();
            const auto result = CreateXRLoaderPreflightSnapshot(request, AvailableEvidence());
            REQUIRE(result.HasValue());
            const auto snapshot = result.Value();

            CHECK(snapshot.ContractVersion() == CurrentXRContractVersion);
            CHECK(snapshot.Attempt() == request.attempt);
            CHECK(snapshot.Backend() == request.backend);
            CHECK(snapshot.InstallRecord() == request.installRecord);
            CHECK(snapshot.ProductProfile() == request.productProfile);
            CHECK(snapshot.LoaderSource() == XRLoaderSourcePolicy::BundledVerified);
            CHECK(snapshot.ProductMode() == XRPreflightProductMode::Shipping);
            CHECK(snapshot.RuntimeSelection() == XRRuntimeSelectionPolicy::SystemDefault);
            CHECK(snapshot.LoaderApiVersion() == XRLoaderApiVersion{1, 1, 3});
            CHECK(snapshot.RuntimeGeneration().Value() == 21);
            CHECK(snapshot.ConsumedProbeSteps() == 3);
            static_assert(std::is_trivially_copyable_v<XRLoaderPreflightSnapshot>);
        }

        TEST_CASE("XR loader preflight admits only an approved development override", "[unit][xr][loader]") {
            auto request = Request();
            request.productMode = XRPreflightProductMode::Development;
            request.runtimeSelection = XRRuntimeSelectionPolicy::ApprovedDeveloperOverride;
            request.developerOverrideApproved = true;
            const auto result = CreateXRLoaderPreflightSnapshot(request, AvailableEvidence());
            REQUIRE(result.HasValue());
            CHECK(result.Value().ProductMode() == XRPreflightProductMode::Development);
            CHECK(result.Value().RuntimeSelection() == XRRuntimeSelectionPolicy::ApprovedDeveloperOverride);

            request.productMode = XRPreflightProductMode::Shipping;
            RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, AvailableEvidence()), XRErrors::RuntimeOverrideRejected);
            request.productMode = XRPreflightProductMode::Development;
            request.developerOverrideApproved = false;
            RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, AvailableEvidence()), XRErrors::RuntimeOverrideRejected);
        }

        TEST_CASE("XR loader preflight preserves distinct actionable loader failures", "[unit][xr][loader]") {
            const std::array failures{std::pair{XRLoaderAvailability::Absent, &XRErrors::LoaderAbsent},
                                      std::pair{XRLoaderAvailability::Incompatible, &XRErrors::LoaderIncompatible},
                                      std::pair{XRLoaderAvailability::OpenFailed, &XRErrors::LoaderOpenFailed}};
            for (const auto &[availability, error] : failures) {
                auto evidence = AvailableEvidence();
                evidence = {.attempt = evidence.attempt,
                            .loader = availability,
                            .loaderApiVersion =
                                availability == XRLoaderAvailability::Incompatible ? XRLoaderApiVersion{2, 0, 0} : XRLoaderApiVersion{},
                            .consumedProbeSteps = 1};
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(Request(), evidence), *error);
            }
        }

        TEST_CASE("XR loader preflight accepts exact version and work boundaries", "[unit][xr][loader]") {
            auto evidence = AvailableEvidence();
            evidence.loaderApiVersion = Request().admittedLoaderVersions.minimum;
            evidence.consumedProbeSteps = MaximumXRLoaderPreflightSteps;
            REQUIRE(CreateXRLoaderPreflightSnapshot(Request(), evidence).HasValue());
            evidence.loaderApiVersion = Request().admittedLoaderVersions.maximum;
            REQUIRE(CreateXRLoaderPreflightSnapshot(Request(), evidence).HasValue());
        }

        TEST_CASE("XR loader preflight preserves runtime and system discovery failures", "[unit][xr][loader]") {
            auto evidence = AvailableEvidence();
            evidence.runtime = XRRuntimeAvailability::Unavailable;
            evidence.system = XRSystemAvailability::Unsupported;
            evidence.runtimeGeneration = {};
            RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(Request(), evidence), XRErrors::RuntimeUnavailable);

            evidence.runtime = XRRuntimeAvailability::Rejected;
            RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(Request(), evidence), XRErrors::RuntimeRejected);

            evidence = AvailableEvidence();
            evidence.system = XRSystemAvailability::Unsupported;
            evidence.runtimeGeneration = {};
            RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(Request(), evidence), XRErrors::SystemUnsupported);
            evidence.system = XRSystemAvailability::TemporarilyUnavailable;
            RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(Request(), evidence), XRErrors::SystemTemporarilyUnavailable);
        }

        TEST_CASE("XR loader preflight rejects malformed contradictory stale and over-budget evidence", "[unit][xr][loader]") {
            auto request = Request();
            auto evidence = AvailableEvidence();

            SECTION("cancelled before publication") {
                request.cancellationRequested = true;
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, evidence), XRErrors::LoaderPreflightCancelled);
            }
            SECTION("replaced attempt") {
                evidence.attempt = Horo::Tests::IdentityValue<XRLoaderPreflightAttempt>(12);
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, evidence), XRErrors::LoaderPreflightStale);
            }
            SECTION("unsupported loader enum") {
                evidence.loader = XRLoaderAvailability::Count;
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, evidence), XRErrors::LoaderPreflightInvalid);
            }
            SECTION("zero work") {
                evidence.consumedProbeSteps = 0;
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, evidence), XRErrors::LoaderPreflightInvalid);
            }
            SECTION("work exceeds hard bound") {
                evidence.consumedProbeSteps = MaximumXRLoaderPreflightSteps + 1;
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, evidence), XRErrors::LoaderPreflightInvalid);
            }
            SECTION("failed runtime exposes partial generation") {
                evidence.runtime = XRRuntimeAvailability::Unavailable;
                evidence.system = XRSystemAvailability::Unsupported;
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, evidence), XRErrors::LoaderPreflightInvalid);
            }
            SECTION("system success omits runtime generation") {
                evidence.runtimeGeneration = {};
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, evidence), XRErrors::LoaderPreflightInvalid);
            }
            SECTION("observed loader is outside admitted interval") {
                evidence.loaderApiVersion = {1, 3, 0};
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(request, evidence), XRErrors::LoaderIncompatible);
            }
        }

        TEST_CASE("XR loader preflight rejects malformed composition policy", "[unit][xr][loader]") {
            auto request = Request();
            auto verify = [](const XRLoaderPreflightRequest &candidate) {
                RequireFailureIdentity(CreateXRLoaderPreflightSnapshot(candidate, AvailableEvidence()), XRErrors::LoaderPreflightInvalid);
            };

            request.attempt = {};
            verify(request);
            request = Request();
            request.backend = {};
            verify(request);
            request = Request();
            request.installRecord = {};
            verify(request);
            request = Request();
            request.productProfile = {};
            verify(request);
            request = Request();
            request.admittedLoaderVersions = {{1, 2, 0}, {1, 1, 0}};
            verify(request);
            request = Request();
            request.developerOverrideApproved = true;
            verify(request);
            request = Request();
            request.loaderSource = XRLoaderSourcePolicy::Count;
            verify(request);
            request = Request();
            request.productMode = XRPreflightProductMode::Count;
            verify(request);
            request = Request();
            request.runtimeSelection = XRRuntimeSelectionPolicy::Count;
            verify(request);
        }

        TEST_CASE("XR loader preflight revalidation fences replacement and shutdown", "[unit][xr][loader]") {
            const auto request = Request();
            const auto created = CreateXRLoaderPreflightSnapshot(request, AvailableEvidence());
            REQUIRE(created.HasValue());
            const auto snapshot = created.Value();
            REQUIRE(ValidateXRLoaderPreflight(snapshot, request.attempt, request.backend, request.installRecord, request.productProfile)
                        .HasValue());
            RequireFailureIdentity(ValidateXRLoaderPreflight(snapshot, {}, request.backend, request.installRecord, request.productProfile),
                                   XRErrors::LoaderPreflightInvalid);
            RequireFailureIdentity(ValidateXRLoaderPreflight(snapshot, Horo::Tests::IdentityValue<XRLoaderPreflightAttempt>(12),
                                                             request.backend, request.installRecord, request.productProfile),
                                   XRErrors::LoaderPreflightStale);
            RequireFailureIdentity(ValidateXRLoaderPreflight(snapshot, request.attempt, Horo::Tests::IdentityValue<XRBackendId>(99),
                                                             request.installRecord, request.productProfile),
                                   XRErrors::LoaderPreflightStale);
        }

        TEST_CASE("XR loader preflight formation and revalidation allocate no heap storage", "[unit][xr][loader]") {
            const auto request = Request();
            const auto evidence = AvailableEvidence();
            const auto before = Tests::AllocationProbe::Count();
            const auto result = CreateXRLoaderPreflightSnapshot(request, evidence);
            REQUIRE(result.HasValue());
            REQUIRE(
                ValidateXRLoaderPreflight(result.Value(), request.attempt, request.backend, request.installRecord, request.productProfile)
                    .HasValue());
            CHECK(Tests::AllocationProbe::Count() == before);
        }
    }  // namespace
}  // namespace Horo::XR
