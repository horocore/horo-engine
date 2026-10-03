#pragma once

/** @file NavigationSceneLinkCapture.h
 * @brief One-way committed Scene-link conversion into canonical profile-specific bake inputs.
 */

#include "Horo/Navigation/NavigationLinkValidation.h"
#include "Horo/Runtime/Scene/NavigationSceneComponents.h"

namespace Horo::Navigation {
    /** @brief One committed Scene object's link and canonical-metre object transform borrowed during capture only. */
    struct NavigationSceneLinkCaptureInput final {
        const Runtime::NavigationLinkComponent *link{};
        Math::Transform localToCanonicalMeters;
    };

    /**
     * @brief Copies enabled links selecting one exact profile after committed Scene-wide validation.
     * @param links Links and transforms from one committed Scene revision; no pointers survive this synchronous call.
     * @param profile Exact partition profile; links selecting other profiles and disabled links remain authored but are omitted.
     * @return Owned canonical ordered endpoints, kind, direction and cost; malformed transforms/components fail the transaction.
     * @pre The host has validated the complete committed Scene and captures its revision in NavigationBakeInputSnapshot.
     * @note Connection radii are already metres and are not scaled by an object's authored transform.
     */
    [[nodiscard]] Result<std::vector<NavigationBakeLinkInput>> CaptureNavigationSceneLinks(
        std::span<const NavigationSceneLinkCaptureInput> links, NavigationAgentProfileId profile);
}  // namespace Horo::Navigation
