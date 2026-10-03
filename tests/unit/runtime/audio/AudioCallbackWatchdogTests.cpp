#include "Horo/Audio/Internal/AudioCallbackWatchdog.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <new>
#include <thread>
#include <type_traits>

namespace {
    std::atomic<std::size_t> heapAllocations{};
}

void *operator new(const std::size_t size) {
    heapAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void *memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void *operator new[](const std::size_t size) {
    return ::operator new(size);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

namespace Horo::Audio::Backend {
    namespace {
        AudioDeviceEpoch Epoch(const std::uint64_t generation = 1) {
            return {.device = {AudioRuntimeId::Create(19).Value(), 2, 3}, .formatRevision = 4, .callbackEpoch = generation};
        }

        RenderResult HookedRender(void *, const RenderInvocation &) noexcept {
            AudioCallbackWatchdog::OnAllocationAttempt();
            AudioCallbackWatchdog::OnLockAttempt();
            return {.disposition = RenderDisposition::Rendered, .fault = AudioCallbackFaultCode::None};
        }

        RenderInvocation Invocation(const std::uint64_t frame = 0) {
            return {.epoch = Epoch(), .phase = RenderPhase::Rendering, .sampleFrame = frame};
        }

        TEST_CASE("Audio watchdog measures exact callback epochs and rate-limits overruns", "[unit][audio][watchdog]") {
            static_assert(std::is_trivially_copyable_v<AudioCallbackViolation>);
            AudioCallbackWatchdog watchdog;
            watchdog.Configure(100, 10);
            watchdog.ObserveDuration(Epoch(), 0, 100);
            watchdog.ObserveDuration(Epoch(), 1, 101);
            watchdog.ObserveDuration(Epoch(), 5, 102);
            watchdog.ObserveDuration(Epoch(), 11, 103);
            watchdog.ObserveDuration(Epoch(2), 0, 104);
            std::array<AudioCallbackViolation, 4> records{};
            const auto drained = watchdog.Drain(records);
#if !defined(NDEBUG)
            REQUIRE(drained.count == 3);
            REQUIRE(drained.rateLimited == 1);
            REQUIRE(drained.dropped == 0);
            REQUIRE(drained.sampledCallbacks == 5);
            REQUIRE(drained.latestDurationNanoseconds == 104);
            CHECK(records[0].kind == AudioCallbackViolationKind::Deadline);
            CHECK(records[0].sampleFrame == 1);
            CHECK(records[0].observedNanoseconds == 101);
            CHECK(records[1].sampleFrame == 11);
            CHECK(records[2].epoch == Epoch(2));
#else
            REQUIRE(drained.count == 0);
#endif
        }

        TEST_CASE("Audio watchdog saturates its fixed ring and retains dropped count", "[unit][audio][watchdog]") {
            AudioCallbackWatchdog watchdog;
            watchdog.Configure(1, 0);
            for (std::uint64_t frame = 0; frame < 100; ++frame)
                watchdog.ObserveDuration(Epoch(), frame, 2);
            std::array<AudioCallbackViolation, AudioCallbackWatchdog::Capacity> records{};
            const auto drained = watchdog.Drain(records);
#if !defined(NDEBUG)
            REQUIRE(drained.count == AudioCallbackWatchdog::Capacity);
            REQUIRE(drained.dropped == 36);
            REQUIRE(drained.sampledCallbacks == 100);
            REQUIRE(records.front().sampleFrame == 0);
            REQUIRE(records.back().sampleFrame == AudioCallbackWatchdog::Capacity - 1);
            const auto empty = watchdog.Drain(records);
            REQUIRE(empty.count == 0);
            REQUIRE(empty.dropped == 0);
#else
            REQUIRE(drained.count == 0);
#endif
        }

        TEST_CASE("Audio watchdog retains undrained facts across detached epoch reconfiguration", "[unit][audio][watchdog]") {
            AudioCallbackWatchdog watchdog;
            watchdog.Configure(1, 0);
            watchdog.ObserveDuration(Epoch(), 4, 2);
            watchdog.Configure(4, 0);  // Control has detached the old callback but has not drained its facts.
            watchdog.ObserveDuration(Epoch(2), 0, 5);
            std::array<AudioCallbackViolation, 2> records{};
            const auto drained = watchdog.Drain(records);
#if !defined(NDEBUG)
            REQUIRE(drained.count == 2);
            REQUIRE(drained.sampledCallbacks == 2);
            CHECK(records[0].epoch == Epoch());
            CHECK(records[0].budgetNanoseconds == 1);
            CHECK(records[1].epoch == Epoch(2));
            CHECK(records[1].budgetNanoseconds == 4);
#else
            REQUIRE(drained.count == 0);
#endif
        }

        TEST_CASE("Audio watchdog rate control is per violation kind", "[unit][audio][watchdog]") {
            AudioCallbackWatchdog watchdog;
            watchdog.Configure(0, 32);
            const RenderPort port{nullptr, HookedRender};
            static_cast<void>(watchdog.InvokeWithoutDeadline(port, Invocation(0)));
            static_cast<void>(watchdog.InvokeWithoutDeadline(port, Invocation(1)));
            static_cast<void>(watchdog.InvokeWithoutDeadline(port, Invocation(32)));
            std::array<AudioCallbackViolation, 4> records{};
            const auto drained = watchdog.Drain(records);
#if !defined(NDEBUG)
            REQUIRE(drained.count == 4);
            REQUIRE(drained.rateLimited == 2);
            CHECK(records[0].kind == AudioCallbackViolationKind::AllocationAttempt);
            CHECK(records[1].kind == AudioCallbackViolationKind::LockAttempt);
            CHECK(records[2].sampleFrame == 32);
            CHECK(records[3].sampleFrame == 32);
#else
            REQUIRE(drained.count == 0);
#endif
        }

        TEST_CASE("Audio watchdog callback invocation and drain perform no C++ heap allocation", "[unit][audio][watchdog]") {
            AudioCallbackWatchdog watchdog;
            watchdog.Configure(0, 0);
            std::array<AudioCallbackViolation, 4> records{};
            const auto invocation = Invocation();
            const RenderPort port{nullptr, HookedRender};
            const auto before = heapAllocations.load(std::memory_order_relaxed);
            static_cast<void>(watchdog.Invoke(port, invocation));
            const auto drained = watchdog.Drain(records);
            const auto after = heapAllocations.load(std::memory_order_relaxed);
            REQUIRE(after == before);
#if !defined(NDEBUG)
            REQUIRE(drained.count == 2);
#else
            REQUIRE(drained.count == 0);
#endif
        }

        TEST_CASE("Audio watchdog hooks stay scoped to callbacks and drain concurrently without losing accounting",
                  "[unit][audio][watchdog]") {
            AudioCallbackWatchdog watchdog;
            watchdog.Configure(0, 0);
            AudioCallbackWatchdog::OnAllocationAttempt();
            AudioCallbackWatchdog::OnLockAttempt();
            std::array<AudioCallbackViolation, 8> records{};
            REQUIRE(watchdog.Drain(records).count == 0);

            constexpr std::uint64_t CallbackCount = 5'000;
            std::atomic<bool> finished{};
            std::jthread producer([&] {
                for (std::uint64_t frame = 0; frame < CallbackCount; ++frame)
                    static_cast<void>(watchdog.Invoke({nullptr, HookedRender}, Invocation(frame)));
                finished.store(true, std::memory_order_release);
            });
            std::uint64_t accounted{};
            while (!finished.load(std::memory_order_acquire)) {
                const auto drained = watchdog.Drain(records);
                accounted += drained.count + drained.dropped + drained.rateLimited;
                std::this_thread::yield();
            }
            producer.join();  // Native/callback detachment precedes the last control drain and object teardown.
            for (;;) {
                const auto drained = watchdog.Drain(records);
                accounted += drained.count + drained.dropped + drained.rateLimited;
                if (drained.count == 0)
                    break;
            }
#if !defined(NDEBUG)
            REQUIRE(accounted == CallbackCount * 2);
#else
            REQUIRE(accounted == 0);
#endif
        }
    }  // namespace
}  // namespace Horo::Audio::Backend
