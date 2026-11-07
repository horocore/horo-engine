#include "GameplayRuntimeTestSupport.h"
#include "GameplayWorldComposition.h"
#include "Horo/Gameplay/GameplayVfxScene.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "support/VfxTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Runtime;
    using namespace Horo::Application::Internal;
    using Status = Vfx::VfxSpawnStatus;

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

    BehaviorTypeId Type() {
        return BehaviorTypeId::Parse("game.tests.vfx_emitter").Value();
    }

    struct ScriptFiles final {
        std::filesystem::path directory;
        std::filesystem::path source;
        std::filesystem::path sidecar;

        explicit ScriptFiles(const std::string &body) {
            static std::atomic<unsigned> serial{};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            directory =
                std::filesystem::temp_directory_path() / ("horo-vfx-host-" + std::to_string(stamp) + "-" + std::to_string(++serial));
            REQUIRE(std::filesystem::create_directory(directory));
            source = directory / "emitter.horo_script";
            sidecar = directory / "emitter.horo_script.horo";
            std::ofstream program{source};
            program << "return horo.behavior {type_id='game.tests.vfx_emitter',display_name='Host emitter',"
                       "on_fixed_update=function(ctx,dt) "
                    << body << " end}";
            REQUIRE(program.good());
            std::ofstream metadata{sidecar};
            metadata << R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.vfx_emitter"})";
            REQUIRE(metadata.good());
        }

        ~ScriptFiles() {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    };

    constexpr auto EmitOne = "ctx.vfx.emit('game.tests.present',{x_mm=1000,y_mm=0,z_mm=0}) ";
    constexpr auto EmitTwo = "ctx.vfx.emit('game.tests.present',{x_mm=1000,y_mm=0,z_mm=0}) "
                             "ctx.vfx.emit('game.tests.present',{x_mm=2000,y_mm=0,z_mm=0}) ";

    struct DispatchObservations final {
        unsigned fixed{};
        unsigned prepared{};
        unsigned committed{};
        unsigned aborted{};
        bool proofValid{true};
        bool advancedBeforeScene{};
        bool retireEntity{};
        std::shared_ptr<const SceneStructuralReceipt> retirement;
    };

    // Test composition adapter: all timing and commitment authority comes from the actual RuntimeHost scheduler.
    class GameplayParticipant final : public RuntimeLifecycleParticipant {
    public:
        GameplayParticipant(GameplayWorldComposition &gameplay, RuntimeSceneService &scenes, GameplayVfxScene &vfx,
                            RuntimeDispatchSource source, DispatchObservations &observations)
            : gameplay_(gameplay), scenes_(scenes), vfx_(vfx), source_(std::move(source)), observations_(observations) {}

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const FixedStepContext &context) override {
            observations_.proofValid = observations_.proofValid && Valid(context, false);
            ++observations_.fixed;
            auto result = gameplay_.FixedTick(context);
            if (result.HasError() || !observations_.retireEntity)
                return result;
            SceneCommandBuffer commands;
            const auto actor = scenes_.ActiveScene()->Find({1});
            REQUIRE(actor);
            commands.Destroy(*actor);
            auto queued = scenes_.QueueTrackedStructuralCommands(std::move(commands));
            if (queued.HasError())
                return Result<void>::Failure(queued.ErrorValue());
            observations_.retirement = std::move(queued).Value();
            return Result<void>::Success();
        }

        Result<void> PrepareFixedCommit(const FixedStepContext &context) override {
            ++observations_.prepared;
            ObserveBeforePublication();
            observations_.proofValid = observations_.proofValid && Valid(context, false);
            return gameplay_.PrepareFixedCommit(context);
        }

        void CommitFixedUpdate(const FixedStepContext &context) noexcept override {
            observations_.proofValid = observations_.proofValid && Valid(context, true);
            gameplay_.CommitFixedUpdate(context);
            ++observations_.committed;
            ObserveBeforePublication();
        }

        void AbortFixedUpdate(const FixedStepContext &context) noexcept override {
            gameplay_.AbortFixedUpdate(context);
            ++observations_.aborted;
        }

        Result<void> OnPhase(RuntimePhase phase, const FrameContext &context) override {
            if (phase == RuntimePhase::VariableUpdate)
                ObserveBeforePublication();
            return gameplay_.OnPhase(phase, context);
        }

        void Shutdown() noexcept override {
            gameplay_.Shutdown();
        }

    private:
        bool Valid(const FixedStepContext &context, const bool committed) const noexcept {
            RuntimeDispatchFacts facts;
            if (context.dispatchEvidence.Read(source_, RuntimePhase::FixedUpdate, facts) != RuntimeDispatchStatus::Valid)
                return false;
            const bool same = facts.fixedTick == context.simulationTick && facts.frame == context.frameNumber &&
                              facts.fixedDuration == context.fixedDelta;
            if (!committed)
                return same;
            return same && facts.committedTick == facts.fixedTick && facts.committedAttempt == facts.fixedAttempt;
        }

        void ObserveBeforePublication() noexcept {
            observations_.advancedBeforeScene = observations_.advancedBeforeScene || vfx_.LastTick().world.live != 0;
        }

        GameplayWorldComposition &gameplay_;
        RuntimeSceneService &scenes_;
        GameplayVfxScene &vfx_;
        RuntimeDispatchSource source_;
        DispatchObservations &observations_;
    };

    class RejectPrepare final : public RuntimeLifecycleParticipant {
    public:
        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnPhase(RuntimePhase, const FrameContext &) override {
            return Result<void>::Success();
        }

        Result<void> OnFixedUpdate(const FixedStepContext &) override {
            return Result<void>::Success();
        }

        Result<void> PrepareFixedCommit(const FixedStepContext &) override {
            return Result<void>::Failure(MakeError(GameplayErrors::GameplayCancelled));
        }

        void Shutdown() noexcept override {}
    };

    // Explicit authority-fault injection; the genuine Scene service owns rejection and receipt completion.
    class RejectStructuralAuthority final : public SceneStructuralParticipant {
    public:
        explicit RejectStructuralAuthority(const bool &reject) : reject_(reject) {}

        SceneStructuralOwner Owner() const noexcept override {
            return SceneStructuralOwner::Physics;
        }

        SceneStructuralPreparationPhase PreparationPhase() const noexcept override {
            return reject_ ? SceneStructuralPreparationPhase::Identity : SceneStructuralPreparationPhase::DerivedResources;
        }

        Result<std::unique_ptr<SceneStructuralCandidate>> Prepare(const RuntimeSceneView &, std::span<const RuntimeEntityView>,
                                                                  std::span<const EntityRef>) override {
            return Result<std::unique_ptr<SceneStructuralCandidate>>::Failure(MakeError(GameplayErrors::GameplayCancelled));
        }

    private:
        const bool &reject_;
    };

    struct HostFixture final {
        ScriptFiles files;
        Assets::AssetRegistry assets;
        std::shared_ptr<Assets::MemoryAssetProvider> provider{std::make_shared<Assets::MemoryAssetProvider>()};
        std::shared_ptr<GameplayVfxScene> vfx;
        bool rejectStructuralAuthority{};
        RuntimeSceneService scenes{SceneRuntimeId{174980}};
        GameplayWorldComposition *active{};
        std::unique_ptr<GameplayWorldComposition> gameplay;
        CancellationToken cancellation;
        DeterministicClock clock;
        DispatchObservations observations;
        std::unique_ptr<RuntimeHost> host;

        explicit HostFixture(const std::string &body = EmitOne, const std::uint32_t instances = 8, const bool malformedOwnerPolicy = false)
            : files(body) {
            PrepareAssets(instances);
            REQUIRE(scenes.AddActivationParticipant(vfx->MakeActivationParticipant()).HasValue());
            REQUIRE(scenes.AddStructuralParticipant(MakeGameplayStructuralParticipant(active)).HasValue());
            if (malformedOwnerPolicy)
                REQUIRE(scenes.AddStructuralParticipant(std::make_unique<RejectStructuralAuthority>(rejectStructuralAuthority)).HasValue());
            REQUIRE(scenes.Startup(cancellation).HasValue());
            REQUIRE(scenes.QueuePreparation(Tests::SingleBehaviorSceneDefinition({1749}, {1}, {1}, {1}, Type())).HasValue());
            ScenePhase(RuntimePhase::CommitDeferredLifecycleChanges);
            GameplayWorldSelection selection;
            selection.moduleId = "game.tests";
            selection.scriptSource = files.source;
            selection.scriptSidecar = files.sidecar;
            selection.vfxScene = vfx;
            auto created = GameplayWorldComposition::Create(scenes, nullptr, 1, selection);
            REQUIRE(created.HasValue());
            gameplay = std::move(created).Value();
            active = gameplay.get();
            auto runtime = RuntimeHost::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)});
            REQUIRE(runtime.HasValue());
            host = std::move(runtime).Value();
            REQUIRE(
                host->AddParticipant(std::make_unique<GameplayParticipant>(*gameplay, scenes, *vfx, host->DispatchSource(), observations))
                    .HasValue());
        }

        ~HostFixture() {
            host->Shutdown();
            active = nullptr;
            gameplay.reset();
            scenes.Shutdown();
        }

        void PrepareAssets(const std::uint32_t instances) {
            const Assets::AssetRecord record{.id = EffectAsset(),
                                             .type = Assets::AssetTypeId::Parse("core.vfx.effect").Value(),
                                             .sourcePath = ProjectPath::Parse("Assets/Effects/presentation.vfx").Value(),
                                             .metadataPath = ProjectPath::Parse("Assets/Effects/presentation.vfx.horo").Value()};
            REQUIRE(assets.Publish({record}).status == Assets::AssetRegistryBuildStatus::Complete);
            provider->Insert(EffectAsset(), {'{', '}'});
            auto config = Configuration(assets.Snapshot(), provider);
            config.recipes.front().maximumInstances = instances;
            config.spawn.maximumInstances = instances;
            auto created = GameplayVfxScene::Create(std::move(config));
            REQUIRE(created.HasValue());
            vfx = std::move(created).Value();
        }

        void Start() {
            REQUIRE(host->Startup().HasValue());
            REQUIRE(host->RunFrame().HasValue());
        }

        void ScenePhase(const RuntimePhase phase) {
            REQUIRE(scenes.OnPhase(phase, {.frameNumber = 100, .cancellation = cancellation}).HasValue());
        }

        void Extract(const std::size_t expected) {
            REQUIRE(host->RunFrame().HasValue());
            REQUIRE(scenes.VfxFrame().report.status == Vfx::VfxExtractionStatus::Complete);
            CHECK(scenes.VfxFrame().snapshot.Instances().size() == expected);
        }
    };
}  // namespace

