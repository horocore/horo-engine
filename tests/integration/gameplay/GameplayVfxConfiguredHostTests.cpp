#include "CharacterSceneProductFixture.h"
#include "CommittedSceneAssets.h"
#include "HeadlessNetworkServices.h"
#include "Horo/Gameplay/GameplayVfxScene.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "support/VfxTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

#if HORO_TEST_PHYSICS_NATIVE
namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Runtime;
    namespace App = Horo::Application::Internal;
    using PairKind = Network::NetworkModeServiceKind;

    template <typename T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "success"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    Vfx::VfxIdentityScope SourceOwner() {
        const auto scope = Vfx::VfxIdentityScope::Create(174920);
        REQUIRE(scope.HasValue());
        return scope.Value();
    }

    Assets::AssetId EffectAsset() {
        return Vfx::Tests::ParticleMaterial("17490000-0000-4000-8000-000000000088");
    }

    Vfx::VfxSpawnAssetPreparation Recipe() {
        auto data = Vfx::Tests::ValidParticleDescriptorData();
        data.maximumParticles = 4;
        data.simulationPreference = Vfx::SimulationPreference::RequireCPU;
        data.shape = Vfx::ParticleEmitterShape::Point;
        data.spawnRate = {0.0, 0.0};
        data.lifetimeSeconds = {2.0, 2.0};
        data.initialSpeed = {0.0, 0.0};
        data.initialSize = {1.0, 1.0};
        data.initialOpacity = {1.0, 1.0};
        data.sortMode = Vfx::ParticleSortMode::None;
        data.collisionMode = Vfx::ParticleCollisionMode::None;
        return {.asset = EffectAsset(),
                .particles = std::make_shared<const Vfx::ParticleSystemDescriptor>(Vfx::Tests::ParticleDescriptor(data)),
                .simulation = {.effectSeed = 7, .maximumBurstParticles = 4, .maximumDeltaSeconds = 1.0F},
                .maximumInstances = 8,
                .initialBurst = 1,
                .durationSeconds = 1.0F,
                .material = {1}};
    }

    Gameplay::GameplayVfxBindingDocument Bindings(const std::uint64_t generation = 1, const float scale = 1.0F) {
        Gameplay::GameplayVfxBinding binding;
        binding.destination = 1;
        binding.event = Gameplay::GameplayEventTypeId{"game.tests.present"};
        binding.layers.push_back({.effectAsset = EffectAsset(), .scale = scale});
        return {.generation = generation, .bindings = {std::move(binding)}};
    }

    Gameplay::GameplayVfxSceneConfiguration Configuration(const Assets::AssetRegistrySnapshot &registry,
                                                          const std::shared_ptr<const Assets::IAssetProvider> &provider) {
        return {.recipes = {Recipe()},
                .spawn = {.maximumPending = 8,
                          .maximumResults = 8,
                          .maximumOwners = 2,
                          .maximumProducers = 2,
                          .maximumInstances = 8,
                          .maximumParticles = 32,
                          .maximumBytes = 8U * 1024U * 1024U,
                          .maximumDeltaSeconds = 1.0F},
                .extraction =
                    {.maximumBatches = 8, .maximumInstances = 32, .maximumDrawCalls = 8, .frameSlots = 3, .maximumBytes = 1024U * 1024U},
                .source = {.maximumPending = 8, .maximumModules = 1, .maximumBytes = 1024U * 1024U},
                .bindings = Bindings(),
                .registry = registry,
                .provider = provider,
                .grants = {{.moduleId = "game.tests",
                            .owner = SourceOwner(),
                            .ownerGeneration = 1,
                            .producer = 9,
                            .events = {Gameplay::GameplayEventTypeId{"game.tests.present"}}}}};
    }

    /** @brief Forwards the real configured services through the genuine scheduler; it creates no World or commit evidence. */
    class ConfiguredServices final : public RuntimeLifecycleParticipant {
    public:
        explicit ConfiguredServices(std::array<std::unique_ptr<Network::INetworkModeService>, 2> services)
            : services(std::move(services)) {}

        Result<void> Startup(const CancellationToken &) override {
            if (auto result = services[0]->Prepare(); result.HasError())
                return result;
            if (auto result = services[1]->Prepare(); result.HasError())
                return result;
            if (auto result = services[0]->Activate(); result.HasError())
                return result;
            auto result = services[1]->Activate();
            live = result.HasValue();
            return result;
        }

        Result<void> OnFixedUpdate(const FixedStepContext &context) override {
            return live ? services[1]->RunFixedTick(context) : Result<void>::Success();
        }

        Result<void> OnPhase(RuntimePhase phase, const FrameContext &) override {
            if (!live)
                return Result<void>::Success();
            if (auto first = services[0]->RunPhase(phase); first.HasError())
                return first;
            return services[1]->RunPhase(phase);
        }

        void Shutdown() noexcept override {
            live = false;
            services[1]->Shutdown();
            services[0]->Shutdown();
        }

        const Vfx::VfxExtractionOutcome *Frame() const noexcept {
            return App::InspectHeadlessVfxFrame(*services[1]);
        }

        std::shared_ptr<const GameplayPhysicsContext> Physics() const {
            return App::InspectHeadlessGameplayContext(*services[1]);
        }

    private:
        std::array<std::unique_ptr<Network::INetworkModeService>, 2> services;
        bool live{};
    };

    struct ConfiguredFixture final {
        Application::CharacterSceneProductTest::Fixture project;
        std::unique_ptr<App::CommittedSceneAssets> committed;
        std::unique_ptr<Assets::AssetLoadService> loads;
        Assets::AssetRegistry effects;
        std::shared_ptr<Assets::MemoryAssetProvider> effectProvider{std::make_shared<Assets::MemoryAssetProvider>()};
        std::shared_ptr<GameplayVfxScene> prototype;
        CancellationSource cancellation;
        App::HeadlessSceneLoadBoundary load{cancellation.Token(), std::chrono::steady_clock::now() + std::chrono::seconds{30}};
        DeterministicClock clock;
        App::NetworkProductComposition composition;
        std::unique_ptr<RuntimeHost> host;
        std::array<ConfiguredServices *, 2> worlds{};

        ConfiguredFixture() {
            PrepareProject();
            PreparePrototype();
            // Cold project/prototype cooking is outside the actual Scene admission operation.
            load.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
            composition = Compose();
            REQUIRE(composition.fixedParticipant);
            host = Take(RuntimeHost::Create(clock, {.fixedStep = Duration::FromNanoseconds(16'666'667)}));
            REQUIRE(host->AddParticipant(Take(composition.fixedParticipant(host->DispatchSource()))).HasValue());
            AddWorld(0, {174901});
            AddWorld(1, {174902});
            REQUIRE(host->Startup().HasValue());
            REQUIRE(host->RunFrame().HasValue());
        }

        ~ConfiguredFixture() {
            host->Shutdown();
            host.reset();
            composition = {};
            loads.reset();
            committed.reset();
        }

        void PrepareProject() {
            project.source.objects.front().components.behaviors.emplace_back(BehaviorInstanceId{1749},
                                                                             BehaviorTypeId::Parse("game.tests.vfx_emitter").Value(), 1,
                                                                             true, std::vector<BehaviorField>{});
            project.project.WriteText("assets/level.horo",
                                      SceneSource::EncodeSceneSource({project.source.objects, project.source.prefabInstances}));
            project.project.WriteText("assets/presentation.horo_script", R"(return horo.behavior {
                type_id='game.tests.vfx_emitter', display_name='Configured emitter',
                on_fixed_update=function(ctx,dt) ctx.vfx.emit('game.tests.present',{x_mm=1000,y_mm=0,z_mm=0}) end
            })");
            project.project.WriteText("assets/presentation.horo_script.meta",
                                      R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.vfx_emitter"})");
            const auto root = project.project.project.dir.path;
            auto program = Take(
                LuaBehaviorProgram::LoadFiles(root / "assets/presentation.horo_script", root / "assets/presentation.horo_script.meta"));
            ComponentRegistry components;
            REQUIRE(components.Freeze().HasValue());
            project.project.request.schemas = Take(
                Application::PrefabCookSchemaContext::Capture(components, std::span<const BehaviorDescriptor>{&program->Descriptor(), 1}));
            project.Cook();
            committed = Take(App::CommittedSceneAssets::Capture(project.project.files,
                                                                {project.project.cooked.path, project.project.request.assets.target},
                                                                project.project.registry.Snapshot()));
            loads = std::make_unique<Assets::AssetLoadService>(project.project.jobs, committed->SceneProvider());
        }

        void PreparePrototype() {
            const Assets::AssetRecord record{.id = EffectAsset(),
                                             .type = Assets::AssetTypeId::Parse("core.vfx.effect").Value(),
                                             .sourcePath = ProjectPath::Parse("Effects/configured.vfx").Value(),
                                             .metadataPath = ProjectPath::Parse("Effects/configured.vfx.horo").Value()};
            REQUIRE(effects.Publish({record}).status == Assets::AssetRegistryBuildStatus::Complete);
            effectProvider->Insert(EffectAsset(), {'{', '}'});
            prototype = Take(GameplayVfxScene::Create(Configuration(effects.Snapshot(), effectProvider)));
        }

        App::NetworkProductComposition Compose() {
            auto collision = Take(Application::LoadProjectCollisionSettings(project.project.project.dir.path, "p1"));
            auto simulation = Take(collision.settings->Capture(1));
            auto physics = Take(Physics::PhysicsWorldSettings::Capture({}));
            auto character = Take(Character::CharacterWorldSettings::Capture({}));
            auto definition = std::make_shared<const RuntimeSceneDefinition>(Take(committed->DecodeScene(project.project.sceneId)));
            const auto root = project.project.project.dir.path;
            App::GameplayWorldSelection gameplay{.moduleId = "game.tests",
                                                 .scriptSource = root / "assets/presentation.horo_script",
                                                 .scriptSidecar = root / "assets/presentation.horo_script.meta",
                                                 .physicsPermission = App::GameplayPhysicsPermission::Granted,
                                                 .vfxScene = prototype};
            return Take(App::ComposeHeadlessSceneNetworkServices(std::move(definition),
                                                                 {Physics::PhysicsRuntimeMode::Canonical,
                                                                  {std::move(physics), std::move(character), std::move(simulation)},
                                                                  project.project.registry,
                                                                  *loads,
                                                                  load},
                                                                 {}, gameplay));
        }

        void AddWorld(std::size_t index, SceneRuntimeId scene) {
            std::array<std::unique_ptr<Network::INetworkModeService>, 2> pair;
            for (std::size_t slot = 0; slot < pair.size(); ++slot) {
                const auto kind = slot == 0 ? PairKind::Scene : PairKind::Physics;
                pair[slot] = Take(composition.factories.services[static_cast<std::size_t>(kind)](
                    {kind, Network::NetworkProjectRole::Standalone, {}, Network::NetworkModeWorldKind::Standalone, scene}));
            }
            auto participant = std::make_unique<ConfiguredServices>(std::move(pair));
            worlds[index] = participant.get();
            REQUIRE(host->AddParticipant(std::move(participant)).HasValue());
        }

        void Tick(std::uint32_t count = 1) {
            clock.Advance(Duration::FromNanoseconds(16'666'667LL * count));
            REQUIRE(host->RunFrame().HasValue());
            const auto committed = host->Statistics().completedSimulationTick;
            // Extraction precedes deferred Scene publication; the next real presentation frame observes the committed effects.
            REQUIRE(host->RunFrame().HasValue());
            CHECK(host->Statistics().completedSimulationTick == committed);
        }

        void CheckParticles(std::size_t world, std::size_t count) {
            const auto *frame = worlds[world]->Frame();
            REQUIRE(frame);
            REQUIRE(frame->report.status == Vfx::VfxExtractionStatus::Complete);
            REQUIRE(frame->snapshot.Instances().size() == count);
            if (count != 0)
                CHECK(frame->snapshot.Instances().front().position.x == 1.0F);
        }
    };
}  // namespace

