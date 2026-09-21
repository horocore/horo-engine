#pragma once

#include "Horo/Navigation/NavigationBackend.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavigationTestAssertions.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>

namespace Horo::Navigation::TestSupport {
    /** @brief Expected successful-query behavior for a provider contract fixture. */
    enum class NavigationProviderFixtureOutcome : std::uint8_t {
        Path,
        NoNavigationData,
    };

    /**
     * @brief Apply the provider-neutral capability, cancellation, and deterministic-result contract.
     * @param provider Production or test provider under qualification.
     * @param request Valid request within the provider's advertised bounds.
     * @param expected Expected result for the fixture topology.
     */
    inline void RequireNavigationProviderContract(const INavigationQueryBackend &provider, const NavigationPathRequest &request,
                                                  const NavigationProviderFixtureOutcome expected) {
        const NavigationProviderCapabilities capabilities = provider.Capabilities();
        REQUIRE(ValidateNavigationProviderCapabilities(capabilities));
        REQUIRE(QueryNavigationSupport(capabilities, request.requirement.query, request.requirement.quality) ==
                NavigationSupport::Available);

        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(provider.FindPath(request, cancellation.Token()), NavigationErrors::QueryCancelled);

        const auto first = provider.FindPath(request, {});
        const auto second = provider.FindPath(request, {});
        if (expected == NavigationProviderFixtureOutcome::NoNavigationData) {
            RequireError(first, NavigationErrors::NoNavigationData);
            RequireError(second, NavigationErrors::NoNavigationData);
            return;
        }

        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().points == second.Value().points);
        REQUIRE(first.Value().lengthMeters == second.Value().lengthMeters);
        REQUIRE(first.Value().points.size() <= request.requirement.limits.maximumResultPoints);
        REQUIRE_FALSE(first.Value().points.empty());
        REQUIRE(first.Value().status == NavigationPathStatus::Reachable);
        REQUIRE(first.Value().stopReason == NavigationPathStopReason::None);
        REQUIRE(first.Value().sourceGeneration == request.topology);
        REQUIRE(std::isfinite(first.Value().cost));
        REQUIRE(first.Value().cost >= 0.0F);
        REQUIRE(first.Value().points.front() == request.start);
        REQUIRE(first.Value().points.back() == request.destination);
    }
}  // namespace Horo::Navigation::TestSupport
