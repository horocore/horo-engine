#include "CanonicalPhysicsStateCloneInternal.h"
#include "PhysicsStateNativeTestSupport.h"

namespace Horo::Physics::Detail {
    using namespace StateTest;

    namespace {
        struct Frame final {
            std::vector<std::byte> bytes = std::vector<std::byte>(1024 * 1024);
            std::size_t used{};
        };

        void CaptureFrame(Fixture &fixture, CanonicalWorldHandle world, Frame &frame) {
            const auto state = Take(CaptureCanonicalPhysicsState(world, fixture.settings, fixture.bindings.View(), *fixture.latest,
                                                                 {fixture.temporary, fixture.nativeBytes, fixture.active}));
            frame.used =
                Take(StateDetail::EncodeSolverState(state, fixture.latest->limits, fixture.latest->activeBodyScratch, frame.bytes));
        }

        void Advance(Fixture &fixture, CanonicalWorldHandle world, std::uint64_t firstTick, std::span<Frame> frames) {
            for (std::size_t index = 0; index < frames.size(); ++index) {
                auto stepped = StepCanonicalWorld(world, 1.0F / 60.0F, firstTick + index);
                INFO((stepped.HasError() ? stepped.ErrorValue().message : std::string{"success"}));
                REQUIRE(stepped.HasValue());
                CaptureFrame(fixture, world, frames[index]);
            }
        }
    }  // namespace

    TEST_CASE("Real rotated compound Physics restore preserves every subsequent "
              "solver tick",
              "[physics][native][state][continuation]") {
        Fixture fixture{Profile::RotatedCompound};
        const auto initial = fixture.Capture(*fixture.captured);
        std::array<Frame, 8> expected, actual;
        Advance(fixture, fixture.world.handle, 31, expected);
        const WorldOwner candidate{Take(CloneCanonicalPhysicsStateTopology(fixture.world.handle, fixture.settings))};
        Take(PrepareCanonicalPhysicsStateRestore(fixture.world.handle, candidate.handle, fixture.settings, fixture.bindings.View(), initial,
                                                 fixture.Native().continuousCollision.policy, fixture.Scratch()));
        Advance(fixture, candidate.handle, 31, actual);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            INFO(index);
            REQUIRE(actual[index].used == expected[index].used);
            CHECK(std::ranges::equal(std::span{actual[index].bytes}.first(actual[index].used),
                                     std::span{expected[index].bytes}.first(expected[index].used)));
        }
    }
}  // namespace Horo::Physics::Detail
