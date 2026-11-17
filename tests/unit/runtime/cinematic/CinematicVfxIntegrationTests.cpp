#include "Horo/Cinematic/EventDispatcher.h"
#include "Horo/Cinematic/EventSession.h"
#include "Horo/Cinematic/ScriptEventCook.h"
#include "Horo/Cinematic/VfxEventAdapter.h"
#include "Horo/Gameplay/GameplayVfxScene.h"
#include "support/AllocationProbe.h"
#include "support/VfxTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace Horo::Cinematic {
    namespace {
        constexpr CinematicRuntimeSessionId VfxSession{1749, 1};
        constexpr EventBindingId VfxBinding{1792, 1};
        constexpr EventPayloadSchemaId VfxSchema{7002, 1};
        constexpr TrackId VfxTrack{10, 1};
        constexpr KeyframeId VfxKey{11, 1};
        using Status = Vfx::VfxSpawnStatus;
        namespace Probe = Horo::Tests::AllocationProbe;

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
                    .extraction = {.maximumBatches = 8,
                                   .maximumInstances = 32,
                                   .maximumDrawCalls = 8,
                                   .frameSlots = 3,
                                   .maximumBytes = 1024U * 1024U},
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

        Runtime::RuntimeSceneDefinition SceneDefinition(const std::uint64_t revision = 1) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1749}, Runtime::SceneDefinitionRevision{revision}};
            auto built = std::move(builder).Build();
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        Extensions::ScriptExportDescriptorSnapshotPtr Exports(std::vector<Extensions::ScriptExportParameterDescriptor> parameters) {
            using namespace Extensions;
            ScriptExportDescriptor api{
                .moduleId = "game.tests",
                .id = "game.tests.vfx.events",
                .nameSpace = "game.tests.vfx",
                .version = {1, 0, 0},
                .compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 0, 0}},
                .service = {.capability = {"game.tests.vfx"},
                            .serviceId = "game.tests.vfx.service",
                            .contractId = "game.tests.vfx.contract",
                            .minimumVersion = {1, 0, 0}},
            };
            api.functions.push_back({.id = "present",
                                     .introducedVersion = {1, 0, 0},
                                     .invocation = ScriptExportInvocationMode::Synchronous,
                                     .parameters = std::move(parameters)});
            auto snapshot = BuildScriptExportDescriptorSnapshot(std::array{api});
            INFO((snapshot.HasError() ? snapshot.ErrorValue().domain.Value() : "captured"));
            INFO((snapshot.HasError() ? snapshot.ErrorValue().code.Value() : "captured"));
            INFO((snapshot.HasError() ? snapshot.ErrorValue().message : "captured"));
            REQUIRE(snapshot.HasValue());
            return snapshot.Value();
        }

        ScriptEventDescriptor Descriptor() {
            return {.qualifiedName = "game.tests.vfx.present",
                    .binding = VfxBinding,
                    .schema = VfxSchema,
                    .exportApiId = "game.tests.vfx.events",
                    .exportFunctionId = "present",
                    .exportVersion = {1, 0, 0},
                    .allowedContexts = static_cast<std::byte>(EventRuntimeContext::Headless)};
        }

        Extensions::ScriptExportDescriptorSnapshotPtr PayloadExports() {
            using Kind = Extensions::ScriptExportPrimitiveKind;
            return Exports({{.id = "px", .type = {.primitive = Kind::SignedInteger}, .introducedVersion = {1, 0, 0}},
                            {.id = "py", .type = {.primitive = Kind::SignedInteger}, .introducedVersion = {1, 0, 0}},
                            {.id = "pz", .type = {.primitive = Kind::SignedInteger}, .introducedVersion = {1, 0, 0}},
                            {.id = "scale", .type = {.primitive = Kind::Number}, .introducedVersion = {1, 0, 0}}});
        }

        AuthoredScriptEventKey Authored(const KeyframeId key = VfxKey, const CurveTime time = 2, const std::int64_t millimeters = 10'000,
                                        const double scale = 2.0) {
            return {VfxTrack,
                    key,
                    "game.tests.vfx.present",
                    {Extensions::ScriptValue{millimeters}, Extensions::ScriptValue{std::int64_t{20'000}},
                     Extensions::ScriptValue{std::int64_t{30'000}}, Extensions::ScriptValue{scale}},
                    time,
                    true};
        }

        VfxEventBinding AdapterBinding(const std::size_t results = 8) {
            return {.binding = VfxBinding,
                    .schema = VfxSchema,
                    .context = EventRuntimeContext::Headless,
                    .generation = 1,
                    .semanticEvent = "game.tests.present",
                    .maximumKeys = 8,
                    .maximumResults = results};
        }

        CinematicEventDispatcher MakeDispatcher(const std::size_t capacity = 8) {
            auto created = CinematicEventDispatcher::Create(VfxSession, EventRuntimeContext::Headless, capacity, capacity);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        CinematicRuntimeService MakeRuntime() {
            auto created = CinematicRuntimeService::Create({VfxSession, SequenceCookTier::Standard});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        SequenceFrameEvaluationPlan Evaluation(const CookedEventPlan &cooked) {
            std::vector<SequenceFrameEventKey> keys;
            keys.reserve(cooked.Keys().size());
            for (const auto &key : cooked.Keys())
                keys.push_back({key.track, key.key, key.time, key.fireInReverse});
            auto plan = SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 8, {}, keys, {});
            REQUIRE(plan.HasValue());
            return std::move(plan).Value();
        }

        std::shared_ptr<const CookedEventPlan> Cook(std::span<const AuthoredScriptEventKey> keys, const ScriptEventDescriptor &descriptor,
                                                    const Extensions::ScriptExportDescriptorSnapshotPtr &exports) {
            auto result = CookScriptEvents(keys, std::span{&descriptor, 1}, exports, EventRuntimeContext::Headless);
            INFO((result.HasError() ? result.ErrorValue().domain.Value() : "cooked"));
            INFO((result.HasError() ? result.ErrorValue().code.Value() : "cooked"));
            INFO((result.HasError() ? result.ErrorValue().message : "cooked"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct Playback final {
            CinematicEventDispatcher dispatcher{MakeDispatcher()};
            CinematicRuntimeService runtime{MakeRuntime()};
            CinematicEventSession session{runtime, dispatcher};
            SequencePlayerHandle player{VfxSession, {1, 1}};
            std::array<SequenceFrameEventOccurrence, 8> events;

            void Activate(const std::shared_ptr<const CookedEventPlan> &plan) {
                REQUIRE(session.Activate({{player, 10}, Evaluation(*plan)}, plan, 0).HasValue());
                REQUIRE(runtime.Play(player).HasValue());
            }

            void Evaluate(const std::uint64_t tick, const SequenceTime delta = 3) {
                REQUIRE(session.BeginTick(tick).HasValue());
                REQUIRE(session.Evaluate(player, delta, {{}, events, {}}).HasValue());
            }
        };

        struct SceneFixture final {
            Assets::AssetRegistry registry;
            std::shared_ptr<Assets::MemoryAssetProvider> provider{std::make_shared<Assets::MemoryAssetProvider>()};
            std::shared_ptr<Gameplay::GameplayVfxScene> vfx;
            Runtime::RuntimeSceneService scenes;
            CancellationToken cancellation;
            std::uint64_t lastFrame{};

            SceneFixture() {
                const Assets::AssetRecord record{.id = EffectAsset(),
                                                 .type = Assets::AssetTypeId::Parse("core.vfx.effect").Value(),
                                                 .sourcePath = ProjectPath::Parse("Assets/Effects/cinematic.vfx").Value(),
                                                 .metadataPath = ProjectPath::Parse("Assets/Effects/cinematic.vfx.horo").Value()};
                REQUIRE(registry.Publish({record}).status == Assets::AssetRegistryBuildStatus::Complete);
                provider->Insert(EffectAsset(), {'{', '}'});
                auto created = Gameplay::GameplayVfxScene::Create(Configuration(registry.Snapshot(), provider));
                REQUIRE(created.HasValue());
                vfx = std::move(created).Value();
                REQUIRE(scenes.AddActivationParticipant(vfx->MakeActivationParticipant()).HasValue());
                REQUIRE(scenes.Startup(cancellation).HasValue());
                REQUIRE(scenes.QueuePreparation(SceneDefinition()).HasValue());
                CHECK_FALSE(vfx->Module("game.tests"));
                Phase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, 1);
                REQUIRE(scenes.ActiveScene());
                REQUIRE(vfx->Module("game.tests"));
            }

            ~SceneFixture() {
                scenes.Shutdown();
            }

            void Phase(const Runtime::RuntimePhase phase, const std::uint64_t frame) {
                lastFrame = std::max(frame, lastFrame + 1);
                REQUIRE(scenes.OnPhase(phase, {.frameNumber = lastFrame, .cancellation = cancellation}).HasValue());
            }

            Render::RenderWorldSnapshot Extract(const std::uint64_t frame) {
                Phase(Runtime::RuntimePhase::RenderExtraction, frame);
                REQUIRE(scenes.VfxFrame().report.status == Vfx::VfxExtractionStatus::Complete);
                return scenes.VfxFrame().snapshot;
            }

            Gameplay::GameplayVfxTickResult Commit(const std::uint64_t tick) {
                Phase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, tick + 1);
                return vfx->CommitTick(tick);
            }
        };

        struct Integration final {
            SceneFixture scene;
            Playback playback;
            std::shared_ptr<const CookedEventPlan> plan;
            std::shared_ptr<CinematicVfxEventAdapter> adapter;

            explicit Integration(const bool twoKeys = false, const std::size_t results = 8) {
                std::vector<AuthoredScriptEventKey> keys{Authored()};
                if (twoKeys)
                    keys.push_back(Authored({12, 1}, 3, 40'000));
                plan = Cook(keys, Descriptor(), PayloadExports());
                auto created = CinematicVfxEventAdapter::Create(AdapterBinding(results), plan, scene.vfx->CommittedProducer("game.tests"),
                                                                {.scaleArgument = 3});
                INFO((created.HasError() ? created.ErrorValue().message : "prepared"));
                REQUIRE(created.HasValue());
                adapter = std::move(created).Value();
                REQUIRE(playback.dispatcher.Register(adapter->Registration()).HasValue());
                playback.Activate(plan);
            }

            void Begin(const std::uint64_t tick = 1) {
                REQUIRE(scene.vfx->BeginTick(tick, 0.125F) == Status::Accepted);
                playback.Evaluate(tick);
            }

            void CommitCinematic() {
                REQUIRE(playback.session.CommitTick().HasValue());
            }
        };
    }  // namespace

    TEST_CASE("Actual cinematic commit and Scene source commit gate a real extracted VFX effect", "[cinematic][vfx][scene][integration]") {
        Integration fixture;
        fixture.Begin();
        CHECK(fixture.adapter->Results().empty());
        CHECK(fixture.scene.Extract(2).Instances().empty());
        REQUIRE(fixture.playback.session.AbortTick().HasValue());
        fixture.scene.vfx->AbortTick(1);
        CHECK(fixture.playback.dispatcher.Drain(8).Value() == 0);
        CHECK(fixture.scene.Extract(3).Instances().empty());

        fixture.Begin();
        fixture.CommitCinematic();
        CHECK(fixture.adapter->Results().empty());
        REQUIRE(fixture.playback.dispatcher.Drain(8).Value() == 1);
        REQUIRE(fixture.adapter->Results().size() == 1);
        const auto record = fixture.adapter->Results()[0];
        CHECK(record.occurrence == fixture.playback.dispatcher.Results()[0].occurrence);
        CHECK(record.committedTick == 1);
        REQUIRE(record.submission.status == Status::Accepted);
        const auto client = fixture.scene.vfx->Module("game.tests");
        CHECK(client->Poll(record.submission.receipt).pending);
        CHECK(fixture.scene.Extract(4).Instances().empty());
        REQUIRE(fixture.scene.Commit(1).submitted == 1);
        const auto result = client->Poll(record.submission.receipt);
        REQUIRE(result.layerCount == 1);
        CHECK(result.status == Status::Spawned);
        CHECK(result.layers[0].occurrence == record.submission.receipt.occurrence);
        const auto frame = fixture.scene.Extract(5);
        REQUIRE(frame.Instances().size() == 1);
        CHECK(frame.Instances()[0].position == Math::Vec3{10.0F, 20.0F, 30.0F});
        CHECK(frame.Instances()[0].size == Math::Vec2{2.0F, 2.0F});
    }

    TEST_CASE("Cinematic VFX dispatch and source commit use bounded prepared storage without allocation",
              "[cinematic][vfx][scene][integration][allocation]") {
        Integration fixture;
        fixture.Begin();
        fixture.CommitCinematic();
        Result<std::size_t> drained = Result<std::size_t>::Success(0);
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            drained = fixture.playback.dispatcher.Drain(8);
            allocations = measure.Snapshot();
        }
        REQUIRE(drained.HasValue());
        CHECK(drained.Value() == 1);
        CHECK(allocations.requests == 0);
        fixture.scene.Phase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, 2);
        Gameplay::GameplayVfxTickResult committed;
        {
            Probe::ScopedMeasurement measure;
            committed = fixture.scene.vfx->CommitTick(1);
            allocations = measure.Snapshot();
        }
        REQUIRE(committed.status == Status::Accepted);
        CHECK(allocations.requests == 0);
        CHECK(fixture.scene.Extract(3).Instances().size() == 1);
    }

    TEST_CASE("Cinematic VFX capacity denial reserves correlation before any semantic submission", "[cinematic][vfx][scene][integration]") {
        Integration fixture{true, 1};
        fixture.Begin();
        fixture.CommitCinematic();
        REQUIRE(fixture.playback.dispatcher.Drain(8).Value() == 2);
        REQUIRE(fixture.adapter->Results().size() == 1);
        REQUIRE(fixture.playback.dispatcher.Results().size() == 2);
        CHECK(fixture.playback.dispatcher.Results()[0].outcome == EventDispatchOutcome::Accepted);
        CHECK(fixture.playback.dispatcher.Results()[1].outcome == EventDispatchOutcome::Backpressured);
        REQUIRE(fixture.scene.Commit(1).submitted == 1);
        const auto frame = fixture.scene.Extract(2);
        REQUIRE(frame.Instances().size() == 1);
        CHECK(frame.Instances()[0].position.x == 10.0F);
    }

    TEST_CASE("Full cinematic occurrence correlation survives adapter evidence acknowledgement", "[cinematic][vfx][scene][integration]") {
        Integration fixture;
        const SequencePlayerHandle second{VfxSession, {2, 1}};
        REQUIRE(fixture.playback.session.Activate({{second, 10}, Evaluation(*fixture.plan)}, fixture.plan, 0).HasValue());
        REQUIRE(fixture.playback.runtime.Play(second).HasValue());
        std::array<SequenceFrameEventOccurrence, 8> secondEvents;
        REQUIRE(fixture.scene.vfx->BeginTick(1, 0.125F) == Status::Accepted);
        fixture.playback.Evaluate(1);
        REQUIRE(fixture.playback.session.Evaluate(second, 3, {{}, secondEvents, {}}).HasValue());
        fixture.CommitCinematic();
        REQUIRE(fixture.playback.dispatcher.Drain(1).Value() == 1);
        REQUIRE(fixture.adapter->Results().size() == 1);
        const auto first = fixture.adapter->Results()[0];
        fixture.adapter->ClearResults();
        REQUIRE(fixture.playback.dispatcher.Drain(1).Value() == 1);
        REQUIRE(fixture.adapter->Results().size() == 1);
        const auto next = fixture.adapter->Results()[0];
        CHECK(first.occurrence.player != next.occurrence.player);
        CHECK(first.occurrence.track == next.occurrence.track);
        CHECK(first.occurrence.key == next.occurrence.key);
        CHECK(first.occurrence.traversal == next.occurrence.traversal);
        CHECK(first.submission.receipt.occurrence != next.submission.receipt.occurrence);
        CHECK(first.occurrence.direction == SequenceTraversalDirection::Forward);
        CHECK(next.occurrence.time == 2);
        fixture.adapter->ClearResults();
        fixture.playback.dispatcher.ClearResults();
        CHECK(fixture.playback.dispatcher.Drain(8).Value() == 0);
        REQUIRE(fixture.scene.Commit(1).submitted == 2);
        CHECK(fixture.scene.Extract(2).Instances().size() == 2);
    }

    TEST_CASE("A revoked cinematic VFX registration cannot turn queued events into effects", "[cinematic][vfx][scene][integration]") {
        Integration fixture;
        fixture.Begin();
        fixture.CommitCinematic();
        REQUIRE(fixture.playback.dispatcher.Revoke(VfxBinding, 1).HasValue());
        REQUIRE(fixture.playback.dispatcher.Drain(8).Value() == 1);
        CHECK(fixture.playback.dispatcher.Results()[0].outcome == EventDispatchOutcome::StaleBinding);
        CHECK(fixture.adapter->Results().empty());
        REQUIRE(fixture.scene.Commit(1).submitted == 0);
        CHECK(fixture.scene.Extract(2).Instances().empty());
    }

    TEST_CASE("Scene replacement revokes the old cinematic producer and retains immutable effect frames",
              "[cinematic][vfx][scene][integration][lifetime]") {
        Integration fixture;
        fixture.Begin();
        fixture.CommitCinematic();
        REQUIRE(fixture.playback.dispatcher.Drain(8).Value() == 1);
        REQUIRE(fixture.scene.Commit(1).submitted == 1);
        const auto retained = fixture.scene.Extract(2);
        const auto oldProducer = fixture.scene.vfx->CommittedProducer("game.tests");
        const auto oldScene = fixture.scene.scenes.ActiveScene()->RuntimeId();
        REQUIRE(fixture.scene.scenes.QueuePreparation(SceneDefinition(2)).HasValue());
        CHECK(oldProducer->IsUsable());
        fixture.scene.Phase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, 3);
        REQUIRE(fixture.scene.scenes.ActiveScene());
        CHECK(fixture.scene.scenes.ActiveScene()->RuntimeId() != oldScene);
        CHECK_FALSE(oldProducer->IsUsable());
        CHECK(fixture.scene.vfx->CommittedProducer("game.tests")->IsUsable());
        REQUIRE(retained.Instances().size() == 1);
        CHECK(retained.Instances()[0].position.x == 10.0F);
        CHECK(fixture.scene.Extract(4).Instances().empty());
    }

    TEST_CASE("Cinematic VFX preparation rejects incompatible projection metadata before any effect",
              "[cinematic][vfx][scene][integration][validation]") {
        Integration fixture;
        auto binding = AdapterBinding();
        VfxEventPayloadProjection projection{.scaleArgument = 3};
        SECTION("Out of range argument") {
            projection.positionArguments = {0, 1, 99};
        }
        SECTION("Number cannot be used for exact millimeter position") {
            projection.positionArguments = {3, 1, 2};
        }
        SECTION("SignedInteger cannot be used for Number scale") {
            projection.scaleArgument = 0;
        }
        SECTION("Unknown cooked binding") {
            binding.binding = {999, 1};
        }
        SECTION("Mismatched cooked payload schema") {
            binding.schema = {999, 1};
        }
        SECTION("Preview is not a gameplay effect source") {
            binding.context = EventRuntimeContext::Preview;
        }
        const auto rejected =
            CinematicVfxEventAdapter::Create(binding, fixture.plan, fixture.scene.vfx->CommittedProducer("game.tests"), projection);
        REQUIRE(rejected.HasError());
        CHECK(fixture.scene.Extract(2).Instances().empty());
    }

    TEST_CASE("A retained cinematic registration lease keeps its prepared payload cache alive",
              "[cinematic][vfx][scene][integration][lifetime]") {
        Integration fixture;
        fixture.adapter.reset();
        fixture.Begin();
        fixture.CommitCinematic();
        REQUIRE(fixture.playback.dispatcher.Drain(8).Value() == 1);
        REQUIRE(fixture.playback.dispatcher.Results().size() == 1);
        CHECK(fixture.playback.dispatcher.Results()[0].outcome == EventDispatchOutcome::Accepted);
        REQUIRE(fixture.scene.Commit(1).submitted == 1);
        CHECK(fixture.scene.Extract(2).Instances().size() == 1);
    }

    TEST_CASE("Cinematic adapter revocation is terminal while its dispatcher registration remains retained",
              "[cinematic][vfx][scene][integration][lifetime]") {
        Integration fixture;
        fixture.Begin();
        fixture.CommitCinematic();
        fixture.adapter->Revoke();
        REQUIRE(fixture.playback.dispatcher.Drain(8).Value() == 1);
        CHECK(fixture.playback.dispatcher.Results()[0].outcome == EventDispatchOutcome::BindingUnavailable);
        CHECK(fixture.adapter->Results().empty());
        REQUIRE(fixture.scene.Commit(1).submitted == 0);
        CHECK(fixture.scene.Extract(2).Instances().empty());
    }

    TEST_CASE("A cinematic adapter never silently rebinds its captured semantic mapping generation",
              "[cinematic][vfx][scene][integration][reload]") {
        Integration fixture;
        REQUIRE(fixture.scene.vfx->Reload(Bindings(2, 3.0F), fixture.scene.registry.Snapshot(), *fixture.scene.provider).HasValue());
        fixture.Begin();
        fixture.CommitCinematic();
        REQUIRE(fixture.playback.dispatcher.Drain(8).Value() == 1);
        REQUIRE(fixture.adapter->Results().size() == 1);
        const auto record = fixture.adapter->Results()[0];
        CHECK(record.submission.status == Status::StaleBinding);
        CHECK(record.occurrence == fixture.playback.dispatcher.Results()[0].occurrence);
        CHECK(fixture.playback.dispatcher.Results()[0].outcome == EventDispatchOutcome::StaleBinding);
        REQUIRE(fixture.scene.Commit(1).submitted == 0);
        CHECK(fixture.scene.Extract(2).Instances().empty());
    }

    TEST_CASE("Prepared cinematic registration rejects altered exact cooked evidence without parsing or submitting",
              "[cinematic][vfx][scene][integration][validation][allocation]") {
        Integration fixture;
        fixture.Begin();
        fixture.CommitCinematic();
        REQUIRE(fixture.playback.dispatcher.Drain(8).Value() == 1);
        REQUIRE(fixture.adapter->Results().size() == 1);
        const auto &key = fixture.plan->Keys().front();
        EventDispatchRequest request{fixture.playback.dispatcher.Results()[0].occurrence, key.binding, key.schema, key.payload, 1};
        auto altered = key.payload;
        SECTION("Different bytes") {
            altered.back() ^= std::byte{1};
            request.payload = altered;
        }
        SECTION("Different key") {
            request.occurrence.key = {999, 1};
        }
        SECTION("Different binding") {
            request.binding = {999, 1};
        }
        SECTION("Different schema") {
            request.schema = {999, 1};
        }
        const auto registration = fixture.adapter->Registration();
        REQUIRE(registration.callback);
        EventDispatchOutcome outcome{};
        Probe::Measurement allocations;
        {
            Probe::ScopedMeasurement measure;
            outcome = registration.callback(registration.callbackContext, request);
            allocations = measure.Snapshot();
        }
        CHECK(outcome != EventDispatchOutcome::Accepted);
        CHECK(outcome != EventDispatchOutcome::OperationStarted);
        CHECK(allocations.requests == 0);
        CHECK(fixture.adapter->Results().size() == 1);
        REQUIRE(fixture.scene.Commit(1).submitted == 1);
        CHECK(fixture.scene.Extract(2).Instances().size() == 1);
    }
}  // namespace Horo::Cinematic
