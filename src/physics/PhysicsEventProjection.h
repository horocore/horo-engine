#pragma once

/**
 * @file PhysicsEventProjection.h
 * @brief Target-private bounded solver-evidence capture and lifecycle reduction.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/PhysicsEvents.h"
#include "Horo/Physics/PhysicsWorldBudgets.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Physics::Detail {
    /** @brief Per-tick result produced after copied solver evidence has been reconciled. */
    struct PhysicsEventProjectionResult final {
        std::uint32_t publishedRecordCount{};
        std::uint64_t droppedRecordCount{};
        bool overflowed{};
    };

    /**
     * @brief Reduces copied native contact evidence into bounded deterministic lifecycle records.
     *
     * Native callbacks are multi-producer, allocation-free evidence writers. They never call a
     * consumer and never mutate the lifecycle table. The owner thread sorts and reconciles the
     * complete captured prefix after the solver joins, then swaps one immutable publication buffer.
     */
    class PhysicsEventProjection final {
    public:
        /**
         * @brief Preallocates all callback, lifecycle and publication storage.
         * @param maximumEvents Maximum published records in one completed tick.
         * @param maximumInFlightPairs Maximum copied callback observations retained before reduction.
         * @param overflowPolicy Policy applied to canonical records beyond maximumEvents.
         * @throws std::bad_alloc When the bounded world reservation cannot be prepared.
         */
        PhysicsEventProjection(std::uint32_t maximumEvents, std::uint32_t maximumInFlightPairs, PhysicsEventOverflowPolicy overflowPolicy);
        ~PhysicsEventProjection() = default;
        PhysicsEventProjection(const PhysicsEventProjection &) = delete;
        PhysicsEventProjection &operator=(const PhysicsEventProjection &) = delete;

        /** @brief Opens one owner-thread capture window before native stepping. */
        void BeginTick(std::uint64_t simulationTick) noexcept;
        /**
         * @brief Copies one callback observation into a reserved slot.
         * @param observation Complete Horo-owned evidence valid only for this call.
         * @return True when retained; false for stale/malformed or capacity-exhausted evidence.
         * @note This function is safe for concurrent native callback producers and performs no allocation,
         * locking, consumer invocation or world mutation.
         */
        [[nodiscard]] bool TryCapture(const PhysicsContactObservation &observation) noexcept;
        /**
         * @brief Reconciles the capture window and publishes the next immutable event buffer.
         * @param simulationTick Exact tick opened by BeginTick.
         * @return Per-tick publication counts, or CapacityExceeded for FailTick overflow.
         * @pre Native stepping and every callback producer have quiesced.
         */
        [[nodiscard]] Result<PhysicsEventProjectionResult> CompleteTick(std::uint64_t simulationTick);
        /** @brief Abandons the open capture window without changing the prior lifecycle/publication state. */
        void AbortTick() noexcept;
        /** @brief Clears lifecycle and publication state at a world reset/unload boundary. */
        void Reset() noexcept;
        /** @brief Returns the most recently published immutable event records until the next publication boundary. */
        [[nodiscard]] std::span<const PhysicsEventRecord> PublishedEvents() const noexcept;
        /** @brief Returns the tick owning PublishedEvents, or zero before first publication. */
        [[nodiscard]] std::uint64_t PublishedTick() const noexcept;
        /** @brief Returns dropped callback/output records for the currently open or just-completed tick. */
        [[nodiscard]] std::uint64_t DroppedRecordCount() const noexcept;

    private:
        struct PairState final {
            PhysicsEventPairKey pair;
            std::optional<PhysicsEventMaterial> firstMaterial;
            std::optional<PhysicsEventMaterial> secondMaterial;
            PhysicsContactSummary contact;
            bool sensor{};
        };

        /** @brief Checks copied evidence without constructing diagnostics in a solver callback. */
        [[nodiscard]] static bool ValidObservation(const PhysicsContactObservation &observation) noexcept;
        /** @brief Canonicalizes endpoint order and reverses the normal when the pair is swapped. */
        static void Canonicalize(PhysicsContactObservation &observation) noexcept;
        /** @brief Coalesces the retained callback prefix into one state per canonical pair. */
        void BuildCurrentPairs(std::uint32_t retained);
        /** @brief Reconciles prior and current pair sets into bounded lifecycle records. */
        void ReconcileLifecycle() noexcept;
        /** @brief Appends one record or accounts for canonical event-buffer overflow. */
        void Append(PhysicsEventKind kind, const PairState &state) noexcept;
        /** @brief Appends the correct begin/end/persist transition for one old/current pair match. */
        void AppendMatchedTransition(const PairState &previous, const PairState &current) noexcept;
        /** @brief Returns the mutable staging buffer distinct from the published buffer. */
        [[nodiscard]] std::vector<PhysicsEventRecord> &StagingEvents() noexcept;
        const std::uint32_t maximumEvents_;
        const std::uint32_t maximumInFlightPairs_;
        const PhysicsEventOverflowPolicy overflowPolicy_;
        std::vector<PhysicsContactObservation> observations_;
        std::vector<PairState> previousPairs_;
        std::vector<PairState> currentPairs_;
        std::array<std::vector<PhysicsEventRecord>, 2> eventBuffers_;
        std::atomic<std::uint32_t> callbackWrite_{};
        std::atomic<std::uint64_t> callbackDropped_{};
        std::atomic_bool callbackOverflowed_{};
        std::uint64_t activeTick_{};
        std::uint64_t publishedTick_{};
        std::size_t publishedBuffer_{};
        std::uint64_t droppedDuringTick_{};
        bool overflowedDuringTick_{};
    };
}  // namespace Horo::Physics::Detail
