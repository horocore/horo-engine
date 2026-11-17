#include "AndroidWindowBridgeTestFixture.h"

namespace {
    using namespace Horo::Tests::AndroidWindow;

    TEST_CASE("Android window bridge rejects no-presentation intent before native acquisition", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge(AndroidWindowSelection::NoPresentation);
        const auto admitted = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(admitted.HasError());
        CHECK(admitted.ErrorValue().code.Value() == "android_window.no_presentation");
        CHECK(fixture.native.acquireCount == 0);
        const auto state = fixture.State();
        CHECK(state.process == AndroidProcessState::Running);
        CHECK_FALSE(state.hasWindow);
        CHECK_FALSE(state.hasPresentation);
        CHECK(state.queuedObservationCount == 0);
    }

    TEST_CASE("Android window bridge rejects unsupported selection without fallback", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge(AndroidWindowSelection::Unsupported);
        const auto admitted = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(admitted.HasError());
        CHECK(admitted.ErrorValue().code.Value() == "android_window.unsupported_backend");
        CHECK(fixture.native.acquireCount == 0);
        CHECK(fixture.native.releaseCount == 0);
        CHECK_FALSE(fixture.State().hasWindow);
        CHECK_FALSE(fixture.State().hasPresentation);
    }

    TEST_CASE("Android window candidate retains before enqueue and commits only its issued generation",
              "[unit][platform][android][window]") {
        Fixture fixture;
        {
            auto bridge = fixture.Bridge();
            const auto admitted = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
            REQUIRE(admitted.HasValue());
            REQUIRE(fixture.native.acquireCount == 1);
            CHECK(fixture.native.acquired[0] == fixture.native.First());
            CHECK(admitted.Value().value != 0);
            CHECK_FALSE(fixture.State().hasWindow);
            CHECK(fixture.State().queuedObservationCount == 1);
            REQUIRE(bridge.Drain().HasValue());
            CHECK(fixture.State().hasWindow);
            CHECK(fixture.State().windowGeneration == admitted.Value());
            CHECK_FALSE(fixture.State().hasPresentation);
            CHECK(fixture.native.releaseCount == 0);
        }
        CHECK(fixture.native.releaseCount == 1);
        CHECK(fixture.native.released[0] == fixture.native.First());
    }

