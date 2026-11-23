#include "ProjectNavigationLinkClearance.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <cmath>

namespace Horo::Application::Internal {
    namespace {
        using Evidence = Result<std::optional<Navigation::NavigationLinkClearanceEvidence>>;

        /** @brief Checks the finite physical envelope independently of the link's authored endpoint reach. */
        [[nodiscard]] bool ValidEnvelope(const ProjectNavigationClearanceRequest &request) noexcept {
            return std::isfinite(request.radiusMeters) && request.radiusMeters > 0 && std::isfinite(request.heightMeters) &&
                   request.heightMeters > 2 * request.radiusMeters;
        }

        /** @brief Checks the exact Navigation authority before spending Physics query budget. */
        [[nodiscard]] bool ValidRequest(const ProjectNavigationClearanceRequest &request) noexcept {
            return request.bakeFingerprint != Sha256Digest{} && request.profile.IsValid() && request.start.surface.IsValid() &&
                   request.end.surface.IsValid() && request.channel.IsValid() && ValidEnvelope(request);
        }

        /** @brief Builds endpoint overlaps and a continuous translation of the same capsule between them. */
        [[nodiscard]] Result<std::array<Physics::PhysicsQueryGeometry, 3>> ClearanceProbes(
            const ProjectNavigationClearanceRequest &request, const ProjectNavigationClearancePublication &publication) {
            using Probes = Result<std::array<Physics::PhysicsQueryGeometry, 3>>;
            const float halfHeight = request.heightMeters / 2;
            const Physics::PhysicsCapsuleShape capsule{request.radiusMeters, halfHeight - request.radiusMeters};
            const auto offset = publication.canonicalToPhysicsOffset + Math::Vec3{0, halfHeight, 0};
            const auto start = request.start.position + offset;
            const auto end = request.end.position + offset;
            const auto delta = end - start;
            const auto distance = Math::Length(delta);
            if (!Math::IsFinite(start) || !Math::IsFinite(end) || !std::isfinite(distance) || distance <= 0)
                return Probes::Failure(MakeError(Navigation::NavigationErrors::BakeInputInvalid));
            return Probes::Success({Physics::PhysicsCapsuleOverlapQuery{capsule, start}, Physics::PhysicsCapsuleOverlapQuery{capsule, end},
                                    Physics::PhysicsCapsuleSweepQuery{capsule, start, {0, 1, 0}, delta / distance, distance}});
        }

        /** @brief Stamps every probe with the captured authority and consumes at most one blocking hit. */
        [[nodiscard]] Result<bool> IsClear(const Physics::PhysicsQueryEventCapability &capability,
                                           const ProjectNavigationClearancePublication &publication,
                                           const Physics::PhysicsQueryChannelId channel, const Physics::PhysicsQueryGeometry &geometry) {
            const auto identity = capability.Identity();
            const Physics::PhysicsQueryDescriptor descriptor{.world = identity.world,
                                                             .sceneGeneration = publication.sceneGeneration,
                                                             .geometry = geometry,
                                                             .filter = {.channel = channel, .blockingOnly = true},
                                                             .collection = Physics::PhysicsQueryCollection::Any};
            std::array<Physics::PhysicsQueryHit, 1> hits{};
            const auto result = capability.Submit({identity, publication.revision, descriptor}, hits);
            if (result.HasError())
                return Result<bool>::Failure(result.ErrorValue());
            const auto &completion = result.Value();
            if (completion.publicationRevision != publication.revision || completion.completedTick != publication.completedTick)
                return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
            return Result<bool>::Success(completion.result.hitCount == 0 && !completion.result.truncated);
        }
    }  // namespace

    /** @copydoc MeasureProjectNavigationLinkClearance */
    Result<std::optional<Navigation::NavigationLinkClearanceEvidence>> MeasureProjectNavigationLinkClearance(
        const Physics::PhysicsQueryEventCapability &capability, const ProjectNavigationClearancePublication &publication,
        const ProjectNavigationClearanceRequest &request, const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Evidence::Failure(MakeError(Navigation::NavigationErrors::BakeInputCancelled));
        if (!ValidRequest(request))
            return Evidence::Failure(MakeError(Navigation::NavigationErrors::BakeInputInvalid));
        const auto probes = ClearanceProbes(request, publication);
        if (probes.HasError())
            return Evidence::Failure(probes.ErrorValue());
        if (auto valid = capability.ValidateQueryPublication(publication.sceneGeneration, publication.completedTick, publication.revision);
            valid.HasError())
            return Evidence::Failure(valid.ErrorValue());
        for (const auto &probe : probes.Value()) {
            if (cancellation.IsCancellationRequested())
                return Evidence::Failure(MakeError(Navigation::NavigationErrors::BakeInputCancelled));
            const auto clear = IsClear(capability, publication, request.channel, probe);
            if (clear.HasError())
                return Evidence::Failure(clear.ErrorValue());
            if (!clear.Value())
                return Evidence::Success(std::nullopt);
        }
        if (cancellation.IsCancellationRequested())
            return Evidence::Failure(MakeError(Navigation::NavigationErrors::BakeInputCancelled));
        return Evidence::Success(Navigation::NavigationLinkClearanceEvidence{request.bakeFingerprint, request.profile, request.start,
                                                                             request.end, request.radiusMeters, request.heightMeters});
    }
}  // namespace Horo::Application::Internal
