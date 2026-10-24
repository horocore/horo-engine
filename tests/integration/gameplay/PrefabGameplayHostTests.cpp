#include "GameplayModuleTestSupport.h"
#include "GameplayWorldComposition.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSpawnService.h"

#include <bit>
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

    /** @brief Retains module-produced values across real native unload and later host weak-slot retirement. */
    struct EscapedPrefabOperations final {
        Prefab::PrefabOperation spawn;
        Prefab::PrefabOperation despawn;
        Prefab::PrefabInstance instance;
        std::unique_ptr<Platform::DynamicLibrary> image;
    };

    /** @brief Copies values through a real module export; the function pointer never outlives the pinned call. */
    EscapedPrefabOperations CapturePrefabOperations() {
        auto loaded = Platform::LoadDynamicLibrary(HORO_TEST_GAME_MODULE_PATH);
        REQUIRE(loaded.HasValue());
        EscapedPrefabOperations escaped;
        escaped.image = std::move(loaded).Value();
        void *symbol = escaped.image->GetSymbol("HoroTestCopyPrefabOperations");
        REQUIRE(symbol != nullptr);
        using CopyOperations = void (*)(Prefab::PrefabOperation *, Prefab::PrefabOperation *, Prefab::PrefabInstance *);
        static_assert(sizeof(CopyOperations) == sizeof(symbol));
        const auto copy = std::bit_cast<CopyOperations>(symbol);
        copy(&escaped.spawn, &escaped.despawn, &escaped.instance);
        return escaped;
    }

    /** @brief Observes host-owned completion storage after its originating module code has been unloaded. */
    void RequireCommitted(const EscapedPrefabOperations &escaped) {
        REQUIRE(escaped.spawn.State() == Prefab::PrefabSpawnState::Committed);
        REQUIRE(escaped.spawn.Spawned().HasValue());
        REQUIRE(escaped.instance.Root().IsValid());
        REQUIRE(escaped.despawn.State() == Prefab::PrefabSpawnState::Committed);
    }

    /** @brief Persistent payload admitted by the real module's frozen schema without interpreting or rewriting its bytes. */
    Prefab::RawComponentPayload NativeMovementComponent() {
        Prefab::RawComponentPayload component{.instance = Prefab::PrefabComponentInstanceId::Create(42).Value(),
                                              .component = {.typeId =
                                                                Gameplay::ComponentTypeId::Parse("game.tests.movement_settings").Value(),
                                                            .schemaVersion = 2,
                                                            .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson}};
        for (const unsigned char value : std::string_view{R"({"speed":4.25,"retained":"opaque"})"})
            component.component.payload.push_back(static_cast<std::byte>(value));
        return component;
    }

    /** @brief Publishes the exact immutable two-entity cooked artifact used by the native module. */
    Sha256Digest PublishSpawnArtifact(Assets::MemoryAssetProvider &bytes, const Assets::AssetId asset, const Assets::AssetTypeId &type,
                                      const AssetCookTargetId &target, const bool includeNativeComponent = false) {
        Prefab::CookedPrefabData candidate;
        candidate.assetId = asset;
        Prefab::CookedPrefabEntity root;
        root.provenance = {asset, {}, ComputeSha256(std::as_bytes(std::span("source", 6)))};
        root.members.emplace_back(Gameplay::BehaviorComponent{{5}, Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover").Value()});
        if (includeNativeComponent)
            root.members.emplace_back(NativeMovementComponent());
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

    /** @brief Asserts complete actual native spawn and despawn publication before testing post-image observation. */
    void RequireSpawnThenDespawn(GameplayWorldComposition &gameplay, const Runtime::RuntimeSceneService &scenes,
                                 const Runtime::FrameContext &frame, const CancellationToken &cancellation,
                                 const bool nativeComponents = false) {
        FixedTick(gameplay, 1, cancellation);
        CHECK(Count(scenes) == 1);
        for (std::size_t attempt = 0; attempt < 2000 && Count(scenes) != 3; ++attempt) {
            REQUIRE(gameplay.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
            if (Count(scenes) != 3)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(Count(scenes) == 3);
        if (nativeComponents) {
            const auto scene = scenes.ActiveScene();
            const auto expected = NativeMovementComponent().component;
            std::size_t projected{};
            for (std::size_t index = 0; index < scene->SlotCount(); ++index) {
                const auto entity = scene->EntityAt(index);
                if (!entity || entity->components->gameplayComponents.empty())
                    continue;
                REQUIRE(entity->components->gameplayComponents.size() == 1);
                CHECK(entity->components->gameplayComponents.front() == expected);
                ++projected;
            }
            CHECK(projected == 2);
        }
        FixedTick(gameplay, 2, cancellation);
        REQUIRE(gameplay.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        CHECK(Count(scenes) == 1);
    }

    /** @brief Owns authorities in dependency order, including the stable structural-participant slot. */
    struct HostFixture final {
        const Assets::AssetId asset{Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value()};
        const Assets::AssetTypeId type{Assets::AssetTypeId::Parse("core.prefab").Value()};
        const AssetCookTargetId target{AssetCookTargetId::Parse("linux-x64").Value()};
        bool nativeComponents{};
        Assets::MemoryAssetProvider bytes;
        const Sha256Digest artifactDigest{PublishSpawnArtifact(bytes, asset, type, target, nativeComponents)};
        JobSystem jobs{{1, 16}};
        Assets::AssetLoadService loads{jobs, bytes};
        Assets::AssetRegistry registry;
        CancellationSource cancellation;
        GameplayWorldComposition *activeGameplay{};
        Runtime::RuntimeSceneService scenes{registry, loads};
        Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation.Token()};
        std::unique_ptr<Prefab::PrefabTemplateProvider> templates;
        std::unique_ptr<GameplayWorldComposition> gameplay;

        explicit HostFixture(const bool includeNativeComponents = false) : nativeComponents(includeNativeComponents) {
            REQUIRE(registry
                        .Publish({{asset, type, ProjectPath::Parse("assets/spawn.prefab").Value(),
                                   ProjectPath::Parse("assets/spawn.prefab.horo").Value()}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            REQUIRE(scenes.AddStructuralParticipant(MakeGameplayStructuralParticipant(activeGameplay)).HasValue());
            REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
            REQUIRE(scenes.QueuePreparation(MakeSpawnerScene(asset, artifactDigest)).HasValue());
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        }

        ~HostFixture() {
            activeGameplay = nullptr;
            if (gameplay)
                gameplay->Shutdown();
            scenes.Shutdown();
        }

        void Activate(const bool ownedProvider) {
            if (!ownedProvider) {
                templates = std::make_unique<Prefab::PrefabTemplateProvider>(registry, loads, scenes,
                                                                             Prefab::PrefabLimitProfile::Create({}).Value());
                REQUIRE(templates->Startup(cancellation.Token()).HasValue());
            }
            GameplayWorldSelection selection{.moduleId = "game.tests",
                                             .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                             .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                             .prefabPermission = GameplayPrefabPermission::Granted,
                                             .moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>()};
            const GameplayPrefabServices assets{registry, loads, Prefab::PrefabLimitProfile::Create({}).Value(), {}, cancellation.Token()};
            auto composition = ownedProvider ? GameplayWorldComposition::Create(scenes, assets, nullptr, 1, selection)
                                             : GameplayWorldComposition::Create(scenes, *templates, nullptr, 1, selection);
            REQUIRE(composition.HasValue());
            gameplay = std::move(composition).Value();
            activeGameplay = gameplay.get();
        }

        void Retire(EscapedPrefabOperations &escaped) {
            activeGameplay->Shutdown();
            UNSCOPED_INFO("Prefab host shutdown returned before actual native image release.");
            escaped.image.reset();  // Actual native unload precedes observation and service weak-slot retirement.
            UNSCOPED_INFO("Actual native image release returned before Scene retirement.");
            gameplay.reset();
            activeGameplay = nullptr;
            if (templates)
                templates->Shutdown();
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
            CHECK(Count(scenes) == 1);
            REQUIRE(scenes.QueueUnload().HasValue());
            UNSCOPED_INFO("Scene unload admission returned after actual native image release.");
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
            CHECK_FALSE(scenes.ActiveScene().has_value());
        }
    };
}  // namespace

TEST_CASE("Production Gameplay composition routes a native module through cooked preparation Scene publication and despawn",
          "[prefab][gameplay][host]") {
    HostFixture host;
    SECTION("borrowed provider") {
        host.Activate(false);
        RequireSpawnThenDespawn(*host.gameplay, host.scenes, host.frame, host.cancellation.Token());
        auto escaped = CapturePrefabOperations();
        host.Retire(escaped);
        RequireCommitted(escaped);
    }
    SECTION("composition-owned provider") {
        host.Activate(true);
        RequireSpawnThenDespawn(*host.gameplay, host.scenes, host.frame, host.cancellation.Token());
        auto escaped = CapturePrefabOperations();
        host.Retire(escaped);
        RequireCommitted(escaped);
    }
    SECTION("composition-owned provider retires an unpublished native request") {
        host.Activate(true);
        FixedTick(*host.gameplay, 1, host.cancellation.Token());
        auto escaped = CapturePrefabOperations();
        REQUIRE(escaped.spawn.State() == Prefab::PrefabSpawnState::Pending);
        REQUIRE(Count(host.scenes) == 1);
        host.Retire(escaped);
        CHECK(escaped.spawn.State() == Prefab::PrefabSpawnState::Cancelled);
        const auto result = escaped.spawn.Spawned();
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == Prefab::PrefabErrors::Cancelled.code.Value());
    }
}

TEST_CASE("Owned cooked prefab composition rejects invalid asset admission before module activation", "[prefab][gameplay][host]") {
    Assets::MemoryAssetProvider bytes;
    JobSystem jobs{{1, 16}};
    Assets::AssetLoadService loads{jobs, bytes};
    Assets::AssetRegistry registry;
    Runtime::RuntimeSceneService scenes{registry, loads};
    CancellationSource cancellation;
    Prefab::PrefabTemplateProviderLimits limits;
    GameplayWorldSelection selection{.moduleId = "game.tests", .nativeArtifact = "must-not-load"};
    const ErrorCodeDescriptor *expected = &Prefab::PrefabErrors::AdmissionRejected;
    SECTION("missing host permission") {
        // Keep the default denied prefab permission.
    }
    SECTION("cancelled host scope") {
        selection.prefabPermission = GameplayPrefabPermission::Granted;
        cancellation.RequestCancellation();
        expected = &Prefab::PrefabErrors::Cancelled;
    }
    SECTION("invalid provider capacity") {
        selection.prefabPermission = GameplayPrefabPermission::Granted;
        limits.maximumOutstanding = 0;
        expected = &Prefab::PrefabErrors::LimitProfileInvalid;
    }
    const GameplayPrefabServices assets{registry, loads, Prefab::PrefabLimitProfile::Create({}).Value(), limits, cancellation.Token()};
    const auto result = GameplayWorldComposition::Create(scenes, assets, nullptr, 1, selection);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == expected->code.Value());
    CHECK_FALSE(scenes.ActiveScene().has_value());
}

TEST_CASE("Production cooked prefab composition retains opaque components from its actual native module registry",
          "[prefab][gameplay][host][components]") {
    HostFixture host{true};
    bool owned{};
    SECTION("borrowed provider") {
        // Keep the default borrowed provider ownership.
    }
    SECTION("composition-owned provider") {
        owned = true;
    }
    host.Activate(owned);
    RequireSpawnThenDespawn(*host.gameplay, host.scenes, host.frame, host.cancellation.Token(), true);
    auto escaped = CapturePrefabOperations();
    host.Retire(escaped);
    RequireCommitted(escaped);
}