    TEST_CASE("Android native acquisition failure never queues or publishes a window", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();
        fixture.native.failAcquire = true;
        const auto rejected = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "android_window.acquire_failed");
        CHECK(fixture.native.acquireCount == 0);
        CHECK(fixture.native.releaseCount == 0);
        CHECK(fixture.State().queuedObservationCount == 0);
        REQUIRE(bridge.Drain().HasValue());
        CHECK_FALSE(fixture.State().hasWindow);
        CHECK_FALSE(fixture.State().hasPresentation);
    }

    TEST_CASE("Android saturated queue rolls back only the candidate native reference", "[unit][platform][android][window]") {
        Fixture fixture(1);
        auto bridge = fixture.Bridge();
        const auto first = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(first.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        REQUIRE(fixture.adapter.OnActivityResumed(fixture.activity).HasValue());
        const auto rejected = bridge.OnWindowAvailable(fixture.activity, fixture.native.Second());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "android_lifecycle.queue_saturated");
        CHECK(fixture.native.acquireCount == 2);
        REQUIRE(fixture.native.releaseCount == 1);
        CHECK(fixture.native.released[0] == fixture.native.Second());
        CHECK(fixture.State().windowGeneration == first.Value());
        CHECK(fixture.State().hasWindow);
        REQUIRE(bridge.Drain().HasValue());
        CHECK(fixture.native.releaseCount == 1);
        CHECK(fixture.State().windowGeneration == first.Value());
    }

    TEST_CASE("Android duplicate native observation reuses one pending and committed generation", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();
        const auto first = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(first.HasValue());
        const auto pendingDuplicate = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(pendingDuplicate.HasValue());
        CHECK(pendingDuplicate.Value() == first.Value());
        CHECK(fixture.native.acquireCount == 1);
        CHECK(fixture.State().queuedObservationCount == 1);
        REQUIRE(bridge.Drain().HasValue());
        const auto committedDuplicate = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(committedDuplicate.HasValue());
        CHECK(committedDuplicate.Value() == first.Value());
        CHECK(fixture.native.acquireCount == 1);
        CHECK(fixture.State().queuedObservationCount == 0);
    }

    TEST_CASE("Android same-address replacement survives late loss of the retired generation", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();
        const auto old = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(old.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        REQUIRE(bridge.OnWindowLost(old.Value()).HasValue());
        const auto replacement = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value() > old.Value());
        CHECK(fixture.native.acquireCount == 2);
        CHECK(fixture.native.releaseCount == 0);
        REQUIRE(bridge.Drain().HasValue());
        CHECK(fixture.native.releaseCount == 1);
        REQUIRE(bridge.OnWindowLost(old.Value()).HasValue());
        const auto staleLoss = bridge.Drain();
        REQUIRE(staleLoss.HasError());
        CHECK(staleLoss.ErrorValue().code.Value() == "android_lifecycle.stale_generation");
        CHECK(fixture.State().hasWindow);
        CHECK(fixture.State().windowGeneration == replacement.Value());
        CHECK(fixture.native.releaseCount == 1);
    }

    TEST_CASE("Android committed window lease pins a lost generation until the native consumer retires",
              "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();
        const auto generation = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(generation.HasValue());
        CHECK(bridge.AcquireWindow(generation.Value()).HasError());
        REQUIRE(bridge.Drain().HasValue());
        auto acquired = bridge.AcquireWindow(generation.Value());
        REQUIRE(acquired.HasValue());
        auto lease = std::move(acquired).Value();
        CHECK(lease.Get() == fixture.native.First());
        REQUIRE(bridge.OnWindowLost(generation.Value()).HasValue());
        CHECK(bridge.AcquireWindow(generation.Value()).HasError());
        REQUIRE(bridge.Drain().HasValue());
        CHECK_FALSE(fixture.State().hasWindow);
        CHECK(fixture.native.releaseCount == 0);
        CHECK(lease.Get() == fixture.native.First());
        AndroidWindowLease moved(std::move(lease));
        CHECK(lease.Get() == nullptr);
        moved.Reset();
        CHECK(moved.Get() == nullptr);
        CHECK(fixture.native.releaseCount == 1);
        moved.Reset();
        CHECK(fixture.native.releaseCount == 1);
    }

    TEST_CASE("Android native callback reentry cannot drain an in-progress retention transaction", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();

        struct Probe {
            AndroidWindowBridge &bridge;
            bool rejected{};
        } probe{bridge};

        fixture.native.hookContext = &probe;
        fixture.native.onAcquire = [](void *context) noexcept {
            auto &state = *static_cast<Probe *>(context);
            const auto drained = state.bridge.Drain();
            state.rejected = drained.HasError() && drained.ErrorValue().code.Value() == "android_window.operation_in_progress";
        };
        const auto admitted = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(admitted.HasValue());
        CHECK(probe.rejected);
        CHECK_FALSE(fixture.State().hasWindow);
        REQUIRE(bridge.Drain().HasValue());
        CHECK(fixture.State().hasWindow);
        CHECK(fixture.native.acquireCount == 1);
    }

    TEST_CASE("Android window shutdown closes callbacks before waiting for a retained consumer", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();
        const auto generation = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(generation.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        auto acquired = bridge.AcquireWindow(generation.Value());
        REQUIRE(acquired.HasValue());
        auto lease = std::move(acquired).Value();
        const auto pending = bridge.Shutdown();
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(fixture.State().process == AndroidProcessState::ShuttingDown);
        CHECK_FALSE(fixture.State().hasWindow);
        CHECK(fixture.native.releaseCount == 0);
        const auto late = bridge.OnWindowAvailable(fixture.activity, fixture.native.Second());
        REQUIRE(late.HasError());
        CHECK(late.ErrorValue().code.Value() == "android_lifecycle.admission_closed");
        CHECK(fixture.adapter.OnActivityPaused(fixture.activity).HasError());
        CHECK(bridge.AcquireWindow(generation.Value()).HasError());
        lease.Reset();
        CHECK(fixture.native.releaseCount == 1);
        const auto complete = bridge.Shutdown();
        REQUIRE(complete.HasValue());
        CHECK(complete.Value());
        REQUIRE(fixture.controller->CompleteShutdown().HasValue());
        CHECK(fixture.State().process == AndroidProcessState::Stopped);
        REQUIRE(bridge.Shutdown().HasValue());
        CHECK(fixture.native.releaseCount == 1);
    }

    TEST_CASE("Android lost window closes new leases even when its queue handoff is saturated", "[unit][platform][android][window]") {
        Fixture fixture(1);
        auto bridge = fixture.Bridge();
        const auto generation = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(generation.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        REQUIRE(fixture.adapter.OnActivityResumed(fixture.activity).HasValue());
        const auto saturated = bridge.OnWindowLost(generation.Value());
        REQUIRE(saturated.HasError());
        CHECK(saturated.ErrorValue().code.Value() == "android_lifecycle.queue_saturated");
        CHECK(bridge.AcquireWindow(generation.Value()).HasError());
        REQUIRE(bridge.Drain().HasValue());
        CHECK(bridge.AcquireWindow(generation.Value()).HasError());
        CHECK(fixture.native.releaseCount == 0);
        REQUIRE(bridge.OnWindowLost(generation.Value()).HasValue());
        REQUIRE(bridge.Drain().HasValue());
        CHECK_FALSE(fixture.State().hasWindow);
        CHECK(fixture.native.releaseCount == 1);
    }
}  // namespace
