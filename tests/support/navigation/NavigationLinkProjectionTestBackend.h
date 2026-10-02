#pragma once

#include "navigation/NavigationLinkValidationTestFixtures.h"

#include <limits>

namespace Horo::Navigation::TestSupport {
    class ProjectionBackend final : public INavigationQueryBackend {
    public:
        mutable std::uint32_t calls{};
        bool failStart{};
        bool failEnd{};
        bool foreignSurface{};
        bool staleTopology{};
        bool malformedHit{};
        bool distantHit{};
        bool shiftHitsOutward{};
        bool changeCapabilities{};
        const CancellationSource *cancelOnProjection{};

        NavigationProviderCapabilities Capabilities() const noexcept override {
            return MakeAvailableGroundedQueryCapabilities(changeCapabilities && calls > 0 ? 2 : 1,
                                                          {.maximumNodeExpansions = 64,
                                                           .maximumResultPoints = 8,
                                                           .maximumSearchDistanceMeters = 10.0F},
                                                          1);
        }

        Result<NavigationPath> FindPath(const NavigationPathRequest &, const CancellationToken &) const override {
            return Result<NavigationPath>::Failure(MakeError(NavigationErrors::OperationUnsupported));
        }

        Result<NavigationProjectionResult> ProjectPoint(const NavigationPointProjectionRequest &request,
                                                        const CancellationToken &) const override {
            ++calls;
            if (cancelOnProjection)
                cancelOnProjection->RequestCancellation();
            const bool start = request.point.x < 1.0F;
            if ((start && failStart) || (!start && failEnd))
                return Result<NavigationProjectionResult>::Failure(MakeError(NavigationErrors::NoNavigationData));
            const auto expectedSurface = start ? 10 : 20;
            const auto surface = Id<SurfaceId>(foreignSurface ? 999 : expectedSurface);
            auto point = request.point;
            if (malformedHit)
                point.y = std::numeric_limits<float>::quiet_NaN();
            if (distantHit)
                point.y += 1.0F;
            if (shiftHitsOutward)
                point.x += start ? -0.2F : 0.2F;
            return Result<NavigationProjectionResult>::Success(
                {.hit = {.position = point,
                         .surface = surface,
                         .area = Id<NavigationAreaId>(1),
                         .provenance = {.world = request.world,
                                        .topology = staleTopology ? Id<NavigationGeneration>(99) : request.topology,
                                        .surface = surface,
                                        .polygonIndex = start ? 0U : 1U}}});
        }
    };

    inline void RequireRule(const NavigationLinkValidationSnapshot &snapshot, const NavigationLinkValidationRule rule,
                            const NavigationBakeLinkInput &link) {
        REQUIRE(snapshot.Authored().empty());
        REQUIRE_FALSE(snapshot.Diagnostics().empty());
        REQUIRE(snapshot.Diagnostics().front().rule == rule);
        REQUIRE(snapshot.Diagnostics().front().start.surface == link.start.surface);
        REQUIRE(snapshot.Diagnostics().front().end.surface == link.end.surface);
        REQUIRE(snapshot.Diagnostics().front().start.position.x == link.start.position.x);
        REQUIRE(snapshot.Diagnostics().front().end.position.x == link.end.position.x);
    }
}  // namespace Horo::Navigation::TestSupport
