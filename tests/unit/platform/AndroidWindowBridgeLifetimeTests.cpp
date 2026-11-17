#include "AndroidWindowBridgeTestFixture.h"

namespace {
    using namespace Horo::Tests::AndroidWindow;

    TEST_CASE("Android late lease release cannot retire a same-address replacement", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();
        const auto old = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(old.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        auto oldResult = bridge.AcquireWindow(old.Value());
        REQUIRE(oldResult.HasValue());
        auto oldLease = std::move(oldResult).Value();
        REQUIRE(bridge.OnWindowLost(old.Value()).HasValue());
        const auto replacement = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(replacement.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        auto newResult = bridge.AcquireWindow(replacement.Value());
        REQUIRE(newResult.HasValue());
        auto newLease = std::move(newResult).Value();
        CHECK(fixture.native.releaseCount == 0);
        CHECK(bridge.AcquireWindow(replacement.Value()).HasError());
        oldLease = std::move(newLease);
        CHECK(newLease.Get() == nullptr);
        CHECK(oldLease.Get() == fixture.native.First());
        CHECK(fixture.native.releaseCount == 1);
        CHECK(fixture.State().windowGeneration == replacement.Value());
        CHECK(fixture.State().hasWindow);
        oldLease.Reset();
        CHECK(fixture.native.releaseCount == 1);
        const auto shutdown = bridge.Shutdown();
        REQUIRE(shutdown.HasValue());
        CHECK(shutdown.Value());
        CHECK(fixture.native.releaseCount == 2);
    }

    TEST_CASE("Android window shutdown retries queue saturation without reopening admission", "[unit][platform][android][window]") {
        Fixture fixture(1);
        auto bridge = fixture.Bridge();
        const auto generation = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(generation.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        REQUIRE(fixture.adapter.OnActivityResumed(fixture.activity).HasValue());
        const auto saturated = bridge.Shutdown();
        REQUIRE(saturated.HasError());
        CHECK(saturated.ErrorValue().code.Value() == "android_lifecycle.queue_saturated");
        CHECK(bridge.OnWindowAvailable(fixture.activity, fixture.native.First()).HasError());
        CHECK(bridge.AcquireWindow(generation.Value()).HasError());
        CHECK(fixture.native.acquireCount == 1);
        CHECK(fixture.native.releaseCount == 0);
        REQUIRE(bridge.Drain().HasValue());
        const auto completed = bridge.Shutdown();
        REQUIRE(completed.HasValue());
        CHECK(completed.Value());
        CHECK(fixture.native.releaseCount == 1);
        CHECK(fixture.State().process == AndroidProcessState::ShuttingDown);
        REQUIRE(fixture.controller->CompleteShutdown().HasValue());
    }

    TEST_CASE("Android window shutdown admits an empty partially initialized host", "[unit][platform][android][window]") {
        NativeReferences native;
        auto controller = MakeController(1);
        AndroidLifecycleAdapter adapter(*controller);
        AndroidWindowBridge bridge(adapter, AndroidWindowSelection::NoPresentation, native.Operations());
        const auto completed = bridge.Shutdown();
        REQUIRE(completed.HasValue());
        CHECK(completed.Value());
        const auto state = controller->Snapshot();
        REQUIRE(state.HasValue());
        CHECK(state.Value().processGeneration == 0);
        CHECK(state.Value().process == AndroidProcessState::ShuttingDown);
        CHECK(native.acquireCount == 0);
        CHECK(native.releaseCount == 0);
        REQUIRE(controller->CompleteShutdown().HasValue());
        REQUIRE(bridge.Shutdown().HasValue());
    }

    TEST_CASE("Android window bridge rejects every non-owner operation before native use", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();
        const auto generation = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(generation.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        std::array<bool, 5> rejected{};
        std::thread other([&] {
            const auto available = bridge.OnWindowAvailable(fixture.activity, fixture.native.Second());
            const auto lost = bridge.OnWindowLost(generation.Value());
            const auto lease = bridge.AcquireWindow(generation.Value());
            const auto drain = bridge.Drain();
            const auto shutdown = bridge.Shutdown();
            const auto ownerError = [](const auto &result) {
                return result.HasError() && result.ErrorValue().code.Value() == "android_lifecycle.owner_thread_required";
            };
            rejected = {ownerError(available), ownerError(lost), ownerError(lease), ownerError(drain), ownerError(shutdown)};
        });
        other.join();
        for (const bool value : rejected)
            CHECK(value);
        CHECK(fixture.native.acquireCount == 1);
        CHECK(fixture.native.releaseCount == 0);
        CHECK(fixture.State().hasWindow);
        CHECK(fixture.State().queuedObservationCount == 0);
    }

    TEST_CASE("Android native release callback cannot resurrect or drain a retiring owner", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();
        const auto generation = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(generation.HasValue());
        REQUIRE(bridge.Drain().HasValue());

        struct Probe {
            AndroidWindowBridge &bridge;
            AndroidActivityGeneration activity;
            ANativeWindow *window;
            bool acquireRejected{};
            bool drainRejected{};
        } probe{bridge, fixture.activity, fixture.native.Second()};

        fixture.native.hookContext = &probe;
        fixture.native.onRelease = [](void *context) noexcept {
            auto &state = *static_cast<Probe *>(context);
            const auto acquired = state.bridge.OnWindowAvailable(state.activity, state.window);
            const auto drained = state.bridge.Drain();
            state.acquireRejected = acquired.HasError() && acquired.ErrorValue().code.Value() == "android_window.operation_in_progress";
            state.drainRejected = drained.HasError() && drained.ErrorValue().code.Value() == "android_window.operation_in_progress";
        };
        REQUIRE(bridge.OnWindowLost(generation.Value()).HasValue());
        REQUIRE(bridge.Drain().HasValue());
        fixture.native.onRelease = nullptr;
        CHECK(probe.acquireRejected);
        CHECK(probe.drainRejected);
        CHECK(fixture.native.releaseCount == 1);
        CHECK(fixture.native.acquireCount == 1);
        CHECK_FALSE(fixture.State().hasWindow);
    }

    TEST_CASE("Android rejected stale activity candidate releases after real controller rejection", "[unit][platform][android][window]") {
        Fixture fixture;
        auto bridge = fixture.Bridge();
        const auto replacement = fixture.adapter.OnActivityCreated();
        REQUIRE(replacement.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        const auto stale = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(stale.HasValue());
        const auto admitted = bridge.OnWindowAvailable(replacement.Value(), fixture.native.Second());
        REQUIRE(admitted.HasValue());
        CHECK(bridge.AcquireWindow(stale.Value()).HasError());
        CHECK(bridge.AcquireWindow(admitted.Value()).HasError());
        const auto drained = bridge.Drain();
        REQUIRE(drained.HasError());
        CHECK(drained.ErrorValue().code.Value() == "android_lifecycle.stale_generation");
        CHECK(fixture.native.releaseCount == 1);
        CHECK(fixture.native.released[0] == fixture.native.First());
        CHECK(fixture.State().windowGeneration == admitted.Value());
        auto lease = bridge.AcquireWindow(admitted.Value());
        REQUIRE(lease.HasValue());
        CHECK(lease.Value().Get() == fixture.native.Second());
    }

    TEST_CASE("Android retained candidates remain bounded by the real lifecycle queue", "[unit][platform][android][window]") {
        Fixture fixture;
        std::array<unsigned, AndroidLifecycleController::MaximumQueueCapacity + 2> storage{};
        auto bridge = fixture.Bridge();
        const auto first = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(first.HasValue());
        REQUIRE(bridge.Drain().HasValue());
        AndroidWindowGeneration last{};
        for (std::size_t i = 0; i < AndroidLifecycleController::MaximumQueueCapacity; ++i) {
            const auto candidate = bridge.OnWindowAvailable(fixture.activity, reinterpret_cast<ANativeWindow *>(&storage[i]));
            REQUIRE(candidate.HasValue());
            CHECK(candidate.Value() > last);
            last = candidate.Value();
        }
        const auto excess = bridge.OnWindowAvailable(fixture.activity, fixture.native.Second());
        REQUIRE(excess.HasError());
        CHECK(excess.ErrorValue().code.Value() == "android_lifecycle.queue_saturated");
        CHECK(fixture.native.acquireCount == AndroidLifecycleController::MaximumQueueCapacity + 1);
        CHECK(fixture.native.releaseCount == 0);
        REQUIRE(bridge.Drain().HasValue());
        CHECK(fixture.State().windowGeneration == last);
        CHECK(fixture.native.releaseCount == AndroidLifecycleController::MaximumQueueCapacity);
        const auto shutdown = bridge.Shutdown();
        REQUIRE(shutdown.HasValue());
        CHECK(shutdown.Value());
        CHECK(fixture.native.releaseCount == fixture.native.acquireCount);
    }

#if !defined(__ANDROID__)
    TEST_CASE("Android native reference operations are unavailable on a non-Android host", "[unit][platform][android][window]") {
        Fixture fixture;
        const auto operations = NativeAndroidWindowOperations();
        CHECK(operations.acquire == nullptr);
        CHECK(operations.release == nullptr);
        AndroidWindowBridge bridge(fixture.adapter, AndroidWindowSelection::AndroidVulkan, operations);
        const auto rejected = bridge.OnWindowAvailable(fixture.activity, fixture.native.First());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "android_lifecycle.invalid_configuration");
        CHECK(fixture.State().queuedObservationCount == 0);
        CHECK_FALSE(fixture.State().hasWindow);
    }
#endif
}  // namespace
