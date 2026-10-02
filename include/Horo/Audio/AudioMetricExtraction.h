#pragma once

/**
 * @file AudioMetricExtraction.h
 * @brief Fixed callback observations and a bounded callback-to-control extraction queue.
 */

#include "Horo/Audio/AudioDeviceTiming.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Horo::Audio {
    inline constexpr std::uint32_t MaximumAudioExtractionSlots = 4'096;

    /** @brief Closed numeric vocabulary; diagnostic kinds never contain text or exporter handles. */
    enum class AudioExtractionKind : std::uint8_t {
        CallbackDuration,
        MixerDuration,
        EffectDuration,
        SpatialDuration,
        DeadlineOverrun,
        AllocationAttempt,
        LockAttempt,
        Count
    };

    /** @brief One fixed callback observation; coalesced values retain the greatest measured duration. */
    struct AudioExtractionRecord final {
        AudioDeviceEpoch epoch;
        std::uint64_t sampleFrame{}; /**< Latest frame represented by this record. */
        AudioExtractionKind kind{AudioExtractionKind::CallbackDuration};
        double seconds{};         /**< Finite nonnegative duration; zero for allocation/lock attempts. */
        std::uint32_t samples{1}; /**< Number of source observations represented by this record. */
    };

    /** @brief Callback publication result; Queued means held until the callback calls Flush. */
    enum class AudioExtractionStatus : std::uint8_t {
        Queued,
        Coalesced,
        Dropped,
        RateLimited,
        Invalid,
        Closed,
        Inactive
    };

    /** @brief Immutable preparation dimensions for one exact callback epoch. */
    struct AudioExtractionDescriptor final {
        AudioDeviceEpoch epoch;
        std::uint32_t slots{};                   /**< Power of two in [2, MaximumAudioExtractionSlots]. */
        std::size_t budgetBytes{};               /**< At least slots times record size; owner metadata excluded. */
        std::uint64_t diagnosticFrameInterval{}; /**< Minimum frames between published records of one diagnostic kind. */
    };

    /** @brief Cumulative saturating queue facts; dropped counts source observations, not only ring entries. */
    struct AudioExtractionStats final {
        std::uint32_t depth{};
        std::uint64_t published{};
        std::uint64_t coalesced{};
        std::uint64_t dropped{};
        std::uint64_t rateLimited{};
    };

    /**
     * @brief Preallocated SPSC extraction from one callback producer to its control owner.
     * @details Create and destroy off-callback. The callback alone calls TryRecord, Flush, and Close;
     *          control alone calls TryConsume and Stats. Flush is required at each callback boundary.
     *          It inspects only the fixed kind vocabulary, never allocates, locks, formats or exports.
     *          The creating control thread remains the consumer owner after a move. After Close,
     *          the host detaches/joins the callback and drains before destruction or move.
     */
    class AudioMetricExtractionQueue final {
    public:
        /** @brief Allocates the exact ring off-callback.
         * @param descriptor Valid epoch, bounded power-of-two slots, byte budget and diagnostic rate policy.
         * @return Prepared queue or typed invalid/budget/allocation failure. */
        [[nodiscard]] static Result<AudioMetricExtractionQueue> Create(const AudioExtractionDescriptor &descriptor);
        /** @brief Destroys storage only after callback detachment and control drain. */
        ~AudioMetricExtractionQueue();
        /** @brief Moves quiescent storage on the creating control thread; owner-thread identity remains fixed. */
        AudioMetricExtractionQueue(AudioMetricExtractionQueue &&) noexcept;
        AudioMetricExtractionQueue &operator=(AudioMetricExtractionQueue &&) noexcept;
        AudioMetricExtractionQueue(const AudioMetricExtractionQueue &) = delete;
        AudioMetricExtractionQueue &operator=(const AudioMetricExtractionQueue &) = delete;

        /** @brief Callback-only admission to one fixed per-kind pending slot.
         * @param record Exact-epoch, finite single observation; samples must equal one.
         * @return Queued, Coalesced, Dropped, RateLimited, Invalid, Closed or Inactive. */
        [[nodiscard]] AudioExtractionStatus TryRecord(const AudioExtractionRecord &record) noexcept;
        /** @brief Callback-only bounded publication of pending records; full-ring observations become counted drops.
         * @return Number of ring records published by this flush. */
        [[nodiscard]] std::uint32_t Flush() noexcept;
        /** @brief Callback-only final flush and admission close; does not prove native detachment. */
        void Close() noexcept;
        /** @brief Control-owner copy of one record; false leaves output unchanged. */
        [[nodiscard]] bool TryConsume(AudioExtractionRecord &record) noexcept;
        /** @brief Control-owner cumulative pressure snapshot published at callback Flush boundaries. */
        [[nodiscard]] AudioExtractionStats Stats() const noexcept;
        /** @brief True only after close and full control drain. */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        struct State;
        explicit AudioMetricExtractionQueue(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio
