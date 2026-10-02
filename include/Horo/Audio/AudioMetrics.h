#pragma once

/**
 * @file AudioMetrics.h
 * @brief Bounded audio measurements and backend-neutral observability publication.
 */

#include "Horo/Audio/AudioCommandStaging.h"
#include "Horo/Audio/AudioEventQueue.h"
#include "Horo/Audio/AudioMemory.h"
#include "Horo/Audio/AudioMetricExtraction.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <thread>

namespace Horo::Audio {
    /** @brief Fixed cumulative audio event classes; no asset, device or voice identity is a metric label. */
    enum class AudioMetricCounter : std::uint8_t {
        Underruns,
        CommandOrdinaryRejections,
        CommandCriticalRetries,
        CommandOutputStalls,
        EventTelemetryDrops,
        EventCriticalRetries,
        EventDuplicateTerminals,
        MemoryAllocationFailures,
        VoiceRejections,
        SpatialFallbacks,
        CallbackFaults,
        BackendFailures,
        ParameterLookupFailures,
        EventLookupFailures,
        ExtractionDrops,
        ExtractionCoalesced,
        ExtractionRateLimited,
        CallbackDeadlineOverruns,
        CallbackAllocationAttempts,
        CallbackLockAttempts,
        Count
    };

    /** @brief Fixed current audio measurements; absence is distinct from a measured zero. */
    enum class AudioMetricGauge : std::uint8_t {
        CommandQueueDepth,
        EventQueueDepth,
        ActiveVoices,
        VirtualVoices,
        StreamFillRatio,
        MemoryReservedBytes,
        MemoryUsedBytes,
        MemoryPeakBytes,
        DeviceSampleRate,
        DeviceBufferFrames,
        CallbackBudgetUtilization,
        OcclusionStalenessSeconds,
        BusPeakRatio,
        BusRmsRatio,
        ExtractionQueueDepth,
        Count
    };

    /** @brief Fixed cost observations recorded only when an owning source measured them. */
    enum class AudioMetricTiming : std::uint8_t {
        CallbackDuration,
        MixerDuration,
        EffectDuration,
        SpatialDuration,
        Count
    };

    inline constexpr std::size_t AudioMetricCounterCount = static_cast<std::size_t>(AudioMetricCounter::Count);
    inline constexpr std::size_t AudioMetricGaugeCount = static_cast<std::size_t>(AudioMetricGauge::Count);
    inline constexpr std::size_t AudioMetricTimingCount = static_cast<std::size_t>(AudioMetricTiming::Count);
    inline constexpr std::size_t MaximumAudioMetricMemorySources = 16;

    /** @brief One live pool or arena's owning-thread facts with a host-issued non-reused source generation. */
    struct AudioMetricMemorySource final {
        std::uint64_t sourceGeneration{}; /**< Strictly increasing when a new source is created within the audio owner. */
        AudioMemoryStats stats;           /**< Actual AudioMemoryPool/AudioScratchArena::Stats result. */
    };

    /** @brief One complete owner-safe-point projection, usable by editor, headless host and OBS sinks. */
    struct AudioMetricSnapshot final {
        std::uint32_t schemaVersion{1};                                     /**< Stable snapshot schema. */
        std::uint64_t ownerGeneration{};                                    /**< Nonzero host-issued runtime generation. */
        std::uint64_t revision{};                                           /**< Strictly increasing safe-point publication. */
        std::array<std::uint64_t, AudioMetricCounterCount> counters{};      /**< Saturating lifetime totals. */
        std::array<double, AudioMetricGaugeCount> gauges{};                 /**< Last measured values. */
        std::array<bool, AudioMetricGaugeCount> gaugeAvailable{};           /**< False means no source observation. */
        std::array<double, AudioMetricTimingCount> timings{};               /**< Last finite duration in seconds. */
        std::array<std::uint64_t, AudioMetricTimingCount> timingSequence{}; /**< Increases for each cost observation. */
        std::uint64_t invalidObservations{};                                /**< Rejected invalid or stale source facts. */
        bool saturated{};                                                   /**< At least one total reached its integer ceiling. */
        bool enabled{};                                                     /**< Host elected to collect this owner. */
        bool closed{};                                                      /**< No further observations may enter. */
    };