TEST_CASE("Gameplay host seals consecutive real catch-up ticks until Scene publication", "[vfx][gameplay][host][lua]") {
    HostFixture fixture;
    fixture.Start();
    fixture.clock.Advance(Duration::FromMilliseconds(20));
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(fixture.host->Statistics().completedSimulationTick == 2);
    CHECK(fixture.observations.fixed == 2);
    CHECK(fixture.observations.prepared == 2);
    CHECK(fixture.observations.committed == 2);
    CHECK(fixture.observations.aborted == 0);
    CHECK(fixture.observations.proofValid);
    CHECK_FALSE(fixture.observations.advancedBeforeScene);
    CHECK(fixture.scenes.VfxFrame().snapshot.Instances().empty());
    CHECK(fixture.vfx->LastTick().world.live == 2);
    fixture.Extract(2);
}

TEST_CASE("Gameplay host treats cosmetic instance capacity as a retained nonfatal outcome", "[vfx][gameplay][host][lua]") {
    HostFixture fixture{EmitTwo, 1};
    fixture.Start();
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(fixture.host->Statistics().completedSimulationTick == 1);
    CHECK(fixture.vfx->LastTick().world.firstFailure.status == Status::EffectCapacityExceeded);
    CHECK(fixture.vfx->LastTick().world.live == 1);
    CHECK(fixture.observations.proofValid);
    CHECK_FALSE(fixture.observations.advancedBeforeScene);
    fixture.Extract(1);
}

