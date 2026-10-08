#pragma once

/** @file RuntimeSimulationTimingStorage.h
 * @brief Actual host-owned bounded timing records; external releases only signal their own live slot.
 */
#include "Horo/Runtime/RuntimeSimulationTiming.h"

#include <atomic>
#include <thread>
#include <vector>

namespace Horo::Runtime::SimulationTimingDetail {
    static_assert(std::atomic<bool>::is_always_lock_free);

    /** @brief Reusable owner-thread pause record; generation changes only after its release signal is consumed. */
    struct PauseRecord final {
        std::atomic<bool> released{true};
        std::uint64_t generation{};
        RuntimeSimulationPauseReason reason{};
        bool occupied{};
        bool releaseAtCutoff{};
    };

    /** @brief Bounded step result; observation release never cancels a pending admitted command. */
    struct StepRecord final {
        std::atomic<bool> released{true};
        std::uint64_t generation{};
        std::uint64_t pauseRevision{};
        RuntimeSingleStepResult result;
        bool occupied{};
        bool admitted{};
    };

    /**
     * @brief One actual scheduler namespace, retained by the host and outstanding opaque reads/leases.
     * @details All fields except release flags are owner-thread only. Release flags permit cross-thread lease destruction;
     *          the owner never reuses a record until observing that signal. No callback executes during cutoff or retirement.
     *          The live host retains this object, so releasing a frame-hot lease cannot perform last-owner reclamation.
     */
    struct Storage final {
        Storage(std::uint32_t pauseCapacity, std::uint32_t stepCapacity) : pauses(pauseCapacity), steps(stepCapacity) {}

        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;

        const std::thread::id owner{std::this_thread::get_id()};
        std::vector<PauseRecord> pauses;
        std::vector<StepRecord> steps;
        RuntimeSimulationPolicy desired;
        RuntimeSimulationPolicy active;
        std::uint64_t nextStepSequence{};
        bool closed{};
    };
}  // namespace Horo::Runtime::SimulationTimingDetail
