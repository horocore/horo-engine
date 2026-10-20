#pragma once

#include "Horo/Application/NavigationProjectBakeInput.h"
#include "NavigationSceneBakeGraphInternal.h"

namespace Horo::Application::NavigationBakeDetail {
    /** @brief Temporary catalog borrows authored values from the retained immutable project source. */
    struct ProjectBakeSurface final {
        Navigation::NavigationBakeSurfaceKey key;
        const Runtime::NavigationSurfaceComponent *authored{};
        std::optional<Math::Aabb> bounds;
    };

    using ProjectBakeSurfaces = std::map<Navigation::NavigationBakeSurfaceKey, ProjectBakeSurface>;

    /** @brief Produces a complete sorted bounded profile-specific grid without invoking a provider. */
    [[nodiscard]] Result<std::vector<Navigation::NavigationBakeTile>> CaptureProjectBakeTiles(
        const Navigation::NavigationBakeInputSnapshot &input, const Navigation::NavigationDefinition &definition,
        const ProjectBakeSurfaces &surfaces, std::size_t maximumTiles, CancellationToken cancellation);
}  // namespace Horo::Application::NavigationBakeDetail
