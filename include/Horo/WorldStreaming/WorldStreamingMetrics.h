#pragma once

/**
 * @file WorldStreamingMetrics.h
 * @brief Bounded low-cardinality World Streaming measurements and lifecycle binding.
 */

#include "Horo/Foundation/Telemetry/Telemetry.h"
#include "Horo/WorldStreaming/WorldStreamingDiagnosticSnapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Tag separating metric-binding revisions from runtime-composition revisions. */
        struct StreamingMetricBindingRevisionTag;
        /** @brief Tag separating metric-sample revisions from diagnostic snapshot revisions. */
        struct StreamingMetricSampleRevisionTag;
    }  // namespace Detail

    /** @brief Monotonic revision of one owner-scoped metric binding. */
    using StreamingMetricBindingRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingMetricBindingRevisionTag, WorldStreamingErrors::MetricInvalid>;
    /** @brief Monotonic revision of one complete owner-produced metric sample. */
    using StreamingMetricSampleRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingMetricSampleRevisionTag, WorldStreamingErrors::MetricInvalid>;

    /** @brief Host policy for the availability of World Streaming metric publication. */
    enum class StreamingMetricAvailability : std::uint8_t {
        Off,
        Unavailable,
        Available
    };

    /** @brief Whether an owner lifetime may run without an available metric binding. */
    enum class StreamingMetricRequirement : std::uint8_t {
        Optional,
        Required
    };

    /** @brief Observable result of a validated measurement publication attempt. */
    enum class StreamingMetricPublishDisposition : std::uint8_t {
        Submitted,
        SuppressedByPolicy,
        SuppressedUnavailable
    };

    /** @brief Explicit lifecycle of the unique owner-thread publication binding. */
    enum class StreamingMetricBindingState : std::uint8_t {
        Active,
        Cancelled,
        Closed
    };

    /** @brief Closed pipeline-stage vocabulary used by the latency histogram. */
    enum class StreamingMetricStage : std::uint8_t {
        AssetRead,
        Decode,
        ProviderStage,
        Activation,
        Retirement,
        Count
    };
    /** @brief Closed byte-flow vocabulary. */
    enum class StreamingMetricByteFlow : std::uint8_t {
        Loaded,
        Retired,
        Count
    };
    /** @brief Closed queue vocabulary. */
    enum class StreamingMetricQueue : std::uint8_t {
        Operations,
        Completions,
        Count
    };
    /** @brief Closed aggregate residency vocabulary; cell identities are deliberately excluded. */
    enum class StreamingMetricResidency : std::uint8_t {
        Loading,
        Resident,
        Active,
        Evicting,
        Count
    };
    /** @brief Closed observation-loss vocabulary. */
    enum class StreamingMetricDropReason : std::uint8_t {
        Capacity,
        Cancelled,
        Stale,
        Count
    };
    /** @brief Closed terminal-failure vocabulary. */
    enum class StreamingMetricFailureReason : std::uint8_t {
        Io,
        Integrity,
        Decode,
        Provider,
        Activation,
        Count
    };

    inline constexpr std::size_t StreamingMetricStageCount = static_cast<std::size_t>(StreamingMetricStage::Count);
    inline constexpr std::size_t StreamingMetricByteFlowCount = static_cast<std::size_t>(StreamingMetricByteFlow::Count);
    inline constexpr std::size_t StreamingMetricQueueCount = static_cast<std::size_t>(StreamingMetricQueue::Count);
    inline constexpr std::size_t StreamingMetricResidencyCount = static_cast<std::size_t>(StreamingMetricResidency::Count);
    inline constexpr std::size_t StreamingMetricDropReasonCount = static_cast<std::size_t>(StreamingMetricDropReason::Count);
    inline constexpr std::size_t StreamingMetricFailureReasonCount = static_cast<std::size_t>(StreamingMetricFailureReason::Count);

    /** @brief Qualified maxima used to reject impossible samples before any metric handle is invoked. */
    struct StreamingMetricBounds final {
        double maximumLatencySeconds{};           /**< Maximum total or stage latency in one sample. */
        std::uint64_t maximumBytesPerSample{};    /**< Maximum bytes for each closed flow category. */
        std::uint64_t maximumQueueDepth{};        /**< Maximum depth for each closed queue category. */
        std::uint64_t maximumResidentCells{};     /**< Maximum cells in each aggregate residency category. */
        std::uint64_t maximumDropsPerSample{};    /**< Maximum drops for each closed reason. */
        std::uint64_t maximumFailuresPerSample{}; /**< Maximum failures for each closed reason. */
    };

    /** @brief One complete immutable safe-point measurement with no high-cardinality labels. */
    struct StreamingMetricSample final {
        std::uint32_t schemaVersion{1};                                          /**< Exact measurement schema version. */
        StreamingRuntimeOwnerToken owner;                                        /**< Exact mounted authority lifetime. */
        StreamingRuntimeCompositionRevision ownerRevision;                       /**< Exact composition revision at measurement. */
        StreamingMetricSampleRevision sampleRevision;                            /**< Monotonic sample publication revision. */
        WorldStreamingDiagnosticRevision diagnosticRevision;                     /**< Correlated bounded diagnostic snapshot revision. */
        double operationLatencySeconds{};                                        /**< Complete authority operation latency. */
        std::array<double, StreamingMetricStageCount> stageLatencySeconds{};     /**< Closed per-stage latency values. */
        std::array<std::uint64_t, StreamingMetricByteFlowCount> bytes{};         /**< Bytes by closed flow category. */
        std::array<std::uint64_t, StreamingMetricQueueCount> queueDepth{};       /**< Current bounded queue depths. */
        std::array<std::uint64_t, StreamingMetricResidencyCount> residency{};    /**< Current aggregate cell counts. */
        std::array<std::uint64_t, StreamingMetricDropReasonCount> drops{};       /**< New drops in this sample window. */
        std::array<std::uint64_t, StreamingMetricFailureReasonCount> failures{}; /**< New failures in this sample window. */
    };

    /** @brief Pre-bound process Telemetry handles; registration belongs to host composition. */
    struct StreamingMetricHandles final {
        Telemetry::Histogram operationLatency;                                      /**< Core complete-operation latency. */
        std::array<Telemetry::Histogram, StreamingMetricStageCount> stageLatencies; /**< Detailed stage latencies. */
        std::array<Telemetry::Counter, StreamingMetricByteFlowCount> bytes;         /**< Core byte flow counters. */
        std::array<Telemetry::Gauge, StreamingMetricQueueCount> queueDepth;         /**< Core queue depth gauges. */
        std::array<Telemetry::Gauge, StreamingMetricResidencyCount> residency;      /**< Core aggregate residency gauges. */
        std::array<Telemetry::Counter, StreamingMetricDropReasonCount> drops;       /**< Core drop counters. */
        std::array<Telemetry::Counter, StreamingMetricFailureReasonCount> failures; /**< Core failure counters. */
    };

    /** @brief Complete immutable-at-admission policy for one owner-scoped streaming metric binding. */
    struct StreamingMetricBindingConfiguration final {
        StreamingMetricBindingRevision bindingRevision;    /**< Non-zero host-issued binding revision. */
        StreamingRuntimeCompositionRevision ownerRevision; /**< Admitted runtime-composition revision. */
        StreamingMetricBounds bounds;                      /**< Qualified measurement maxima. */
        StreamingMetricAvailability availability;          /**< Explicit capability state. */
        StreamingMetricRequirement requirement;            /**< Admission behavior when unavailable. */
        Telemetry::MetricCollectionLevel collectionLevel;  /**< Host-selected collection detail. */
    };

    /**
     * @brief Registers and pre-binds the complete closed World Streaming metric vocabulary.
     * @param level Host-selected process metric collection level.
     * @return Handles bound outside streaming work; empty handles truthfully represent unavailable collection.
     * @pre Process composition after Telemetry initialization, never a streaming callback or authority hot path.
     */
    [[nodiscard]] StreamingMetricHandles RegisterStreamingMetricHandles(Telemetry::MetricCollectionLevel level);

    /** @brief Unique revision-fenced owner-thread binding from one streaming authority to process metrics. */
    class WorldStreamingMetricBinding final {
    public:
        WorldStreamingMetricBinding(const WorldStreamingMetricBinding &) = delete;
        WorldStreamingMetricBinding &operator=(const WorldStreamingMetricBinding &) = delete;
        /** @brief Transfers the unique publication binding without changing its declaring owner-thread affinity. */
        WorldStreamingMetricBinding(WorldStreamingMetricBinding &&other) noexcept;
        WorldStreamingMetricBinding &operator=(WorldStreamingMetricBinding &&) = delete;

        /**
         * @brief Creates one owner-scoped metric binding without registering instruments.
         * @param owner Exact mounted runtime owner lifetime.
         * @param configuration Complete revisions, bounds and collection policy.
         * @param handles Pre-registered and pre-bound process Telemetry handles.
         * @return Binding or a typed invalid, unsupported or unavailable error.
         * @pre Called on the authority owner thread that will publish, replace, cancel and close the binding.
         * @post Failure retains no handles and publishes no metric record.
         */
        [[nodiscard]] static Result<WorldStreamingMetricBinding> Create(const StreamingRuntimeOwnerToken &owner,
                                                                        const StreamingMetricBindingConfiguration &configuration,
                                                                        StreamingMetricHandles handles);

        /**
         * @brief Replaces the complete binding policy and handles transactionally for the same owner lifetime.
         * @param expectedRevision Exact current binding revision.
         * @param configuration Complete successor revisions, bounds and collection policy.
         * @param handles Complete successor pre-bound handles.
         * @return Success or a typed stale, invalid, unsupported, unavailable or lifecycle error.
         * @post Failure leaves the current binding and every caller-owned argument unchanged.
         */
        [[nodiscard]] Result<void> Replace(StreamingMetricBindingRevision expectedRevision,
                                           const StreamingMetricBindingConfiguration &configuration, StreamingMetricHandles handles);

        /**
         * @brief Validates and publishes one complete owner-produced measurement sample.
         * @param sample Immutable values captured at one authority safe point.
         * @param expectedBindingRevision Exact binding revision captured by the producer.
         * @return Submitted/suppressed disposition or a typed invalid, stale, capacity or lifecycle error.
         * @post Invalid input invokes no metric handle; metric loss never changes World Streaming state.
         */
        [[nodiscard]] Result<StreamingMetricPublishDisposition> Publish(const StreamingMetricSample &sample,
                                                                        StreamingMetricBindingRevision expectedBindingRevision) const;

        /** @brief Cancels future publication immediately and idempotently. @return Success or a thread-affinity error. */
        [[nodiscard]] Result<void> Cancel();
        /** @brief Closes future publication for shutdown and idempotently releases retained handles. @return Success or a thread-affinity
         * error. */
        [[nodiscard]] Result<void> Close();

        /** @brief Returns the exact mounted owner. @return Immutable owner token. */
        [[nodiscard]] const StreamingRuntimeOwnerToken &Owner() const noexcept;
        /** @brief Returns the active binding revision. @return Non-zero revision. */
        [[nodiscard]] StreamingMetricBindingRevision Revision() const noexcept;
        /** @brief Returns the admitted runtime-composition revision. @return Non-zero revision. */
        [[nodiscard]] StreamingRuntimeCompositionRevision OwnerRevision() const noexcept;
        /** @brief Returns the current binding lifecycle. @return Active, cancelled or closed. */
        [[nodiscard]] StreamingMetricBindingState State() const noexcept;

    private:
        /**
         * @brief Retains one fully validated binding without performing registration.
         * @param owner Exact mounted owner lifetime.
         * @param configuration Complete admitted revisions, bounds and collection policy.
         * @param handles Complete pre-bound handles when available.
         */
        WorldStreamingMetricBinding(const StreamingRuntimeOwnerToken &owner, const StreamingMetricBindingConfiguration &configuration,
                                    StreamingMetricHandles handles) noexcept;

        StreamingRuntimeOwnerToken owner_;
        StreamingMetricBindingRevision bindingRevision_;
        StreamingRuntimeCompositionRevision ownerRevision_;
        StreamingMetricBounds bounds_;
        StreamingMetricAvailability availability_{StreamingMetricAvailability::Unavailable};
        Telemetry::MetricCollectionLevel level_{Telemetry::MetricCollectionLevel::Off};
        StreamingMetricHandles handles_;
        std::thread::id ownerThread_;
        StreamingMetricBindingState state_{StreamingMetricBindingState::Closed};
        mutable std::uint64_t lastSampleRevision_{};
    };
}  // namespace Horo::WorldStreaming
