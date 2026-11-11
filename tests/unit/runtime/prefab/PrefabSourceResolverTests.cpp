#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSourceResolver.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Prefab {
    namespace {
        Assets::AssetId ResolverAsset(const std::uint8_t suffix) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = suffix;
            return Assets::AssetId::FromBytes(bytes);
        }

        Application::HoroVersion ResolverProjectVersion() {
            return Application::ParseHoroVersion("1.2.3").Value();
        }

        PrefabSourceRevision ResolverRevision(const std::uint8_t suffix) {
            Sha256Digest digest{};
            digest.bytes.back() = suffix;
            return {ResolverProjectVersion(), digest};
        }

        PrefabLimitProfile ResolverLimits(PrefabProjectPolicy policy = {}) {
            return PrefabLimitProfile::Create(std::move(policy)).Value();
        }

        PrefabObjectNode ResolverObject(const std::uint32_t id, std::optional<LocalObjectId> parent = std::nullopt) {
            return {.localId = {id}, .parentLocalId = parent, .name = id == 0 ? "Root" : "Child"};
        }

        PrefabDocument ResolverDocument(const Assets::AssetId id, std::vector<PrefabObjectNode> objects,
                                        std::optional<PrefabComposition> composition = std::nullopt,
                                        std::vector<Assets::AssetId> dependencies = {}) {
            return PrefabDocument::Create({.projectVersion = ResolverProjectVersion(),
                                           .assetId = id,
                                           .objects = std::move(objects),
                                           .composition = std::move(composition),
                                           .referencedAssets = std::move(dependencies)},
                                          ResolverLimits())
                .Value();
        }

        Assets::AssetRecord ResolverRecord(const Assets::AssetId id, const std::string_view path) {
            return {id, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse(path).Value(),
                    ProjectPath::Parse(std::string{path} + ".horo").Value()};
        }

        void PublishResolverRegistry(Assets::AssetRegistry &registry, std::vector<Assets::AssetRecord> records) {
            const auto publication = registry.Publish(std::move(records));
            REQUIRE(publication.status == Assets::AssetRegistryBuildStatus::Complete);
        }

        /** @brief Builds the shared two-source nested resolver fixture with a caller-selected placement and inner hierarchy. */
        Result<PrefabSourceResolverSnapshot> BuildNestedResolverSnapshot(Assets::AssetRegistry &registry, const LocalObjectId placement,
                                                                         std::vector<PrefabObjectNode> nestedObjects) {
            const Assets::AssetId outer = ResolverAsset(1);
            const Assets::AssetId nested = ResolverAsset(2);
            const PrefabSourceRevision nestedRevision = ResolverRevision(2);
            PublishResolverRegistry(registry, {ResolverRecord(outer, "assets/prefabs/outer.prefab"),
                                               ResolverRecord(nested, "assets/prefabs/nested.prefab")});
            PrefabComposition composition{.nestedPlacements = {{.placementLocalId = placement,
                                                                .sourcePrefab = PrefabAssetReference::Create(nested).Value(),
                                                                .authoredAgainst = nestedRevision}}};
            return BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                     {{ResolverDocument(outer, {ResolverObject(0)}, std::move(composition), {nested}),
                                                       ResolverRevision(1)},
                                                      {ResolverDocument(nested, std::move(nestedObjects)), nestedRevision}},
                                                     ResolverLimits());
        }

        /** @brief Captures an actual two-edge nested chain so a one-edge policy tests the first rejected edge, not the root. */
        Result<PrefabSourceResolverSnapshot> BuildTwoEdgeResolverSnapshot(Assets::AssetRegistry &registry) {
            const auto outer = ResolverAsset(1), middle = ResolverAsset(2), leaf = ResolverAsset(3);
            PublishResolverRegistry(registry, {ResolverRecord(outer, "assets/prefabs/outer.prefab"),
                                               ResolverRecord(middle, "assets/prefabs/middle.prefab"),
                                               ResolverRecord(leaf, "assets/prefabs/leaf.prefab")});
            const PrefabComposition first{.nestedPlacements = {{.placementLocalId = {1},
                                                                .sourcePrefab = PrefabAssetReference::Create(middle).Value(),
                                                                .authoredAgainst = ResolverRevision(2)}}};
            const PrefabComposition second{.nestedPlacements = {{.placementLocalId = {7},
                                                                 .sourcePrefab = PrefabAssetReference::Create(leaf).Value(),
                                                                 .authoredAgainst = ResolverRevision(3)}}};
            return BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                     {{ResolverDocument(outer, {ResolverObject(0)}, first, {middle}), ResolverRevision(1)},
                                                      {ResolverDocument(middle, {ResolverObject(0)}, second, {leaf}), ResolverRevision(2)},
                                                      {ResolverDocument(leaf, {ResolverObject(0)}), ResolverRevision(3)}},
                                                     ResolverLimits());
        }

        TEST_CASE("Prefab source resolver pins one registry and document context across nested expansion", "[unit][prefab][resolver]") {
            const auto outer = ResolverAsset(1);
            const auto nested = ResolverAsset(2);
            Assets::AssetRegistry registry;
            auto snapshot =
                BuildNestedResolverSnapshot(registry, LocalObjectId{7}, {ResolverObject(0), ResolverObject(4, LocalObjectId{0})});
            REQUIRE(snapshot.HasValue());

            auto candidate = snapshot.Value().Resolve(outer, PrefabInstanceId::Create(11).Value(), ResolverLimits());
            REQUIRE(candidate.HasValue());
            REQUIRE(candidate.Value().Revision().registry == registry.Snapshot().Revision());
            REQUIRE(candidate.Value().Revision().rootSource == ResolverRevision(1));
            REQUIRE(candidate.Value().Objects().size() == 3);
            REQUIRE(candidate.Value().Objects()[1].sourcePrefab == nested);
            REQUIRE(candidate.Value().Objects()[1].key.instance == PrefabInstanceId::Create(11).Value());
            REQUIRE(candidate.Value().Objects()[1].key.object.NestedInstanceScope().size() == 1);
            REQUIRE(candidate.Value().Objects()[1].key.object.NestedInstanceScope().front() == LocalObjectId{7});
            REQUIRE(candidate.Value().Objects()[2].key.object.SourceObject() == LocalObjectId{4});
        }

        TEST_CASE("Prefab source resolver enforces aggregate object and recursion boundaries transactionally",
                  "[unit][prefab][resolver][boundary]") {
            const auto outer = ResolverAsset(1);
            Assets::AssetRegistry registry;
            auto snapshot = BuildNestedResolverSnapshot(registry, LocalObjectId{1}, {ResolverObject(0)}).Value();
            PrefabProjectPolicy policy;
            policy.maximumObjectCount = 1;
            auto result = snapshot.Resolve(outer, PrefabInstanceId::Create(1).Value(), ResolverLimits(policy));
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PrefabErrors::ObjectCountExceeded.code.Value());

            policy.maximumObjectCount = 2;
            policy.maximumNestedPrefabDepth = 1;
            result = snapshot.Resolve(outer, PrefabInstanceId::Create(1).Value(), ResolverLimits(policy));
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Objects().size() == 2);
            const auto accepted = result.Value();
            auto twoEdges = BuildTwoEdgeResolverSnapshot(registry);
            REQUIRE(twoEdges.HasValue());
            policy.maximumObjectCount = 3;
            result = twoEdges.Value().Resolve(outer, PrefabInstanceId::Create(1).Value(), ResolverLimits(policy));
            INFO((result.HasError() ? result.ErrorValue().message
                                    : "Two-edge nested chain unexpectedly admitted under an edge limit of one."));
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PrefabErrors::HierarchyDepthExceeded.code.Value());
            CHECK(accepted.Objects().size() == 2);
        }

        TEST_CASE("Prefab nested fanout admits exact expansion and preserves retained candidates on overflow",
                  "[unit][prefab][resolver][stress]") {
            const auto outer = ResolverAsset(1);
            const auto leaf = ResolverAsset(2);
            Assets::AssetRegistry registry;
            PublishResolverRegistry(registry, {ResolverRecord(outer, "assets/prefabs/outer.prefab"),
                                               ResolverRecord(leaf, "assets/prefabs/leaf.prefab")});
            constexpr std::uint32_t placements = 64;
            PrefabComposition composition;
            for (std::uint32_t index = 1; index <= placements; ++index) {
                composition.nestedPlacements.push_back({.placementLocalId = {index},
                                                        .sourcePrefab = PrefabAssetReference::Create(leaf).Value(),
                                                        .authoredAgainst = ResolverRevision(2)});
            }
            const auto snapshot =
                BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                  {{ResolverDocument(outer, {ResolverObject(0)}, std::move(composition), {leaf}),
                                                    ResolverRevision(1)},
                                                   {ResolverDocument(leaf, {ResolverObject(0), ResolverObject(17, LocalObjectId{0})}),
                                                    ResolverRevision(2)}},
                                                  ResolverLimits());
            REQUIRE(snapshot.HasValue());
            PrefabProjectPolicy policy;
            policy.maximumDirectNestedPlacements = placements;
            policy.maximumNestedPrefabDepth = 1;
            policy.maximumObjectCount = 1 + 2 * placements;
            const auto instance = PrefabInstanceId::Create(1).Value();
            const auto accepted = snapshot.Value().Resolve(outer, instance, ResolverLimits(policy));
            REQUIRE(accepted.HasValue());
            REQUIRE(accepted.Value().Objects().size() == policy.maximumObjectCount);
            for (std::uint32_t index = 0; index < placements; ++index) {
                const auto &object = accepted.Value().Objects()[1 + 2 * index];
                REQUIRE(object.key.object.NestedInstanceScope().size() == 1);
                CHECK(object.key.object.NestedInstanceScope().front() == LocalObjectId{index + 1});
                CHECK(accepted.Value().Objects()[2 + 2 * index].key.object.SourceObject() == LocalObjectId{17});
            }
            --policy.maximumObjectCount;
            for (unsigned attempt = 0; attempt < 3; ++attempt) {
                const auto rejected = snapshot.Value().Resolve(outer, instance, ResolverLimits(policy));
                REQUIRE(rejected.HasError());
                CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::ObjectCountExceeded.code.Value());
                CHECK(accepted.Value().Objects().size() == 1 + 2 * placements);
                CHECK(snapshot.Value().DependencyGraph().Nodes().size() == 2);
            }
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            const auto cancelled = snapshot.Value().Resolve(outer, instance, ResolverLimits(), cancellation.Token());
            REQUIRE(cancelled.HasError());
            CHECK(cancelled.ErrorValue().code.Value() == PrefabErrors::Cancelled.code.Value());
            CHECK(accepted.Value().Objects().size() == 1 + 2 * placements);
        }

        TEST_CASE("Prefab source resolver inherits a variant parent hierarchy", "[unit][prefab][resolver][variant]") {
            const auto base = ResolverAsset(1);
            const auto variant = ResolverAsset(2);
            const auto baseRevision = ResolverRevision(1);
            Assets::AssetRegistry registry;
            PublishResolverRegistry(registry, {ResolverRecord(base, "assets/prefabs/base.prefab"),
                                               ResolverRecord(variant, "assets/prefabs/variant.prefab")});
            PrefabComposition composition{
                .variantParent = PrefabAssetReference::Create(base).Value(),
                .variantAuthoredAgainst = baseRevision,
            };
            auto snapshot =
                BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                  {{ResolverDocument(base, {ResolverObject(0), ResolverObject(4, LocalObjectId{0})}),
                                                    baseRevision},
                                                   {ResolverDocument(variant, {}, std::move(composition), {base}), ResolverRevision(2)}},
                                                  ResolverLimits())
                    .Value();

            const auto candidate = snapshot.Resolve(variant, PrefabInstanceId::Create(3).Value(), ResolverLimits());
            REQUIRE(candidate.HasValue());
            CHECK(candidate.Value().RootAsset() == variant);
            REQUIRE(candidate.Value().Objects().size() == 2);
            CHECK(candidate.Value().Objects()[0].sourcePrefab == base);
            CHECK(candidate.Value().Objects()[1].key.object.SourceObject() == LocalObjectId{4});
        }

        TEST_CASE("Prefab variant stress admits the hard inheritance depth and rejects its next edge",
                  "[unit][prefab][resolver][variant][stress]") {
            constexpr std::uint8_t depth = static_cast<std::uint8_t>(PrefabHardLimits::VariantInheritanceDepth);
            Assets::AssetRegistry registry;
            std::vector<Assets::AssetRecord> records;
            std::vector<PrefabDependencySource> sources;
            for (std::uint8_t index = 1; index <= depth + 2; ++index) {
                const auto asset = ResolverAsset(index);
                records.push_back(ResolverRecord(asset, "assets/prefabs/" + std::to_string(index) + ".prefab"));
                if (index == 1) {
                    sources.push_back({ResolverDocument(asset, {ResolverObject(0)}), ResolverRevision(index)});
                    continue;
                }
                const auto parent = static_cast<std::uint8_t>(index - 1);
                PrefabComposition composition{.variantParent = PrefabAssetReference::Create(ResolverAsset(parent)).Value(),
                                              .variantAuthoredAgainst = ResolverRevision(parent)};
                sources.push_back({ResolverDocument(asset, {}, std::move(composition), {ResolverAsset(parent)}), ResolverRevision(index)});
            }
            PublishResolverRegistry(registry, std::move(records));
            const auto snapshot = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), std::move(sources), ResolverLimits());
            REQUIRE(snapshot.HasValue());
            const auto instance = PrefabInstanceId::Create(1).Value();
            const auto accepted = snapshot.Value().Resolve(ResolverAsset(depth + 1), instance, ResolverLimits());
            REQUIRE(accepted.HasValue());
            REQUIRE(accepted.Value().Objects().size() == 1);
            CHECK(accepted.Value().Objects().front().sourcePrefab == ResolverAsset(1));
            const auto rejected = snapshot.Value().Resolve(ResolverAsset(depth + 2), instance, ResolverLimits());
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::HierarchyDepthExceeded.code.Value());
            CHECK(accepted.Value().Objects().front().sourcePrefab == ResolverAsset(1));
            REQUIRE(snapshot.Value().Resolve(ResolverAsset(depth + 1), instance, ResolverLimits()).HasValue());
        }

        TEST_CASE("Prefab source resolver rejects malformed cyclic source graphs", "[unit][prefab][resolver][malformed]") {
            const auto first = ResolverAsset(1);
            const auto second = ResolverAsset(2);
            const auto firstRevision = ResolverRevision(1);
            const auto secondRevision = ResolverRevision(2);
            Assets::AssetRegistry registry;
            PublishResolverRegistry(registry, {ResolverRecord(first, "assets/prefabs/first.prefab"),
                                               ResolverRecord(second, "assets/prefabs/second.prefab")});
            PrefabComposition firstComposition{.nestedPlacements = {{.placementLocalId = {1},
                                                                     .sourcePrefab = PrefabAssetReference::Create(second).Value(),
                                                                     .authoredAgainst = secondRevision}}};
            PrefabComposition secondComposition{.nestedPlacements = {{.placementLocalId = {2},
                                                                      .sourcePrefab = PrefabAssetReference::Create(first).Value(),
                                                                      .authoredAgainst = firstRevision}}};
            auto snapshot =
                BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                  {{ResolverDocument(first, {ResolverObject(0)}, std::move(firstComposition), {second}),
                                                    firstRevision},
                                                   {ResolverDocument(second, {ResolverObject(0)}, std::move(secondComposition), {first}),
                                                    secondRevision}},
                                                  ResolverLimits())
                    .Value();
            auto result = snapshot.Resolve(first, PrefabInstanceId::Create(1).Value(), ResolverLimits());
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PrefabErrors::DependencyGraphInvalid.code.Value());
        }

        TEST_CASE("Prefab captured graph revisions invalidate only reachable publications", "[unit][prefab][resolver][lifecycle]") {
            const auto root = ResolverAsset(1);
            const auto resource = ResolverAsset(2);
            const auto unrelated = ResolverAsset(3);
            Assets::AssetRegistry registry;
            PublishResolverRegistry(registry,
                                    {ResolverRecord(root, "assets/root.prefab"), ResolverRecord(resource, "assets/resource.prefab"),
                                     ResolverRecord(unrelated, "assets/other.prefab")});
            auto snapshot = BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                              {{ResolverDocument(root, {ResolverObject(0)}, std::nullopt, {resource}),
                                                                ResolverRevision(1)},
                                                               {ResolverDocument(resource, {ResolverObject(0)}), ResolverRevision(2)},
                                                               {ResolverDocument(unrelated, {ResolverObject(0)}), ResolverRevision(3)}},
                                                              ResolverLimits())
                                .Value();
            const auto candidate = snapshot.Resolve(root, PrefabInstanceId::Create(1).Value(), ResolverLimits()).Value();
            REQUIRE(candidate.Revision().dependencies.size() == 2);
            CHECK(candidate.Revision().dependencies[0].assetId == root);
            CHECK(candidate.Revision().dependencies[1].sourceRevision == ResolverRevision(2));
            const auto oldObjects = std::vector(candidate.Objects().begin(), candidate.Objects().end());
            PublishResolverRegistry(registry,
                                    {ResolverRecord(root, "assets/moved.prefab"), ResolverRecord(resource, "assets/resource.prefab"),
                                     ResolverRecord(unrelated, "assets/other.prefab")});
            auto sources = std::vector(snapshot.Sources().begin(), snapshot.Sources().end());
            sources.back().sourceRevision = ResolverRevision(9);
            auto current = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), sources, ResolverLimits()).Value();
            REQUIRE(current.ValidateRevisionPublication(root, candidate.Revision(), std::array{unrelated}, ResolverLimits()).HasValue());
            sources[1].sourceRevision = ResolverRevision(8);
            current = BuildPrefabSourceResolverSnapshot(registry.Snapshot(), sources, ResolverLimits()).Value();
            CHECK(current.ValidateRevisionPublication(root, candidate.Revision(), {}, ResolverLimits()).ErrorValue().code.Value() ==
                  PrefabErrors::ResolutionStale.code.Value());
            CHECK(std::ranges::equal(candidate.Objects(), oldObjects));
            CHECK(candidate.Revision().dependencies[1].sourceRevision == ResolverRevision(2));
        }

        TEST_CASE("Prefab nested dependency edits are detected without relying on the outer revision",
                  "[unit][prefab][resolver][lifecycle]") {
            Assets::AssetRegistry registry;
            const auto root = ResolverAsset(1);
            const auto nested = ResolverAsset(2);
            const auto snapshot = BuildNestedResolverSnapshot(registry, LocalObjectId{7}, {ResolverObject(0)}).Value();
            const auto candidate = snapshot.Resolve(root, PrefabInstanceId::Create(1).Value(), ResolverLimits()).Value();
            const auto newRevision = ResolverRevision(8);
            PrefabComposition composition{.nestedPlacements = {{.placementLocalId = {7},
                                                                .sourcePrefab = PrefabAssetReference::Create(nested).Value(),
                                                                .authoredAgainst = newRevision}}};
            const auto current =
                BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                  {{ResolverDocument(root, {ResolverObject(0)}, composition, {nested}),
                                                    ResolverRevision(1)},
                                                   {ResolverDocument(nested, {ResolverObject(0), ResolverObject(4, LocalObjectId{0})}),
                                                    newRevision}},
                                                  ResolverLimits())
                    .Value();
            REQUIRE(candidate.Revision().rootSource == ResolverRevision(1));
            CHECK(current.ValidateRevisionPublication(root, candidate.Revision(), {}, ResolverLimits()).HasError());
            CHECK(candidate.Objects().size() == 2);
            CHECK(current.Resolve(root, PrefabInstanceId::Create(1).Value(), ResolverLimits()).Value().Objects().size() == 3);
        }

        TEST_CASE("Prefab revision fencing covers ordinary resource publications and malformed evidence",
                  "[unit][prefab][resolver][malformed]") {
            const auto root = ResolverAsset(1);
            const auto resource = ResolverAsset(2);
            Assets::AssetRegistry registry;
            auto record = ResolverRecord(resource, "assets/material.bin");
            record.type = Assets::AssetTypeId::Parse("core.material").Value();
            PublishResolverRegistry(registry, {ResolverRecord(root, "assets/root.prefab"), record});
            auto snapshot = BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                              {{ResolverDocument(root, {ResolverObject(0)}, std::nullopt, {resource}),
                                                                ResolverRevision(1)}},
                                                              ResolverLimits())
                                .Value();
            const auto candidate = snapshot.Resolve(root, PrefabInstanceId::Create(1).Value(), ResolverLimits()).Value();
            REQUIRE(snapshot.ValidateRevisionPublication(root, candidate.Revision(), {}, ResolverLimits()).HasValue());
            CHECK(snapshot.ValidateRevisionPublication(root, candidate.Revision(), std::array{resource}, ResolverLimits()).HasError());
            CHECK(snapshot.ValidateRevisionPublication(root, candidate.Revision(), std::array{Assets::AssetId{}}, ResolverLimits())
                      .HasError());
            auto malformed = candidate.Revision();
            malformed.dependencies.clear();
            CHECK(snapshot.ValidateRevisionPublication(root, malformed, {}, ResolverLimits()).HasError());
            malformed = candidate.Revision();
            malformed.dependencies.push_back(malformed.dependencies.front());
            CHECK(snapshot.ValidateRevisionPublication(root, malformed, {}, ResolverLimits()).HasError());
            malformed = candidate.Revision();
            malformed.registry.value += 1;
            CHECK(snapshot.ValidateRevisionPublication(root, malformed, {}, ResolverLimits()).HasError());
            const std::vector excessive(ResolverLimits().MaximumExpansionWorkItems() + 1, resource);
            CHECK(snapshot.ValidateRevisionPublication(root, candidate.Revision(), excessive, ResolverLimits()).ErrorValue().code.Value() ==
                  PrefabErrors::WorkBudgetExceeded.code.Value());
        }

        TEST_CASE("Prefab publication gate rejects stale registry and document completions", "[unit][prefab][resolver][lifecycle]") {
            const auto root = ResolverAsset(1);
            Assets::AssetRegistry registry;
            PublishResolverRegistry(registry, {ResolverRecord(root, "assets/prefabs/original.prefab")});
            auto snapshot =
                BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {{ResolverDocument(root, {ResolverObject(0)}), ResolverRevision(1)}},
                                                  ResolverLimits())
                    .Value();
            auto candidate = snapshot.Resolve(root, PrefabInstanceId::Create(1).Value(), ResolverLimits()).Value();
            REQUIRE(ValidatePrefabCandidatePublication(candidate, candidate.Revision()).HasValue());

            PublishResolverRegistry(registry, {ResolverRecord(root, "assets/prefabs/moved.prefab")});
            PrefabResolutionRevision moved{registry.Snapshot().Revision(), candidate.Revision().rootSource};
            REQUIRE(ValidatePrefabCandidatePublication(candidate, moved).ErrorValue().code.Value() ==
                    PrefabErrors::ResolutionStale.code.Value());

            PrefabResolutionRevision edited{candidate.Revision().registry, ResolverRevision(9)};
            REQUIRE(ValidatePrefabCandidatePublication(candidate, edited).ErrorValue().code.Value() ==
                    PrefabErrors::ResolutionStale.code.Value());
        }
    }  // namespace
}  // namespace Horo::Prefab
