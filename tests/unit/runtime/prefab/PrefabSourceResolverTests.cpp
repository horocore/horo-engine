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
            REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
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
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PrefabErrors::HierarchyDepthExceeded.code.Value());
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
            const std::vector<Assets::AssetId> excessive(ResolverLimits().MaximumExpansionWorkItems() + 1, resource);
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