    /**
     * @brief Control-owner accumulator over real queue, callback and memory facts.
     * @details Callers pass values copied from owning APIs at a safe point. No callback uses this class;
     * publication copies into a bounded reader snapshot. The host retains source owners through sampling.
     */
    class AudioMetrics final {
    public:
        /** @brief Creates an owner-bound collector. @param generation Nonzero runtime generation.
         * @param enabled False disables all source sampling work. */
        AudioMetrics(std::uint64_t generation, bool enabled) noexcept;
        AudioMetrics(const AudioMetrics &) = delete;
        AudioMetrics &operator=(const AudioMetrics &) = delete;

        /** @brief Samples the real callback-to-control queue counters and occupancy.
         * @param sourceGeneration Host-issued increasing queue generation; replacement advances it, and reuse is forbidden.
         * @param stats AudioEventQueue::Stats from this owner's queue. @return False for stale, disabled or wrong-thread input. */
        [[nodiscard]] bool ObserveEventQueue(std::uint64_t sourceGeneration, const AudioEventQueueStats &stats) noexcept;
        /** @brief Samples exact-source extraction pressure without resetting lifetime totals on replacement.
         * @param sourceGeneration Nonzero strictly increasing generation for each replacement.
         * @param stats Control-owner queue snapshot. @return False for stale or regressing input. */
        [[nodiscard]] bool ObserveExtractionQueue(std::uint64_t sourceGeneration, const AudioExtractionStats &stats) noexcept;
        /** @brief Projects one consumed fixed callback record into the existing metric catalog.
         * @param record Control-consumed same-owner record. @return False for invalid kind or value. */
        [[nodiscard]] bool ObserveExtractedRecord(const AudioExtractionRecord &record) noexcept;
        /** @brief Samples actual command ingress and callback occupancies.
         * @param stats AudioCommandStaging::Stats from this owner's staging. @return False when collection is unavailable. */
        [[nodiscard]] bool ObserveCommandQueue(const AudioCommandStagingStats &stats) noexcept;
        /** @brief Counts one actual command admission result without treating caller-owned retry as accepted work.
         * @param status Status returned by AudioCommandStaging. @return False when disabled or invalid. */
        [[nodiscard]] bool ObserveCommandAdmission(AudioCommandStagingStatus status) noexcept;
        /** @brief Counts one actual control-to-callback output stall.
         * @param status Status returned by AudioCommandStaging::Pump. @return False when disabled or invalid. */
        [[nodiscard]] bool ObserveCommandPump(AudioCommandStagingStatus status) noexcept;
        /** @brief Classifies one validated callback fact after control consumes it.
         * @param record Consumed AudioEventQueue record. @return False when disabled or invalid. */
        [[nodiscard]] bool ObserveControlEvent(const AudioControlEventRecord &record) noexcept;
        /** @brief Aggregates real prepared pool/arena statistics at an owning-thread safe point.
         * @param sources Complete set of current source facts, sorted by unique generation; at most
         * MaximumAudioMetricMemorySources. New sources use generations greater than every prior source,
         * while omitted sources retire without erasing their lifetime failures.
         * @return False for invalid, stale, over-budget or disabled input. */
        [[nodiscard]] bool ObserveMemory(std::span<const AudioMetricMemorySource> sources) noexcept;
        /** @brief Records a finite nonnegative cost from the owning callback, mixer, effect or spatial source.
         * @param kind Fixed cost category. @param seconds Measured elapsed duration. @return False for invalid or disabled input. */
        [[nodiscard]] bool ObserveTiming(AudioMetricTiming kind, double seconds) noexcept;
        /** @brief Records an actually measured gauge without creating a dynamic series.
         * @param kind Fixed gauge category. @param value Finite nonnegative measurement. @return False for invalid or disabled input. */
        [[nodiscard]] bool ObserveGauge(AudioMetricGauge kind, double value) noexcept;
        /** @brief Records one owning-source rejected voice, fallback or stable-ID lookup failure.
         * @param kind Supported discrete failure category. @param count Number of observed events. @return False for invalid input. */
        [[nodiscard]] bool ObserveFailure(AudioMetricCounter kind, std::uint64_t count = 1) noexcept;

