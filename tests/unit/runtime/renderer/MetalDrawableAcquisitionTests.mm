#include "AllocationProbe.h"
#include "runtime/renderer/modules/metal/MetalDrawableAcquisition.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;
    using Clock = std::chrono::steady_clock;

    struct SourceState final {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<unsigned> calls{};
        bool entered{};
        bool release{};
        bool unavailable{};
        bool fail{};
    };

    class ControlledSource final : public IMetalDrawableSource {
    public:
        explicit ControlledSource(std::shared_ptr<SourceState> state) : state_(std::move(state)) {}

        id<CAMetalDrawable> Acquire(CAMetalLayer *) override {
            std::unique_lock lock{state_->mutex};
            ++state_->calls;
            state_->entered = true;
            state_->changed.notify_all();
            state_->changed.wait(lock, [this] {
                return state_->release;
            });
            if (state_->fail)
                @throw [NSException exceptionWithName:@"HoroInjectedDrawableFailure" reason:nil userInfo:nil];
            // This sentinel exercises ownership/slots only; native output and drawable-texture qualification are separate.
            return state_->unavailable ? nil : (id<CAMetalDrawable>)[NSObject new];
        }

    private:
        std::shared_ptr<SourceState> state_;
    };

    struct AcquisitionFixture final {
        std::shared_ptr<SourceState> source{std::make_shared<SourceState>()};
        std::unique_ptr<MetalDrawableAcquisition> worker;
        __weak CAMetalLayer *observedLayer{nil};
        __weak id<MTLDevice> observedDevice{nil};

        AcquisitionFixture() {
            @autoreleasepool {
                CAMetalLayer *layer = [CAMetalLayer layer];
                id<MTLDevice> device = (id<MTLDevice>)[NSObject new];
                observedLayer = layer;
                observedDevice = device;
                auto created = MetalDrawableAcquisition::Create(layer, device, std::make_unique<ControlledSource>(source));
                REQUIRE(created.HasValue());
                worker = std::move(created).Value();
            }
        }

        ~AcquisitionFixture() {
            Release();
            const auto deadline = Clock::now() + std::chrono::seconds{2};
            while ((!worker->Close() || !worker->IsDrained()) && Clock::now() < deadline)
                std::this_thread::yield();
            if (!worker->IsDrained())
                std::terminate();
        }

        bool Request(std::uint64_t generation, FramebufferExtent extent) {
            const auto deadline = Clock::now() + std::chrono::seconds{2};
            do {
                if (worker->Request(generation, extent))
                    return true;
                std::this_thread::yield();
            } while (Clock::now() < deadline);
            return false;
        }

        void AwaitEntered() {
            std::unique_lock lock{source->mutex};
            REQUIRE(source->changed.wait_for(lock, std::chrono::seconds{2}, [this] {
                return source->entered;
            }));
        }

        void Release() {
            std::lock_guard lock{source->mutex};
            source->release = true;
            source->changed.notify_all();
        }

        MetalDrawableResult AwaitResult(std::uint64_t generation) {
            const auto deadline = Clock::now() + std::chrono::seconds{2};
            MetalDrawableResult result;
            do {
                result = worker->Poll(generation);
                if (result.progress != MetalDrawablePoll::Pending)
                    return result;
                std::this_thread::yield();
            } while (Clock::now() < deadline);
            FAIL("The explicitly released acquisition did not publish within its test budget");
            return result;
        }
    };
}  // namespace

TEST_CASE("Metal acquisition has one nonblocking slot and retains native parents while cancellation is pending", "[metal][presentation]") {
    AcquisitionFixture fixture;
    REQUIRE(fixture.Request(1, {64, 32}));
    fixture.AwaitEntered();
    CHECK_FALSE(fixture.worker->Request(2, {128, 64}));
    CHECK_FALSE(fixture.worker->Idle());
    CHECK(fixture.worker->Poll(1).progress == MetalDrawablePoll::Pending);
    const auto closeDeadline = Clock::now() + std::chrono::seconds{2};
    while (!fixture.worker->Close() && Clock::now() < closeDeadline)
        std::this_thread::yield();
    CHECK_FALSE(fixture.worker->IsDrained());
    CHECK_FALSE(fixture.worker->Request(2, {128, 64}));
    CHECK(fixture.observedLayer != nil);
    CHECK(fixture.observedDevice != nil);
    CHECK(fixture.source->calls == 1);
    fixture.Release();
}

