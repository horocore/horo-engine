#pragma once

#include "Horo/PlatformServices/PlatformServicesBackend.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::PlatformServices::TestSupport {
    template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
    }

    [[nodiscard]] inline PlatformServiceCapabilitySnapshot AvailableCapabilities(
        const PlatformProviderGeneration generation = {7},
        const PlatformServiceLimits limits = {.maxConcurrentRequests = 8, .maxPageEntries = 64, .maxPayloadBytes = 4096}) {
        PlatformServiceCapabilitySnapshot snapshot{.interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                        PlatformServicesBackendInterfaceMinor},
                                                   .provider = {41},
                                                   .providerGeneration = generation};
        for (std::size_t index = 0; index < snapshot.services.size(); ++index) {
            snapshot.services[index] = {.service = static_cast<PlatformServiceKind>(index),
                                        .availability = PlatformServiceAvailability::Available,
                                        .limits = limits,
                                        .binding = PlatformServiceBindingId{index + 1}};
        }
        snapshot.services[static_cast<std::size_t>(PlatformServiceKind::LeaderboardsAndStats)].leaderboardQueries = {.ranked = true,
                                                                                                                     .aroundSubject = true,
                                                                                                                     .friends = true};
        return snapshot;
    }
}  // namespace Horo::PlatformServices::TestSupport
