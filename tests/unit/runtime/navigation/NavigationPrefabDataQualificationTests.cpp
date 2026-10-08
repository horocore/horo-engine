#include "Horo/Prefab/PrefabSceneExpansion.h"
#include "navigation/NavigationDataQualificationCorpus.h"

#include <array>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Id;
        using TestSupport::NavigationDataQualificationCorpus;

        /** @brief Resolve a reload of the same durable prefab root; source path and display label are not identity inputs. */
        [[nodiscard]] Prefab::EffectivePrefabCandidate Candidate(const std::uint8_t revision, const std::string_view label,
                                                                 const std::string_view path) {
            const auto asset = Assets::AssetId::Parse("88992233-4455-6677-8899-aabbccddeeff").Value();
            const auto version = Application::ParseHoroVersion("1.2.3").Value();
            const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
            Assets::AssetRegistry registry;
            REQUIRE(registry
                        .Publish({{asset, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse(path).Value(),
                                   ProjectPath::Parse(std::string{path} + ".horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            auto document = Prefab::PrefabDocument::Create({.projectVersion = version,
                                                            .assetId = asset,
                                                            .objects = {{.localId = {}, .name = std::string{label}}}},
                                                           limits);
            REQUIRE(document.HasValue());
            auto resolver =
                Prefab::BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                          {{std::move(document).Value(), {version, TestSupport::Digest(revision)}}},
                                                          limits);
            REQUIRE(resolver.HasValue());
            auto candidate = resolver.Value().Resolve(asset, Prefab::PrefabInstanceId::Create(1).Value(), limits);
            REQUIRE(candidate.HasValue());
            return std::move(candidate).Value();
        }

        /** @brief Expand the resolved root with a copied typed navigation projection. */
        [[nodiscard]] Prefab::ExpandedPrefabSceneSubtree Expand(const Prefab::EffectivePrefabCandidate &candidate,
                                                                const Runtime::NavigationSurfaceComponent &surface) {
            const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
            REQUIRE_FALSE(candidate.Objects().empty());
            const auto remapped = Prefab::RemapPrefabCandidateToScene(candidate, {}, {}, limits);
            REQUIRE(remapped.HasValue());
            const std::array projections{
                Prefab::PrefabRuntimeComponentProjection{candidate.Objects().front().key, {.navigationSurface = surface}}};
            auto expanded = Prefab::ExpandPrefabSceneSubtree(candidate, remapped.Value(), projections, {}, limits);
            REQUIRE(expanded.HasValue());
            return std::move(expanded).Value();
        }
    }  // namespace

    TEST_CASE("Portable navigation model keeps exact authored identities through Prefab rename and Scene definition reload",
              "[unit][navigation][data_qualification][prefab][reload]") {
        const NavigationDataQualificationCorpus corpus;
        const auto decoded = DecodeNavigationDefinitionRecord(corpus.Record());
        REQUIRE(decoded.HasValue());
        const auto &definition = decoded.Value();
        const auto asset = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff").Value();
        Runtime::NavigationSurfaceComponent surface{.id = Id<SurfaceId>(17), .definition = asset, .generation = 1};
        for (const auto &profile : definition.Profiles())
            surface.profiles.push_back(profile.id);
        const auto first = Candidate(1, "Original", "assets/navigation.prefab");
        const auto reloaded = Candidate(2, "Yeniden 地面", "assets/renamed navigation.prefab");
        const auto before = Expand(first, surface);
        surface.generation = 2;
        const auto after = Expand(reloaded, surface);
        REQUIRE(before.Entities().size() == 1);
        REQUIRE(after.Entities().size() == 1);
        REQUIRE(before.Entities().front().components.navigationSurface.has_value());
        REQUIRE(after.Entities().front().components.navigationSurface.has_value());
        CHECK(before.Entities().front().object == after.Entities().front().object);
        CHECK(before.Entities().front().components.navigationSurface->id == after.Entities().front().components.navigationSurface->id);
        CHECK(after.Entities().front().components.navigationSurface->definition == asset);
        CHECK(after.Entities().front().components.navigationSurface->profiles == surface.profiles);
        CHECK(after.Entities().front().components.navigationSurface->generation == 2);
        Runtime::SceneDefinitionBuilder scene{{4}, {2}};
        scene.Add(after.Entities().front());
        const auto committed = std::move(scene).Build();
        REQUIRE(committed.HasValue());
        REQUIRE(committed.Value().AssetDependencies().size() == 1);
        CHECK(committed.Value().AssetDependencies().front().id == asset);
        CHECK(committed.Value().AssetDependencies().front().expectedType.Value() == "core.navmesh");
    }
}  // namespace Horo::Navigation
