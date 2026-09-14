#pragma once

/**
 * @file WorldStreamingTrace.h
 * @brief Bounded owner-scoped correlation of World Streaming lifecycle spans.
 */

#include "Horo/Foundation/StrongId.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"
#include "Horo/WorldStreaming/StreamingCellActivation.h"
#include "Horo/WorldStreaming/StreamingCellAssetRequest.h"
#include "Horo/WorldStreaming/StreamingSourceDescriptor.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Tag separating trace roots from cell-operation and span identities. */
        struct StreamingTraceOperationIdTag;
        /** @brief Tag separating trace spans from roots and subsystem operation identities. */
        struct StreamingTraceSpanIdTag;
        /** @brief Tag separating trace binding revisions from owner and diagnostic revisions. */
        struct StreamingTraceBindingRevisionTag;
    }  // namespace Detail

    /** @brief Stable process-local root identity shared by one correlated streaming workflow. */
    using StreamingTraceOperationId =
        Foundation::Detail::NonZeroId64<Detail::StreamingTraceOperationIdTag, WorldStreamingErrors::TraceInvalid>;
    /** @brief Stable process-local identity of one stage span beneath a trace root. */
    using StreamingTraceSpanId = Foundation::Detail::NonZeroId64<Detail::StreamingTraceSpanIdTag, WorldStreamingErrors::TraceInvalid>;
    /** @brief Non-zero compare-and-swap revision for trace binding replacement. */
    using StreamingTraceBindingRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingTraceBindingRevisionTag, WorldStreamingErrors::TraceInvalid>;

    /** @brief Closed low-cardinality stage vocabulary for the streaming trace graph. */
    enum class StreamingTraceStage : std::uint8_t {
        SourceEvaluation,
        AssetRequest,
        Activation,
        ProviderWork,
        Count,
    };

    /** @brief Owner-side lifecycle of one bounded trace session. */
    enum class StreamingTraceLifecycle : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count,
    };

    /** @brief Whether a terminal trace span reached Telemetry or was observability-only loss. */
    enum class StreamingTracePublishDisposition : std::uint8_t {
        Submitted,
        Dropped,
    };

    /** @brief Typed stage-specific identities carried as span fields, never metric dimensions. */
    struct StreamingTraceSubject final {
        StreamingSourceId source{};                   /**< Required only for source evaluation. */
        StreamingCellOperationHandle cellOperation{}; /**< Required for asset, activation and provider stages. */
        StreamingCellAssetRequestId assetRequest{};   /**< Required only for asset request spans. */
        StreamingCellActivationId activation{};       /**< Required only for activation spans. */
        StreamingRuntimeServiceId provider{};         /**< Required only for provider work spans. */
    };

    /** @brief Complete immutable binding facts for one trace root. */
    struct StreamingTraceConfiguration final {
        /** @brief Hard implementation ceiling for retained stage spans. */
        static constexpr std::size_t MaximumSpanCapacity = 4096;

        StreamingRuntimeOwnerToken owner{};                /**< Exact mounted authority lifetime. */
        StreamingRuntimeCompositionRevision ownerRevision; /**< Admitted runtime composition revision. */
        StreamingTraceBindingRevision bindingRevision;     /**< Host-issued binding revision. */
        StreamingTraceOperationId operation{};             /**< Stable correlated trace root. */
        std::size_t maximumSpans{};                        /**< Positive ceiling no larger than MaximumSpanCapacity. */
    };

    /** @brief One validated stage admission beneath a stable trace root or earlier stage. */
    struct StreamingTraceStageBegin final {
        StreamingTraceSpanId span{};            /**< Unique span identity within the current binding. */
        StreamingTraceOperationId parentRoot{}; /**< Root parent; required when parentSpan is absent. */
        StreamingTraceSpanId parentSpan{};      /**< Optional earlier stage parent within this binding. */
        StreamingTraceStage stage{};            /**< Closed stage category. */
        StreamingTraceSubject subject{};        /**< Stage-specific typed correlation identities. */
    };

    /** @brief Immutable bounded evidence retained for one admitted stage. */
    struct StreamingTraceStageSnapshot final {
        StreamingTraceStageBegin begin{};                           /**< Original validated stage facts. */
        Telemetry::SpanStatus status{Telemetry::SpanStatus::Unset}; /**< Unset while active, terminal afterward. */
        std::chrono::nanoseconds duration{};                        /**< Owner-measured monotonic duration. */
        StreamingTracePublishDisposition publication{StreamingTracePublishDisposition::Dropped}; /**< Emission outcome. */
    };

    /** @brief Aggregate best-effort emission outcome for cancellation or shutdown. */
    struct StreamingTraceEmissionSummary final {
        std::size_t submitted{}; /**< Terminal spans admitted by Telemetry. */
        std::size_t dropped{};   /**< Terminal spans rejected by Telemetry. */
    };

    /** @brief Unique owner-thread trace graph with bounded storage and revision-fenced replacement. */
    class WorldStreamingTrace final {
    public:
        WorldStreamingTrace(const WorldStreamingTrace &) = delete;
        WorldStreamingTrace &operator=(const WorldStreamingTrace &) = delete;
        /** @brief Transfers unique ownership while retaining declaring owner-thread affinity. */
        WorldStreamingTrace(WorldStreamingTrace &&other) noexcept;
        WorldStreamingTrace &operator=(WorldStreamingTrace &&) = delete;

        /**
         * @brief Creates an empty active trace without emitting or registering Telemetry state.
         * @param configuration Exact owner, revisions, root identity and storage ceiling.
         * @return Active trace or a typed invalid, unsupported or capacity error.
         * @pre Called on the authority thread that will mutate this trace.
         */
        [[nodiscard]] static Result<WorldStreamingTrace> Create(const StreamingTraceConfiguration &configuration);

        /**
         * @brief Admits one unique bounded stage span transactionally.
         * @param begin Exact parentage, stage and typed subject identities.
         * @param expectedRevision Current binding revision observed by the caller.
         * @return Success or a typed invalid, stale, unsupported, capacity or lifecycle error.
         * @post Failure neither retains the span nor emits a record.
         */
        [[nodiscard]] Result<void> Begin(const StreamingTraceStageBegin &begin, StreamingTraceBindingRevision expectedRevision);

        /**
         * @brief Terminates and emits one active stage exactly once.
         * @param span Exact admitted stage span identity.
         * @param status Succeeded, failed, cancelled or timed-out terminal status.
         * @param expectedRevision Current binding revision observed by the caller.
         * @return Submitted/dropped disposition or a typed invalid, stale or lifecycle error.
         * @post A valid terminal transition remains terminal even when Telemetry drops the record.
         */
        [[nodiscard]] Result<StreamingTracePublishDisposition> Complete(StreamingTraceSpanId span, Telemetry::SpanStatus status,
                                                                        StreamingTraceBindingRevision expectedRevision);

        /**
         * @brief Replaces an idle binding with a strictly newer configuration for the same owner.
         * @param expectedRevision Exact current binding revision.
         * @param successor Complete successor configuration.
         * @return Success or a typed stale, invalid, capacity or lifecycle error.
         * @post Failure leaves all current configuration and evidence unchanged.
         */
        [[nodiscard]] Result<void> Replace(StreamingTraceBindingRevision expectedRevision, const StreamingTraceConfiguration &successor);

        /** @brief Cancels every active span and closes new admission. @return Emission summary or thread-affinity error. */
        [[nodiscard]] Result<StreamingTraceEmissionSummary> Cancel();
        /** @brief Cancels remaining work and closes the trace idempotently. @return Emission summary or thread-affinity error. */
        [[nodiscard]] Result<StreamingTraceEmissionSummary> Close();

        /** @brief Returns the exact owner lifetime. @return Immutable owner token. */
        [[nodiscard]] const StreamingRuntimeOwnerToken &Owner() const noexcept;
        /** @brief Returns the current binding revision. @return Non-zero revision. */
        [[nodiscard]] StreamingTraceBindingRevision Revision() const noexcept;
        /** @brief Returns the current root operation. @return Stable trace root identity. */
        [[nodiscard]] StreamingTraceOperationId Operation() const noexcept;
        /** @brief Returns the owner-side lifecycle. @return Active, cancelling or closed. */
        [[nodiscard]] StreamingTraceLifecycle Lifecycle() const noexcept;
        /** @brief Returns all admitted bounded stage evidence. @return View valid until mutation or destruction. */
        [[nodiscard]] std::span<const StreamingTraceStageSnapshot> Stages() const noexcept;

    private:
        explicit WorldStreamingTrace(const StreamingTraceConfiguration &configuration);
        /** @brief Validates the declaring owner thread and active lifecycle. @return Success or typed lifecycle/thread error. */
        [[nodiscard]] Result<void> ValidateActiveMutation() const;
        [[nodiscard]] StreamingTracePublishDisposition Emit(std::size_t index, Telemetry::SpanStatus status) noexcept;
        [[nodiscard]] StreamingTraceEmissionSummary CancelActive() noexcept;

        StreamingTraceConfiguration configuration_{};
        std::vector<StreamingTraceStageSnapshot> snapshots_;
        std::vector<std::chrono::steady_clock::time_point> startedAt_;
        std::vector<Telemetry::Record> preparedRecords_;
        std::unordered_map<std::uint64_t, std::size_t> spanIndices_;
        std::thread::id ownerThread_;
        StreamingTraceLifecycle lifecycle_{StreamingTraceLifecycle::Closed};
    };
}  // namespace Horo::WorldStreaming
