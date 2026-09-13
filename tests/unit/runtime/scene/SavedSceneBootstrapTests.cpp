#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Runtime/Scene/SavedSceneBootstrap.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Runtime {
    namespace {
        template <typename Identity> Identity Id(const std::uint8_t marker) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = marker;
            return Identity::FromBytes(bytes).Value();
        }

        Assets::AssetTypeId SceneAssetType(const std::string_view value = "core.scene") {
            return Assets::AssetTypeId::Parse(value).Value();
        }

        ProjectPath Path(const std::string_view value) {
            return ProjectPath::Parse(value).Value();
        }

        Sha256Digest Digest(const std::uint8_t marker) {
            Sha256Digest digest;
            digest.bytes.back() = marker;
            return digest;
        }

        RuntimeEntityDefinition Entity(const std::uint64_t id) {
            RuntimeEntityDefinition entity;
            entity.object = SceneObjectId{id};
            return entity;
        }

        RuntimeSceneDefinition Definition(const std::uint64_t id = 7, const std::uint64_t revision = 3) {
            SceneDefinitionBuilder builder{SceneDefinitionId{id}, SceneDefinitionRevision{revision}};
            builder.Add(Entity(10));
            builder.Add(Entity(20));
            return std::move(builder).Build().Value();
        }

        SavedSceneBootstrapDescriptor Descriptor() {
            return {.world = Id<SaveWorldId>(1),
                    .baseScene = Id<SaveBaseSceneId>(2),
                    .expectedAssetType = SceneAssetType(),
                    .definition = SceneDefinitionId{7},
                    .revision = SceneDefinitionRevision{3},
                    .contentDigest = Digest(4),
                    .spawnAnchor = SceneObjectId{20},
                    .transition = {.slot = Id<SaveGameSlotId>(5), .generation = Id<SlotGenerationId>(6), .priorWorld = Id<SaveWorldId>(7)}};
        }

        Assets::AssetRegistrySnapshot Registry(const SavedSceneBootstrapDescriptor &descriptor,
                                               const Assets::AssetTypeId &type = SceneAssetType()) {
            Assets::AssetRegistry registry;
            const Assets::AssetId asset = Assets::AssetId::FromBytes(descriptor.baseScene.Bytes());
            const std::string identity = asset.ToString();
            const auto report = registry.Publish(
                {Assets::AssetRecord{asset, type, Path("unrelated/source/" + identity), Path("unrelated/metadata/" + identity)}});
            REQUIRE(report.status == Assets::AssetRegistryBuildStatus::Complete);
            return registry.Snapshot();
        }

        FrameContext Context(const CancellationToken &token) {
            return FrameContext{1, {}, 0.0, 0, {}, false, token};
        }

        void RequireCode(const auto &result, const std::string_view code) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == code);
        }

        TEST_CASE("Saved scene bootstrap validates every durable identity and content requirement", "[runtime][scene][save_bootstrap]") {
            auto descriptor = Descriptor();
            REQUIRE(descriptor.IsValid());

            descriptor.world = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.baseScene = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.expectedAssetType = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.definition = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.revision = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.contentDigest = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.spawnAnchor = SceneObjectId{};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.transition.slot = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.transition.generation = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.transition.priorWorld = SaveWorldId{};
            REQUIRE_FALSE(descriptor.IsValid());

            descriptor = Descriptor();
            descriptor.spawnAnchor.reset();
            descriptor.transition.priorWorld.reset();
            REQUIRE(descriptor.IsValid());
        }

        TEST_CASE("Saved scene bootstrap resolves a logical cooked asset and owns deterministic defaults",
                  "[runtime][scene][save_bootstrap]") {
            const auto descriptor = Descriptor();
            const auto registry = Registry(descriptor);

            auto prepared = PrepareSavedSceneBootstrap(descriptor, SceneAssetType(), registry, Definition(), Digest(4));
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value().BaseSceneAsset() == Assets::AssetId::FromBytes(descriptor.baseScene.Bytes()));
            REQUIRE(prepared.Value().RegistryRevision() == registry.Revision());
            REQUIRE(prepared.Value().Descriptor() == descriptor);
            REQUIRE(prepared.Value().Definition().Id() == descriptor.definition);

            auto first = RuntimeScene::Create(prepared.Value().Definition(), SceneRuntimeId{1});
            auto second = RuntimeScene::Create(prepared.Value().Definition(), SceneRuntimeId{2});
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());
            REQUIRE(first.Value()->View().Find(*descriptor.spawnAnchor).has_value());
            REQUIRE(second.Value()->View().Find(*descriptor.spawnAnchor).has_value());
            REQUIRE(first.Value()->View().SlotCount() == second.Value()->View().SlotCount());
        }

        TEST_CASE("Saved scene bootstrap rejects unavailable and incompatible baselines before queueing",
                  "[runtime][scene][save_bootstrap]") {
            const auto descriptor = Descriptor();

            RequireCode(PrepareSavedSceneBootstrap(descriptor, {}, Registry(descriptor), Definition(), Digest(4)),
                        "scene.save_bootstrap.invalid");

            Assets::AssetRegistry unrelatedRegistry;
            const auto unrelated = unrelatedRegistry.Publish(
                {Assets::AssetRecord{Assets::AssetId::FromBytes(Id<SaveBaseSceneId>(99).Bytes()), SceneAssetType(),
                                     Path("unrelated/source.scene"), Path("unrelated/source.scene.horo")}});
            REQUIRE(unrelated.status == Assets::AssetRegistryBuildStatus::Complete);
            RequireCode(PrepareSavedSceneBootstrap(descriptor, SceneAssetType(), unrelatedRegistry.Snapshot(), Definition(), Digest(4)),
                        "scene.save_bootstrap.asset_unavailable");

            RequireCode(PrepareSavedSceneBootstrap(descriptor, SceneAssetType(), Registry(descriptor, SceneAssetType("core.prefab")),
                                                   Definition(), Digest(4)),
                        "scene.save_bootstrap.incompatible");
            auto selfApprovedPrefab = descriptor;
            selfApprovedPrefab.expectedAssetType = SceneAssetType("core.prefab");
            RequireCode(PrepareSavedSceneBootstrap(selfApprovedPrefab, SceneAssetType(),
                                                   Registry(selfApprovedPrefab, SceneAssetType("core.prefab")), Definition(), Digest(4)),
                        "scene.save_bootstrap.incompatible");
            RequireCode(PrepareSavedSceneBootstrap(descriptor, SceneAssetType(), Registry(descriptor), Definition(8), Digest(4)),
                        "scene.save_bootstrap.incompatible");
            RequireCode(PrepareSavedSceneBootstrap(descriptor, SceneAssetType(), Registry(descriptor), Definition(7, 4), Digest(4)),
                        "scene.save_bootstrap.incompatible");
            RequireCode(PrepareSavedSceneBootstrap(descriptor, SceneAssetType(), Registry(descriptor), Definition(), Digest(9)),
                        "scene.save_bootstrap.incompatible");

            auto missingSpawn = descriptor;
            missingSpawn.spawnAnchor = SceneObjectId{99};
            RequireCode(PrepareSavedSceneBootstrap(missingSpawn, SceneAssetType(), Registry(missingSpawn), Definition(), Digest(4)),
                        "scene.save_bootstrap.spawn_missing");
        }

        TEST_CASE("Malformed restore evidence cannot create pending work or replace the active scene", "[runtime][scene][save_bootstrap]") {
            CancellationSource cancellation;
            RuntimeSceneService service;
            REQUIRE(service.Startup(cancellation.Token()).HasValue());
            REQUIRE(service.QueuePreparation(Definition(42, 1)).HasValue());
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE(service.ActiveScene().has_value());
            REQUIRE(service.ActiveScene()->DefinitionId() == SceneDefinitionId{42});

            auto malformed = Descriptor();
            malformed.transition.generation = {};
            const auto rejected = PrepareSavedSceneBootstrap(malformed, SceneAssetType(), Registry(malformed), Definition(), Digest(4));
            RequireCode(rejected, "scene.save_bootstrap.invalid");
            REQUIRE(service.ActiveScene()->DefinitionId() == SceneDefinitionId{42});

            auto compatible = Descriptor();
            auto prepared = PrepareSavedSceneBootstrap(compatible, SceneAssetType(), Registry(compatible), Definition(), Digest(4));
            REQUIRE(prepared.HasValue());
            auto proof = std::move(prepared).Value();
            RequireCode(std::move(prepared).Value().Queue(service), "scene.save_bootstrap.invalid");
            REQUIRE(std::move(proof).Queue(service).HasValue());
            REQUIRE(service.ActiveScene()->DefinitionId() == SceneDefinitionId{42});
            RequireCode(std::move(proof).Queue(service), "scene.save_bootstrap.invalid");
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE(service.ActiveScene()->DefinitionId() == compatible.definition);
            service.Shutdown();
        }
    }  // namespace
}  // namespace Horo::Runtime
