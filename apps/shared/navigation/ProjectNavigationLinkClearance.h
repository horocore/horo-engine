#pragma once

/** @file ProjectNavigationLinkClearance.h
 * @brief Host-only conversion of Physics corridor measurements into owned Navigation evidence.
 */

#include "Horo/Navigation/NavigationLinkValidation.h"
#include "Horo/Physics/PhysicsQueryEventCapability.h"

namespace Horo::Application::Internal {
    /** @brief Exact binding between the owner-observed Physics publication and canonical Navigation coordinates. */
    struct ProjectNavigationClearancePublication final {
        std::uint64_t sceneGeneration{};
        std::uint64_t completedTick{};
        std::uint64_t revision{};
        Math::Vec3 canonicalToPhysicsOffset; /**< Translation from canonical metres to the captured Physics origin, with shared Y up. */
    };

    /** @brief One projected pair and the upright capsule whose free envelope must be verified. */
    struct ProjectNavigationClearanceRequest final {
        Sha256Digest bakeFingerprint;
        Navigation::NavigationAgentProfileId profile;
        Navigation::NavigationBakeLinkEndpoint start;
        Navigation::NavigationBakeLinkEndpoint end;
        Physics::PhysicsQueryChannelId channel;
        float radiusMeters{};
        float heightMeters{};
    };

    /** @brief Measures a straight upright capsule corridor in one owner-thread Physics publication.
     * @param capability Query access issued to the exact current Physics world.
     * @param publication Owner-captured tick, Scene and coordinate binding.
     * @param request Exact projected pair and requested clearance dimensions.
     * @param cancellation Cooperative cancellation observed between each bounded query.
     * @return Owned evidence for the measured dimensions, empty for a blocking hit, or a typed validation/provider failure.
     * @pre Called on the Physics owner lane after a completed fixed tick. The caller binds the publication and origin to the same
     *      immutable post-change geometry capture as the Navigation fingerprint. Both projected endpoints represent feet positions.
     */
    [[nodiscard]] Result<std::optional<Navigation::NavigationLinkClearanceEvidence>> MeasureProjectNavigationLinkClearance(
        const Physics::PhysicsQueryEventCapability &capability, const ProjectNavigationClearancePublication &publication,
        const ProjectNavigationClearanceRequest &request, const CancellationToken &cancellation = {});
}  // namespace Horo::Application::Internal
