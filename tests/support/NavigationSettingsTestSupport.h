#pragma once
#include "Horo/Editor/NavigationSettingsWorkflow.h"
#include "Horo/Scene/SceneSource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Tests::NavigationSettingsSupport {
    namespace N = Navigation;
    namespace E = Editor;

    template <class T> T Id(std::uint64_t value) {
        return T::Create(value).Value();
    }

    inline Assets::AssetId Asset(std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    inline N::NavigationDefinitionInput Definition() {
        const N::NavigationDescriptorSource source{N::NavigationDescriptorSourceKind::Project, Id<N::NavigationDescriptorSourceId>(20)};
        return {.profiles = {{Id<N::NavigationAgentProfileId>(1), "Walker"}, {Id<N::NavigationAgentProfileId>(2), "Large"}},
                .areas = {{Id<N::NavigationAreaId>(3), source, 1, {1}}, {Id<N::NavigationAreaId>(5), source, 2, {2}}},
                .filters = {{Id<N::NavigationFilterId>(4), source, {1}, {}, {{Id<N::NavigationAreaId>(3), 2}}},
                            {Id<N::NavigationFilterId>(6), source, {2}, {}, {}}}};
    }

    inline Assets::AssetRecord Record(std::uint8_t asset, std::string_view type, std::string_view path) {
        return {Asset(asset), Assets::AssetTypeId::Parse(type).Value(), ProjectPath::Parse(path).Value(),
                ProjectPath::Parse(std::string(path) + ".horo").Value()};
    }

    inline std::vector<SceneSource::SceneObjectSnapshot> Objects() {
        SceneSource::SceneObjectSnapshot surface{.id = {1}, .name = "Surface"};
        surface.components.navigationSurface = Runtime::NavigationSurfaceComponent{.id = Id<N::SurfaceId>(10),
                                                                                   .definition = Asset(1),
                                                                                   .profiles = {Id<N::NavigationAgentProfileId>(1)}};
        SceneSource::SceneObjectSnapshot agent{.id = {2}, .name = "Agent"};
        agent.components.navigationAgent =
            Runtime::NavigationAgentComponent{.profile = Id<N::NavigationAgentProfileId>(1), .filter = Id<N::NavigationFilterId>(4)};
        SceneSource::SceneObjectSnapshot modifier{.id = {3}, .name = "Modifier"};
        modifier.components.navigationModifier =
            Runtime::NavigationModifierComponent{.id = Id<N::NavigationModifierId>(30),
                                                 .surface = Id<N::SurfaceId>(10),
                                                 .operation = Runtime::NavigationModifierOperation::OverrideArea,
                                                 .area = Id<N::NavigationAreaId>(3)};
        return {surface, agent, modifier};
    }

    struct Fixture final {
        Assets::AssetRegistry registry;
        E::NavigationDefinitionDocument definition =
            E::NavigationDefinitionDocument::Open(Asset(1), 1, 1, N::NavigationDefinition::Create(Definition()).Value()).Value();
        E::SceneDocumentTransactions first = E::SceneDocumentTransactions::Create({Asset(2), 1}, Objects()).Value();
        E::SceneDocumentTransactions second = E::SceneDocumentTransactions::Create({Asset(3), 1}, Objects()).Value();
        std::array<E::NavigationDefinitionDocument *, 1> definitions{&definition};
        std::array<E::SceneDocumentTransactions *, 2> scenes{&first, &second};
        E::NavigationSettingsWorkflow workflow{registry, definitions, scenes};

        Fixture() {
            REQUIRE(registry
                        .Publish({Record(1, "core.navmesh", "assets/nav.horoasset"), Record(2, "core.scene", "assets/a.horo"),
                                  Record(3, "core.scene", "assets/b.horo")})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
        }

        E::NavigationSettingsReview Removal() {
            const std::array<E::NavigationDefinitionEdit, 1> edits{
                E::RemoveNavigationProfile{Id<N::NavigationAgentProfileId>(1), Id<N::NavigationAgentProfileId>(2)}};
            auto review = workflow.Preview(Asset(1), edits);
            REQUIRE(review.HasValue());
            return std::move(review).Value();
        }
    };
}  // namespace Horo::Tests::NavigationSettingsSupport
