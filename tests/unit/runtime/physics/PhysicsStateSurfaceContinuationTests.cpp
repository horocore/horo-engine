#include "PhysicsStateSurfaceBinding.h"
#include "PhysicsStateWorldTestSupport.h"

#include <algorithm>

namespace Horo::Physics::StateTest {
    namespace {
        PhysicsSurfaceWorldBinding SurfaceBinding(std::uint64_t epoch, std::uint64_t revision = 9) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = 19;
            const auto surface = SurfaceMaterialId::FromBytes(bytes);
            auto catalog = Take(SurfaceCatalogSnapshot::Create({"actual-native-state-surfaces", revision, {{surface, "Stone"}}}));
            return {std::make_shared<const SurfaceCatalogSnapshot>(std::move(catalog)), epoch, surface};
        }

        void AdvanceOtherWorld(PhysicsWorld &world, std::uint64_t tick) {
            Check(world.AdvanceFixedTick(
                {.simulationTick = tick, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)}));
        }

        void ConfigureReceiver(WorldFixture &fixture, PhysicsWorld &world, const WorldLayout &layout) {
            auto config = fixture.Configuration();
            config.bodies = layout.bodies;
            config.shapes = layout.shapes;
            Take(world.ConfigureStateSnapshots(config));
        }
    }  // namespace

    TEST_CASE("Real native World state survives local filter and surface epochs while live evidence stays owner-qualified",
              "[physics][native][state][surface][continuation]") {
        const auto sourceSurface = SurfaceBinding(5);
        WorldFixture fixture{StateWorldSettings(), PhysicsStateConstructionTracking::Required, sourceSurface, 3};
        static_cast<void>(fixture.Configure());
        const auto checkpoint = fixture.Capture();
        const auto targetSurface = SurfaceBinding(107);
        auto receiver = Take(fixture.runtime->PrepareWorld(fixture.world->Settings(), {fixture.schema, 83, targetSurface}));
        Check(receiver->Activate(Take(PhysicsWorldId::Create(82))));
        const auto layout = PopulateStateWorld(*receiver, targetSurface);
        for (std::uint64_t tick = 1; tick <= 5; ++tick)
            AdvanceOtherWorld(*receiver, tick);
        ConfigureReceiver(fixture, *receiver, layout);
        auto decoded = Take(receiver->DecodeStateSnapshot(checkpoint->Bytes()));
        auto pending = Take(receiver->PrepareStateRestore(decoded));
        Check(pending.ValidatePublication());
        pending.Publish();
        CHECK(fixture.world->SurfaceFingerprint() != receiver->SurfaceFingerprint());
        fixture.Advance();
        AdvanceOtherWorld(*receiver, 6);
        const auto expected = fixture.Capture();
        const auto actual = Take(receiver->CaptureState());
        CHECK(std::ranges::equal(expected->Bytes(), actual->Bytes()));
        const auto events = ReadTranscript(*receiver);
        REQUIRE(events.count != 0);
        for (const auto &event : std::span{events.events}.first(events.count)) {
            REQUIRE(event.firstSurface.has_value());
            REQUIRE(event.secondSurface.has_value());
            CHECK(event.firstSurface->mappingGeneration == 107);
            CHECK(event.secondSurface->mappingGeneration == 107);
        }
    }

    TEST_CASE("Real receiver rejects changed catalogue revision before state publication",
              "[physics][native][state][surface][compatibility]") {
        WorldFixture fixture{StateWorldSettings(), PhysicsStateConstructionTracking::Required, SurfaceBinding(5), 3};
        static_cast<void>(fixture.Configure());
        const auto checkpoint = fixture.Capture();
        auto receiver = Take(fixture.runtime->PrepareWorld(fixture.world->Settings(), {fixture.schema, 83, SurfaceBinding(107, 10)}));
        Check(receiver->Activate(Take(PhysicsWorldId::Create(83))));
        const auto layout = PopulateStateWorld(*receiver, SurfaceBinding(107, 10));
        for (std::uint64_t tick = 1; tick <= 5; ++tick)
            AdvanceOtherWorld(*receiver, tick);
        ConfigureReceiver(fixture, *receiver, layout);
        const auto before = receiver->PublishedTick();
        Test::RequireError(receiver->DecodeStateSnapshot(checkpoint->Bytes()), PhysicsErrors::ProfileUnsupported);
        CHECK(SamePublication(before, receiver->PublishedTick()));
    }
}  // namespace Horo::Physics::StateTest
