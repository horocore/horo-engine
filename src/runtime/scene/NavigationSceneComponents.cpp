#include "Horo/Runtime/Scene/NavigationSceneComponents.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor, std::string message) {
            return Result<void>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] bool IsValid(const NavigationLocalBounds &bounds) noexcept {
            return Math::IsFinite(bounds.center) && Math::IsFinite(bounds.halfExtents) && bounds.halfExtents.x > 0.0F &&
                   bounds.halfExtents.y > 0.0F && bounds.halfExtents.z > 0.0F;
        }

        [[nodiscard]] bool IsValid(const NavigationCylinderVolume &volume) noexcept {
            return Math::IsFinite(volume.center) && std::isfinite(volume.radius) && std::isfinite(volume.halfHeight) &&
                   volume.radius > 0.0F && volume.halfHeight > 0.0F;
        }

        [[nodiscard]] bool IsValidProfileSet(const std::span<const Navigation::NavigationAgentProfileId> profiles) {
            if (profiles.empty() || profiles.size() > MaximumNavigationSurfaceProfiles)
                return false;
            std::unordered_set<std::uint64_t> identities;
            identities.reserve(profiles.size());
            return std::ranges::all_of(profiles, [&identities](const Navigation::NavigationAgentProfileId profile) {
                return profile.IsValid() && identities.insert(profile.Value()).second;
            });
        }

        [[nodiscard]] bool HasProfile(const NavigationSurfaceComponent &surface,
                                      const Navigation::NavigationAgentProfileId profile) noexcept {
            return std::ranges::find(surface.profiles, profile) != surface.profiles.end();
        }

        using NavigationSurfaceMap = std::unordered_map<std::uint64_t, const NavigationSurfaceComponent *>;

        [[nodiscard]] Result<void> ValidateSurfaces(const std::span<const NavigationSceneComponentView> components,
                                                    NavigationSurfaceMap &surfaces) {
            for (const NavigationSceneComponentView component : components) {
                if (component.surface == nullptr)
                    continue;
                if (Result<void> valid = ValidateNavigationSurfaceComponent(*component.surface); valid.HasError())
                    return valid;
                if (!surfaces.emplace(component.surface->id.Value(), component.surface).second) {
                    return Failure(Navigation::NavigationErrors::SceneComponentConflict,
                                   "Navigation surface identities must be unique within one committed Scene snapshot.");
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateUniqueIdentity(Result<void> componentValidation, const std::uint64_t identity,
                                                          std::unordered_set<std::uint64_t> &identities,
                                                          const std::string_view conflictMessage) {
            if (componentValidation.HasError())
                return componentValidation;
            if (!identities.insert(identity).second)
                return Failure(Navigation::NavigationErrors::SceneComponentConflict, std::string{conflictMessage});
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateSurfaceReference(Result<void> componentValidation, const std::uint64_t identity,
                                                            const Navigation::SurfaceId surface,
                                                            std::unordered_set<std::uint64_t> &identities,
                                                            const NavigationSurfaceMap &surfaces, const std::string_view conflictMessage,
                                                            const std::string_view missingMessage) {
            if (Result<void> valid = ValidateUniqueIdentity(std::move(componentValidation), identity, identities, conflictMessage);
                valid.HasError())
                return valid;
            if (!surfaces.contains(surface.Value()))
                return Failure(Navigation::NavigationErrors::SceneSurfaceMissing, std::string{missingMessage});
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRegion(const NavigationRegionComponent &component, const NavigationSurfaceMap &surfaces,
                                                  std::unordered_set<std::uint64_t> &identities) {
            return ValidateSurfaceReference(ValidateNavigationRegionComponent(component), component.id.Value(), component.surface,
                                            identities, surfaces,
                                            "Navigation region identities must be unique within one committed Scene snapshot.",
                                            "Navigation regions must reference an exact surface in the same committed Scene snapshot.");
        }

        [[nodiscard]] Result<void> ValidateModifier(const NavigationModifierComponent &component, const NavigationSurfaceMap &surfaces,
                                                    std::unordered_set<std::uint64_t> &identities) {
            return ValidateSurfaceReference(ValidateNavigationModifierComponent(component), component.id.Value(), component.surface,
                                            identities, surfaces,
                                            "Navigation modifier identities must be unique within one committed Scene snapshot.",
                                            "Navigation modifiers must reference an exact surface in the same committed Scene snapshot.");
        }

        [[nodiscard]] Result<void> ValidateLink(const NavigationLinkComponent &component, const NavigationSurfaceMap &surfaces,
                                                std::unordered_set<std::uint64_t> &identities) {
            if (Result<void> valid =
                    ValidateUniqueIdentity(ValidateNavigationLinkComponent(component), component.id.Value(), identities,
                                           "Navigation link identities must be unique within one committed Scene snapshot.");
                valid.HasError())
                return valid;
            const auto start = surfaces.find(component.start.surface.Value());
            const auto end = surfaces.find(component.end.surface.Value());
            if (start == surfaces.end() || end == surfaces.end()) {
                return Failure(Navigation::NavigationErrors::SceneSurfaceMissing,
                               "Navigation link endpoints must reference exact surfaces in the same committed Scene snapshot.");
            }
            if (std::ranges::any_of(component.profiles, [&](const Navigation::NavigationAgentProfileId profile) {
                return !HasProfile(*start->second, profile) || !HasProfile(*end->second, profile);
            })) {
                return Failure(Navigation::NavigationErrors::SceneProfileMismatch,
                               "Navigation link profiles must be selected by both endpoint surfaces.");
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAgent(const NavigationAgentComponent &component) {
            if (component.schemaVersion != 1 || !component.profile.IsValid() || !component.filter.IsValid() ||
                component.movementCapability >= Navigation::NavigationAgentMovementCapability::Count ||
                (component.radiusOverride.has_value() &&
                 (!std::isfinite(*component.radiusOverride) || *component.radiusOverride <= 0.0F))) {
                return Failure(Navigation::NavigationErrors::AgentDescriptorInvalid,
                               "Navigation agents require valid schema, profile, filter, movement capability, and radius values.");
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateReferences(const std::span<const NavigationSceneComponentView> components,
                                                      const NavigationSurfaceMap &surfaces) {
            std::unordered_set<std::uint64_t> regionIds;
            std::unordered_set<std::uint64_t> modifierIds;
            std::unordered_set<std::uint64_t> linkIds;
            regionIds.reserve(components.size());
            modifierIds.reserve(components.size());
            linkIds.reserve(components.size());
            for (const NavigationSceneComponentView component : components) {
                if (component.region != nullptr) {
                    if (Result<void> valid = ValidateRegion(*component.region, surfaces, regionIds); valid.HasError())
                        return valid;
                }
                if (component.modifier != nullptr) {
                    if (Result<void> valid = ValidateModifier(*component.modifier, surfaces, modifierIds); valid.HasError())
                        return valid;
                }
                if (component.link != nullptr) {
                    if (Result<void> valid = ValidateLink(*component.link, surfaces, linkIds); valid.HasError())
                        return valid;
                }
                if (component.agent != nullptr) {
                    if (Result<void> valid = ValidateAgent(*component.agent); valid.HasError())
                        return valid;
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateNavigationSurfaceComponent */
    Result<void> ValidateNavigationSurfaceComponent(const NavigationSurfaceComponent &component) {
        if (!component.id.IsValid() || !component.definition.IsValid() || component.schemaVersion != 1 || component.generation == 0 ||
            component.bakeScope >= NavigationBakeScope::Count || component.profiles.empty() ||
            component.profiles.size() > MaximumNavigationSurfaceProfiles) {
            return Failure(Navigation::NavigationErrors::SceneComponentInvalid,
                           "Navigation surfaces require valid identities, schema, generation, definition, scope, and bounded profiles.");
        }
        if ((component.bakeScope == NavigationBakeScope::LocalBounds) != component.localBounds.has_value() ||
            (component.localBounds.has_value() && !IsValid(*component.localBounds))) {
            return Failure(Navigation::NavigationErrors::SceneComponentInvalid,
                           "Navigation surface bounds must be finite, positive, and present only for a local-bounds scope.");
        }
        if (!IsValidProfileSet(component.profiles))
            return Failure(Navigation::NavigationErrors::SceneComponentInvalid,
                           "Navigation surface profile identities must be non-zero and unique.");
        return Result<void>::Success();
    }

    /** @copydoc ValidateNavigationRegionComponent */
    Result<void> ValidateNavigationRegionComponent(const NavigationRegionComponent &component) {
        if (!component.id.IsValid() || !component.surface.IsValid() || component.schemaVersion != 1 || component.generation == 0 ||
            component.sourceSelection >= NavigationRegionSourceSelection::Count || component.mode >= NavigationRegionMode::Count ||
            !IsValid(component.localBounds)) {
            return Failure(Navigation::NavigationErrors::SceneComponentInvalid,
                           "Navigation regions require valid identities, schema, generation, bounds, source selection, and mode.");
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateNavigationModifierComponent */
    Result<void> ValidateNavigationModifierComponent(const NavigationModifierComponent &component) {
        const auto *box = std::get_if<NavigationLocalBounds>(&component.volume);
        if (const auto *cylinder = std::get_if<NavigationCylinderVolume>(&component.volume);
            !component.id.IsValid() || !component.surface.IsValid() || component.schemaVersion != 1 || component.generation == 0 ||
            component.operation >= NavigationModifierOperation::Count || (box == nullptr && cylinder == nullptr) ||
            (box != nullptr && !IsValid(*box)) || (cylinder != nullptr && !IsValid(*cylinder))) {
            return Failure(Navigation::NavigationErrors::SceneComponentInvalid,
                           "Navigation modifiers require valid identities, schema, generation, surface, shape, and operation.");
        }

        const bool validArea = component.area.has_value() && component.area->IsValid();
        const bool validCost =
            component.traversalCost.has_value() && std::isfinite(*component.traversalCost) && *component.traversalCost >= 0.0F;
        using enum NavigationModifierOperation;
        if (const bool policyValid =
                (component.operation == Exclude && !component.area.has_value() && !component.traversalCost.has_value()) ||
                (component.operation == OverrideArea && validArea && !component.traversalCost.has_value()) ||
                (component.operation == OverrideAreaAndCost && validArea && validCost);
            !policyValid) {
            return Failure(Navigation::NavigationErrors::SceneComponentInvalid,
                           "Navigation modifier area and traversal cost must match the selected operation exactly.");
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateNavigationLinkComponent */
    Result<void> ValidateNavigationLinkComponent(const NavigationLinkComponent &component) {
        const auto validEndpoint = [](const NavigationLinkEndpoint &endpoint) noexcept {
            return endpoint.surface.IsValid() && Math::IsFinite(endpoint.localPosition) && std::isfinite(endpoint.connectionRadiusMeters) &&
                   endpoint.connectionRadiusMeters > 0.0F;
        };
        if (const bool sameEndpoint =
                component.start.surface == component.end.surface && component.start.localPosition == component.end.localPosition;
            !component.id.IsValid() || component.schemaVersion != 1 || component.generation == 0 || !validEndpoint(component.start) ||
            !validEndpoint(component.end) || sameEndpoint || component.kind >= NavigationLinkKind::Count ||
            component.direction >= NavigationLinkDirection::Count || !IsValidProfileSet(component.profiles) ||
            !std::isfinite(component.traversalCost) || component.traversalCost < 0.0F)
            return Failure(Navigation::NavigationErrors::SceneComponentInvalid,
                           "Navigation links require valid identities, schema, generation, endpoints, kind, direction, profiles, and "
                           "cost.");
        return Result<void>::Success();
    }

    /** @copydoc ValidateNavigationAgentComponent */
    Result<void> ValidateNavigationAgentComponent(const NavigationAgentComponent &component) {
        return ValidateAgent(component);
    }

    /** @copydoc ValidateNavigationSceneComponents */
    Result<void> ValidateNavigationSceneComponents(const std::span<const NavigationSurfaceComponent> surfaces,
                                                   const std::span<const NavigationRegionComponent> regions,
                                                   const std::span<const NavigationModifierComponent> modifiers,
                                                   const std::span<const NavigationLinkComponent> links,
                                                   const std::span<const NavigationAgentComponent> agents) {
        std::vector<NavigationSceneComponentView> views;
        views.reserve(surfaces.size() + regions.size() + modifiers.size() + links.size() + agents.size());
        for (const NavigationSurfaceComponent &surface : surfaces)
            views.push_back({.surface = &surface});
        for (const NavigationRegionComponent &region : regions)
            views.push_back({.region = &region});
        for (const NavigationModifierComponent &modifier : modifiers)
            views.push_back({.modifier = &modifier});
        for (const NavigationLinkComponent &link : links)
            views.push_back({.link = &link});
        for (const NavigationAgentComponent &agent : agents)
            views.push_back({.agent = &agent});
        return ValidateNavigationSceneComponentViews(views);
    }

    /** @copydoc ValidateNavigationSceneComponentViews */
    Result<void> ValidateNavigationSceneComponentViews(const std::span<const NavigationSceneComponentView> components) {
        NavigationSurfaceMap surfaces;
        surfaces.reserve(components.size());
        if (Result<void> valid = ValidateSurfaces(components, surfaces); valid.HasError())
            return valid;
        return ValidateReferences(components, surfaces);
    }
}  // namespace Horo::Runtime
