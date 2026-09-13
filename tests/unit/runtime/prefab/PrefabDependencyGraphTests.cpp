#include "Horo/Prefab/PrefabDependencyGraph.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Prefab {
    namespace {
        using Test::Asset;

        Assets::AssetTypeId Type(const std::string_view value) {
            return Assets::AssetTypeId::Parse(value).Value();
        }

        ProjectPath Path(const std::string_view value) {
            return ProjectPath::Parse(value).Value();
        }

        Application::HoroVersion ProjectVersion() {
            return Application::ParseHoroVersion("1.2.3").Value();
        }

        PrefabSourceRevision Revision(const std::uint8_t suffix) {
            Sha256Digest digest{};
            digest.bytes.back() = suffix;
            return {.projectVersion = ProjectVersion(), .contentDigest = digest};
        }

        PrefabLimitProfile Limits(PrefabProjectPolicy policy = {}) {
            return PrefabLimitProfile::Create(std::move(policy)).Value();
        }

        PrefabObjectNode Root() {
            return {.localId = {}, .name = "Root"};
        }

        PrefabDocument Concrete(const Assets::AssetId id, std::vector<Assets::AssetId> dependencies = {}) {
            PrefabDocumentData data{
                .projectVersion = ProjectVersion(),
                .assetId = id,
                .objects = {Root()},
                .referencedAssets = std::move(dependencies),
            };
            return PrefabDocument::Create(std::move(data), Limits()).Value();
        }

        PrefabDocument Nested(const Assets::AssetId id, const Assets::AssetId nested, const Assets::AssetId resource,
                              const PrefabSourceRevision nestedRevision) {
            PrefabDocumentData data{
                .projectVersion = ProjectVersion(),
                .assetId = id,
                .objects = {Root()},
                .composition =
                    PrefabComposition{
                        .nestedPlacements =
                            {
                                {.placementLocalId = {1},
                                 .sourcePrefab = PrefabAssetReference::Create(nested).Value(),
                                 .authoredAgainst = nestedRevision},
                                {.placementLocalId = {2},
                                 .sourcePrefab = PrefabAssetReference::Create(nested).Value(),
                                 .authoredAgainst = nestedRevision},
                            },
                    },
                .referencedAssets = {resource, nested},
            };
            return PrefabDocument::Create(std::move(data), Limits()).Value();
        }

        PrefabDocument Variant(const Assets::AssetId id, const Assets::AssetId parent, const PrefabSourceRevision parentRevision) {
            PrefabDocumentData data{
                .projectVersion = ProjectVersion(),
                .assetId = id,
                .composition =
                    PrefabComposition{
                        .variantParent = PrefabAssetReference::Create(parent).Value(),
                        .variantAuthoredAgainst = parentRevision,
                    },
                .referencedAssets = {parent},
            };
            return PrefabDocument::Create(std::move(data), Limits()).Value();
        }

        Assets::AssetRecord Record(const Assets::AssetId id, const std::string_view type, const std::string_view source) {
            return {id, Type(type), Path(source), Path(std::string{source} + ".horo")};
        }

        void PublishRegistry(Assets::AssetRegistry &registry, std::vector<Assets::AssetRecord> records) {
            REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
        }

        void RequireCode(const Error &error, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(error.code.Value() == descriptor.code.Value());
        }

        TEST_CASE("Prefab dependency graph is canonical revision-pinned and deduplicated", "[unit][prefab][dependency]") {
            const Assets::AssetId outer = Asset(1);
            const Assets::AssetId nested = Asset(2);
            const Assets::AssetId mesh = Asset(3);
            const Assets::AssetId variant = Asset(4);
            Assets::AssetRegistry registry;
            PublishRegistry(registry, {Record(mesh, "core.mesh", "assets/models/mesh.obj"),
                                       Record(nested, "core.prefab", "assets/prefabs/nested.prefab"),
                                       Record(variant, "core.prefab", "assets/prefabs/variant.prefab"),
                                       Record(outer, "core.prefab", "assets/prefabs/outer.prefab")});
            const PrefabSourceRevision outerRevision = Revision(1);
            const PrefabSourceRevision nestedRevision = Revision(2);
            std::vector<PrefabDependencySource> sources{
                {Concrete(nested, {mesh}), nestedRevision},
                {Variant(variant, nested, nestedRevision), Revision(4)},
                {Nested(outer, nested, mesh, nestedRevision), outerRevision},
            };

            auto built = BuildPrefabDependencyGraph(registry.Snapshot(), std::move(sources), Limits());
            REQUIRE(built.HasValue());
            const PrefabDependencyGraphSnapshot &graph = built.Value();
            REQUIRE(graph.RegistryRevision() == registry.Snapshot().Revision());
            REQUIRE(graph.Nodes().size() == 4);
            REQUIRE(graph.Nodes()[0].assetId == outer);
            REQUIRE(graph.Nodes()[1].assetId == nested);
            REQUIRE(graph.Nodes()[2].assetId == mesh);
            REQUIRE(graph.Nodes()[3].assetId == variant);
            REQUIRE(graph.Nodes()[0].sourceRevision == outerRevision);
            REQUIRE(graph.Nodes()[1].sourceRevision == nestedRevision);
            REQUIRE_FALSE(graph.Nodes()[2].sourceRevision.has_value());
            REQUIRE(graph.Edges().size() == 4);
            REQUIRE(graph.DirectDependencies(outer).size() == 2);
            REQUIRE(graph.DirectDependencies(outer)[0].targetAsset == nested);
            REQUIRE(graph.DirectDependencies(outer)[0].kind == PrefabDependencyKind::NestedPrefab);
            REQUIRE(graph.DirectDependencies(outer)[1].targetAsset == mesh);
            REQUIRE(graph.DirectDependencies(nested).size() == 1);
            REQUIRE(graph.DirectDependencies(variant).size() == 1);
            REQUIRE(graph.DirectDependencies(variant)[0].kind == PrefabDependencyKind::VariantParent);
        }

        TEST_CASE("Prefab dependency readers compute closure and reverse invalidation from owned graph data",
                  "[unit][prefab][dependency]") {
            const Assets::AssetId outer = Asset(1);
            const Assets::AssetId nested = Asset(2);
            const Assets::AssetId mesh = Asset(3);
            Assets::AssetRegistry registry;
            PublishRegistry(registry, {Record(outer, "core.prefab", "assets/prefabs/outer.prefab"),
                                       Record(nested, "core.prefab", "assets/prefabs/nested.prefab"),
                                       Record(mesh, "core.mesh", "assets/models/mesh.obj")});
            const PrefabSourceRevision nestedRevision = Revision(2);
            std::vector<PrefabDependencySource> sources{
                {Nested(outer, nested, mesh, nestedRevision), Revision(1)},
                {Concrete(nested, {mesh}), nestedRevision},
            };
            auto graph = BuildPrefabDependencyGraph(registry.Snapshot(), std::move(sources), Limits()).Value();

            const auto closure = graph.DependencyClosure(std::array{outer});
            REQUIRE(closure.HasValue());
            REQUIRE(closure.Value() == std::vector{nested, mesh});
            REQUIRE(graph.InvalidatedPrefabs(std::array{mesh}) == std::vector{outer, nested});
            REQUIRE(graph.InvalidatedPrefabs(std::array{nested}) == std::vector{outer, nested});
            REQUIRE(graph.InvalidatedPrefabs(std::array{Asset(99)}).empty());
            const auto missingRoot = graph.DependencyClosure(std::array{Asset(99)});
            REQUIRE(missingRoot.HasError());
            RequireCode(missingRoot.ErrorValue(), PrefabErrors::DependencyUnavailable);
        }

        TEST_CASE("Prefab dependency snapshots survive registry replacement without source document ownership",
                  "[unit][prefab][dependency]") {
            const Assets::AssetId prefab = Asset(1);
            const Assets::AssetId mesh = Asset(2);
            Assets::AssetRegistry registry;
            PublishRegistry(registry, {Record(prefab, "core.prefab", "assets/prefabs/original.prefab"),
                                       Record(mesh, "core.mesh", "assets/models/mesh.obj")});
            const auto firstRegistryRevision = registry.Snapshot().Revision();
            std::vector<PrefabDependencySource> sources{{Concrete(prefab, {mesh}), Revision(1)}};
            auto graph = BuildPrefabDependencyGraph(registry.Snapshot(), std::move(sources), Limits()).Value();

            REQUIRE(registry
                        .Publish({Record(prefab, "core.prefab", "assets/prefabs/moved.prefab"),
                                  Record(mesh, "core.mesh", "assets/models/moved.obj")})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            REQUIRE(graph.RegistryRevision() == firstRegistryRevision);
            REQUIRE(graph.RegistryRevision() != registry.Snapshot().Revision());
            REQUIRE(graph.DependencyClosure(std::array{prefab}).Value() == std::vector{mesh});
        }

        TEST_CASE("Prefab dependency graph rejects incoherent registry type source and authored revisions", "[unit][prefab][dependency]") {
            const Assets::AssetId outer = Asset(1);
            const Assets::AssetId nested = Asset(2);
            const Assets::AssetId mesh = Asset(3);
            Assets::AssetRegistry missingRegistry;
            PublishRegistry(missingRegistry, {Record(outer, "core.prefab", "assets/prefabs/outer.prefab")});
            auto missing = BuildPrefabDependencyGraph(missingRegistry.Snapshot(),
                                                      {PrefabDependencySource{Concrete(outer, {mesh}), Revision(1)}}, Limits());
            REQUIRE(missing.HasError());
            RequireCode(missing.ErrorValue(), PrefabErrors::DependencyUnavailable);

            Assets::AssetRegistry wrongType;
            PublishRegistry(wrongType, {Record(outer, "core.mesh", "assets/models/outer.obj")});
            auto mismatchedType =
                BuildPrefabDependencyGraph(wrongType.Snapshot(), {PrefabDependencySource{Concrete(outer), Revision(1)}}, Limits());
            REQUIRE(mismatchedType.HasError());
            RequireCode(mismatchedType.ErrorValue(), PrefabErrors::DependencyTypeMismatch);

            Assets::AssetRegistry registry;
            PublishRegistry(registry, {Record(outer, "core.prefab", "assets/prefabs/outer.prefab"),
                                       Record(nested, "core.prefab", "assets/prefabs/nested.prefab"),
                                       Record(mesh, "core.mesh", "assets/models/mesh.obj")});
            auto stale = BuildPrefabDependencyGraph(registry.Snapshot(),
                                                    {PrefabDependencySource{Nested(outer, nested, mesh, Revision(9)), Revision(1)},
                                                     PrefabDependencySource{Concrete(nested), Revision(2)}},
                                                    Limits());
            REQUIRE(stale.HasError());
            RequireCode(stale.ErrorValue(), PrefabErrors::DependencyRevisionMismatch);

            PrefabSourceRevision wrongProjectVersion = Revision(1);
            wrongProjectVersion.projectVersion = Application::ParseHoroVersion("1.2.4").Value();
            auto incoherent =
                BuildPrefabDependencyGraph(registry.Snapshot(), {PrefabDependencySource{Concrete(outer), wrongProjectVersion}}, Limits());
            REQUIRE(incoherent.HasError());
            RequireCode(incoherent.ErrorValue(), PrefabErrors::DependencyGraphInvalid);

            auto incomplete =
                BuildPrefabDependencyGraph(registry.Snapshot(),
                                           {PrefabDependencySource{Nested(outer, nested, mesh, Revision(2)), Revision(1)}}, Limits());
            REQUIRE(incomplete.HasError());
            RequireCode(incomplete.ErrorValue(), PrefabErrors::DependencyUnavailable);
        }

        TEST_CASE("Prefab dependency graph rejects duplicate sources and exhausts captured work budget transactionally",
                  "[unit][prefab][dependency]") {
            const Assets::AssetId prefab = Asset(1);
            Assets::AssetRegistry registry;
            PublishRegistry(registry, {Record(prefab, "core.prefab", "assets/prefabs/one.prefab")});
            auto duplicate = BuildPrefabDependencyGraph(registry.Snapshot(),
                                                        {PrefabDependencySource{Concrete(prefab), Revision(1)},
                                                         PrefabDependencySource{Concrete(prefab), Revision(1)}},
                                                        Limits());
            REQUIRE(duplicate.HasError());
            RequireCode(duplicate.ErrorValue(), PrefabErrors::DependencyGraphInvalid);

            PrefabProjectPolicy policy;
            policy.maximumHierarchyDepth = 1;
            policy.maximumObjectCount = 1;
            policy.maximumSourcePayloadBytes = 1;
            policy.maximumExpandedPayloadBytes = 1;
            policy.maximumCookedPayloadBytes = 1;
            policy.maximumComponentsPerObject = 1;
            policy.maximumReferencedAssets = 1;
            policy.maximumDirectNestedPlacements = 1;
            policy.maximumVariantInheritanceDepth = 1;
            policy.maximumNestedPrefabDepth = 1;
            policy.maximumOverrideRecords = 1;
            policy.maximumConflictAndOrphanRecords = 1;
            policy.maximumPropertyPathSegments = 1;
            policy.maximumOverrideValueBytes = 1;
            policy.maximumOverrideSetBytes = 1;
            policy.maximumBindingSlots = 1;
            policy.maximumBindingUses = 1;
            policy.maximumInstanceBindings = 1;
            policy.maximumRuntimeSpawnDepth = 1;
            auto limited = Limits(policy);

            std::vector<Assets::AssetRecord> records;
            std::vector<PrefabDependencySource> sources;
            for (std::uint16_t id = 1; id <= 10; ++id) {
                records.push_back(Record(Asset(id), "core.prefab", "assets/prefabs/" + std::to_string(id) + ".prefab"));
                sources.push_back({Concrete(Asset(id)), Revision(static_cast<std::uint8_t>(id))});
            }
            Assets::AssetRegistry boundedRegistry;
            PublishRegistry(boundedRegistry, std::move(records));
            std::vector<PrefabDependencySource> exactSources(sources.begin(), sources.begin() + 9);
            auto exact = BuildPrefabDependencyGraph(boundedRegistry.Snapshot(), std::move(exactSources), limited);
            REQUIRE(exact.HasValue());
            REQUIRE(exact.Value().Nodes().size() == 9);
            auto exhausted = BuildPrefabDependencyGraph(boundedRegistry.Snapshot(), std::move(sources), limited);
            REQUIRE(exhausted.HasError());
            RequireCode(exhausted.ErrorValue(), PrefabErrors::WorkBudgetExceeded);
        }
    }  // namespace
}  // namespace Horo::Prefab
