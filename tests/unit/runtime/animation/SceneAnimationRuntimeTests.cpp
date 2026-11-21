#include "AllocationProbe.h"
#include "AnimationActionTestFixtures.h"
#include "AnimationClipNotifyTestFixtures.h"
#include "AnimationGraphInstanceTestFixtures.h"
#include "AnimationPoseCompositionTestFixtures.h"
#include "AnimationStateGraphTestFixtures.h"
#include "Horo/Gameplay/GameplayAnimationScene.h"
#include "animation/AnimationRuntimeFixture.h"

#include <catch2/catch_approx.hpp>
#include <thread>

namespace Horo::Animation {
    namespace {
        /** @brief Cooks the socket-bearing graph through the same portable Scene path used by the host. */
        std::optional<AnimationAuthoringComponent> PrepareSocketGraph(Test::Cook::Fixture &cook) {
            const auto graph = Test::Graph::Simple();
            const Test::Instance::Prepared source(graph, 6, false, Test::Instance::SocketSkeleton());
            cook.Replace(*source.skeleton, source.compileClips, graph);
            return std::nullopt;
        }

        /** @brief Cooks a genuine authoritative graph with a tracked root and ten units of travel per fixture tick. */
        std::optional<AnimationAuthoringComponent> PrepareRootedGraph(Test::Cook::Fixture &cook) {
            const auto graph = Test::Graph::Simple();
            const Test::Instance::Prepared source(graph, 6, true);
            cook.Replace(*source.skeleton, source.compileClips, graph);
            AnimationAuthoringComponent component;
            component.component = Test::Id<AnimationComponentId>(7);
            component.skeleton = source.skeleton->Data().skeleton;
            component.source = {AnimationSourceKind::Graph, {}, graph.id};
            component.rootMotion = RootMotionPolicy::RequestCharacterMovement;
            return component;
        }

        /** @brief Confirms malformed and duplicate movement feedback preserve the single valid response. */
        void ResolveFixtureMovement(Runtime::SceneAnimationRuntime &owner, Runtime::EntityRef entity, const RootMotionRequestId &id,
                                    const Runtime::FixedStepContext &context) {
            const AnimationRootMotionDelta malformed{{4.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 0.0F}};
            CHECK(owner.ResolveRootMotion(entity, id, malformed, context).HasError());
            const AnimationRootMotionDelta moved{{4.0F, 0.0F, 0.0F}, {}};
            REQUIRE(owner.ResolveRootMotion(entity, id, moved, context).HasValue());
            CHECK(owner.ResolveRootMotion(entity, id, moved, context).HasError());
        }
    }  // namespace

