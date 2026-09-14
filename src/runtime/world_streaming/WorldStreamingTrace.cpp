#include "Horo/WorldStreaming/WorldStreamingTrace.h"

#include "WorldStreamingInternal.h"

#include <algorithm>
#include <new>
#include <string>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        [[nodiscard]] bool KnownStage(const StreamingTraceStage stage) noexcept {
            return stage >= StreamingTraceStage::SourceEvaluation && stage < StreamingTraceStage::Count;
        }

        [[nodiscard]] bool TerminalStatus(const Telemetry::SpanStatus status) noexcept {
            return status >= Telemetry::SpanStatus::Succeeded && status <= Telemetry::SpanStatus::TimedOut;
        }

        [[nodiscard]] bool ConfigurationValid(const StreamingTraceConfiguration &configuration) noexcept {
            return configuration.owner.IsValid() && configuration.ownerRevision.IsValid() && configuration.bindingRevision.IsValid() &&
                   configuration.operation.IsValid() && configuration.maximumSpans != 0;
        }

        [[nodiscard]] std::uint8_t SubjectMask(const StreamingTraceSubject &subject) noexcept {
            return static_cast<std::uint8_t>(subject.source.IsValid()) | static_cast<std::uint8_t>(subject.cellOperation.IsValid()) << 1U |
                   static_cast<std::uint8_t>(subject.assetRequest.IsValid()) << 2U |
                   static_cast<std::uint8_t>(subject.activation.IsValid()) << 3U |
                   static_cast<std::uint8_t>(subject.provider.IsValid()) << 4U;
        }

        [[nodiscard]] bool SubjectValid(const StreamingTraceStageBegin &begin) noexcept {
            const auto mask = SubjectMask(begin.subject);
            switch (begin.stage) {
                case StreamingTraceStage::SourceEvaluation:
                    return mask == 1U;
                case StreamingTraceStage::AssetRequest:
                    return mask == 6U;
                case StreamingTraceStage::Activation:
                    return mask == 10U;
                case StreamingTraceStage::ProviderWork:
                    return mask == 18U;
                case StreamingTraceStage::Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateStageBegin(const StreamingTraceStageBegin &begin, const StreamingTraceOperationId operation) {
            if (!begin.span.IsValid() || !begin.parentRoot.IsValid() || begin.parentRoot != operation)
                return Internal::Failure<void>(WorldStreamingErrors::TraceInvalid);
            if (!KnownStage(begin.stage))
                return Internal::Failure<void>(WorldStreamingErrors::TraceUnsupported);
            if (!SubjectValid(begin))
                return Internal::Failure<void>(WorldStreamingErrors::TraceInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateStageAdmission(std::span<const StreamingTraceStageSnapshot> snapshots,
                                                          const StreamingTraceStageBegin &begin, const std::size_t maximumSpans) {
            const auto spanMatch = [&begin](const StreamingTraceStageSnapshot &snapshot) {
                return snapshot.begin.span == begin.span;
            };
            if (std::ranges::any_of(snapshots, spanMatch))
                return Internal::Failure<void>(WorldStreamingErrors::TraceIdentityConflict);
            if (snapshots.size() == maximumSpans)
                return Internal::Failure<void>(WorldStreamingErrors::TraceCapacityExceeded);
            if (!begin.parentSpan.IsValid())
                return Result<void>::Success();
            const auto parentMatch = [&begin](const StreamingTraceStageSnapshot &snapshot) {
                return snapshot.begin.span == begin.parentSpan;
            };
            return std::ranges::any_of(snapshots, parentMatch) ? Result<void>::Success()
                                                               : Internal::Failure<void>(WorldStreamingErrors::TraceIdentityConflict);
        }

        [[nodiscard]] bool HasActiveStage(std::span<const StreamingTraceStageSnapshot> snapshots) noexcept {
            return std::ranges::any_of(snapshots, [](const StreamingTraceStageSnapshot &snapshot) {
                return snapshot.status == Telemetry::SpanStatus::Unset;
            });
        }

        [[nodiscard]] Result<void> ValidateSuccessor(const StreamingTraceConfiguration &current,
                                                     const StreamingTraceConfiguration &successor,
                                                     const StreamingTraceBindingRevision expectedRevision) {
            if (!ConfigurationValid(successor))
                return Internal::Failure<void>(WorldStreamingErrors::TraceInvalid);
            if (successor.maximumSpans > StreamingTraceConfiguration::MaximumSpanCapacity)
                return Internal::Failure<void>(WorldStreamingErrors::TraceCapacityExceeded);
            if (expectedRevision != current.bindingRevision || successor.owner != current.owner)
                return Internal::Failure<void>(WorldStreamingErrors::TraceStale);
            if (successor.bindingRevision.Value() <= current.bindingRevision.Value() ||
                successor.ownerRevision.Value() < current.ownerRevision.Value())
                return Internal::Failure<void>(WorldStreamingErrors::TraceStale);
            if (successor.operation == current.operation)
                return Internal::Failure<void>(WorldStreamingErrors::TraceIdentityConflict);
            return Result<void>::Success();
        }

        [[nodiscard]] const char *StageName(const StreamingTraceStage stage) noexcept {
            switch (stage) {
                case StreamingTraceStage::SourceEvaluation:
                    return "world_streaming.source_evaluation";
                case StreamingTraceStage::AssetRequest:
                    return "world_streaming.asset_request";
                case StreamingTraceStage::Activation:
                    return "world_streaming.activation";
                case StreamingTraceStage::ProviderWork:
                    return "world_streaming.provider_work";
                case StreamingTraceStage::Count:
                    return "world_streaming.invalid";
            }
            return "world_streaming.invalid";
        }

        void AddSubjectFields(std::vector<Telemetry::Field> &fields, const StreamingTraceSubject &subject) {
            if (subject.source.IsValid())
                fields.push_back({"source.id", subject.source.Value()});
            if (subject.cellOperation.IsValid()) {
                fields.push_back({"cell.operation.id", subject.cellOperation.operation.Value()});
                fields.push_back({"cell.generation", subject.cellOperation.fence.generation.Value()});
            }
            if (subject.assetRequest.IsValid())
                fields.push_back({"asset_request.id", subject.assetRequest.Value()});
            if (subject.activation.IsValid())
                fields.push_back({"activation.id", subject.activation.Value()});
            if (subject.provider.IsValid())
                fields.push_back({"provider.id", subject.provider.Value()});
        }
    }  // namespace

    /** @copydoc WorldStreamingTrace::WorldStreamingTrace */
    WorldStreamingTrace::WorldStreamingTrace(const StreamingTraceConfiguration &configuration)
        : configuration_(configuration), ownerThread_(std::this_thread::get_id()), lifecycle_(StreamingTraceLifecycle::Active) {
        snapshots_.reserve(configuration.maximumSpans);
        startedAt_.reserve(configuration.maximumSpans);
    }

    /** @copydoc WorldStreamingTrace::WorldStreamingTrace */
    WorldStreamingTrace::WorldStreamingTrace(WorldStreamingTrace &&other) noexcept
        : configuration_(other.configuration_), snapshots_(std::move(other.snapshots_)), startedAt_(std::move(other.startedAt_)),
          ownerThread_(other.ownerThread_), lifecycle_(other.lifecycle_) {
        other.lifecycle_ = StreamingTraceLifecycle::Closed;
    }

    /** @copydoc WorldStreamingTrace::Create */
    Result<WorldStreamingTrace> WorldStreamingTrace::Create(const StreamingTraceConfiguration &configuration) {
        if (!ConfigurationValid(configuration))
            return Internal::Failure<WorldStreamingTrace>(WorldStreamingErrors::TraceInvalid);
        if (configuration.maximumSpans > StreamingTraceConfiguration::MaximumSpanCapacity)
            return Internal::Failure<WorldStreamingTrace>(WorldStreamingErrors::TraceCapacityExceeded);
        try {
            return Result<WorldStreamingTrace>::Success(WorldStreamingTrace{configuration});
        } catch (const std::bad_alloc &) {
            return Internal::Failure<WorldStreamingTrace>(WorldStreamingErrors::TraceStorageUnavailable);
        }
    }

    /** @copydoc WorldStreamingTrace::Begin */
    Result<void> WorldStreamingTrace::Begin(const StreamingTraceStageBegin &begin, const StreamingTraceBindingRevision expectedRevision) {
        if (auto mutation = ValidateActiveMutation(); mutation.HasError())
            return mutation;
        if (expectedRevision != configuration_.bindingRevision)
            return Internal::Failure<void>(WorldStreamingErrors::TraceStale);
        if (auto validation = ValidateStageBegin(begin, configuration_.operation); validation.HasError())
            return validation;
        if (auto admission = ValidateStageAdmission(snapshots_, begin, configuration_.maximumSpans); admission.HasError())
            return admission;
        snapshots_.push_back({.begin = begin});
        startedAt_.push_back(std::chrono::steady_clock::now());
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingTrace::Complete */
    Result<StreamingTracePublishDisposition> WorldStreamingTrace::Complete(const StreamingTraceSpanId span,
                                                                           const Telemetry::SpanStatus status,
                                                                           const StreamingTraceBindingRevision expectedRevision) {
        if (std::this_thread::get_id() != ownerThread_)
            return Internal::Failure<StreamingTracePublishDisposition>(WorldStreamingErrors::TraceThreadAffinityViolation);
        if (lifecycle_ == StreamingTraceLifecycle::Closed)
            return Internal::Failure<StreamingTracePublishDisposition>(WorldStreamingErrors::TraceLifecycleUnavailable);
        if (expectedRevision != configuration_.bindingRevision)
            return Internal::Failure<StreamingTracePublishDisposition>(WorldStreamingErrors::TraceStale);
        if (!span.IsValid())
            return Internal::Failure<StreamingTracePublishDisposition>(WorldStreamingErrors::TraceInvalid);
        if (!TerminalStatus(status))
            return Internal::Failure<StreamingTracePublishDisposition>(WorldStreamingErrors::TraceUnsupported);
        const auto found = std::ranges::find_if(snapshots_, [span](const StreamingTraceStageSnapshot &snapshot) {
            return snapshot.begin.span == span;
        });
        if (found == snapshots_.end())
            return Internal::Failure<StreamingTracePublishDisposition>(WorldStreamingErrors::TraceStale);
        if (found->status != Telemetry::SpanStatus::Unset)
            return Internal::Failure<StreamingTracePublishDisposition>(WorldStreamingErrors::TraceLifecycleUnavailable);
        const auto index = static_cast<std::size_t>(std::distance(snapshots_.begin(), found));
        return Result<StreamingTracePublishDisposition>::Success(Emit(index, status));
    }

    /** @copydoc WorldStreamingTrace::Replace */
    Result<void> WorldStreamingTrace::Replace(const StreamingTraceBindingRevision expectedRevision,
                                              const StreamingTraceConfiguration &successor) {
        if (auto mutation = ValidateActiveMutation(); mutation.HasError())
            return mutation;
        if (auto validation = ValidateSuccessor(configuration_, successor, expectedRevision); validation.HasError())
            return validation;
        if (HasActiveStage(snapshots_))
            return Internal::Failure<void>(WorldStreamingErrors::TraceLifecycleUnavailable);
        try {
            std::vector<StreamingTraceStageSnapshot> successorSnapshots;
            std::vector<std::chrono::steady_clock::time_point> successorStarts;
            successorSnapshots.reserve(successor.maximumSpans);
            successorStarts.reserve(successor.maximumSpans);
            configuration_ = successor;
            snapshots_.swap(successorSnapshots);
            startedAt_.swap(successorStarts);
        } catch (const std::bad_alloc &) {
            return Internal::Failure<void>(WorldStreamingErrors::TraceStorageUnavailable);
        }
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingTrace::Cancel */
    Result<StreamingTraceEmissionSummary> WorldStreamingTrace::Cancel() {
        if (std::this_thread::get_id() != ownerThread_)
            return Internal::Failure<StreamingTraceEmissionSummary>(WorldStreamingErrors::TraceThreadAffinityViolation);
        if (lifecycle_ == StreamingTraceLifecycle::Closed)
            return Internal::Failure<StreamingTraceEmissionSummary>(WorldStreamingErrors::TraceLifecycleUnavailable);
        lifecycle_ = StreamingTraceLifecycle::Cancelling;
        return Result<StreamingTraceEmissionSummary>::Success(CancelActive());
    }

    /** @copydoc WorldStreamingTrace::Close */
    Result<StreamingTraceEmissionSummary> WorldStreamingTrace::Close() {
        if (std::this_thread::get_id() != ownerThread_)
            return Internal::Failure<StreamingTraceEmissionSummary>(WorldStreamingErrors::TraceThreadAffinityViolation);
        if (lifecycle_ == StreamingTraceLifecycle::Closed)
            return Result<StreamingTraceEmissionSummary>::Success({});
        lifecycle_ = StreamingTraceLifecycle::Cancelling;
        auto summary = CancelActive();
        lifecycle_ = StreamingTraceLifecycle::Closed;
        return Result<StreamingTraceEmissionSummary>::Success(summary);
    }

    /** @copydoc WorldStreamingTrace::Owner */
    const StreamingRuntimeOwnerToken &WorldStreamingTrace::Owner() const noexcept {
        return configuration_.owner;
    }

    /** @copydoc WorldStreamingTrace::Revision */
    StreamingTraceBindingRevision WorldStreamingTrace::Revision() const noexcept {
        return configuration_.bindingRevision;
    }

    /** @copydoc WorldStreamingTrace::Operation */
    StreamingTraceOperationId WorldStreamingTrace::Operation() const noexcept {
        return configuration_.operation;
    }

    /** @copydoc WorldStreamingTrace::Lifecycle */
    StreamingTraceLifecycle WorldStreamingTrace::Lifecycle() const noexcept {
        return lifecycle_;
    }

    /** @copydoc WorldStreamingTrace::Stages */
    std::span<const StreamingTraceStageSnapshot> WorldStreamingTrace::Stages() const noexcept {
        return snapshots_;
    }

    /** @copydoc WorldStreamingTrace::ValidateActiveMutation */
    Result<void> WorldStreamingTrace::ValidateActiveMutation() const {
        if (std::this_thread::get_id() != ownerThread_)
            return Internal::Failure<void>(WorldStreamingErrors::TraceThreadAffinityViolation);
        if (lifecycle_ != StreamingTraceLifecycle::Active)
            return Internal::Failure<void>(WorldStreamingErrors::TraceLifecycleUnavailable);
        return Result<void>::Success();
    }

    StreamingTracePublishDisposition WorldStreamingTrace::Emit(const std::size_t index, const Telemetry::SpanStatus status) noexcept {
        auto &snapshot = snapshots_[index];
        snapshot.status = status;
        snapshot.duration = std::chrono::steady_clock::now() - startedAt_[index];
        try {
            std::vector<Telemetry::Field> fields;
            fields.reserve(10);
            fields.push_back({"trace.root.id", configuration_.operation.Value()});
            fields.push_back({"trace.binding.revision", configuration_.bindingRevision.Value()});
            fields.push_back({"owner.id", configuration_.owner.owner.Value()});
            fields.push_back({"owner.revision", configuration_.ownerRevision.Value()});
            AddSubjectFields(fields, snapshot.begin.subject);
            Telemetry::SpanRecord span{.operationId = snapshot.begin.span.Value(),
                                       .parentOperationId = snapshot.begin.parentSpan.IsValid() ? snapshot.begin.parentSpan.Value()
                                                                                                : snapshot.begin.parentRoot.Value(),
                                       .name = StageName(snapshot.begin.stage),
                                       .status = status,
                                       .duration = snapshot.duration,
                                       .fields = std::move(fields)};
            Telemetry::Record record{.subsystem = "world_streaming", .context = Log::CaptureLogContext(), .payload = std::move(span)};
            snapshot.publication = Telemetry::Runtime::EmitRecord(std::move(record)) ? StreamingTracePublishDisposition::Submitted
                                                                                     : StreamingTracePublishDisposition::Dropped;
        } catch (...) {
            snapshot.publication = StreamingTracePublishDisposition::Dropped;
        }
        return snapshot.publication;
    }

    StreamingTraceEmissionSummary WorldStreamingTrace::CancelActive() noexcept {
        StreamingTraceEmissionSummary summary;
        for (std::size_t index = 0; index < snapshots_.size(); ++index) {
            if (snapshots_[index].status != Telemetry::SpanStatus::Unset)
                continue;
            if (Emit(index, Telemetry::SpanStatus::Cancelled) == StreamingTracePublishDisposition::Submitted)
                ++summary.submitted;
            else
                ++summary.dropped;
        }
        return summary;
    }
}  // namespace Horo::WorldStreaming
