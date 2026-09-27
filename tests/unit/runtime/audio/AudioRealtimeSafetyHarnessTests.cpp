#include "AllocationProbe.h"
#include "Horo/Audio/AudioCommandBuffer.h"
#include "Horo/Audio/AudioResampler.h"
#include "Horo/Audio/Internal/AudioCallbackWatchdog.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <utility>

namespace Horo::Audio {
    namespace {
        using namespace Backend;

        constexpr std::uint32_t BlockFrames = 1024;
        constexpr std::uint64_t PublishedCommands = 255;  // Saturates ordinary capacity in a 256-slot ring with one critical slot.
        constexpr std::uint64_t BlockBudgetNanoseconds = 1'000'000'000ULL * BlockFrames / 48'000;

        struct alignas(64) Plane final {
            std::array<float, BlockFrames> samples{};
        };

        struct Workload final {
            AudioCommandBuffer &commands;
            AudioResampler &resampler;
            std::array<Plane, 2> input{};
            std::array<Plane, 2> output{};
            std::uint32_t consumed{};
            AudioResamplerProgress progress{};
        };

        RenderResult RenderWorkload(void *const context, const RenderInvocation &) noexcept {
            auto &workload = *static_cast<Workload *>(context);
            AudioCommandRecord record;
            while (workload.commands.TryConsume(record))
                ++workload.consumed;
            const std::array<std::span<const float>, 2> source{workload.input[0].samples, workload.input[1].samples};
            const std::array<std::span<float>, 2> destination{workload.output[0].samples, workload.output[1].samples};
            workload.progress = workload.resampler.Process({source, BlockFrames, false}, {destination, BlockFrames});
            return {.disposition = RenderDisposition::Rendered, .fault = AudioCallbackFaultCode::None};
        }

        struct ForbiddenOperations final {
            std::mutex mutex;
            std::size_t lockAttempts{};
        };

        RenderResult InjectHeapAllocation(void *, const RenderInvocation &) noexcept {
            // Actual C++ allocation, counted by the test executable even when NDEBUG removes Horo hooks.
            void *const storage = ::operator new(64);
            ::operator delete(storage);
            return {.disposition = RenderDisposition::Rendered, .fault = AudioCallbackFaultCode::None};
        }

        RenderResult InjectMutexLock(void *const context, const RenderInvocation &) noexcept {
            auto &operations = *static_cast<ForbiddenOperations *>(context);
            ++operations.lockAttempts;  // Test-owned qualified probe before the actual mutex acquisition.
            const std::lock_guard lock(operations.mutex);
            AudioCallbackWatchdog::OnLockAttempt();
            return {.disposition = RenderDisposition::Rendered, .fault = AudioCallbackFaultCode::None};
        }

        RenderResult InjectOverrun(void *, const RenderInvocation &) noexcept {
            std::this_thread::sleep_for(std::chrono::nanoseconds(BlockBudgetNanoseconds + 5'000'000));
            return {.disposition = RenderDisposition::Rendered, .fault = AudioCallbackFaultCode::None};
        }

        AudioDeviceEpoch Epoch() {
            const auto owner = AudioRuntimeId::Create(71).Value();
            return {.device = {owner, 2, 3}, .formatRevision = 4, .callbackEpoch = 5};
        }

        // CI's single failure predicate: neither the callback nor the bounded recorder may lose facts.
        bool Safe(const AudioCallbackViolationDrain &drain, const std::span<const AudioCallbackViolation> records,
                  const std::size_t callbackAllocations, const std::size_t callbackLockAttempts,
                  const std::uint64_t elapsedNanoseconds) noexcept {
            if (callbackAllocations != 0 || callbackLockAttempts != 0 || elapsedNanoseconds > BlockBudgetNanoseconds ||
                drain.dropped != 0 || drain.rateLimited != 0)
                return false;
            for (const auto &record : records) {
                if (record.kind == AudioCallbackViolationKind::AllocationAttempt ||
                    record.kind == AudioCallbackViolationKind::LockAttempt || record.kind == AudioCallbackViolationKind::Deadline)
                    return false;
            }
            return true;
        }

        struct ProbeResult final {
            RenderResult render;
            std::size_t allocations{};
            std::uint64_t elapsedNanoseconds{};
        };

        ProbeResult InvokeProbed(AudioCallbackWatchdog &watchdog, const RenderPort &port, const RenderInvocation &invocation) noexcept {
            const auto beforeAllocations = Tests::AllocationProbe::Count();
            const auto start = std::chrono::steady_clock::now();
            const auto result = watchdog.Invoke(port, invocation);
            const auto end = std::chrono::steady_clock::now();
            const auto afterAllocations = Tests::AllocationProbe::Count();
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            return {result, afterAllocations - beforeAllocations, static_cast<std::uint64_t>(elapsed)};
        }

