#include "AllocationProbe.h"
#include "HeadlessAnimationProductFixture.h"
#include "HeadlessReplicaAnimationInputs.h"
#include "Horo/Network/SceneReplicationApplyOwner.h"
#include "ReplicationStateCodecTestSupport.h"

#include <catch2/generators/catch_generators.hpp>

namespace Horo::Network::AnimationProduct {
    TEST_CASE("Replica animation consumes only committed public fields for its exact Scene actor",
              "[animation][headless][product][replica-inputs]") {
        namespace Rep = StateCodecTestSupport;
        using App::HeadlessNetworkDetail::ReplicaAnimationInputs;
        Rep::CodecFixture fixture;
        auto mapping = Rep::Object();
        const Runtime::SceneObjectId authored{1};
        mapping.provenance.authoredObject = authored;
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, {1}};
        Runtime::RuntimeEntityDefinition definition;
        definition.object = authored;
        builder.Add(definition);
        std::shared_ptr<Runtime::RuntimeScene> scene =
            Take(Runtime::RuntimeScene::Create(Take(std::move(builder).Build()), mapping.entity.runtime));
        REQUIRE(scene->View().Find(authored) == mapping.entity);
        const auto field = Rep::FieldIdValue(1);
        const bool velocity = GENERATE(false, true);
        const auto property = velocity ? SceneReplicationProperty::LinearVelocityX : SceneReplicationProperty::TranslationX;
        const std::array sceneBindings{SceneReplicationFieldBinding{field, property}};
        const auto parameter = Take(Animation::GraphParameterId::Create(731));
        const std::array bindings{App::HeadlessReplicaAnimationBinding{authored, field, parameter}};
        const auto inputs = std::make_shared<ReplicaAnimationInputs>(bindings);
        const auto sceneWriter =
            Take(SceneReplicationApplyOwner::Create(scene, mapping.provenance.schema, mapping.provenance.schemaVersion, sceneBindings));
        const auto writer = ReplicaAnimationInputs::Bind(inputs, sceneWriter);
        CancellationSource world;
        const CancellationToken live;
        const Runtime::FixedStepContext tick{.cancellation = live};
        const auto prepare = [&](ReplicationDecodedState decoded) {
            const std::array updates{ReplicationApplyUpdate{mapping, Rep::Recipient(), std::move(decoded), live, world.Token()}};
            return Take(writer->Prepare(updates));
        };
        const auto read = [&] {
            const auto snapshot = inputs->Read(mapping.entity, tick);
            REQUIRE(snapshot.parameters.size() == 1);
            CHECK(snapshot.parameters.front().id == parameter);
            return std::get<float>(snapshot.parameters.front().value);
        };
        auto initial = prepare(Take(fixture.codec->Decode(fixture.full, mapping)));
        CHECK(inputs->Read(mapping.entity, tick).parameters.empty());
        REQUIRE(initial->Commit(live).HasValue());
        CHECK(read() == 1.0F);
        CHECK(Take(scene->View().Get(mapping.entity)).localTransform->translation.x == (velocity ? 0.0F : 1.0F));
        CHECK(inputs->Read({Runtime::SceneRuntimeId{90}, mapping.entity.entity}, tick).parameters.empty());

