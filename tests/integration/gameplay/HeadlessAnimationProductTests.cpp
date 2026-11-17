#include "AllocationProbe.h"
#include "HeadlessAnimationProductFixture.h"

namespace Horo::Network::AnimationProduct {
#if HORO_TEST_PHYSICS_NATIVE
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
        Fixture fixture;
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
        CHECK(fixture.trace->Observe().component->currentPose.generation.Value() == 3);
    }

    TEST_CASE("Actual transport-qualified Headless listen pair aborts both real native Animation owners on later-world failure",
              "[animation][headless][product][listen][rollback]") {
        Fixture fixture(false, true);
        REQUIRE(fixture.Tick().HasValue());
        const auto first = fixture.trace->Observe();
        REQUIRE(first.component);
        const auto *client = fixture.trace->Owner({72});
        REQUIRE(client);
        CHECK(first.instance.owner.Value() == 1001);
        fixture.trace->expected = first.component->currentPose;
        fixture.trace->failScene = {72};
        fixture.trace->failAfterTick = true;
        const auto failed = fixture.Tick();
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == "test.headless.native_after_tick");
        CHECK(fixture.trace->observedOldPublication);
        CHECK(fixture.inputs->calls == 4);
        CHECK(fixture.trace->Owner({71}) == nullptr);
        CHECK(fixture.trace->Owner({72}) == nullptr);
        CHECK(fixture.host->RunFrame().HasError());
    }

    TEST_CASE("Actual Headless Scene facade retirement defers its native aggregate until exact fixed abort",
              "[animation][headless][product][shutdown][ownership]") {
        Fixture fixture;
        REQUIRE(fixture.Tick().HasValue());
        fixture.trace->expected = fixture.trace->Observe().component->currentPose;
        fixture.trace->closeAfterTick = true;
        const auto stopped = fixture.Tick();
        REQUIRE(stopped.HasError());
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
