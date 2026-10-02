#include "AllocationProbe.h"
#include "AudioStreamingTestFixture.h"

namespace Horo::Audio::StreamingTests {
    TEST_CASE("Streaming service cancellation joins package open and releases worker ownership", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdOpen.store(true);
        auto service = Service(jobs, fixture);
        auto admitted = service->Admit(Request());
        REQUIRE(admitted.HasValue());
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opening.load();
        }));
        REQUIRE(service->Stop(admitted.Value()).HasValue());
        REQUIRE(service->Retire(admitted.Value()).HasValue());
        CHECK(fixture.opens.load() == 0);
        CHECK(fixture.releases.load() == 0);
    }

    TEST_CASE("Streaming cancellation discards and releases a decoder returned by a late open", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdOpen.store(true);
        fixture.ignoreCancellation.store(true);
        fixture.openAfterCancellation = true;
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opening.load();
        }));
        CHECK(service->Stop(handle).HasValue());
        fixture.holdOpen.store(false);
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.opens.load() == 1);
        CHECK(fixture.releases.load() == 1);
        CHECK(fixture.decodes.load() == 0);
    }

    TEST_CASE("Streaming stop cancels a published decoder blocked on its private flag", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdFirstDecode.store(true);
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        service->Pump();
        REQUIRE(Until([&fixture] {
            return fixture.firstDecodeEntered.load();
        }));
        REQUIRE(service->Stop(handle).HasValue());
        CallbackBlock output;
        CHECK(output.Render(port, 4).silentFrames == 4);
        REQUIRE(Until([&fixture] {
            return fixture.privateCancellationObserved.load();
        }));
        REQUIRE(service->Retire(handle).HasValue());
        CHECK_FALSE(fixture.privateCancellationTimedOut.load());
        CHECK(fixture.decodes.load() == 1);
        CHECK(fixture.releases.load() == 1);
    }

    TEST_CASE("Streaming concurrent late open and stop cannot strand a private decoder cancellation", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        for (std::uint32_t iteration = 0; iteration < 32; ++iteration) {
            PackageFixture fixture;
            fixture.holdOpen.store(true);
            fixture.ignoreCancellation.store(true);
            fixture.openAfterCancellation = true;
            fixture.holdFirstDecode.store(true);
            auto service = Service(jobs, fixture);
            const auto handle = service->Admit(Request()).Value();
            service->Pump();
            REQUIRE(Until([&fixture] {
                return fixture.opening.load();
            }));
            std::atomic<bool> race{};
            std::jthread opener([&fixture, &race] {
                while (!race.load())
                    std::this_thread::yield();
                fixture.holdOpen.store(false);
            });
            race.store(true);
            REQUIRE(service->Stop(handle).HasValue());
            opener.join();
            REQUIRE(service->Retire(handle).HasValue());
            CHECK_FALSE(fixture.privateCancellationTimedOut.load());
            CHECK(fixture.releases.load() == 1);
            CHECK((fixture.decodes.load() == 0 || fixture.privateCancellationObserved.load()));
        }
    }

    TEST_CASE("Streaming shutdown timeout retains storage and closes new admission until retry", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdOpen.store(true);
        fixture.ignoreCancellation.store(true);
        auto service = Service(jobs, fixture, {2, 1, 1U << 20U, 1});
        const auto handle = service->Admit(Request()).Value();
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opening.load();
        }));
        const auto first = service->Shutdown();
        CHECK(first.HasError());
        CHECK(service->Snapshot(handle).HasValue());
        CHECK(service->Admit(Request(2)).HasError());
        fixture.holdOpen.store(false);
        REQUIRE(Until([&] {
            return service->Shutdown().HasValue();
        }));
        CHECK(service->Snapshot(handle).HasError());
        CHECK(service->Shutdown().HasValue());
    }

    TEST_CASE("Streaming starvation recovers and cancellation interrupts an active decoder before release", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdOpen.store(true);
        fixture.holdFinalDecode.store(true);
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opening.load();
        }));
        CallbackBlock output;
        CHECK(output.Render(port, 4).silentFrames == 4);
        fixture.holdOpen.store(false);
        REQUIRE(PumpUntil(*service, handle, 4));
        CHECK(output.Render(port, 4).availableFrames == 4);
        CHECK(output.left[0] == 1.0F);
        CHECK(output.right[3] == 4.0F);
        REQUIRE(PumpUntil(*service, handle, 2));
        REQUIRE(Until([&] {
            service->Pump();
            return fixture.finalDecodeEntered.load();
        }));
        CHECK(fixture.releases.load() == 0);
        REQUIRE(service->Stop(handle).HasValue());
        CHECK(output.Render(port, 4).silentFrames == 4);
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.releases.load() == 1);
    }

}  // namespace Horo::Audio::StreamingTests