TEST_CASE("Metal acquisition discards stale generations and frees the bounded slot for latest extent", "[metal][presentation]") {
    AcquisitionFixture fixture;
    REQUIRE(fixture.Request(4, {64, 32}));
    fixture.AwaitEntered();
    fixture.Release();
    const auto stale = fixture.AwaitResult(5);
    CHECK(stale.progress == MetalDrawablePoll::Stale);
    CHECK(stale.drawable == nil);
    CHECK(stale.generation == 4);
    CHECK(stale.extent == FramebufferExtent{64, 32});
    REQUIRE(fixture.Request(5, {128, 96}));
    const auto ready = fixture.AwaitResult(5);
    CHECK(ready.progress == MetalDrawablePoll::Ready);
    CHECK(ready.drawable != nil);
    CHECK(ready.extent == FramebufferExtent{128, 96});
    CHECK(fixture.source->calls == 2);
}

TEST_CASE("Metal worker publishes unavailable and native exception outcomes without guessing device loss", "[metal][presentation]") {
    for (const bool fail : {false, true}) {
        AcquisitionFixture fixture;
        fixture.source->unavailable = true;
        fixture.source->fail = fail;
        REQUIRE(fixture.Request(8, {16, 16}));
        fixture.AwaitEntered();
        fixture.Release();
        const auto result = fixture.AwaitResult(8);
        CHECK(result.progress == (fail ? MetalDrawablePoll::Failed : MetalDrawablePoll::Unavailable));
        CHECK(result.drawable == nil);
        if (fail)
            CHECK(std::string_view{result.failure.data()}.find("HoroInjectedDrawableFailure") != std::string_view::npos);
        CHECK(fixture.worker->Idle());
    }
}

TEST_CASE("An unstarted Metal acquisition closes synchronously during initialization rollback", "[metal][presentation]") {
    AcquisitionFixture fixture;
    CHECK(fixture.worker->IsDrained());
    CHECK(fixture.source->calls == 0);
    CHECK_FALSE(fixture.worker->Request(0, {16, 16}));
    CHECK_FALSE(fixture.worker->Request(1, {}));
    REQUIRE(fixture.worker->Close());
    CHECK(fixture.worker->IsDrained());
    CHECK_FALSE(fixture.worker->Request(1, {16, 16}));
}

TEST_CASE("Warm pending drawable polls allocate no owner-thread C++ handoff storage", "[metal][presentation][allocation]") {
    AcquisitionFixture fixture;
    REQUIRE(fixture.Request(1, {64, 32}));
    fixture.AwaitEntered();
    static_cast<void>(fixture.worker->Poll(1));
    Horo::Tests::AllocationProbe::Measurement allocations;
    bool boundedPending = true;
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement measured;
        for (unsigned frame = 0; frame != 128; ++frame) {
            boundedPending = boundedPending && fixture.worker->Poll(1).progress == MetalDrawablePoll::Pending;
            boundedPending = boundedPending && !fixture.worker->Request(2, {128, 96});
            boundedPending = boundedPending && !fixture.worker->Idle();
        }
        allocations = measured.Snapshot();
    }
    CHECK(boundedPending);
    CHECK(allocations.requests == 0);
    CHECK(fixture.source->calls == 1);
    // The existing probe measures C++ allocations on this owner only, not Foundation/native or worker allocations.
}

TEST_CASE("A failed native acquisition remains observable when cancellation races its completion", "[metal][presentation]") {
    AcquisitionFixture fixture;
    fixture.source->fail = true;
    REQUIRE(fixture.Request(9, {32, 32}));
    fixture.AwaitEntered();
    REQUIRE(fixture.worker->Close());
    fixture.Release();
    const auto deadline = Clock::now() + std::chrono::seconds{2};
    while (!fixture.worker->IsDrained() && Clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(fixture.worker->IsDrained());
    REQUIRE(fixture.worker->Close());
    const auto failure = fixture.worker->Poll(9);
    CHECK(failure.progress == MetalDrawablePoll::Failed);
    CHECK(std::string_view{failure.failure.data()}.find("HoroInjectedDrawableFailure") != std::string_view::npos);
}
