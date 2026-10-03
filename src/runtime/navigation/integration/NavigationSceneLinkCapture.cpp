#include "Horo/Navigation/NavigationSceneLinkCapture.h"

#include <algorithm>

namespace Horo::Navigation {
    /** @copydoc CaptureNavigationSceneLinks */
    Result<std::vector<NavigationBakeLinkInput>> CaptureNavigationSceneLinks(const std::span<const NavigationSceneLinkCaptureInput> links,
                                                                             const NavigationAgentProfileId profile) {
        if (!profile.IsValid())
            return Result<std::vector<NavigationBakeLinkInput>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
        if (links.size() > NavigationLinkValidationLimits::MaximumAuthoredLinks)
            return Result<std::vector<NavigationBakeLinkInput>>::Failure(MakeError(NavigationErrors::BakeInputCapacityExceeded));
        std::vector<NavigationBakeLinkInput> result;
        result.reserve(links.size());
        for (const auto &input : links) {
            if (!input.link)
                return Result<std::vector<NavigationBakeLinkInput>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            if (const auto valid = Runtime::ValidateNavigationLinkComponent(*input.link); valid.HasError())
                return Result<std::vector<NavigationBakeLinkInput>>::Failure(valid.ErrorValue());
            const auto &link = *input.link;
            if (!link.enabled || std::ranges::find(link.profiles, profile) == link.profiles.end())
                continue;
            const auto matrix = input.localToCanonicalMeters.TryToMatrix();
            if (matrix.HasError() || input.localToCanonicalMeters.scale.x <= 0.0F || input.localToCanonicalMeters.scale.y <= 0.0F ||
                input.localToCanonicalMeters.scale.z <= 0.0F)
                return Result<std::vector<NavigationBakeLinkInput>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            const auto start = Math::TryTransformPoint(matrix.Value(), link.start.localPosition);
            const auto end = Math::TryTransformPoint(matrix.Value(), link.end.localPosition);
            if (start.HasError() || end.HasError())
                return Result<std::vector<NavigationBakeLinkInput>>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
            result.push_back(
                {.id = link.id,
                 .profile = profile,
                 .start = {.surface = link.start.surface,
                           .position = start.Value(),
                           .connectionRadiusMeters = link.start.connectionRadiusMeters},
                 .end = {.surface = link.end.surface, .position = end.Value(), .connectionRadiusMeters = link.end.connectionRadiusMeters},
                 .kind = link.kind,
                 .direction = link.direction,
                 .traversalCost = link.traversalCost});
        }
        std::ranges::sort(result, {}, &NavigationBakeLinkInput::id);
        if (std::ranges::adjacent_find(result, {}, &NavigationBakeLinkInput::id) != result.end())
            return Result<std::vector<NavigationBakeLinkInput>>::Failure(MakeError(NavigationErrors::DescriptorConflict));
        return Result<std::vector<NavigationBakeLinkInput>>::Success(std::move(result));
    }
}  // namespace Horo::Navigation
