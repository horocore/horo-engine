#include "Horo/Runtime/Scene/NavigationSceneComponents.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;

    [[nodiscard]] Assets::AssetId Asset(const std::uint8_t suffix = 1) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = suffix;
        return Assets::AssetId::FromBytes(bytes);
    }

    [[nodiscard]] Runtime::NavigationSurfaceComponent Surface(const std::uint64_t id = 1) {
        return {
            .id = Navigation::SurfaceId::Create(id).Value(),
            .definition = Asset(),
            .profiles = {Navigation::NavigationAgentProfileId::Create(7).Value()},
        };
    }

    [[nodiscard]] Runtime::NavigationRegionComponent Region(const std::uint64_t id = 2, const std::uint64_t surface = 1) {
        return {
            .id = Navigation::NavigationRegionId::Create(id).Value(),
            .surface = Navigation::SurfaceId::Create(surface).Value(),
        };
    }

    [[nodiscard]] Runtime::NavigationModifierComponent Modifier(const std::uint64_t id = 3, const std::uint64_t surface = 1) {
        return {
            .id = Navigation::NavigationModifierId::Create(id).Value(),
            .surface = Navigation::SurfaceId::Create(surface).Value(),
        };
    }

    [[nodiscard]] Runtime::NavigationLinkComponent Link(const std::uint64_t id = 4, const std::uint64_t startSurface = 1,
                                                        const std::uint64_t endSurface = 1) {
        return {
            .id = Navigation::NavigationLinkId::Create(id).Value(),
            .start = {.surface = Navigation::SurfaceId::Create(startSurface).Value(), .localPosition = {0.0F, 0.0F, 0.0F}},
            .end = {.surface = Navigation::SurfaceId::Create(endSurface).Value(), .localPosition = {2.0F, 0.0F, 0.0F}},
            .profiles = {Navigation::NavigationAgentProfileId::Create(7).Value()},
        };
    }

    [[nodiscard]] Runtime::NavigationAgentComponent Agent(const std::uint64_t profile = 7, const std::uint64_t filter = 9) {
        return {.profile = Navigation::NavigationAgentProfileId::Create(profile).Value(),
                .filter = Navigation::NavigationFilterId::Create(filter).Value(),
                .radiusOverride = 0.6F};
    }

    TEST_CASE("Navigation Scene surfaces and regions validate bounded payloads", "[unit][navigation][scene]") {
        auto surface = Surface();
        REQUIRE(Runtime::ValidateNavigationSurfaceComponent(surface).HasValue());

        surface.profiles.push_back(surface.profiles.front());
        REQUIRE(Runtime::ValidateNavigationSurfaceComponent(surface).HasError());
        surface = Surface();
        surface.bakeScope = Runtime::NavigationBakeScope::LocalBounds;
        REQUIRE(Runtime::ValidateNavigationSurfaceComponent(surface).HasError());
        surface.localBounds = Runtime::NavigationLocalBounds{};
        REQUIRE(Runtime::ValidateNavigationSurfaceComponent(surface).HasValue());
        surface.localBounds->halfExtents.x = std::numeric_limits<float>::quiet_NaN();
        REQUIRE(Runtime::ValidateNavigationSurfaceComponent(surface).HasError());

        auto region = Region();
        REQUIRE(Runtime::ValidateNavigationRegionComponent(region).HasValue());
        region.localBounds.halfExtents.z = 0.0F;
        REQUIRE(Runtime::ValidateNavigationRegionComponent(region).HasError());
    }

    TEST_CASE("Navigation Scene modifiers validate bounded typed payloads", "[unit][navigation][scene]") {
        auto modifier = Modifier();
        REQUIRE(Runtime::ValidateNavigationModifierComponent(modifier).HasValue());
        modifier.volume = Runtime::NavigationCylinderVolume{.radius = 2.0F, .halfHeight = 3.0F};
        modifier.operation = Runtime::NavigationModifierOperation::OverrideAreaAndCost;
        modifier.area = Navigation::NavigationAreaId::Create(9).Value();
        modifier.traversalCost = 1.5F;
        REQUIRE(Runtime::ValidateNavigationModifierComponent(modifier).HasValue());
        modifier.traversalCost = std::nullopt;
        REQUIRE(Runtime::ValidateNavigationModifierComponent(modifier).HasError());
        modifier.operation = Runtime::NavigationModifierOperation::Exclude;
        REQUIRE(Runtime::ValidateNavigationModifierComponent(modifier).HasError());
        modifier = Modifier();
        modifier.volume = Runtime::NavigationCylinderVolume{.radius = 0.0F, .halfHeight = 1.0F};
        REQUIRE(Runtime::ValidateNavigationModifierComponent(modifier).HasError());
        modifier = Modifier();
        modifier.operation = Runtime::NavigationModifierOperation::OverrideAreaAndCost;
        modifier.area = Navigation::NavigationAreaId::Create(9).Value();
        modifier.traversalCost = -1.0F;
        REQUIRE(Runtime::ValidateNavigationModifierComponent(modifier).HasError());
    }

    TEST_CASE("Navigation Scene links and agents validate bounded typed payloads", "[unit][navigation][scene]") {
        auto link = Link();
        REQUIRE(Runtime::ValidateNavigationLinkComponent(link).HasValue());
        link.direction = Runtime::NavigationLinkDirection::Bidirectional;
        REQUIRE(Runtime::ValidateNavigationLinkComponent(link).HasValue());
        link.end.localPosition = link.start.localPosition;
        REQUIRE(Runtime::ValidateNavigationLinkComponent(link).HasError());
        link = Link();
        link.profiles.push_back(link.profiles.front());
        REQUIRE(Runtime::ValidateNavigationLinkComponent(link).HasError());
        link = Link();
        link.start.connectionRadiusMeters = 0.0F;
        REQUIRE(Runtime::ValidateNavigationLinkComponent(link).HasError());
        link = Link();
        link.profiles.clear();
        REQUIRE(Runtime::ValidateNavigationLinkComponent(link).HasError());
        link = Link();
        link.traversalCost = -1.0F;
        REQUIRE(Runtime::ValidateNavigationLinkComponent(link).HasError());

        auto agent = Agent();
        REQUIRE(Runtime::ValidateNavigationAgentComponent(agent).HasValue());
        agent.radiusOverride = std::numeric_limits<float>::quiet_NaN();
        REQUIRE(Runtime::ValidateNavigationAgentComponent(agent).HasError());
        agent = Agent();
        agent.movementCapability = Navigation::NavigationAgentMovementCapability::Count;
        REQUIRE(Runtime::ValidateNavigationAgentComponent(agent).HasError());
    }

    TEST_CASE("Navigation Scene identity validation rejects conflicts and missing surfaces", "[unit][navigation][scene]") {
        const std::array surfaces{Surface()};
        const std::array regions{Region()};
        REQUIRE(Runtime::ValidateNavigationSceneComponents(surfaces, regions).HasValue());

        const std::array duplicateSurfaces{Surface(), Surface()};
        REQUIRE(Runtime::ValidateNavigationSceneComponents(duplicateSurfaces, regions).HasError());
        const std::array duplicateRegions{Region(), Region()};
        REQUIRE(Runtime::ValidateNavigationSceneComponents(surfaces, duplicateRegions).HasError());
        const std::array missingRegions{Region(2, 99)};
        REQUIRE(Runtime::ValidateNavigationSceneComponents(surfaces, missingRegions).HasError());

        const std::array modifiers{Modifier()};
        const std::array links{Link()};
        const std::array agents{Agent()};
        REQUIRE(Runtime::ValidateNavigationSceneComponents(surfaces, regions, modifiers, links, agents).HasValue());
        const std::array duplicateModifiers{Modifier(), Modifier()};
        REQUIRE(Runtime::ValidateNavigationSceneComponents(surfaces, regions, duplicateModifiers, links).HasError());
        const std::array duplicateLinks{Link(), Link()};
        REQUIRE(Runtime::ValidateNavigationSceneComponents(surfaces, regions, modifiers, duplicateLinks).HasError());
        const std::array missingModifiers{Modifier(3, 99)};
        REQUIRE(Runtime::ValidateNavigationSceneComponents(surfaces, regions, missingModifiers, links).HasError());
        const std::array missingLinks{Link(4, 1, 99)};
        REQUIRE(Runtime::ValidateNavigationSceneComponents(surfaces, regions, modifiers, missingLinks).HasError());

        auto incompatible = Link();
        incompatible.profiles = {Navigation::NavigationAgentProfileId::Create(8).Value()};
        const std::array incompatibleLinks{incompatible};
        const auto mismatch = Runtime::ValidateNavigationSceneComponents(surfaces, regions, modifiers, incompatibleLinks);
        REQUIRE(mismatch.HasError());
        REQUIRE(mismatch.ErrorValue().code.Value() == Navigation::NavigationErrors::SceneProfileMismatch.code.Value());
    }

    TEST_CASE("Runtime definition pins committed navigation generations and rejects missing references", "[unit][navigation][scene]") {
        Runtime::SceneDefinitionBuilder valid{Runtime::SceneDefinitionId{3}, Runtime::SceneDefinitionRevision{11}};
        valid.Add({.object = Runtime::SceneObjectId{1}, .components = {.navigationSurface = Surface()}});
        valid.Add({.object = Runtime::SceneObjectId{2}, .components = {.navigationRegion = Region()}});
        valid.Add({.object = Runtime::SceneObjectId{3}, .components = {.navigationModifier = Modifier()}});
        valid.Add({.object = Runtime::SceneObjectId{4}, .components = {.navigationLink = Link()}});
        valid.Add({.object = Runtime::SceneObjectId{5}, .components = {.navigationAgent = Agent()}});
        auto definition = std::move(valid).Build();
        REQUIRE(definition.HasValue());
        REQUIRE(definition.Value().Revision().value == 11);
        REQUIRE(definition.Value().Entities()[0].components.navigationSurface->generation == 1);
        REQUIRE(definition.Value().Entities()[2].components.navigationModifier->id == Modifier().id);
        REQUIRE(definition.Value().Entities()[3].components.navigationLink->direction == Runtime::NavigationLinkDirection::StartToEnd);
        REQUIRE(definition.Value().Entities()[4].components.navigationAgent->filter == Navigation::NavigationFilterId::Create(9).Value());

        Runtime::SceneDefinitionBuilder missing{Runtime::SceneDefinitionId{3}, Runtime::SceneDefinitionRevision{12}};
        missing.Add({.object = Runtime::SceneObjectId{2}, .components = {.navigationRegion = Region()}});
        REQUIRE(std::move(missing).Build().HasError());
    }
}  // namespace
