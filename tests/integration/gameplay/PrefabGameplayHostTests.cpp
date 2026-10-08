#include "GameplayModuleTestSupport.h"
#include "GameplayWorldComposition.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Prefab/PrefabSpawnService.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;

    /** @brief Counts only committed live entities through the public immutable Scene view. */
    std::size_t Count(const Runtime::RuntimeSceneService &scenes) {
        const auto active = scenes.ActiveScene();
        std::size_t count{};
        if (active)
            for (std::size_t index = 0; index < active->SlotCount(); ++index)
                if (active->EntityAt(index))
                    ++count;
        return count;
    }

    /** @brief Publishes the exact immutable two-entity cooked artifact used by the native module. */
    Sha256Digest PublishSpawnArtifact(Assets::MemoryAssetProvider &bytes, const Assets::AssetId asset, const Assets::AssetTypeId &type,
                                      const AssetCookTargetId &target) {
        Prefab::CookedPrefabData candidate;
        candidate.assetId = asset;
        Prefab::CookedPrefabEntity root;
        root.provenance = {asset, {}, ComputeSha256(std::as_bytes(std::span("source", 6)))};
        root.members.emplace_back(Gameplay::BehaviorComponent{{5}, Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover").Value()});
        auto child = root;
        child.parent = Prefab::CookedPrefabEntitySlot{0};
        child.provenance.sourceObject = Prefab::PrefabObjectAddress::Create({}, {7}).Value();
        std::get<Gameplay::BehaviorComponent>(child.members.front()).instanceId.value = 6;
        candidate.entities = {root, child};
        auto cooked = Prefab::CookedPrefab::Create(std::move(candidate), Prefab::PrefabLimitProfile::Create({}).Value());
        REQUIRE(cooked.HasValue());
        std::vector<std::uint8_t> payload;
        for (const auto byte : cooked.Value().Bytes())
            payload.push_back(std::to_integer<std::uint8_t>(byte));
        const auto digest = ComputeSha256(std::as_bytes(std::span(payload)));
        auto envelope = Assets::EncodeCookedArtifact({asset, type, target, {}, {}, digest, std::move(payload)});
        REQUIRE(envelope.HasValue());
        const auto artifactDigest = ComputeSha256(std::as_bytes(std::span(envelope.Value())));
        bytes.Insert(asset, std::move(envelope).Value());
        return artifactDigest;
    }

    /** @brief Builds the one native spawner with explicit artifact digest and exact runtime identity. */
    Runtime::RuntimeSceneDefinition MakeSpawnerScene(const Assets::AssetId asset, const Sha256Digest &artifactDigest) {
        Runtime::SceneDefinitionBuilder builder{{7}, {1}};
        Runtime::RuntimeEntityDefinition spawner;
        spawner.object = {1};
        spawner.components.behaviors.push_back({{1},
                                                Gameplay::BehaviorTypeId::Parse("game.tests.prefab_spawner").Value(),
                                                1,
                                                true,
                                                {{"asset", asset.ToString()}, {"digest", FormatSha256(artifactDigest)}}});
        builder.Add(std::move(spawner));
        return std::move(builder).Build().Value();
    }

    /** @brief Advances the published host route with the same fixed-step duration used by both lifecycle assertions. */
    void FixedTick(GameplayWorldComposition &gameplay, const std::uint64_t tick, const CancellationToken &cancellation) {
        REQUIRE(gameplay.FixedTick(Runtime::FixedStepContext{tick, Duration::FromNanoseconds(10'000'000), cancellation}).HasValue());
    }
}  // namespace

TEST_CASE("Production Gameplay composition routes a native module through cooked preparation Scene publication and despawn",
          "[prefab][gameplay][host]") {
    const auto asset = Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value();
    const auto type = Assets::AssetTypeId::Parse("core.prefab").Value();
    const auto target = AssetCookTargetId::Parse("linux-x64").Value();
    Assets::MemoryAssetProvider bytes;
    const auto artifactDigest = PublishSpawnArtifact(bytes, asset, type, target);
    JobSystem jobs{{1, 16}};
    Assets::AssetLoadService loads{jobs, bytes};
    Assets::AssetRegistry registry;
    REQUIRE(registry
                .Publish({{asset, type, ProjectPath::Parse("assets/spawn.prefab").Value(),
                           ProjectPath::Parse("assets/spawn.prefab.horo").Value()}})
                .status == Assets::AssetRegistryBuildStatus::Complete);
    Runtime::RuntimeSceneService scenes{registry, loads};
    std::unique_ptr<GameplayWorldComposition> gameplay;
    GameplayWorldComposition *activeGameplay{};
    REQUIRE(scenes.AddStructuralParticipant(MakeGameplayStructuralParticipant(activeGameplay)).HasValue());
    CancellationSource cancellation;
    REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
    REQUIRE(scenes.QueuePreparation(MakeSpawnerScene(asset, artifactDigest)).HasValue());
    Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation.Token()};
    REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
    Prefab::PrefabTemplateProvider templates{registry, loads, scenes, Prefab::PrefabLimitProfile::Create({}).Value()};
    REQUIRE(templates.Startup(cancellation.Token()).HasValue());
    GameplayWorldSelection selection{.moduleId = "game.tests",
                                     .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                     .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                     .prefabPermission = GameplayPrefabPermission::Granted};
    auto composition = GameplayWorldComposition::Create(scenes, templates, nullptr, 1, selection);
    REQUIRE(composition.HasValue());
    gameplay = std::move(composition).Value();
    activeGameplay = gameplay.get();
    FixedTick(*activeGameplay, 1, cancellation.Token());
    CHECK(Count(scenes) == 1);
    for (std::size_t attempt = 0; attempt < 2000 && Count(scenes) != 3; ++attempt) {
        REQUIRE(activeGameplay->OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        if (Count(scenes) != 3)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(Count(scenes) == 3);
    FixedTick(*activeGameplay, 2, cancellation.Token());
    REQUIRE(activeGameplay->OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
    CHECK(Count(scenes) == 1);
    activeGameplay->Shutdown();
    activeGameplay = nullptr;
    templates.Shutdown();
    scenes.Shutdown();
}
