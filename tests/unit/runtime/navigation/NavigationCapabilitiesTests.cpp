#include "Horo/Navigation/NavigationCapabilities.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>

namespace Horo::Navigation {
    namespace {
        constexpr NavigationQueryLimits SupportedLimits{
            .maximumNodeExpansions = 1'024,
            .maximumResultPoints = 128,
            .maximumSearchDistanceMeters = 2'000.0F,
        };

        using CapabilityMutation = void (*)(NavigationProviderCapabilities &);
        const std::array<CapabilityMutation, 9> MalformedCapabilityMutations{
            [](auto &value) {
            value.contractVersion = 2;
        },
            [](auto &value) {
            value.revision = 0;
        },
            [](auto &value) {
            value.availability = NavigationProviderAvailability::Count;
        },
            [](auto &value) {
            value.capabilities.back() = NavigationSupport::Count;
        },
            [](auto &value) {
            value.querySupport.back().back() = NavigationSupport::Count;
        },
            [](auto &value) {
            value.availability = NavigationProviderAvailability::Unavailable;
        },
            [](auto &value) {
            value.queryLimits.front().front() = SupportedLimits;
        },
            [](auto &value) {
            value.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)] = NavigationSupport::Unsupported;
        },
            [](auto &value) {
            value.maximumConcurrentQueries = 0;
        },
        };

        using LimitMutation = void (*)(NavigationQueryLimits &);
        const std::array<LimitMutation, 7> MalformedLimitMutations{
            [](auto &value) {
            value.maximumNodeExpansions = 0;
        },
            [](auto &value) {
            value.maximumResultPoints = 0;
        },
            [](auto &value) {
            value.maximumSearchDistanceMeters = 0.0F;
        },
            [](auto &value) {
            value.maximumSearchDistanceMeters = -1.0F;
        },
            [](auto &value) {
            value.maximumSearchDistanceMeters = std::numeric_limits<float>::infinity();
        },
            [](auto &value) {
            value.maximumSearchDistanceMeters = -std::numeric_limits<float>::infinity();
        },
            [](auto &value) {
            value.maximumSearchDistanceMeters = std::numeric_limits<float>::quiet_NaN();
        },
        };

        NavigationProviderCapabilities AvailableCapabilities(const NavigationQueryKind query = NavigationQueryKind::Path,
                                                             const NavigationQualityLevel quality = NavigationQualityLevel::Balanced) {
            NavigationProviderCapabilities result{
                .revision = 7,
                .availability = NavigationProviderAvailability::Available,
                .maximumConcurrentQueries = 8,
            };
            result.capabilities.fill(NavigationSupport::Unsupported);
            result.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)] = NavigationSupport::Available;
            for (auto &qualities : result.querySupport)
                qualities.fill(NavigationSupport::Unsupported);
            result.querySupport[static_cast<std::size_t>(query)][static_cast<std::size_t>(quality)] = NavigationSupport::Available;
            result.queryLimits[static_cast<std::size_t>(query)][static_cast<std::size_t>(quality)] = SupportedLimits;
            return result;
        }

        NavigationQueryRequirement Requirement(const NavigationQueryKind query = NavigationQueryKind::Path,
                                               const NavigationQualityLevel quality = NavigationQualityLevel::Balanced) {
            return NavigationQueryRequirement{.query = query, .quality = quality, .limits = SupportedLimits};
        }

        void ExpectError(const Result<void> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().domain.Value() == descriptor.domain.Value());
            REQUIRE(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        TEST_CASE("Navigation capability defaults do not advertise provider support", "[navigation][capability]") {
            NavigationProviderCapabilities capabilities;
            REQUIRE_FALSE(ValidateNavigationProviderCapabilities(capabilities));
            capabilities.revision = 1;
            REQUIRE(ValidateNavigationProviderCapabilities(capabilities));
            REQUIRE(QueryNavigationCapability(capabilities, NavigationCapability::GroundedQueries) == NavigationSupport::Unknown);
            REQUIRE(QueryNavigationSupport(capabilities, NavigationQueryKind::Path, NavigationQualityLevel::Balanced) ==
                    NavigationSupport::Unknown);
            static_assert(std::is_trivially_copyable_v<NavigationProviderCapabilities>);
        }

        TEST_CASE("Available path capability construction centralizes provider evidence", "[navigation][capability]") {
            const auto capabilities = MakeAvailablePathQueryCapabilities(5, SupportedLimits, 3);

            REQUIRE(ValidateNavigationProviderCapabilities(capabilities));
            REQUIRE(capabilities.revision == 5);
            REQUIRE(capabilities.maximumConcurrentQueries == 3);
            REQUIRE(QueryNavigationCapability(capabilities, NavigationCapability::GroundedQueries) == NavigationSupport::Available);
            for (std::size_t quality = 0; quality < static_cast<std::size_t>(NavigationQualityLevel::Count); ++quality) {
                REQUIRE(QueryNavigationSupport(capabilities, NavigationQueryKind::Path, static_cast<NavigationQualityLevel>(quality)) ==
                        NavigationSupport::Available);
                REQUIRE(capabilities.queryLimits[static_cast<std::size_t>(NavigationQueryKind::Path)][quality] == SupportedLimits);
            }
            REQUIRE(QueryNavigationSupport(capabilities, NavigationQueryKind::NearestPoint, NavigationQualityLevel::Balanced) ==
                    NavigationSupport::Unsupported);
        }

        TEST_CASE("Available grounded capability construction covers bounded spatial queries", "[navigation][capability]") {
            const auto capabilities = MakeAvailableGroundedQueryCapabilities(9, SupportedLimits, 4);

            REQUIRE(ValidateNavigationProviderCapabilities(capabilities));
            for (std::size_t query = 0; query < static_cast<std::size_t>(NavigationQueryKind::Count); ++query) {
                REQUIRE(QueryNavigationSupport(capabilities, static_cast<NavigationQueryKind>(query), NavigationQualityLevel::High) ==
                        NavigationSupport::Available);
                REQUIRE(capabilities.queryLimits[query][static_cast<std::size_t>(NavigationQualityLevel::High)] == SupportedLimits);
            }
        }

        TEST_CASE("Navigation capabilities keep omitted unavailable and available compositions distinct", "[navigation][capability]") {
            NavigationProviderCapabilities omitted{
                .revision = 1,
                .availability = NavigationProviderAvailability::Omitted,
            };
            omitted.capabilities.fill(NavigationSupport::Unsupported);
            for (auto &qualities : omitted.querySupport)
                qualities.fill(NavigationSupport::Unsupported);
            REQUIRE(ValidateNavigationProviderCapabilities(omitted));

            omitted.capabilities.back() = NavigationSupport::Unavailable;
            REQUIRE_FALSE(ValidateNavigationProviderCapabilities(omitted));

            NavigationProviderCapabilities unavailable{
                .revision = 2,
                .availability = NavigationProviderAvailability::Unavailable,
            };
            unavailable.capabilities.fill(NavigationSupport::Unavailable);
            for (auto &qualities : unavailable.querySupport)
                qualities.fill(NavigationSupport::Unavailable);
            REQUIRE(ValidateNavigationProviderCapabilities(unavailable));
            unavailable.querySupport.front().front() = NavigationSupport::Available;
            unavailable.queryLimits.front().front() = SupportedLimits;
            REQUIRE_FALSE(ValidateNavigationProviderCapabilities(unavailable));

            REQUIRE(ValidateNavigationProviderCapabilities(AvailableCapabilities()));
        }

        TEST_CASE("Every navigation provider capability preserves all support states", "[navigation][capability]") {
            for (std::size_t index = 1; index < static_cast<std::size_t>(NavigationCapability::Count); ++index) {
                auto capabilities = AvailableCapabilities();
                const auto capability = static_cast<NavigationCapability>(index);
                for (const auto support : {NavigationSupport::Unknown, NavigationSupport::Unsupported, NavigationSupport::Unavailable,
                                           NavigationSupport::Available}) {
                    capabilities.capabilities[index] = support;
                    REQUIRE(ValidateNavigationProviderCapabilities(capabilities));
                    REQUIRE(QueryNavigationCapability(capabilities, capability) == support);
                }
            }
        }

        TEST_CASE("Every grounded query and quality pair has independent typed support", "[navigation][capability]") {
            for (std::size_t query = 0; query < static_cast<std::size_t>(NavigationQueryKind::Count); ++query) {
                for (std::size_t quality = 0; quality < static_cast<std::size_t>(NavigationQualityLevel::Count); ++quality) {
                    const auto queryKind = static_cast<NavigationQueryKind>(query);
                    const auto qualityLevel = static_cast<NavigationQualityLevel>(quality);
                    for (const auto support : {NavigationSupport::Unknown, NavigationSupport::Unsupported, NavigationSupport::Unavailable,
                                               NavigationSupport::Available}) {
                        auto capabilities = AvailableCapabilities(queryKind, qualityLevel);
                        capabilities.querySupport[query][quality] = support;
                        capabilities.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)] = support;
                        if (support != NavigationSupport::Available) {
                            capabilities.queryLimits[query][quality] = {};
                            capabilities.maximumConcurrentQueries = 0;
                        }
                        REQUIRE(ValidateNavigationProviderCapabilities(capabilities));
                        REQUIRE(QueryNavigationSupport(capabilities, queryKind, qualityLevel) == support);
                    }
                }
            }
        }

        TEST_CASE("Navigation capability validation rejects malformed typed evidence", "[navigation][capability]") {
            for (const auto mutate : MalformedCapabilityMutations) {
                auto capabilities = AvailableCapabilities();
                mutate(capabilities);
                REQUIRE_FALSE(ValidateNavigationProviderCapabilities(capabilities));
                REQUIRE(QueryNavigationCapability(capabilities, NavigationCapability::GroundedQueries) == NavigationSupport::Unknown);
            }

            auto staleLimits = AvailableCapabilities();
            staleLimits.querySupport[static_cast<std::size_t>(NavigationQueryKind::Path)]
                                    [static_cast<std::size_t>(NavigationQualityLevel::Balanced)] = NavigationSupport::Unsupported;
            staleLimits.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)] = NavigationSupport::Unsupported;
            staleLimits.maximumConcurrentQueries = 0;
            REQUIRE_FALSE(ValidateNavigationProviderCapabilities(staleLimits));

            const auto valid = AvailableCapabilities();
            for (const auto capability : {NavigationCapability::Count, static_cast<NavigationCapability>(255)})
                REQUIRE(QueryNavigationCapability(valid, capability) == NavigationSupport::Unknown);
            for (const auto query : {NavigationQueryKind::Count, static_cast<NavigationQueryKind>(255)})
                REQUIRE(QueryNavigationSupport(valid, query, NavigationQualityLevel::Balanced) == NavigationSupport::Unknown);
            for (const auto quality : {NavigationQualityLevel::Count, static_cast<NavigationQualityLevel>(255)})
                REQUIRE(QueryNavigationSupport(valid, NavigationQueryKind::Path, quality) == NavigationSupport::Unknown);
        }

        TEST_CASE("Available navigation query limits are finite positive and bounded", "[navigation][capability][limits]") {
            auto maximum = AvailableCapabilities();
            auto &limits = maximum.queryLimits[static_cast<std::size_t>(NavigationQueryKind::Path)]
                                              [static_cast<std::size_t>(NavigationQualityLevel::Balanced)];
            limits = {
                .maximumNodeExpansions = std::numeric_limits<std::uint32_t>::max(),
                .maximumResultPoints = std::numeric_limits<std::uint32_t>::max(),
                .maximumSearchDistanceMeters = std::numeric_limits<float>::max(),
            };
            maximum.maximumConcurrentQueries = std::numeric_limits<std::uint32_t>::max();
            REQUIRE(ValidateNavigationProviderCapabilities(maximum));

            for (const auto mutate : MalformedLimitMutations) {
                auto capabilities = AvailableCapabilities();
                auto &candidate = capabilities.queryLimits[static_cast<std::size_t>(NavigationQueryKind::Path)]
                                                          [static_cast<std::size_t>(NavigationQualityLevel::Balanced)];
                mutate(candidate);
                REQUIRE_FALSE(ValidateNavigationProviderCapabilities(capabilities));
            }
        }

        TEST_CASE("Navigation query admission distinguishes malformed stale unsupported unavailable and excessive requests",
                  "[navigation][admission]") {
            const auto available = AvailableCapabilities();
            REQUIRE(AdmitNavigationQuery(available, 7, Requirement()).HasValue());
            ExpectError(AdmitNavigationQuery(available, 6, Requirement()), NavigationErrors::CapabilityStale);
            ExpectError(AdmitNavigationQuery(available, 0, Requirement()), NavigationErrors::CapabilityDescriptorInvalid);

            auto malformed = Requirement();
            malformed.query = NavigationQueryKind::Count;
            ExpectError(AdmitNavigationQuery(available, 7, malformed), NavigationErrors::CapabilityDescriptorInvalid);
            malformed = Requirement();
            malformed.quality = NavigationQualityLevel::Count;
            ExpectError(AdmitNavigationQuery(available, 7, malformed), NavigationErrors::CapabilityDescriptorInvalid);
            malformed = Requirement();
            malformed.limits.maximumSearchDistanceMeters = std::numeric_limits<float>::quiet_NaN();
            ExpectError(AdmitNavigationQuery(available, 7, malformed), NavigationErrors::CapabilityDescriptorInvalid);

            ExpectError(AdmitNavigationQuery(available, 7, Requirement(NavigationQueryKind::Raycast)),
                        NavigationErrors::OperationUnsupported);

            auto unavailable = AvailableCapabilities();
            unavailable.querySupport[static_cast<std::size_t>(NavigationQueryKind::Path)]
                                    [static_cast<std::size_t>(NavigationQualityLevel::Balanced)] = NavigationSupport::Unavailable;
            unavailable.queryLimits[static_cast<std::size_t>(NavigationQueryKind::Path)]
                                   [static_cast<std::size_t>(NavigationQualityLevel::Balanced)] = {};
            unavailable.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)] = NavigationSupport::Unavailable;
            unavailable.maximumConcurrentQueries = 0;
            REQUIRE(ValidateNavigationProviderCapabilities(unavailable));
            ExpectError(AdmitNavigationQuery(unavailable, 7, Requirement()), NavigationErrors::CapabilityUnavailable);

            auto excessive = Requirement();
            excessive.limits.maximumNodeExpansions += 1;
            ExpectError(AdmitNavigationQuery(available, 7, excessive), NavigationErrors::QueryLimitExceeded);
            excessive = Requirement();
            excessive.limits.maximumResultPoints += 1;
            ExpectError(AdmitNavigationQuery(available, 7, excessive), NavigationErrors::QueryLimitExceeded);
            excessive = Requirement();
            excessive.limits.maximumSearchDistanceMeters = 2'000.5F;
            ExpectError(AdmitNavigationQuery(available, 7, excessive), NavigationErrors::QueryLimitExceeded);
        }

        TEST_CASE("Navigation query admission rejects every malformed request boundary", "[navigation][admission][limits]") {
            const auto available = AvailableCapabilities();
            for (const auto query : {NavigationQueryKind::Count, static_cast<NavigationQueryKind>(255)}) {
                auto malformed = Requirement();
                malformed.query = query;
                ExpectError(AdmitNavigationQuery(available, 7, malformed), NavigationErrors::CapabilityDescriptorInvalid);
            }
            for (const auto quality : {NavigationQualityLevel::Count, static_cast<NavigationQualityLevel>(255)}) {
                auto malformed = Requirement();
                malformed.quality = quality;
                ExpectError(AdmitNavigationQuery(available, 7, malformed), NavigationErrors::CapabilityDescriptorInvalid);
            }

            for (const auto mutate : MalformedLimitMutations) {
                auto malformed = Requirement();
                mutate(malformed.limits);
                ExpectError(AdmitNavigationQuery(available, 7, malformed), NavigationErrors::CapabilityDescriptorInvalid);
            }
        }

        TEST_CASE("Navigation query admission rejects every malformed capability snapshot", "[navigation][admission][capability]") {
            for (const auto mutate : MalformedCapabilityMutations) {
                auto malformed = AvailableCapabilities();
                mutate(malformed);
                ExpectError(AdmitNavigationQuery(malformed, 7, Requirement()), NavigationErrors::CapabilityDescriptorInvalid);
            }

            auto unknown = AvailableCapabilities();
            unknown.querySupport[static_cast<std::size_t>(NavigationQueryKind::Path)]
                                [static_cast<std::size_t>(NavigationQualityLevel::Balanced)] = NavigationSupport::Unknown;
            unknown.queryLimits[static_cast<std::size_t>(NavigationQueryKind::Path)]
                               [static_cast<std::size_t>(NavigationQualityLevel::Balanced)] = {};
            unknown.capabilities[static_cast<std::size_t>(NavigationCapability::GroundedQueries)] = NavigationSupport::Unknown;
            unknown.maximumConcurrentQueries = 0;
            REQUIRE(ValidateNavigationProviderCapabilities(unknown));
            ExpectError(AdmitNavigationQuery(unknown, 7, Requirement()), NavigationErrors::CapabilityUnavailable);
        }

        TEST_CASE("Unsupported navigation work is rejected before caller queue mutation", "[navigation][admission][dispatch]") {
            const auto capabilities = AvailableCapabilities();
            std::uint32_t queuedCount = 0;
            const auto submit = [&capabilities, &queuedCount](const NavigationQueryRequirement &requirement) {
                const auto admission = AdmitNavigationQuery(capabilities, 7, requirement);
                if (admission.HasError())
                    return admission;
                ++queuedCount;
                return Result<void>::Success();
            };

            const auto unsupported = submit(Requirement(NavigationQueryKind::Raycast));
            ExpectError(unsupported, NavigationErrors::OperationUnsupported);
            REQUIRE(queuedCount == 0);

            REQUIRE(submit(Requirement()).HasValue());
            REQUIRE(queuedCount == 1);
        }
    }  // namespace
}  // namespace Horo::Navigation
