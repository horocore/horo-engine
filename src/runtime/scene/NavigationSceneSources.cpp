#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Runtime/Scene/NavigationSceneComponents.h"

#include <algorithm>

namespace Horo::Runtime {
    /** @copydoc ValidateNavigationSourceComponent */
    Result<void> ValidateNavigationSourceComponent(const NavigationSourceComponent &component) {
        if (!component.collider.IsValid() || !component.surface.IsValid() || !component.area.IsValid() || !component.filter.IsValid() ||
            component.schemaVersion != 1 || component.generation == 0)
            return Result<void>::Failure(
                MakeError(Navigation::NavigationErrors::SceneComponentInvalid,
                          "Navigation sources require exact collider, surface, area, filter, schema and generation."));
        return Result<void>::Success();
    }

    /** @copydoc ValidateNavigationSourceBindings */
    Result<void> ValidateNavigationSourceBindings(const std::span<const NavigationSourceComponent> sources,
                                                  const std::span<const ColliderComponent> colliders) {
        if (sources.size() > MaximumNavigationSourcesPerObject)
            return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid,
                                                   "Navigation source bindings exceed the qualified per-object limit."));
        for (std::size_t index = 0; index < sources.size(); ++index) {
            const auto &source = sources[index];
            if (auto valid = ValidateNavigationSourceComponent(source); valid.HasError())
                return valid;
            if (std::ranges::count(colliders, source.collider, &ColliderComponent::collider) != 1)
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid,
                                                       "Navigation sources must reference one exact collider on their containing object."));
            if (std::ranges::any_of(sources.first(index), [&source](const auto &existing) {
                return existing.collider == source.collider && existing.surface == source.surface;
            }))
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentConflict,
                                                       "A collider-to-surface navigation source binding must be unique on its object."));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