        TEST_CASE("Real-time safety gate runs prepared worst-case command and DSP work under a callback deadline",
                  "[unit][audio][realtime-safety]") {
            const auto owner = AudioRuntimeId::Create(71).Value();
            auto queueResult = AudioCommandBuffer::Create({.owner = owner,
                                                           .storageIdentity = AudioMemoryPoolId::Create(72).Value(),
                                                           .epoch = 5,
                                                           .slots = 256,
                                                           .criticalSlots = 1,
                                                           .budgetBytes = 1U << 20});
            REQUIRE(queueResult.HasValue());
            auto commands = std::move(queueResult).Value();
            const auto plan = AudioResamplerPlan::Prepare({.quality = AudioResamplerQuality::Sinc64,
                                                           .inputRate = 48'000,
                                                           .outputRate = 48'000,
                                                           .channels = 2,
                                                           .maximumOutputFrames = BlockFrames},
                                                          {1ULL << 32, 1ULL << 24, 4096});
            REQUIRE(plan.HasValue());
            auto prepared = AudioResampler::Create(plan.Value(), 32ULL << 20);
            REQUIRE(prepared.HasValue());
            auto resampler = std::move(prepared).Value();
            for (std::uint64_t sequence = 1; sequence <= PublishedCommands; ++sequence) {
                const AudioCommandRecord record{sequence,
                                                {.scope = {.owner = owner,
                                                           .epoch = 5,
                                                           .scene = {.owner = owner, .slot = 1, .generation = 1}},
                                                 .payload = AudioStartVoiceCommand{.voice = {.owner = owner, .slot = 2, .generation = 3}}}};
                REQUIRE(commands.TryPublish(record) == AudioCommandPublishStatus::Published);
            }
            Workload workload{commands, resampler};
            for (auto &plane : workload.input)
                plane.samples.fill(0.25F);
            AudioCallbackWatchdog watchdog;
            watchdog.Configure(BlockBudgetNanoseconds, 0);
            const RenderInvocation invocation{.epoch = Epoch(), .phase = RenderPhase::Rendering};
            const RenderPort port{&workload, RenderWorkload};
            const auto probe = InvokeProbed(watchdog, port, invocation);
            std::array<AudioCallbackViolation, AudioCallbackWatchdog::Capacity> records{};
            const auto drain = watchdog.Drain(records);
            INFO("callback allocations=" << probe.allocations << ", violations=" << drain.count << ", dropped=" << drain.dropped
                                         << ", rate-limited=" << drain.rateLimited << ", latest-ns=" << drain.latestDurationNanoseconds
                                         << ", test-probe-ns=" << probe.elapsedNanoseconds << ", budget-ns=" << BlockBudgetNanoseconds);
            REQUIRE(probe.render.disposition == RenderDisposition::Rendered);
            REQUIRE(workload.consumed == PublishedCommands);
            REQUIRE(workload.progress.status != AudioResamplerStatus::InvalidBuffer);
            REQUIRE(workload.progress.status != AudioResamplerStatus::InvalidState);
            REQUIRE(workload.progress.produced > 0);
            REQUIRE(Safe(drain, std::span{records}.first(drain.count), probe.allocations, 0, probe.elapsedNanoseconds));
        }

        TEST_CASE("Real-time safety gate rejects actual allocation, mutex acquisition and overrun in both build modes",
                  "[unit][audio][realtime-safety]") {
            AudioCallbackWatchdog watchdog;
            watchdog.Configure(BlockBudgetNanoseconds, 0);
            const RenderInvocation invocation{.epoch = Epoch(), .phase = RenderPhase::Rendering};
            std::array<AudioCallbackViolation, AudioCallbackWatchdog::Capacity> records{};
            const auto heapProbe = InvokeProbed(watchdog, {nullptr, InjectHeapAllocation}, invocation);
            const auto heapDrain = watchdog.Drain(records);
            REQUIRE(heapProbe.allocations > 0);
            CHECK_FALSE(Safe(heapDrain, std::span{records}.first(heapDrain.count), heapProbe.allocations, 0, heapProbe.elapsedNanoseconds));

            ForbiddenOperations operations;
            const auto lockProbe = InvokeProbed(watchdog, {&operations, InjectMutexLock}, invocation);
            const auto lockDrain = watchdog.Drain(records);
            REQUIRE(operations.lockAttempts == 1);
            CHECK_FALSE(Safe(lockDrain, std::span{records}.first(lockDrain.count), lockProbe.allocations, operations.lockAttempts,
                             lockProbe.elapsedNanoseconds));
#if !defined(NDEBUG)
            REQUIRE(lockDrain.count >= 1);
            CHECK(records[0].kind == AudioCallbackViolationKind::LockAttempt);
#else
            REQUIRE(lockDrain.count == 0);
#endif
            const auto overrunProbe = InvokeProbed(watchdog, {nullptr, InjectOverrun}, invocation);
            const auto overrunDrain = watchdog.Drain(records);
            REQUIRE(overrunProbe.elapsedNanoseconds > BlockBudgetNanoseconds);
            CHECK_FALSE(Safe(overrunDrain, std::span{records}.first(overrunDrain.count), overrunProbe.allocations, 0,
                             overrunProbe.elapsedNanoseconds));
#if !defined(NDEBUG)
            REQUIRE(overrunDrain.count >= 1);
            CHECK(records[0].kind == AudioCallbackViolationKind::Deadline);
#else
            REQUIRE(overrunDrain.count == 0);  // Test-owned wall-clock probe remains active in Release.
#endif
            CHECK(Safe({}, {}, 0, 0, BlockBudgetNanoseconds));
            CHECK_FALSE(Safe({}, {}, 0, 0, BlockBudgetNanoseconds + 1));
            CHECK_FALSE(Safe({.dropped = 1}, {}, 0, 0, 0));
            CHECK_FALSE(Safe({.rateLimited = 1}, {}, 0, 0, 0));
        }
    }  // namespace
}  // namespace Horo::Audio