    TEST_CASE("Gameplay animation grant reads only its Scene and retires with the module", "[animation][scene][gameplay]") {
        Test::Scene::Fixture fixture(false, true, PrepareSocketGraph);
        CancellationSource module;
        const Gameplay::GameplayEntityRef entity{fixture.entity.runtime.value, fixture.entity.entity.index,
                                                 fixture.entity.entity.generation};
        const auto grant = Test::Cook::Take(
            Gameplay::CreateGameplayAnimationContext(fixture.forward->adapter, fixture.entity.runtime, "game.animation", module.Token()));
        CHECK(grant->ModuleId() == "game.animation");
        auto foreign = entity;
        ++foreign.scene;
        const auto denied = grant->Observe(foreign);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == AnimationErrors::HandleOwnerMismatch.code.Value());
        REQUIRE(fixture.Tick().HasValue());
        const auto observed = Test::Cook::Take(grant->Observe(entity));
        const auto socket = Test::Id<SkeletonSocketId>(21);
        Tests::AllocationProbe::Measurement metrics;
        {
            Tests::AllocationProbe::ScopedMeasurement window;
            const auto read = grant->SocketModelTransform(observed.actor, observed.current, socket);
            metrics = window.Snapshot();
            CHECK(read.HasValue());
        }
        CHECK(metrics.requests == 0);
        CHECK(Test::Cook::Take(grant->SocketModelTransform(observed.actor, observed.current, socket)).At(0, 3) == 18.0F);
        CHECK(Test::Cook::Take(grant->CopyClipEvents(observed.actor, observed.current, {})) == 0);
        CHECK(Test::Cook::Take(grant->CopyStateEvents(observed.actor, observed.current, {})) == 0);
        auto stale = observed.actor;
        ++stale.instance.slot.generation;
        const auto rejected = grant->QueueTrigger(stale, Test::Id<GraphParameterId>(1));
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == AnimationErrors::HandleStale.code.Value());
        module.RequestCancellation();
        const auto cancelled = grant->Observe(entity);
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == AnimationErrors::GraphOperationCancelled.code.Value());
    }

    TEST_CASE("Gameplay animation grant observes and releases existing authoritative actions", "[animation][scene][gameplay]") {
        Test::Scene::Fixture fixture(false, true, [](Test::Cook::Fixture &cook) -> std::optional<AnimationAuthoringComponent> {
            const Test::Actions::Prepared action;
            cook.Replace(*action.skeleton, action.compileClips, action.graph);
            return std::nullopt;
        });
        CancellationSource module;
        const Gameplay::GameplayEntityRef entity{fixture.entity.runtime.value, fixture.entity.entity.index,
                                                 fixture.entity.entity.generation};
        const auto grant = Test::Cook::Take(
            Gameplay::CreateGameplayAnimationContext(fixture.forward->adapter, fixture.entity.runtime, "game.animation", module.Token()));
        const auto actor = Test::Cook::Take(grant->Observe(entity)).actor;
        const auto request = Test::Actions::Request();
        const auto handle = Test::Cook::Take(grant->QueueAction(actor, {request.action, request.startSection, request.queue}));
        CHECK(Test::Cook::Take(grant->ObserveAction(actor, handle)).phase == GraphActionPhase::Pending);
        REQUIRE(fixture.Tick().HasValue());
        CHECK(Test::Cook::Take(grant->ObserveAction(actor, handle)).phase == GraphActionPhase::Playing);
        REQUIRE(grant->CancelAction(actor, handle, GraphActionStopPolicy::Cut).HasValue());
        REQUIRE(fixture.Tick().HasValue());
        CHECK(Test::Cook::Take(grant->ObserveAction(actor, handle)).reason == GraphActionCompletionReason::Cancelled);
        REQUIRE(grant->ReleaseAction(actor, handle).HasValue());
        CHECK(grant->ObserveAction(actor, handle).HasError());
    }

    TEST_CASE("Gameplay animation grant copies committed root motion feedback", "[animation][scene][gameplay]") {
        Test::Scene::Fixture fixture(false, true, PrepareRootedGraph, {}, {}, true);
        CancellationSource module;
        const Gameplay::GameplayEntityRef entity{fixture.entity.runtime.value, fixture.entity.entity.index,
                                                 fixture.entity.entity.generation};
        const auto grant = Test::Cook::Take(
            Gameplay::CreateGameplayAnimationContext(fixture.forward->adapter, fixture.entity.runtime, "game.animation", module.Token()));
        CHECK_FALSE(Test::Cook::Take(grant->Observe(entity)).rootMotion.has_value());
        REQUIRE(fixture.Tick().HasValue());
        const auto snapshot = Test::Cook::Take(grant->Observe(entity));
        REQUIRE(snapshot.rootMotion.has_value());
        CHECK(snapshot.rootMotion->request.instance == snapshot.actor.instance);
        CHECK(snapshot.rootMotion->disposition == AnimationRootMotionDisposition::Unavailable);
        CHECK(snapshot.rootMotion->requested.translation.x == 10.0F);
        CHECK(snapshot.rootMotion->remaining.translation.x == 10.0F);
    }

    TEST_CASE("Scene gameplay parameter commands affect only the next committed playback tick", "[animation][scene][parameters]") {
        Test::Scene::Fixture fixture(false, true, [](Test::Cook::Fixture &cook) -> std::optional<AnimationAuthoringComponent> {
            const auto graph = Test::Instance::Blended();
            const Test::Instance::Prepared source(graph);
            cook.Replace(*source.skeleton, source.compileClips, graph);
            return std::nullopt;
        });
        auto &owner = *fixture.forward->adapter;
        const auto weight = Test::Id<GraphParameterId>(1);
        REQUIRE(owner.QueueParameter(fixture.entity, {weight, 1.0F}).HasValue());
        fixture.peer->failUpdate = true;
        REQUIRE(fixture.Tick().HasError());
        CHECK_FALSE(fixture.Observation().component);
        REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
        const auto pose = fixture.Observation().component->currentPose;
        std::array<GraphParameterInput, 1> parameters;
        REQUIRE(owner.CopyParameters(fixture.entity, pose, parameters).HasValue());
        CHECK(std::get<float>(parameters[0].value) == 1.0F);
        REQUIRE(fixture.Tick().HasValue());
        const auto next = fixture.Observation().component->currentPose;
        REQUIRE(owner.CopyParameters(fixture.entity, next, parameters).HasValue());
        CHECK(std::get<float>(parameters[0].value) == 0.5F);
        fixture.lifetime.RequestCancellation();
        CHECK(owner.QueueParameter(fixture.entity, {weight, 0.0F}).failure == GraphInstanceFailure::Cancelled);
    }

    TEST_CASE("Scene socket binding follows committed poses until their residency expires", "[animation][scene][sockets]") {
        Test::Scene::Fixture fixture(false, true, PrepareSocketGraph);
        auto &owner = *fixture.forward->adapter;
        const auto binding = Test::Cook::Take(owner.BindSocket(fixture.entity, Test::Id<SkeletonSocketId>(21)));
        const auto unpublished = owner.SocketModelTransform(binding, {});
        REQUIRE(unpublished.HasError());
        CHECK(unpublished.ErrorValue().code.Value() == AnimationErrors::HandleMalformed.code.Value());
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.Observation().component->currentPose;
        REQUIRE(fixture.Tick().HasValue());
        const auto second = fixture.Observation().component->currentPose;
        CHECK(Test::Cook::Take(owner.SocketModelTransform(binding, first)).At(0, 3) == 18.0F);
        CHECK(Test::Cook::Take(owner.SocketModelTransform(binding, second)).At(0, 3) == 28.0F);
        CHECK(Test::Cook::Take(owner.JointModelTransform(fixture.entity, second, Test::Id<JointId>(2))).At(0, 3) == 20.0F);
        REQUIRE(fixture.Tick().HasValue());
        const auto retired = owner.SocketModelTransform(binding, first);
        REQUIRE(retired.HasError());
        CHECK(retired.ErrorValue().code.Value() == AnimationErrors::HandleStale.code.Value());
    }

    TEST_CASE("Scene socket binding rejects foreign callers and never retargets on reload", "[animation][scene][sockets]") {
        Test::Scene::Fixture fixture(false, true, PrepareSocketGraph);
        auto &owner = *fixture.forward->adapter;
        const auto socket = Test::Id<SkeletonSocketId>(21);
        const auto binding = Test::Cook::Take(owner.BindSocket(fixture.entity, socket));
        ErrorCode foreignCode;
        std::jthread foreign([&owner, &binding, &foreignCode] {
            const auto read = owner.SocketModelTransform(binding, {});
            if (read.HasError())
                foreignCode = read.ErrorValue().code;
        });
        foreign.join();
        CHECK(foreignCode.Value() == AnimationErrors::PoseThreadViolation.code.Value());
        fixture.Activate(2);
        const auto replacement = Test::Cook::Take(owner.BindSocket(fixture.entity, socket));
        CHECK(replacement.instance != binding.instance);
        REQUIRE(fixture.Tick().HasValue());
        const auto pose = fixture.Observation().component->currentPose;
        CHECK(owner.SocketModelTransform(replacement, pose).HasValue());
        const auto retired = owner.SocketModelTransform(binding, pose);
        REQUIRE(retired.HasError());
        CHECK(retired.ErrorValue().code.Value() == AnimationErrors::HandleStale.code.Value());
    }

    TEST_CASE("Scene socket reads give typed missing and lifecycle errors", "[animation][scene][sockets]") {
        Test::Scene::Fixture fixture(false, true, PrepareSocketGraph);
        auto &owner = *fixture.forward->adapter;
        const auto missing = owner.BindSocket(fixture.entity, Test::Id<SkeletonSocketId>(99));
        REQUIRE(missing.HasError());
        CHECK(missing.ErrorValue().code.Value() == AnimationErrors::PoseSocketMissing.code.Value());
        const auto binding = Test::Cook::Take(owner.BindSocket(fixture.entity, Test::Id<SkeletonSocketId>(21)));
        REQUIRE(fixture.Tick().HasValue());
        const auto pose = fixture.Observation().component->currentPose;
        fixture.lifetime.RequestCancellation();
        const auto cancelled = owner.SocketModelTransform(binding, pose);
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == AnimationErrors::GraphOperationCancelled.code.Value());
        owner.Shutdown();
        const auto closed = owner.SocketModelTransform(binding, pose);
        REQUIRE(closed.HasError());
        CHECK(closed.ErrorValue().code.Value() == AnimationErrors::GraphAdmissionRejected.code.Value());
    }

    TEST_CASE("Scene movement claims retire on abort and bind feedback to the exact retry", "[animation][scene][root-motion]") {
        Test::Scene::Fixture fixture(false, true, PrepareRootedGraph, {}, {}, true);
        auto &owner = *fixture.forward->adapter;
        RootMotionRequestId abandoned;
        fixture.peer->movement = [&fixture, &owner, &abandoned](const Runtime::FixedStepContext &context) {
            const auto request = owner.ClaimRootMotion(fixture.entity, context);
            REQUIRE(request.HasValue());
            REQUIRE(request.Value().has_value());
            const auto id = request.Value()->id;
            CHECK(request.Value()->delta.translation.x == 10.0F);
            CHECK(owner.ClaimRootMotion(fixture.entity, context).HasError());
            if (abandoned.IsValid()) {
                CHECK(id.tick == abandoned.tick);
                CHECK(id.generation != abandoned.generation);
                CHECK(owner.ResolveRootMotion(fixture.entity, abandoned, std::nullopt, context).HasError());
            } else {
                abandoned = id;
            }
            CHECK_FALSE(fixture.Observation().rootMotion.has_value());
            ResolveFixtureMovement(owner, fixture.entity, id, context);
            return Result<void>::Success();
        };
        fixture.peer->failUpdate = true;
        REQUIRE(fixture.Tick().HasError());
        CHECK_FALSE(fixture.Observation().rootMotion.has_value());
        REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
        const auto committed = fixture.Observation();
        REQUIRE(committed.rootMotion.has_value());
        CHECK(committed.rootMotion->request.id.instance == committed.instance);
        CHECK(committed.rootMotion->disposition == Runtime::SceneRootMotionDisposition::Consumed);
        CHECK(committed.rootMotion->applied.translation.x == 4.0F);
        CHECK(committed.rootMotion->remaining.translation.x == 6.0F);
    }

    TEST_CASE("Scene root motion distinguishes absent consumers rejection and unresolved claims", "[animation][scene][root-motion]") {
        Test::Scene::Fixture fixture(false, true, PrepareRootedGraph, {}, {}, true);
        auto &owner = *fixture.forward->adapter;
        REQUIRE(fixture.Tick().HasValue());
        const auto initial = fixture.Observation().rootMotion;
        REQUIRE(initial.has_value());
        CHECK(initial->disposition == Runtime::SceneRootMotionDisposition::Unavailable);
        CHECK(initial->remaining.translation.x == 10.0F);
        fixture.peer->movement = [&fixture, &owner](const Runtime::FixedStepContext &context) {
            const auto request = owner.ClaimRootMotion(fixture.entity, context);
            REQUIRE(request.HasValue());
            CHECK(request.Value().has_value());
            return Result<void>::Success();
        };
        REQUIRE(fixture.Tick().HasError());
        REQUIRE(fixture.Observation().rootMotion.has_value());
        CHECK(fixture.Observation().rootMotion->request.id == initial->request.id);
        fixture.peer->movement = [&fixture, &owner](const Runtime::FixedStepContext &context) {
            const auto request = owner.ClaimRootMotion(fixture.entity, context);
            REQUIRE(request.HasValue());
            REQUIRE(request.Value().has_value());
            return owner.ResolveRootMotion(fixture.entity, request.Value()->id, std::nullopt, context);
        };
        REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
        const auto rejected = fixture.Observation().rootMotion;
        REQUIRE(rejected.has_value());
        CHECK(rejected->disposition == Runtime::SceneRootMotionDisposition::Rejected);
        CHECK(rejected->remaining.translation.x == 10.0F);
    }

    TEST_CASE("Scene animation freezes per-actor input before evaluation without fixed-path allocation", "[animation][scene][input]") {
        struct Inputs final : Runtime::SceneAnimationInputSource {
            GraphParameterId parameter{Test::Id<GraphParameterId>(1)};
            Runtime::EntityRef first;
            bool oversized{};
            mutable std::array<GraphParameterInput, 2> scratch;

            Runtime::SceneAnimationInputSnapshot Read(Runtime::EntityRef entity,
                                                      const Runtime::FixedStepContext &) const noexcept override {
                scratch.front() = {parameter, entity == first ? 0.0F : 1.0F};
                return {std::span{scratch}.first(oversized && entity != first ? scratch.size() : 1)};
            }
        };

        auto inputs = std::make_shared<Inputs>();
        JobSystem jobs{{2, 8}};
        std::optional<Runtime::SceneAnimationParallelEvaluation> parallel;
        SECTION("serial evaluation remains allocation free") {
            // The absent policy selects the owner-only path.
        }
        SECTION("dedicated evaluation joins actual CPU jobs") {
            parallel.emplace(jobs, Duration::FromMilliseconds(1000));
        }
        Test::Scene::Fixture fixture(false, true, [](Test::Cook::Fixture &cook) -> std::optional<AnimationAuthoringComponent> {
            const auto graph = Test::Instance::Blended();
            const Test::Instance::Prepared prepared{graph};
            cook.Replace(*prepared.skeleton, prepared.compileClips, graph);
            return std::nullopt;
        }, inputs, parallel);
        inputs->first = fixture.entity;
        Runtime::RuntimeEntityCreateInfo create;
        create.components.animation = fixture.Binding();
        create.components.animation->component = Test::Id<AnimationComponentId>(8);
        Runtime::SceneCommandBuffer commands;
        REQUIRE(commands.Create(create).IsValid());
        const auto created = fixture.Structural(std::move(commands));
        REQUIRE(created->ResultValue());
        REQUIRE(created->ResultValue()->created.size() == 1);
        const auto second = created->ResultValue()->created.front().entity;

        inputs->oversized = true;
        REQUIRE(fixture.Tick().HasError());
        CHECK_FALSE(fixture.Observation().component);
        inputs->oversized = false;
        Tests::AllocationProbe::Measurement measured;
        auto tick = Result<void>::Success();
        {
            Tests::AllocationProbe::ScopedMeasurement window;
            tick = fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false);
            measured = window.Snapshot();
        }
        REQUIRE(tick.HasValue());
        if (parallel) {
            const auto completed = jobs.SnapshotIfChanged(0);
            REQUIRE(completed);
            REQUIRE(completed->jobs.size() == 2);
            for (const auto &job : completed->jobs)
                CHECK(job.state == JobState::Succeeded);
        } else {
            CHECK(measured.requests == 0);
        }
        CHECK(fixture.PoseX() == 10.0F);
        const auto other = Test::Cook::Take(fixture.forward->adapter->Query(second));
        REQUIRE(other.component);
        std::array<Math::Transform, 1> pose;
        REQUIRE(fixture.forward->adapter->CopyPose(second, other.component->currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 110.0F);

        if (parallel) {
            const auto committed = fixture.Observation().component->currentPose;
            fixture.peer->failUpdate = true;
            REQUIRE(fixture.Tick().HasError());
            CHECK(fixture.Observation().component->currentPose == committed);
            REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
            CHECK(fixture.PoseX() == 20.0F);
            fixture.Activate(2);
            REQUIRE(fixture.forward->adapter->Query(second).HasError());
            inputs->first = fixture.entity;
            Runtime::SceneCommandBuffer replacement;
            REQUIRE(replacement.Create(create).IsValid());
            REQUIRE(fixture.Structural(std::move(replacement))->ResultValue());
            REQUIRE(fixture.Tick().HasValue());
            CHECK(fixture.PoseX() == 10.0F);

            const auto replacementPose = fixture.Observation().component->currentPose;
            fixture.peer->cancelDuringPrepare = &fixture.lifetime;
            REQUIRE(fixture.Tick().HasError());
            REQUIRE(fixture.forward->adapter->CopyPose(fixture.entity, replacementPose, pose).HasValue());
            CHECK(pose[0].translation.x == 10.0F);
            fixture.forward->adapter->Shutdown();
            CHECK(fixture.forward->adapter->CopyPose(fixture.entity, replacementPose, pose).HasError());
        }
    }

    TEST_CASE("Scene action slots expose committed authority state and preserve old pose observations", "[animation][scene][actions]") {
        Test::Scene::Fixture fixture(false, true, [](Test::Cook::Fixture &cook) -> std::optional<AnimationAuthoringComponent> {
            const Test::Actions::Prepared action;
            cook.Replace(*action.skeleton, action.compileClips, action.graph);
            return std::nullopt;
        });
        auto &owner = *fixture.forward->adapter;
        const auto queued = owner.QueueAction(fixture.entity, Test::Actions::Request());
        REQUIRE(queued.HasValue());
        const auto pending = owner.QueryAction(fixture.entity, queued.handle);
        REQUIRE(pending.HasValue());
        CHECK(pending.observation.phase == GraphActionPhase::Pending);
        CHECK_FALSE(fixture.Observation().component);
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.Observation();
        REQUIRE(first.component);
        std::array<GraphActionSlotObservation, 1> slots;
        std::uint32_t written{};
        REQUIRE(owner.CopyActionSlots(fixture.entity, first.component->currentPose, slots, written).HasValue());
        REQUIRE(written == 1);
        CHECK(slots.front().active == queued.handle);
        REQUIRE(owner.CancelAction(fixture.entity, queued.handle, GraphActionStopPolicy::Cut).HasValue());
        REQUIRE(fixture.Tick().HasValue());
        const auto idle = fixture.Observation();
        REQUIRE(idle.component);
        REQUIRE(owner.CopyActionSlots(fixture.entity, idle.component->currentPose, slots, written).HasValue());
        CHECK_FALSE(slots.front().active.IsValid());
        REQUIRE(owner.CopyActionSlots(fixture.entity, first.component->currentPose, slots, written).HasValue());
        CHECK(slots.front().action == Test::Actions::Request().action);
        const auto terminal = owner.QueryAction(fixture.entity, queued.handle);
        REQUIRE(terminal.HasValue());
        CHECK(terminal.observation.phase == GraphActionPhase::Terminal);
        REQUIRE(owner.ReleaseActionResult(fixture.entity, queued.handle).HasValue());
        CHECK(owner.QueryAction(fixture.entity, queued.handle).failure == GraphActionFailure::HandleStale);
    }

    TEST_CASE("Actual Animation Scene loads real AST Graph or Clip artifacts and publishes genuine first-pose baseline",
              "[animation][scene][lifecycle][native]") {
        bool clip{};
        SECTION("compiled graph") {
            clip = false;
        }
        SECTION("direct authored clip") {
            clip = true;
        }
        Test::Scene::Fixture fixture(clip);
        CHECK_FALSE(fixture.Observation().component);
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.Observation();
        REQUIRE(first.component);
        CHECK(first.component->source == fixture.Binding().source);
        CHECK(first.component->previousPose == first.component->currentPose);
        CHECK(fixture.PoseX() == 10.0F);
        REQUIRE(ValidateRuntimeAnimationComponent(*first.component, first.authoring).HasValue());
        Tests::AllocationProbe::Measurement measured;
        auto tick = Result<void>::Success();
        {
            Tests::AllocationProbe::ScopedMeasurement window;
            tick = fixture.Tick();
            measured = window.Snapshot();
        }
        INFO((tick.HasError() ? tick.ErrorValue().code.Value() + ": " + tick.ErrorValue().message : "success"));
        REQUIRE(tick.HasValue());
        CHECK(measured.requests == 0);
        CHECK(fixture.PoseX() == 20.0F);
        CHECK(fixture.Observation().component->previousPose == first.component->currentPose);
    }

    TEST_CASE("Actual host pause gaps resume Animation with one genuine fixed delta and no tick-gap catchup",
              "[animation][scene][lifecycle][pause]") {
        Test::Scene::Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.Observation();
        bool hostPause{};
        SECTION("actual local host skips Play evaluation") {
            hostPause = true;
            fixture.forward->enabled = false;
        }
        SECTION("component play intent pauses its existing instance") {
            REQUIRE(fixture.forward->adapter->SetPlaying(fixture.entity, false).HasValue());
        }
        for (unsigned i = 0; i < 4; ++i)
            REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.scheduler->Statistics().completedSimulationTick == 5);
        CHECK(fixture.Observation().component->currentPose == first.component->currentPose);
        CHECK(fixture.PoseX() == 10.0F);
        if (hostPause)
            fixture.forward->enabled = true;
        else
            REQUIRE(fixture.forward->adapter->SetPlaying(fixture.entity, true).HasValue());
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.forward->lastTick == 6);
        CHECK(fixture.PoseX() == 20.0F);
    }

    TEST_CASE("Later actual fixed-owner failure aborts Animation candidate and same-tick real retry preserves cursor",
              "[animation][scene][lifecycle][rollback]") {
        Test::Scene::Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.Observation();
        fixture.peer->failUpdate = true;
        REQUIRE(fixture.Tick().HasError());
        CHECK(fixture.forward->aborts == 1);
        CHECK(fixture.scheduler->Statistics().completedSimulationTick == 1);
        CHECK(fixture.Observation().component->currentPose == first.component->currentPose);
        CHECK(fixture.PoseX() == 10.0F);
        // Same real accumulator/clock: the scheduler issues the retry; no source rerun or synthetic tick is supplied.
        REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
        CHECK(fixture.scheduler->Statistics().completedSimulationTick == 2);
        CHECK(fixture.forward->publications == 2);
        CHECK(fixture.PoseX() == 20.0F);
    }

    TEST_CASE("Actual source retirement before fence aborts only the exact staged Animation attempt",
              "[animation][scene][lifecycle][retirement]") {
        Test::Scene::Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.Observation();
        fixture.peer->retireBeforeFence = fixture.scheduler.get();
        REQUIRE(fixture.Tick().HasError());
        CHECK(fixture.forward->aborts == 1);
        CHECK(fixture.Observation().component->currentPose == first.component->currentPose);
        CHECK(fixture.PoseX() == 10.0F);
        CHECK(fixture.Tick().HasError());
        CHECK(fixture.PoseX() == 10.0F);
    }

    TEST_CASE("Real Scene reload retires old Animation handles and issues fresh same-owner instance generations",
              "[animation][scene][lifecycle][reload]") {
        Test::Scene::Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        const auto previous = fixture.Observation();
        const auto oldEntity = fixture.entity;
        fixture.Activate(2);
        const auto next = fixture.Observation();
        CHECK(next.instance.owner == previous.instance.owner);
        CHECK(next.instance.slot.generation > previous.instance.slot.generation);
        CHECK_FALSE(next.component);
        std::array<Math::Transform, 1> pose;
        CHECK(fixture.forward->adapter->CopyPose(oldEntity, previous.component->currentPose, pose).HasError());
        CHECK(fixture.forward->adapter->CopyPose(fixture.entity, previous.component->currentPose, pose).HasError());
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.PoseX() == 10.0F);
    }

    TEST_CASE("Ordinary canonical Create Destroy admits actual Animation owner and preserves surviving cursor",
              "[animation][scene][structural]") {
        Test::Scene::Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        Runtime::RuntimeEntityCreateInfo create;
        create.components.animation = fixture.Binding();
        create.components.animation->component = Test::Id<AnimationComponentId>(8);
        Runtime::SceneCommandBuffer commands;
        const auto deferred = commands.Create(create);
        const auto created = fixture.Structural(std::move(commands));
        INFO((created->Failure() ? created->Failure()->code.Value() + ": " + created->Failure()->message : "success"));
        REQUIRE(created->ResultValue());
        REQUIRE(created->ResultValue()->created.size() == 1);
        CHECK(created->ResultValue()->created.front().deferred == deferred);
        const auto added = created->ResultValue()->created.front().entity;
        REQUIRE(fixture.forward->adapter->Query(added).HasValue());
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.PoseX() == 20.0F);
        const auto other = Test::Cook::Take(fixture.forward->adapter->Query(added));
        REQUIRE(other.component);
        std::array<Math::Transform, 1> pose;
        REQUIRE(fixture.forward->adapter->CopyPose(added, other.component->currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 10.0F);
        Runtime::SceneCommandBuffer destroy;
        destroy.Destroy(added);
        SECTION("a removed actor leaves its slot vacant") {
            REQUIRE(fixture.Structural(std::move(destroy))->ResultValue());
        }
        SECTION("a replacement takes the slot without inheriting its pose") {
            REQUIRE(destroy.Create(create).IsValid());
            const auto replaced = fixture.Structural(std::move(destroy));
            REQUIRE(replaced->ResultValue());
            REQUIRE(replaced->ResultValue()->created.size() == 1);
            const auto replacement = replaced->ResultValue()->created.front().entity;
            const auto fresh = Test::Cook::Take(fixture.forward->adapter->Query(replacement));
            CHECK(fresh.instance.slot.index == other.instance.slot.index);
            CHECK(fresh.instance.slot.generation > other.instance.slot.generation);
            CHECK_FALSE(fresh.component);
            CHECK(fixture.forward->adapter->CopyPose(replacement, other.component->currentPose, pose).HasError());
        }
        CHECK(fixture.forward->adapter->Query(added).HasError());
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.PoseX() == Catch::Approx(30.0F).epsilon(0).margin(0.00001F));
    }

    TEST_CASE("Missing Animation owner or resources and unsupported native policies reject ordinary Create before mutation",
              "[animation][scene][structural][rollback]") {
        bool owner = true;
        enum class Fault {
            MissingOwner,
            MissingResource,
            Retarget,
            RootMotion
        } fault{};
        SECTION("missing actual structural owner") {
            owner = false;
            fault = Fault::MissingOwner;
        }
        SECTION("missing already loaded resource") {
            fault = Fault::MissingResource;
        }
        SECTION("unsupported retarget owner") {
            fault = Fault::Retarget;
        }
        SECTION("unsupported movement handoff") {
            fault = Fault::RootMotion;
        }
        Test::Scene::Fixture fixture(false, owner);
        REQUIRE(fixture.Tick().HasValue());
        const auto before = *fixture.scenes.ActiveScene();
        const auto pose = fixture.Observation().component->currentPose;
        Runtime::RuntimeEntityCreateInfo create;
        create.components.animation = fixture.Binding();
        create.components.animation->component = Test::Id<AnimationComponentId>(9);
        if (fault == Fault::MissingResource)
            create.components.animation->source.graph = Test::Asset<AnimationGraphId>(99);
        if (fault == Fault::Retarget)
            create.components.animation->retarget = Test::Asset<RetargetProfileId>(99);
        if (fault == Fault::RootMotion)
            create.components.animation->rootMotion = RootMotionPolicy::RequestCharacterMovement;
        Runtime::SceneCommandBuffer commands;
        (void)commands.Create(create);
        const auto result = fixture.Structural(std::move(commands));
        REQUIRE(result->Failure());
        CHECK_FALSE(result->ResultValue());
        CHECK(before.IsCurrent());
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == before.SlotCount());
        CHECK(fixture.Observation().component->currentPose == pose);
    }

    TEST_CASE("Real AST additive Graph source cook load and Scene fixed commit preserve actual reference operands",
              "[animation][scene][cook][additive][native]") {
        Test::PoseComposition::LayerFixture source(Test::PoseComposition::AdditiveClipGraph(), true);
        Test::Scene::Fixture fixture(false, true, [&](Test::Cook::Fixture &cook) -> std::optional<AnimationAuthoringComponent> {
            cook.Replace(*source.skeleton, source.compileClips, source.graph);
            AnimationAuthoringComponent component;
            component.component = Test::Id<AnimationComponentId>(7);
            component.skeleton = source.skeleton->Data().skeleton;
            component.source = {AnimationSourceKind::Graph, {}, source.graph.id};
            return component;
        });
        REQUIRE(fixture.Tick().HasValue());
        const auto observation = fixture.Observation();
        REQUIRE(observation.component);
        std::array<Math::Transform, 3> pose;
        REQUIRE(fixture.forward->adapter->CopyPose(fixture.entity, observation.component->currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 25.0F);
        CHECK(pose[0].scale.x == 8.0F);
        CHECK(pose[1].translation.x == 10.0F);
        CHECK(pose[2].translation.x == 10.0F);
        CHECK(observation.component->source == fixture.Binding().source);
    }

    TEST_CASE("Scene queued transition trigger survives actual failed tick and commits once through real cooked Graph",
              "[animation][scene][transition][rollback][native]") {
        auto graph = Test::StateGraph::Data();
        auto &machine = Test::StateGraph::Machine(graph);
        machine.states.resize(2);
        machine.transitions.resize(1);
        machine.transitions.front().duration = {0};
        machine.transitions.front().conditions = {{Test::Id<GraphParameterId>(2), GraphStatePredicate::Trigger, true}};
        Test::Instance::Prepared source(graph);
        Test::Scene::Fixture fixture(false, true, [&](Test::Cook::Fixture &cook) -> std::optional<AnimationAuthoringComponent> {
            cook.Replace(*source.skeleton, source.compileClips, graph);
            AnimationAuthoringComponent component;
            component.component = Test::Id<AnimationComponentId>(7);
            component.skeleton = source.skeleton->Data().skeleton;
            component.source = {AnimationSourceKind::Graph, {}, graph.id};
            return component;
        });
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.PoseX() == 10.0F);
        auto &owner = *fixture.forward->adapter;
        const auto before = fixture.Observation().component->currentPose;
        const auto trigger = Test::Id<GraphParameterId>(2);
        REQUIRE(owner.QueueTrigger(fixture.entity, trigger).HasValue());
        fixture.peer->failUpdate = true;
        REQUIRE(fixture.Tick().HasError());
        CHECK(fixture.PoseX() == 10.0F);
        REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
        CHECK(fixture.PoseX() == 100.0F);
        const auto after = fixture.Observation().component->currentPose;
        std::array<GraphParameterInput, 2> parameters;
        REQUIRE(owner.CopyParameters(fixture.entity, before, parameters).HasValue());
        REQUIRE(parameters[1].id == trigger);
        CHECK(parameters[1].value == GraphParameterValue{false});
        REQUIRE(owner.CopyParameters(fixture.entity, after, parameters).HasValue());
        CHECK(parameters[1].value == GraphParameterValue{true});
        std::array<GraphStateEvent, 8> events;
        std::uint32_t written{99};
        CHECK(owner.CopyStateEvents(fixture.entity, after, {}, written).failure == GraphInstanceFailure::LimitExceeded);
        CHECK(written == 99);
        REQUIRE(owner.CopyStateEvents(fixture.entity, after, events, written).HasValue());
        REQUIRE(written > 0);
        CHECK(std::ranges::any_of(std::span{events}.first(written), [](const auto &event) {
            return event.kind == GraphStateEventKind::TransitionCompleted;
        }));
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.PoseX() == 110.0F);
        CHECK(owner.CopyParameters(fixture.entity, before, parameters).failure == GraphInstanceFailure::HandleStale);
    }

    TEST_CASE("Scene clip occurrences retain exact committed identity and reject foreign or retired observations",
              "[animation][scene][notifies][lifecycle]") {
        const auto graph = Test::Graph::Simple();
        Test::Instance::Prepared source(graph);
        auto clip = source.compileClips.front().Data();
        clip.eventSchemas = {Test::Notifies::Schema()};
        clip.events = {Test::Notifies::Event(7, 5)};
        source.compileClips.front() = Test::Cook::Take(AnimationClipAsset::Create(std::move(clip), *source.skeleton));
        Test::Scene::Fixture fixture(false, true,
                                     [&source, &graph](Test::Cook::Fixture &cook) -> std::optional<AnimationAuthoringComponent> {
            cook.Replace(*source.skeleton, source.compileClips, graph);
            return std::nullopt;
        });
        auto &owner = *fixture.forward->adapter;
        REQUIRE(fixture.Tick().HasValue());
        const auto pose = fixture.Observation().component->currentPose;
        std::array<AnimationEventOccurrence, 1> events;
        std::uint32_t written{};
        REQUIRE(owner.CopyClipEvents(fixture.entity, pose, events, written).HasValue());
        REQUIRE(written == 1);
        const auto event = events.front();
        CHECK(event.id.marker == Test::Id<AnimationEventMarkerId>(7));
        CHECK(event.id.instance == pose.instance);
        REQUIRE(fixture.Tick().HasValue());
        REQUIRE(owner.CopyClipEvents(fixture.entity, pose, events, written).HasValue());
        CHECK(written == 1);
        CHECK(events.front() == event);

        GraphInstanceFailure foreign{};
        std::jthread thread([&foreign, &owner, &fixture, &pose, &events, &written] {
            foreign = owner.CopyClipEvents(fixture.entity, pose, events, written).failure;
        });
        thread.join();
        CHECK(foreign == GraphInstanceFailure::ThreadViolation);
        CHECK(written == 1);
        fixture.Activate(2);
        CHECK(owner.CopyClipEvents(fixture.entity, pose, events, written).failure == GraphInstanceFailure::HandleStale);
        owner.Shutdown();
        CHECK(owner.CopyClipEvents(fixture.entity, pose, events, written).failure == GraphInstanceFailure::AdmissionRejected);
        CHECK(events.front() == event);
        CHECK(written == 1);
    }

    TEST_CASE("Actual owner shutdown defers staged Graph retirement until the scheduler decision",
              "[animation][scene][lifecycle][shutdown]") {
        bool armed{};
        SECTION("shutdown after evaluation rejects precommit and aborts") {
            armed = false;
        }
        SECTION("shutdown after owner arm permits the actual globally committed publication") {
            armed = true;
        }
        Test::Scene::Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        const auto pose = fixture.Observation().component->currentPose;
        if (armed)
            fixture.peer->closeDuringPrepare = fixture.forward->adapter.get();
        else
            fixture.peer->closeDuringUpdate = fixture.forward->adapter.get();
        const auto result = fixture.Tick();
        // Shutdown rejects either preparation or the next nonfixed phase; it never retracts a real fixed commit.
        REQUIRE(result.HasError());
        CHECK(fixture.scheduler->Statistics().completedSimulationTick == (armed ? 2 : 1));
        CHECK(fixture.forward->publications == (armed ? 2 : 1));
        CHECK(fixture.forward->aborts == (armed ? 0 : 1));
        CHECK(fixture.forward->adapter->Query(fixture.entity).HasError());
        std::array<Math::Transform, 1> copied;
        CHECK(fixture.forward->adapter->CopyPose(fixture.entity, pose, copied).HasError());
        fixture.forward->adapter->Shutdown();
        fixture.forward->adapter->Shutdown();
    }

    TEST_CASE("Actual host cancellation after Animation arm aborts without consuming lastgood pose",
              "[animation][scene][lifecycle][cancellation]") {
        Test::Scene::Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        const auto previous = fixture.Observation().component->currentPose;
        fixture.peer->cancelDuringPrepare = &fixture.lifetime;
        REQUIRE(fixture.Tick().HasError());
        CHECK(fixture.scheduler->Statistics().completedSimulationTick == 1);
        CHECK(fixture.forward->publications == 1);
        CHECK(fixture.forward->aborts == 1);
        std::array<Math::Transform, 1> pose;
        REQUIRE(fixture.forward->adapter->CopyPose(fixture.entity, previous, pose).HasValue());
        CHECK(pose[0].translation.x == 10.0F);
        fixture.forward->adapter->Shutdown();
        CHECK(fixture.forward->adapter->CopyPose(fixture.entity, previous, pose).HasError());
    }

}  // namespace Horo::Animation