        fixture.capture.Capture(2, 2.0);
        auto next = prepare(Take(fixture.codec->Decode(Take(fixture.codec->Encode(fixture.capture.Pin())), mapping)));
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        REQUIRE(next->Commit(cancelled.Token()).HasError());
        CHECK(read() == 1.0F);
        auto retry = prepare(Take(fixture.codec->Decode(Take(fixture.codec->Encode(fixture.capture.Pin())), mapping)));
        REQUIRE(retry->Commit(live).HasValue());
        CHECK(read() == 2.0F);
        CHECK(Take(scene->View().Get(mapping.entity)).localTransform->translation.x == (velocity ? 0.0F : 2.0F));
        world.RequestCancellation();
        CHECK(inputs->Read(mapping.entity, tick).parameters.empty());
    }

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Actual Headless product resolves cooked root motion without a gameplay command",
              "[animation][headless][product][root-motion]") {
        const bool listenPair = GENERATE(false, true);
        Fixture fixture(true, listenPair, {}, true);
        CHECK_FALSE(fixture.trace->Observe().rootMotion);
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.trace->Observe().rootMotion;
        REQUIRE(first);
        CHECK(first->disposition == Runtime::SceneRootMotionDisposition::Consumed);
        CHECK(first->request.id.tick.Value() == 1);
        CHECK(std::abs(first->applied.translation.x - 0.2F) < 0.00001F);
        CHECK(std::abs(first->remaining.translation.x) < 0.00001F);
        const auto unbound = App::InspectHeadlessAnimationEntity(*fixture.trace->entries[0].service, {4});
        REQUIRE(unbound);
        const auto unavailable = Take(fixture.trace->Owner()->Query(*unbound)).rootMotion;
        REQUIRE(unavailable);
        CHECK(unavailable->disposition == Runtime::SceneRootMotionDisposition::Unavailable);
        REQUIRE(fixture.Tick().HasValue());
        const auto second = fixture.trace->Observe().rootMotion;
        REQUIRE(second);
        CHECK(second->request.id.tick.Value() == 2);
        CHECK(second->request.id.generation > first->request.id.generation);
        CHECK(second->disposition == Runtime::SceneRootMotionDisposition::Consumed);
        if (listenPair) {
            const Runtime::SceneRuntimeId client{72};
            const auto *replica = fixture.trace->Owner(client);
            REQUIRE(replica);
            const auto observed = Take(replica->Query(fixture.trace->Entity(client)));
            REQUIRE(observed.component);
            CHECK_FALSE(observed.rootMotion);
        }
    }

    TEST_CASE("Actual configured Headless product commits real cooked Graph or Clip pose once after native fixed work",
              "[animation][headless][product][canonical]") {
        bool clip{};
        SECTION("real Graph source") {}
        SECTION("real direct Clip source") {
            clip = true;
        }
        Fixture fixture(clip);
        REQUIRE_FALSE(fixture.trace->Observe().component);
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.trace->Observe();
        REQUIRE(first.component);
        CHECK(first.component->currentPose.generation.Value() == 1);
        CHECK(first.component->previousPose == first.component->currentPose);
        CHECK(first.instance.owner.Value() == 1001);
        std::array<Math::Transform, 1> pose;
        REQUIRE(fixture.trace->Owner()->CopyPose(first.entity, first.component->currentPose, pose).HasValue());
        CHECK(std::abs(pose[0].translation.x - 1.6666667F) < 0.00001F);  // 100 units over one real second, one unchanged host quantum.
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.trace->Observe().component->currentPose.generation.Value() == 2);
        CHECK(fixture.inputs->calls == 2);  // Two actual authored actors, each visited exactly once.
    }

    TEST_CASE("Actual Headless product error aborts partially evaluated actors and terminalizes without partial publication",
              "[animation][headless][product][rollback]") {
        Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        const auto before = fixture.trace->Observe();
        fixture.trace->expected = before.component->currentPose;
        bool injectedNative{};
        SECTION("real second Animation actor rejects its actual typed snapshot") {
            fixture.inputs->badAt = 2;
        }
        SECTION("later real native callback completed then its surrounding service reports failure") {
            fixture.trace->failAfterTick = true;
            injectedNative = true;
        }
        const auto failed = fixture.Tick();
        REQUIRE(failed.HasError());
        INFO(failed.ErrorValue().code.Value() << ": " << failed.ErrorValue().message);
        if (injectedNative)
            CHECK(failed.ErrorValue().code.Value() == "test.headless.native_after_tick");
        else
            CHECK(failed.ErrorValue().domain.Value().find("animation") != std::string::npos);
        CHECK(fixture.trace->observedOldPublication);
        CHECK(fixture.trace->Owner() == nullptr);
        CHECK(fixture.host->RunFrame().HasError());
        CHECK(fixture.host->Startup().HasError());
    }

    TEST_CASE("Actual Headless callback cancellation aborts global fence before retirement and never revives stopped owners",
              "[animation][headless][product][cancel][retire]") {
        Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        fixture.trace->expected = fixture.trace->Observe().component->currentPose;
        fixture.trace->cancelAfterTick = true;
        CHECK(fixture.Tick().HasError());
        CHECK(fixture.trace->observedOldPublication);
        CHECK(fixture.trace->Owner() == nullptr);
        fixture.host->Shutdown();
        fixture.host->Shutdown();
        CHECK(fixture.host->RunFrame().HasError());
    }

    TEST_CASE("Actual Headless travel preserves old Animation on rejected load and publishes a fresh shared-issuer owner",
              "[animation][headless][product][travel]") {
        Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.trace->Observe();
        const std::array replacement{World(NetworkModeWorldKind::Standalone, 72)};
        const auto deadline = fixture.load.deadline;
        CancellationSource rejectedLoad;
        rejectedLoad.RequestCancellation();
        fixture.load = {rejectedLoad.Token(), deadline};
        REQUIRE(fixture.host->RequestTravel(replacement).HasValue());
        REQUIRE(fixture.host->RunFrame().HasValue());
        const auto failure = fixture.host->TakeTravelFailure();
        REQUIRE(failure);
        CHECK(fixture.trace->Observe().instance == first.instance);
        CHECK(fixture.trace->Observe().component->currentPose == first.component->currentPose);
        fixture.load = {fixture.lifetime.Token(), deadline};
        REQUIRE(fixture.host->RequestTravel(replacement).HasValue());
        REQUIRE(fixture.host->RunFrame().HasValue());
        CHECK_FALSE(fixture.host->TakeTravelFailure());
        const auto current = fixture.trace->Observe();
        CHECK(current.instance.owner != first.instance.owner);
        CHECK(current.instance.owner.Value() > first.instance.owner.Value());
        CHECK_FALSE(current.component);  // Loading/travel did not invent a fixed pose or run Evaluate.
        CHECK(fixture.trace->Owner()->Query(first.entity).HasError());
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.trace->Observe().component->currentPose.generation.Value() == 1);
        CHECK(fixture.load.deadline == deadline);
    }

    TEST_CASE("Actual Headless cold binding rejects repeated foreign and retired sources before service preparation",
              "[animation][headless][product][binding]") {
        Fixture fixture;
        DeterministicClock clock;
        auto another = Take(Runtime::RuntimeHost::Create(clock));
        CHECK(fixture.retainedFactory(another->DispatchSource()).HasError());
        const auto retired = another->DispatchSource();
        another->Shutdown();
        // This second composition exercises cold dispatch admission only; it never prepares native worlds.
        // Keep the running canonical fixture as the sole owner of Jolt process globals.
        auto freshComposition = fixture.Compose(Physics::PhysicsRuntimeMode::Null);
        bool wrongAccepted{};
        std::thread([factory = freshComposition.fixedParticipant, retired, &wrongAccepted] {
            wrongAccepted = factory(retired).HasValue();
        }).join();
        CHECK_FALSE(wrongAccepted);
        CHECK(freshComposition.fixedParticipant(retired).HasError());
        auto active = Take(Runtime::RuntimeHost::Create(clock));
        auto participant = Take(freshComposition.fixedParticipant(active->DispatchSource()));
        REQUIRE(participant);
        CHECK(freshComposition.fixedParticipant(active->DispatchSource()).HasError());
        active->Shutdown();
        CHECK(participant->Startup({}).HasError());  // The authentic issuer retired between cold creation and Startup.
        participant->Shutdown();
        CHECK(participant->Startup({}).HasError());
    }

    TEST_CASE("Actual Headless successful steady native Scene and Animation fixed frame performs no allocation",
              "[animation][headless][product][allocation]") {
        const bool rootMotion = GENERATE(false, true);
        Fixture fixture(rootMotion, false, {}, rootMotion);
        REQUIRE(fixture.Tick().HasValue());
        REQUIRE(fixture.Tick().HasValue());
        auto result = Result<void>::Success();
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement scope;
            result = fixture.Tick();
            measured = scope.Snapshot();
        }
        REQUIRE(result.HasValue());
        CHECK(measured.requests == 0);
        const auto observed = fixture.trace->Observe();
        CHECK(observed.component->currentPose.generation.Value() == 3);
        CHECK(observed.rootMotion.has_value() == rootMotion);
    }

    TEST_CASE("Actual transport-qualified Headless listen pair aborts both real native Animation owners on later-world failure",
              "[animation][headless][product][listen][rollback]") {
        Fixture fixture(false, true);
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.trace->Observe();
        REQUIRE(first.component);
        const auto *client = fixture.trace->Owner({72});
        REQUIRE(client);
        REQUIRE(Take(client->Query(fixture.trace->Entity({72}))).component);
        CHECK(first.instance.owner.Value() == 1001);
        fixture.trace->expected = first.component->currentPose;
        fixture.trace->failScene = {72};
        fixture.trace->failAfterTick = true;
        const auto failed = fixture.Tick();
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == "test.headless.native_after_tick");
        CHECK(fixture.trace->observedOldPublication);
        CHECK(fixture.inputs->calls == 2);
        CHECK(fixture.trace->Owner({71}) == nullptr);
        CHECK(fixture.trace->Owner({72}) == nullptr);
        CHECK(fixture.host->RunFrame().HasError());
    }

    TEST_CASE("Connected animation reads only its explicit presentation source in a real listen pair",
              "[animation][headless][product][listen][presentation]") {
        const auto publicInputs = std::make_shared<Inputs>();
        publicInputs->playbackRate = 500'000;
        Fixture fixture(false, true, publicInputs);
        REQUIRE(fixture.Tick().HasValue());
        const auto server = fixture.trace->Observe();
        REQUIRE(server.component);
        const auto *client = fixture.trace->Owner({72});
        REQUIRE(client);
        const auto replica = Take(client->Query(fixture.trace->Entity({72})));
        REQUIRE(replica.component);
        CHECK(server.component->domain == Animation::AnimationEvaluationDomain::AuthoritativeSimulation);
        CHECK(replica.component->domain == Animation::AnimationEvaluationDomain::Presentation);

        CHECK(fixture.inputs->calls == 2);
        CHECK(fixture.inputs->lastScene == Runtime::SceneRuntimeId{71});
        CHECK(publicInputs->calls == 2);
        CHECK(publicInputs->lastScene == Runtime::SceneRuntimeId{72});
        std::array<Math::Transform, 1> authorityPose;
        std::array<Math::Transform, 1> replicaPose;
        REQUIRE(fixture.trace->Owner()->CopyPose(server.entity, server.component->currentPose, authorityPose).HasValue());
        REQUIRE(client->CopyPose(replica.entity, replica.component->currentPose, replicaPose).HasValue());
        CHECK(std::abs(replicaPose[0].translation.x * 2.0F - authorityPose[0].translation.x) < 0.00001F);
    }

    TEST_CASE("Actual Headless Scene facade retirement defers its native aggregate until exact fixed abort",
              "[animation][headless][product][shutdown][ownership]") {
        Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        fixture.trace->expected = fixture.trace->Observe().component->currentPose;
        fixture.trace->closeAfterTick = true;
        const auto stopped = fixture.Tick();
        REQUIRE(stopped.HasError());
        CHECK(fixture.trace->closeRevokedDiscovery);
        CHECK(fixture.trace->closeDeferred);
        CHECK(fixture.trace->observedOldPublication);
        CHECK(fixture.trace->Owner() == nullptr);
        CHECK(fixture.host->RunFrame().HasError());
    }

    TEST_CASE("Actual Headless consecutive fixed catchup ticks retain record pins and publish each genuine attempt once",
              "[animation][headless][product][catchup]") {
        Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        fixture.inputs->calls = 0;
        fixture.clock.Advance(Duration::FromNanoseconds(33'333'334));
        REQUIRE(fixture.host->RunFrame().HasValue());
        CHECK(fixture.trace->Observe().component->currentPose.generation.Value() == 3);
        CHECK(fixture.inputs->calls == 4);
    }

    TEST_CASE("Actual Headless Scene close after Graph arm cannot skip a scheduler-committed publication",
              "[animation][headless][product][commit][shutdown]") {
        Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        fixture.trace->closeAtPrepare = true;
        fixture.trace->observeCommitted = true;
        REQUIRE(fixture.Tick().HasValue());
        REQUIRE(fixture.trace->observedCommitted);
        CHECK(fixture.trace->observedCommitted->generation.Value() == 2);
        CHECK(fixture.Tick().HasError());
        CHECK(fixture.trace->Owner() == nullptr);
    }
#else
    TEST_CASE("Actual Headless Animation native product qualification requires the canonical backend", "[animation][headless][product]") {
        SKIP("Canonical Physics is not installed; no actual native product acceptance is claimed.");
    }
#endif
}  // namespace Horo::Network::AnimationProduct
