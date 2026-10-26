#include "UiPaintProgramTestSupport.h"
#include "support/AllocationProbe.h"

#include <thread>

using namespace Horo;
using namespace Horo::Render;
using namespace Horo::Tests::RenderFrontendTesting;
using namespace Horo::Tests::UiPaintProgramTesting;

TEST_CASE("UI program destruction retires native generations without allocating error copies", "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    const auto geometry = fixture.Extract();
    auto program = fixture.Create(geometry);
    const auto buffers = lifecycleState.createBufferCount;
    const auto textures = lifecycleState.createTextureCount - 1;
    REQUIRE(buffers > 0);
    REQUIRE(textures > 0);
    Horo::Tests::AllocationProbe::Measurement allocations;
    {
        Horo::Tests::AllocationProbe::ScopedFailure failure;
        Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
        program.reset();
        allocations = measurement.Snapshot();
    }
    CHECK(allocations.requests == 0);
    CHECK(lifecycleState.destroyBufferCount == buffers);
    CHECK(lifecycleState.destroyTextureCount == textures);
    CHECK(fixture.frontend->UploadSnapshot().pendingRequests == 0);
    CHECK(fixture.frontend->MemorySnapshot().reservationCount == 0);
    CHECK(fixture.frontend->MemorySnapshot().allocationCount == 1);
}

TEST_CASE("UI program destructor defers foreign-thread and active-frame resource retirement", "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    const auto geometry = fixture.Extract();
    auto program = fixture.Create(geometry);
    const auto buffers = lifecycleState.createBufferCount;
    const auto textures = lifecycleState.createTextureCount - 1;
    SECTION("A foreign destructor only requests retirement") {
        std::thread foreign([owned = std::move(program)]() mutable {
            owned.reset();
        });
        foreign.join();
        CHECK(lifecycleState.destroyBufferCount == 0);
        CHECK(lifecycleState.destroyTextureCount == 0);
    }
    SECTION("An active frame defers both native owners") {
        auto frame = fixture.frontend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}});
        REQUIRE(frame.HasValue());
        program.reset();
        CHECK(lifecycleState.destroyBufferCount == 0);
        CHECK(lifecycleState.destroyTextureCount == 0);
    }
    REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
    CHECK(lifecycleState.destroyBufferCount == buffers);
    CHECK(lifecycleState.destroyTextureCount == textures);
    CHECK(fixture.frontend->MemorySnapshot().reservationCount == 0);
    CHECK(fixture.frontend->MemorySnapshot().allocationCount == 1);
}
