#include "AllocationProbe.h"
#include "AnimationPoseCompositionTestFixtures.h"
#include "AnimationStateGraphTestFixtures.h"
#include "animation/AnimationRuntimeFixture.h"

#include <catch2/catch_approx.hpp>

namespace Horo::Animation {
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
        REQUIRE(fixture.Structural(std::move(destroy))->ResultValue());
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
        REQUIRE(fixture.forward->adapter->QueueTrigger(fixture.entity, Test::Id<GraphParameterId>(2)).HasValue());
        fixture.peer->failUpdate = true;
        REQUIRE(fixture.Tick().HasError());
        CHECK(fixture.PoseX() == 10.0F);
        REQUIRE(fixture.scheduler->RunFrame(fixture.lifecycle, fixture.token, false).HasValue());
        CHECK(fixture.PoseX() == 100.0F);
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.PoseX() == 110.0F);
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
