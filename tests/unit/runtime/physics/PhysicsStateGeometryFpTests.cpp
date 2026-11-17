#include "CanonicalPhysicsStateFpControl.h"
#include "PhysicsStateWorldTestSupport.h"

#include <algorithm>
#include <cfenv>
#include <exception>

namespace Horo::Physics::StateTest {
    namespace {
        class ScopedRounding final {
        public:
            ScopedRounding() noexcept {
                saved_ = std::fegetenv(&previous_) == 0;
                const auto alternate = std::fegetround() == FE_UPWARD ? FE_DOWNWARD : FE_UPWARD;
                changed_ = saved_ && std::fesetround(alternate) == 0;
            }

            ~ScopedRounding() {
                if (saved_ && std::fesetenv(&previous_) != 0)
                    std::terminate();
            }

            bool Changed() const noexcept {
                return changed_;
            }

        private:
            std::fenv_t previous_{};
            bool saved_{}, changed_{};
        };

        std::unique_ptr<PhysicsWorld> EmptyWorld(WorldFixture &fixture, std::uint64_t id) {
            auto world = Take(fixture.runtime->PrepareWorld(fixture.world->Settings(), {fixture.schema, 1, {}}));
            Check(world->Activate(Take(PhysicsWorldId::Create(id))));
            return world;
        }

        void AdvanceOther(PhysicsWorld &world) {
            for (std::uint64_t tick = 1; tick <= 5; ++tick)
                Check(world.AdvanceFixedTick(
                    {.simulationTick = tick, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)}));
        }

        Result<std::size_t> ConfigureOther(WorldFixture &fixture, PhysicsWorld &world, const WorldLayout &layout) {
            auto configuration = fixture.Configuration();
            configuration.bodies = layout.bodies;
            configuration.shapes = layout.shapes;
            return world.ConfigureStateSnapshots(configuration);
        }
    }  // namespace

    TEST_CASE("Optional geometry preserves ordinary construction but incompatible original FP cannot admit complete state",
              "[physics][native][state][geometry][fp]") {
        WorldFixture fixture{StateWorldSettings(), PhysicsStateConstructionTracking::Optional};
        static_cast<void>(fixture.Configure());
        auto source = fixture.Capture();
        const auto original = Take(Detail::ReadCanonicalPhysicsStateFpControl());
        auto other = EmptyWorld(fixture, 71);
        WorldLayout layout;
        {
            ScopedRounding rounding;
            REQUIRE(rounding.Changed());
            CHECK(Take(Detail::ReadCanonicalPhysicsStateFpControl()) != original);
            layout = PopulateStateWorld(*other);
        }
        REQUIRE(Take(Detail::ReadCanonicalPhysicsStateFpControl()) == original);
        AdvanceOther(*other);
        const auto before = other->PublishedTick();
        Test::RequireError(ConfigureOther(fixture, *other, layout), PhysicsErrors::OperationUnsupported);
        CHECK(SamePublication(other->PublishedTick(), before));
    }

    TEST_CASE("Real unchanged construction arithmetic admits the same Physics state in another exact-topology owner",
              "[physics][native][state][geometry][fp]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto source = fixture.Capture();
        auto other = EmptyWorld(fixture, 72);
        const auto layout = PopulateStateWorld(*other);
        AdvanceOther(*other);
        Take(ConfigureOther(fixture, *other, layout));
        auto decoded = Take(other->DecodeStateSnapshot(source->Bytes()));
        auto candidate = Take(other->PrepareStateRestore(decoded));
        Check(candidate.ValidatePublication());
        candidate.Publish();
        const auto restored = Take(other->CaptureState());
        CHECK(std::ranges::equal(restored->Bytes(), source->Bytes()));
    }
}  // namespace Horo::Physics::StateTest