TEST_CASE("A later real scheduler preparation failure aborts armed Gameplay VFX", "[vfx][gameplay][host][lua]") {
    HostFixture fixture;
    REQUIRE(fixture.host->AddParticipant(std::make_unique<RejectPrepare>()).HasValue());
    fixture.Start();
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    CHECK(fixture.host->RunFrame().HasError());
    CHECK(fixture.host->Statistics().completedSimulationTick == 0);
    CHECK(fixture.observations.prepared == 1);
    CHECK(fixture.observations.committed == 0);
    CHECK(fixture.observations.aborted == 1);
    CHECK(fixture.observations.proofValid);
    fixture.ScenePhase(RuntimePhase::CommitDeferredLifecycleChanges);
    fixture.ScenePhase(RuntimePhase::RenderExtraction);
    CHECK(fixture.scenes.VfxFrame().snapshot.Instances().empty());
    CHECK(fixture.vfx->LastTick().world.live == 0);
}

TEST_CASE("An admitted effect survives actual committed entity retirement", "[vfx][gameplay][host][lua]") {
    HostFixture fixture;
    fixture.observations.retireEntity = true;
    fixture.Start();
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(fixture.observations.retirement);
    REQUIRE(fixture.observations.retirement->Complete());
    REQUIRE(fixture.observations.retirement->ResultValue());
    CHECK_FALSE(fixture.scenes.ActiveScene()->Find({1}).has_value());
    CHECK(fixture.observations.committed == 1);
    CHECK(fixture.vfx->LastTick().world.live == 1);
    fixture.Extract(1);
}

TEST_CASE("Malformed owner policy rejects the real Scene receipt without failing the host phase", "[vfx][gameplay][host][lua]") {
    HostFixture fixture{std::string{EmitOne} + "ctx.transform.set_position(7,0,0)", 8, true};
    fixture.Start();
    fixture.rejectStructuralAuthority = true;
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(fixture.host->Statistics().completedSimulationTick == 1);
    REQUIRE(fixture.scenes.TakeOperationError());
    const auto view = fixture.scenes.ActiveScene().value();
    const auto actor = view.Find({1});
    REQUIRE(actor);
    const auto entity = view.Get(*actor);
    REQUIRE(entity.HasValue());
    CHECK(entity.Value().localTransform->translation.x == 0.0F);
    CHECK(fixture.observations.committed == 1);
    CHECK_FALSE(fixture.observations.advancedBeforeScene);
    CHECK(fixture.vfx->LastTick().rejected == 1);
    CHECK(fixture.vfx->LastTick().world.live == 0);
    fixture.Extract(0);
}