TEST_CASE("Configured product Worlds clone explicit VFX policy and publish genuine fixed source effects independently",
          "[vfx][gameplay][configured][canonical]") {
    ConfiguredFixture fixture;
    CHECK_FALSE(fixture.prototype->Module("game.tests"));
    const auto first = fixture.worlds[0]->Physics();
    const auto second = fixture.worlds[1]->Physics();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->Binding().scene == 174901);
    CHECK(second->Binding().scene == 174902);
    CHECK(first->Binding().world != second->Binding().world);
    fixture.Tick(2);
    fixture.CheckParticles(0, 2);
    fixture.CheckParticles(1, 2);
    const auto retained = fixture.worlds[0]->Frame()->snapshot;
    // More than eight prepared source slots proves each real cutoff is flushed rather than left sealed indefinitely.
    for (int frame = 0; frame < 5; ++frame)
        fixture.Tick(2);
    CHECK(fixture.host->Statistics().completedSimulationTick == 12);
    fixture.CheckParticles(0, 8);
    fixture.CheckParticles(1, 8);
    fixture.worlds[0]->Shutdown();
    CHECK_FALSE(fixture.worlds[0]->Frame());
    fixture.Tick();
    fixture.CheckParticles(1, 8);
    CHECK_FALSE(fixture.prototype->Module("game.tests"));
    CHECK(retained.Instances().size() == 2);
}
#endif