        /** @brief Publishes a coherent safe-point copy. @return False after close, on wrong thread or revision exhaustion. */
        [[nodiscard]] bool Publish() noexcept;
        /** @brief Copies the last published projection without touching live source owners. @return Coherent snapshot. */
        [[nodiscard]] AudioMetricSnapshot Snapshot() const noexcept;
        /** @brief Cheap owner-thread collection gate. @return False when disabled or closed. */
        [[nodiscard]] bool IsCollecting() const noexcept;
        /** @brief Retires collection, zeros observed gauges and retains lifetime totals; unobserved gauges stay unavailable.
         * @return False on wrong thread or revision exhaustion. */
        [[nodiscard]] bool Close() noexcept;

    private:
        [[nodiscard]] bool Invalid() noexcept;
        static void AddSaturating(std::uint64_t &target, std::uint64_t delta, bool &saturated) noexcept;
        [[nodiscard]] std::optional<std::uint64_t> FailureDeltaForSource(const AudioMetricMemorySource &source) const noexcept;

        const std::thread::id ownerThread_;
        AudioMetricSnapshot current_;
        AudioEventQueueStats lastEventStats_;
        std::uint64_t eventSourceGeneration_{};
        AudioExtractionStats lastExtractionStats_;
        std::uint64_t extractionSourceGeneration_{};

        struct MemoryCursor final {
            std::uint64_t sourceGeneration{};
            std::uint64_t failedAllocations{};
        };

        std::array<MemoryCursor, MaximumAudioMetricMemorySources> memoryCursors_{};
        std::size_t memoryCursorCount_{};
        std::uint64_t highestMemorySourceGeneration_{};
        mutable std::mutex publishedMutex_;
        AudioMetricSnapshot published_;
    };

    /** @brief Pre-registered single-series OBS handles; empty when host collection is off. */
    struct AudioMetricHandles final {
        std::array<Telemetry::Counter, AudioMetricCounterCount> counters;
        std::array<Telemetry::Gauge, AudioMetricGaugeCount> gauges;
        std::array<Telemetry::Histogram, AudioMetricTimingCount> timings;
    };

    /** @brief Registers the stable unit-bearing, zero-dimension audio catalog at host activation.
     * @param level Host-selected collection level. @return Fixed-series handles, or inert handles when Off. */
    [[nodiscard]] AudioMetricHandles RegisterAudioMetricHandles(Telemetry::MetricCollectionLevel level);

    /** @brief Owner-thread adapter from safe-point snapshots to sink-neutral Foundation telemetry. */
    class AudioMetricPublisher final {
    public:
        /** @brief Binds one owner generation to pre-registered handles.
         * @param generation Nonzero host-issued audio generation. @param handles Registered fixed-series handles. */
        AudioMetricPublisher(std::uint64_t generation, AudioMetricHandles handles) noexcept;
        /** @brief Publishes only new counter deltas, available gauges and new cost samples.
         * @param snapshot Coherent same-owner projection. @return False on stale, invalid or retired input. */
        [[nodiscard]] bool Publish(const AudioMetricSnapshot &snapshot) noexcept;
        /** @brief Stops future publication on the owner thread. */
        void Close() noexcept;

    private:
        const std::thread::id ownerThread_;
        const std::uint64_t generation_;
        AudioMetricHandles handles_;
        AudioMetricSnapshot previous_;
        bool closed_{};
    };
}  // namespace Horo::Audio
