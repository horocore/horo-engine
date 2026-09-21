#include "editor/document/SceneDocumentPersistenceInternal.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace Horo::Editor::ScenePersistenceDetail {
    [[nodiscard]] Result<Runtime::NavigationLocalBounds> ParseNavigationBounds(const Json &value) {
        if (!value.is_object() || !value.contains("center") || !value.contains("halfExtents"))
            return Result<Runtime::NavigationLocalBounds>::Failure(PersistenceError(SceneInvalid, "Navigation bounds are invalid."));
        auto center = ParseVec3(value["center"]);
        auto halfExtents = ParseVec3(value["halfExtents"]);
        if (center.HasError() || halfExtents.HasError())
            return Result<Runtime::NavigationLocalBounds>::Failure(PersistenceError(SceneInvalid, "Navigation bounds are invalid."));
        return Result<Runtime::NavigationLocalBounds>::Success({center.Value(), halfExtents.Value()});
    }

    [[nodiscard]] bool HasNavigationComponentHeader(const Json &value) {
        return value.is_object() && value.contains("id") && value["id"].is_number_unsigned() && value.contains("schemaVersion") &&
               value["schemaVersion"].is_number_unsigned() && value.contains("generation") && value["generation"].is_number_unsigned();
    }

    [[nodiscard]] bool HasNavigationSurfaceReference(const Json &value) {
        return HasNavigationComponentHeader(value) && value.contains("surface") && value["surface"].is_number_unsigned();
    }

    [[nodiscard]] Result<std::vector<Navigation::NavigationAgentProfileId>> ParseNavigationProfiles(const Json &values) {
        if (!values.is_array())
            return Result<std::vector<Navigation::NavigationAgentProfileId>>::Failure(
                PersistenceError(SceneInvalid, "Navigation profiles must be an array."));
        std::vector<Navigation::NavigationAgentProfileId> profiles;
        profiles.reserve(values.size());
        for (const Json &value : values) {
            if (!value.is_number_unsigned())
                return Result<std::vector<Navigation::NavigationAgentProfileId>>::Failure(
                    PersistenceError(SceneInvalid, "Navigation profile identity is invalid."));
            auto profile = Navigation::NavigationAgentProfileId::Create(value.get<std::uint64_t>());
            if (profile.HasError())
                return Result<std::vector<Navigation::NavigationAgentProfileId>>::Failure(
                    PersistenceError(SceneInvalid, "Navigation profile identity is invalid."));
            profiles.push_back(profile.Value());
        }
        return Result<std::vector<Navigation::NavigationAgentProfileId>>::Success(std::move(profiles));
    }

    [[nodiscard]] Result<Runtime::NavigationSurfaceComponent> ParseNavigationSurface(const Json &value) {
        if (!HasNavigationComponentHeader(value) || !value.contains("definition") || !value["definition"].is_string() ||
            !value.contains("bakeScope") || !value["bakeScope"].is_string() || !value.contains("localBounds") ||
            !value.contains("profiles")) {
            return Result<Runtime::NavigationSurfaceComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation surface schema is incomplete."));
        }
        auto id = Navigation::SurfaceId::Create(value["id"].get<std::uint64_t>());
        auto definition = Assets::AssetId::Parse(value["definition"].get<std::string>());
        auto profiles = ParseNavigationProfiles(value["profiles"]);
        const std::string scope = value["bakeScope"].get<std::string>();
        if (id.HasError() || definition.HasError() || profiles.HasError() || (scope != "object_subtree" && scope != "local_bounds"))
            return Result<Runtime::NavigationSurfaceComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation surface identities or scope are invalid."));

        std::optional<Runtime::NavigationLocalBounds> bounds;
        if (!value["localBounds"].is_null()) {
            auto parsedBounds = ParseNavigationBounds(value["localBounds"]);
            if (parsedBounds.HasError())
                return Result<Runtime::NavigationSurfaceComponent>::Failure(parsedBounds.ErrorValue());
            bounds = parsedBounds.Value();
        }
        Runtime::NavigationSurfaceComponent surface{
            .id = id.Value(),
            .definition = definition.Value(),
            .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
            .generation = value["generation"].get<std::uint64_t>(),
            .bakeScope =
                scope == "object_subtree" ? Runtime::NavigationBakeScope::ObjectSubtree : Runtime::NavigationBakeScope::LocalBounds,
            .localBounds = bounds,
            .profiles = std::move(profiles).Value(),
            .enabled = value.value("enabled", true),
        };
        if (Runtime::ValidateNavigationSurfaceComponent(surface).HasError())
            return Result<Runtime::NavigationSurfaceComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation surface payload is invalid."));
        return Result<Runtime::NavigationSurfaceComponent>::Success(std::move(surface));
    }

    [[nodiscard]] Result<Runtime::NavigationRegionComponent> ParseNavigationRegion(const Json &value) {
        if (!HasNavigationSurfaceReference(value) || !value.contains("localBounds") || !value.contains("sourceSelection") ||
            !value["sourceSelection"].is_string() || !value.contains("mode") || !value["mode"].is_string()) {
            return Result<Runtime::NavigationRegionComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation region schema is incomplete."));
        }
        auto id = Navigation::NavigationRegionId::Create(value["id"].get<std::uint64_t>());
        auto surface = Navigation::SurfaceId::Create(value["surface"].get<std::uint64_t>());
        auto bounds = ParseNavigationBounds(value["localBounds"]);
        const std::string source = value["sourceSelection"].get<std::string>();
        const std::string mode = value["mode"].get<std::string>();
        if (id.HasError() || surface.HasError() || bounds.HasError() ||
            (source != "explicit_contributors" && source != "static_collision_in_bounds") || (mode != "include" && mode != "exclude")) {
            return Result<Runtime::NavigationRegionComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation region identity, bounds, or policy is invalid."));
        }
        Runtime::NavigationRegionComponent region{
            .id = id.Value(),
            .surface = surface.Value(),
            .localBounds = bounds.Value(),
            .sourceSelection = source == "explicit_contributors" ? Runtime::NavigationRegionSourceSelection::ExplicitContributors
                                                                 : Runtime::NavigationRegionSourceSelection::StaticCollisionInBounds,
            .mode = mode == "include" ? Runtime::NavigationRegionMode::Include : Runtime::NavigationRegionMode::Exclude,
            .enabled = value.value("enabled", true),
        };
        region.schemaVersion = value["schemaVersion"].get<std::uint32_t>();
        region.generation = value["generation"].get<std::uint64_t>();
        if (Runtime::ValidateNavigationRegionComponent(region).HasError())
            return Result<Runtime::NavigationRegionComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation region payload is invalid."));
        return Result<Runtime::NavigationRegionComponent>::Success(region);
    }

    [[nodiscard]] Result<Runtime::NavigationModifierVolume> ParseNavigationModifierVolume(const Json &volumeValue) {
        if (!volumeValue.contains("shape") || !volumeValue["shape"].is_string() || !volumeValue.contains("center"))
            return Result<Runtime::NavigationModifierVolume>::Failure(
                PersistenceError(SceneInvalid, "Navigation modifier volume is invalid."));
        auto center = ParseVec3(volumeValue["center"]);
        const std::string shape = volumeValue["shape"].get<std::string>();
        if (center.HasError() || (shape != "box" && shape != "cylinder"))
            return Result<Runtime::NavigationModifierVolume>::Failure(
                PersistenceError(SceneInvalid, "Navigation modifier volume is invalid."));
        if (shape == "box") {
            if (!volumeValue.contains("halfExtents"))
                return Result<Runtime::NavigationModifierVolume>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier box is incomplete."));
            auto bounds = ParseNavigationBounds(volumeValue);
            if (bounds.HasError())
                return Result<Runtime::NavigationModifierVolume>::Failure(bounds.ErrorValue());
            return Result<Runtime::NavigationModifierVolume>::Success(bounds.Value());
        }
        if (!volumeValue.contains("radius") || !volumeValue["radius"].is_number() || !volumeValue.contains("halfHeight") ||
            !volumeValue["halfHeight"].is_number())
            return Result<Runtime::NavigationModifierVolume>::Failure(
                PersistenceError(SceneInvalid, "Navigation modifier cylinder is incomplete."));
        return Result<Runtime::NavigationModifierVolume>::Success(
            Runtime::NavigationCylinderVolume{center.Value(), volumeValue["radius"].get<float>(), volumeValue["halfHeight"].get<float>()});
    }

    struct ParsedNavigationModifierPolicy final {
        Runtime::NavigationModifierOperation operation;
        std::optional<Navigation::NavigationAreaId> area;
        std::optional<float> traversalCost;
    };

    [[nodiscard]] Result<ParsedNavigationModifierPolicy> ParseNavigationModifierPolicy(const Json &value) {
        const std::string operationName = value["operation"].get<std::string>();
        if (operationName != "exclude" && operationName != "override_area" && operationName != "override_area_and_cost")
            return Result<ParsedNavigationModifierPolicy>::Failure(
                PersistenceError(SceneInvalid, "Navigation modifier operation is invalid."));
        std::optional<Navigation::NavigationAreaId> area;
        if (!value["area"].is_null()) {
            if (!value["area"].is_number_unsigned())
                return Result<ParsedNavigationModifierPolicy>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier area is invalid."));
            auto parsedArea = Navigation::NavigationAreaId::Create(value["area"].get<std::uint64_t>());
            if (parsedArea.HasError())
                return Result<ParsedNavigationModifierPolicy>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier area is invalid."));
            area = parsedArea.Value();
        }
        std::optional<float> traversalCost;
        if (!value["traversalCost"].is_null()) {
            if (!value["traversalCost"].is_number())
                return Result<ParsedNavigationModifierPolicy>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier traversal cost is invalid."));
            traversalCost = value["traversalCost"].get<float>();
        }
        Runtime::NavigationModifierOperation operation = Runtime::NavigationModifierOperation::Exclude;
        if (operationName == "override_area")
            operation = Runtime::NavigationModifierOperation::OverrideArea;
        else if (operationName == "override_area_and_cost")
            operation = Runtime::NavigationModifierOperation::OverrideAreaAndCost;
        return Result<ParsedNavigationModifierPolicy>::Success({operation, area, traversalCost});
    }

    [[nodiscard]] Result<Runtime::NavigationModifierComponent> ParseNavigationModifier(const Json &value) {
        if (!HasNavigationSurfaceReference(value) || !value.contains("volume") || !value["volume"].is_object() ||
            !value.contains("operation") || !value["operation"].is_string() || !value.contains("area") || !value.contains("traversalCost"))
            return Result<Runtime::NavigationModifierComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation modifier schema is incomplete."));
        auto id = Navigation::NavigationModifierId::Create(value["id"].get<std::uint64_t>());
        auto surface = Navigation::SurfaceId::Create(value["surface"].get<std::uint64_t>());
        auto volume = ParseNavigationModifierVolume(value["volume"]);
        auto policy = ParseNavigationModifierPolicy(value);
        if (id.HasError() || surface.HasError())
            return Result<Runtime::NavigationModifierComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation modifier identity or surface is invalid."));
        if (volume.HasError())
            return Result<Runtime::NavigationModifierComponent>::Failure(volume.ErrorValue());
        if (policy.HasError())
            return Result<Runtime::NavigationModifierComponent>::Failure(policy.ErrorValue());
        Runtime::NavigationModifierComponent modifier{
            .id = id.Value(),
            .surface = surface.Value(),
            .volume = std::move(volume).Value(),
            .operation = policy.Value().operation,
            .area = policy.Value().area,
            .traversalCost = policy.Value().traversalCost,
            .enabled = value.value("enabled", true),
        };
        modifier.schemaVersion = value["schemaVersion"].get<std::uint32_t>();
        modifier.generation = value["generation"].get<std::uint64_t>();
        if (Runtime::ValidateNavigationModifierComponent(modifier).HasError())
            return Result<Runtime::NavigationModifierComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation modifier payload is invalid."));
        return Result<Runtime::NavigationModifierComponent>::Success(std::move(modifier));
    }

    [[nodiscard]] Result<Runtime::NavigationLinkEndpoint> ParseNavigationLinkEndpoint(const Json &value) {
        if (!value.is_object() || !value.contains("surface") || !value["surface"].is_number_unsigned() ||
            !value.contains("localPosition") || !value.contains("connectionRadiusMeters") || !value["connectionRadiusMeters"].is_number()) {
            return Result<Runtime::NavigationLinkEndpoint>::Failure(
                PersistenceError(SceneInvalid, "Navigation link endpoint schema is incomplete."));
        }
        auto surface = Navigation::SurfaceId::Create(value["surface"].get<std::uint64_t>());
        auto position = ParseVec3(value["localPosition"]);
        if (surface.HasError() || position.HasError())
            return Result<Runtime::NavigationLinkEndpoint>::Failure(PersistenceError(SceneInvalid, "Navigation link endpoint is invalid."));
        return Result<Runtime::NavigationLinkEndpoint>::Success(
            {surface.Value(), position.Value(), value["connectionRadiusMeters"].get<float>()});
    }

    [[nodiscard]] Result<Runtime::NavigationLinkKind> ParseNavigationLinkKind(const std::string_view name) {
        using enum Runtime::NavigationLinkKind;
        if (name == "jump")
            return Result<Runtime::NavigationLinkKind>::Success(Jump);
        if (name == "ladder")
            return Result<Runtime::NavigationLinkKind>::Success(Ladder);
        if (name == "door")
            return Result<Runtime::NavigationLinkKind>::Success(Door);
        if (name == "teleport")
            return Result<Runtime::NavigationLinkKind>::Success(Teleport);
        return Result<Runtime::NavigationLinkKind>::Failure(PersistenceError(SceneInvalid, "Navigation link kind is invalid."));
    }

    [[nodiscard]] Result<Runtime::NavigationLinkDirection> ParseNavigationLinkDirection(const std::string_view name) {
        if (name == "start_to_end")
            return Result<Runtime::NavigationLinkDirection>::Success(Runtime::NavigationLinkDirection::StartToEnd);
        if (name == "bidirectional")
            return Result<Runtime::NavigationLinkDirection>::Success(Runtime::NavigationLinkDirection::Bidirectional);
        return Result<Runtime::NavigationLinkDirection>::Failure(PersistenceError(SceneInvalid, "Navigation link direction is invalid."));
    }

    [[nodiscard]] Result<Runtime::NavigationLinkComponent> ParseNavigationLink(const Json &value) {
        if (!HasNavigationComponentHeader(value) || !value.contains("start") || !value.contains("end") || !value.contains("kind") ||
            !value["kind"].is_string() || !value.contains("direction") || !value["direction"].is_string() || !value.contains("profiles") ||
            !value.contains("traversalCost") || !value["traversalCost"].is_number()) {
            return Result<Runtime::NavigationLinkComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation link schema is incomplete."));
        }
        auto id = Navigation::NavigationLinkId::Create(value["id"].get<std::uint64_t>());
        auto start = ParseNavigationLinkEndpoint(value["start"]);
        auto end = ParseNavigationLinkEndpoint(value["end"]);
        auto kind = ParseNavigationLinkKind(value["kind"].get<std::string>());
        auto direction = ParseNavigationLinkDirection(value["direction"].get<std::string>());
        auto profiles = ParseNavigationProfiles(value["profiles"]);
        if (id.HasError())
            return Result<Runtime::NavigationLinkComponent>::Failure(
                PersistenceError(SceneInvalid, "Navigation link identity is invalid."));
        if (start.HasError())
            return Result<Runtime::NavigationLinkComponent>::Failure(start.ErrorValue());
        if (end.HasError())
            return Result<Runtime::NavigationLinkComponent>::Failure(end.ErrorValue());
        if (kind.HasError())
            return Result<Runtime::NavigationLinkComponent>::Failure(kind.ErrorValue());
        if (direction.HasError())
            return Result<Runtime::NavigationLinkComponent>::Failure(direction.ErrorValue());
        if (profiles.HasError())
            return Result<Runtime::NavigationLinkComponent>::Failure(profiles.ErrorValue());
        Runtime::NavigationLinkComponent link{
            .id = id.Value(),
            .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
            .generation = value["generation"].get<std::uint64_t>(),
            .start = start.Value(),
            .end = end.Value(),
            .kind = kind.Value(),
            .direction = direction.Value(),
            .profiles = std::move(profiles).Value(),
            .traversalCost = value["traversalCost"].get<float>(),
            .enabled = value.value("enabled", true),
        };
        if (Runtime::ValidateNavigationLinkComponent(link).HasError())
            return Result<Runtime::NavigationLinkComponent>::Failure(PersistenceError(SceneInvalid, "Navigation link payload is invalid."));
        return Result<Runtime::NavigationLinkComponent>::Success(std::move(link));
    }

}  // namespace Horo::Editor::ScenePersistenceDetail
