#include "Horo/WorldStreaming/WorldStreamingMetrics.h"

#include "WorldStreamingInternal.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::WorldStreaming {
    namespace {
        constexpr std::array<std::string_view, StreamingMetricStageCount> kStageValues{"asset_read", "decode", "provider_stage",
                                                                                       "activation", "retirement"};
        constexpr std::array<std::string_view, StreamingMetricByteFlowCount> kByteFlowValues{"loaded", "retired"};
        constexpr std::array<std::string_view, StreamingMetricQueueCount> kQueueValues{"operations", "completions"};
        constexpr std::array<std::string_view, StreamingMetricResidencyCount> kResidencyValues{"loading", "resident", "active", "evicting"};
        constexpr std::array<std::string_view, StreamingMetricDropReasonCount> kDropValues{"capacity", "cancelled", "stale"};
        constexpr std::array<std::string_view, StreamingMetricFailureReasonCount> kFailureValues{"io", "integrity", "decode", "provider",
                                                                                                 "activation"};

        [[nodiscard]] Telemetry::DimensionDescriptor Dimension(std::string key, const auto &values) {
            Telemetry::DimensionDescriptor result{.key = std::move(key)};
            result.allowedValues.reserve(values.size());
            for (const std::string_view value : values)
                result.allowedValues.emplace_back(value);
            return result;
        }

        [[nodiscard]] Telemetry::InstrumentDescriptor Descriptor(Telemetry::InstrumentKind kind, std::string name, std::string unit,
                                                                 std::string description, Telemetry::MetricCollectionLevel level,
                                                                 std::vector<Telemetry::DimensionDescriptor> dimensions = {}) {
            const auto maximumSeries = dimensions.empty() ? 1U : static_cast<std::uint32_t>(dimensions.front().allowedValues.size());
            return {.kind = kind,
                    .name = std::move(name),
                    .subsystem = "world_streaming",
                    .unit = std::move(unit),
                    .description = std::move(description),
                    .dimensions = std::move(dimensions),
                    .maxSeries = maximumSeries,
                    .minimumCollectionLevel = level};
        }

        template <typename Handle, std::size_t Size>
        void BindHandles(std::array<Handle, Size> &output, const Handle &root, const std::string_view dimension,
                         const std::array<std::string_view, Size> &values) {
            for (std::size_t index = 0; index < Size; ++index) {
                const std::array selection{Telemetry::DimensionValue{dimension, values[index]}};
                output[index] = root.WithDimensions(selection);
            }
        }

        template <typename Handle, std::size_t Size>
        [[nodiscard]] bool AllHandlesAvailable(const std::array<Handle, Size> &handles) noexcept {
            return std::ranges::all_of(handles, [](const Handle &handle) {
                return static_cast<bool>(handle);
            });
        }

        [[nodiscard]] bool CoreHandlesAvailable(const StreamingMetricHandles &handles) noexcept {
            return handles.operationLatency && AllHandlesAvailable(handles.bytes) && AllHandlesAvailable(handles.queueDepth) &&
                   AllHandlesAvailable(handles.residency) && AllHandlesAvailable(handles.drops) && AllHandlesAvailable(handles.failures);
        }

        [[nodiscard]] bool IsKnownAvailability(const StreamingMetricAvailability value) noexcept {
            return value >= StreamingMetricAvailability::Off && value <= StreamingMetricAvailability::Available;
        }

        [[nodiscard]] bool IsKnownRequirement(const StreamingMetricRequirement value) noexcept {
            return value >= StreamingMetricRequirement::Optional && value <= StreamingMetricRequirement::Required;
        }

        [[nodiscard]] bool IsKnownLevel(const Telemetry::MetricCollectionLevel value) noexcept {
            return value >= Telemetry::MetricCollectionLevel::Off && value <= Telemetry::MetricCollectionLevel::Detailed;
        }

        [[nodiscard]] bool BoundsValid(const StreamingMetricBounds &bounds) noexcept {
            return std::isfinite(bounds.maximumLatencySeconds) && bounds.maximumLatencySeconds > 0.0 && bounds.maximumBytesPerSample != 0 &&
                   bounds.maximumQueueDepth != 0 && bounds.maximumResidentCells != 0 && bounds.maximumDropsPerSample != 0 &&
                   bounds.maximumFailuresPerSample != 0;
        }

        [[nodiscard]] bool BindingIdentityValid(const StreamingRuntimeOwnerToken &owner,
                                                const StreamingMetricBindingRevision bindingRevision,
                                                const StreamingRuntimeCompositionRevision ownerRevision,
                                                const StreamingMetricBounds &bounds) noexcept {
            return owner.IsValid() && bindingRevision.IsValid() && ownerRevision.IsValid() && BoundsValid(bounds);
        }

        [[nodiscard]] bool BindingVocabularyKnown(const StreamingMetricAvailability availability,
                                                  const StreamingMetricRequirement requirement,
                                                  const Telemetry::MetricCollectionLevel level) noexcept {
            return IsKnownAvailability(availability) && IsKnownRequirement(requirement) && IsKnownLevel(level);
        }

        [[nodiscard]] Result<void> ValidateAvailabilityPolicy(const StreamingMetricAvailability availability,
                                                              const StreamingMetricRequirement requirement,
                                                              const Telemetry::MetricCollectionLevel level,
                                                              const StreamingMetricHandles &handles) {
            if ((availability == StreamingMetricAvailability::Off) != (level == Telemetry::MetricCollectionLevel::Off))
                return Internal::Failure<void>(WorldStreamingErrors::MetricInvalid);
            if (availability == StreamingMetricAvailability::Unavailable && requirement == StreamingMetricRequirement::Required)
                return Internal::Failure<void>(WorldStreamingErrors::MetricCapabilityUnavailable);
            if (availability != StreamingMetricAvailability::Available)
                return Result<void>::Success();
            const bool detailedAvailable =
                level != Telemetry::MetricCollectionLevel::Detailed || AllHandlesAvailable(handles.stageLatencies);
            return CoreHandlesAvailable(handles) && detailedAvailable
                       ? Result<void>::Success()
                       : Internal::Failure<void>(WorldStreamingErrors::MetricCapabilityUnavailable);
        }

        [[nodiscard]] Result<void> ValidateBindingPolicy(
            const StreamingRuntimeOwnerToken &owner, const StreamingMetricBindingRevision bindingRevision,
            const StreamingRuntimeCompositionRevision ownerRevision, const StreamingMetricBounds &bounds,
            const StreamingMetricAvailability availability, const StreamingMetricRequirement requirement,
            const Telemetry::MetricCollectionLevel level, const StreamingMetricHandles &handles) {
            if (!BindingIdentityValid(owner, bindingRevision, ownerRevision, bounds))
                return Internal::Failure<void>(WorldStreamingErrors::MetricInvalid);
            if (!BindingVocabularyKnown(availability, requirement, level))
                return Internal::Failure<void>(WorldStreamingErrors::MetricUnsupported);
            return ValidateAvailabilityPolicy(availability, requirement, level, handles);
        }

        [[nodiscard]] bool LatencyValuesValid(const StreamingMetricSample &sample) noexcept {
            const auto valid = [](const double value) {
                return std::isfinite(value) && value >= 0.0;
            };
            return valid(sample.operationLatencySeconds) && std::ranges::all_of(sample.stageLatencySeconds, valid);
        }

        template <std::size_t Size>
        [[nodiscard]] bool AllAtMost(const std::array<std::uint64_t, Size> &values, const std::uint64_t maximum) noexcept {
            return std::ranges::all_of(values, [maximum](const std::uint64_t value) {
                return value <= maximum;
            });
        }

        [[nodiscard]] bool ResidencyFits(const StreamingMetricSample &sample, const std::uint64_t maximum) noexcept {
            std::uint64_t total{};
            for (const std::uint64_t count : sample.residency) {
                if (count > maximum - total)
                    return false;
                total += count;
            }
            return true;
        }

        [[nodiscard]] bool SampleFitsBounds(const StreamingMetricSample &sample, const StreamingMetricBounds &bounds) noexcept {
            const bool latenciesFit = sample.operationLatencySeconds <= bounds.maximumLatencySeconds &&
                                      std::ranges::all_of(sample.stageLatencySeconds, [&bounds](const double value) {
                return value <= bounds.maximumLatencySeconds;
            });
            return latenciesFit && AllAtMost(sample.bytes, bounds.maximumBytesPerSample) &&
                   AllAtMost(sample.queueDepth, bounds.maximumQueueDepth) && ResidencyFits(sample, bounds.maximumResidentCells) &&
                   AllAtMost(sample.drops, bounds.maximumDropsPerSample) && AllAtMost(sample.failures, bounds.maximumFailuresPerSample);
        }

        [[nodiscard]] Result<void> ValidateSample(const StreamingMetricSample &sample, const StreamingRuntimeOwnerToken &owner,
                                                  const StreamingRuntimeCompositionRevision ownerRevision,
                                                  const StreamingMetricBounds &bounds) {
            if (sample.schemaVersion != 1 || !sample.owner.IsValid() || !sample.ownerRevision.IsValid() ||
                !sample.sampleRevision.IsValid() || !sample.diagnosticRevision.IsValid() || !LatencyValuesValid(sample))
                return Internal::Failure<void>(WorldStreamingErrors::MetricInvalid);
            if (sample.owner != owner || sample.ownerRevision != ownerRevision)
                return Internal::Failure<void>(WorldStreamingErrors::MetricStale);
            if (!SampleFitsBounds(sample, bounds))
                return Internal::Failure<void>(WorldStreamingErrors::MetricCapacityExceeded);
            return Result<void>::Success();
        }

        template <typename Handle, std::size_t Size>
        void AddNonZero(const std::array<Handle, Size> &handles, const std::array<std::uint64_t, Size> &values) noexcept {
            for (std::size_t index = 0; index < Size; ++index) {
                if (values[index] != 0)
                    handles[index].Add(values[index]);
            }
        }

        void PublishSampleValues(const StreamingMetricSample &sample, const Telemetry::MetricCollectionLevel level,
                                 const StreamingMetricHandles &handles) noexcept {
            handles.operationLatency.Observe(sample.operationLatencySeconds);
            if (level == Telemetry::MetricCollectionLevel::Detailed) {
                for (std::size_t index = 0; index < sample.stageLatencySeconds.size(); ++index)
                    handles.stageLatencies[index].Observe(sample.stageLatencySeconds[index]);
            }
            AddNonZero(handles.bytes, sample.bytes);
            for (std::size_t index = 0; index < sample.queueDepth.size(); ++index)
                handles.queueDepth[index].Set(static_cast<double>(sample.queueDepth[index]));
            for (std::size_t index = 0; index < sample.residency.size(); ++index)
                handles.residency[index].Set(static_cast<double>(sample.residency[index]));
            AddNonZero(handles.drops, sample.drops);
            AddNonZero(handles.failures, sample.failures);
        }
    }  // namespace

    /** @copydoc RegisterStreamingMetricHandles */
    StreamingMetricHandles RegisterStreamingMetricHandles(const Telemetry::MetricCollectionLevel level) {
        using enum Telemetry::MetricCollectionLevel;
        StreamingMetricHandles handles;
        if (level == Off || level > Detailed)
            return handles;

        handles.operationLatency = Telemetry::Runtime::RegisterHistogram(
            Descriptor(Telemetry::InstrumentKind::Histogram, "horo.world_streaming.operation.duration", "seconds",
                       "Complete World Streaming authority operation latency.", Core));

        auto byteRoot = Telemetry::Runtime::RegisterCounter(Descriptor(Telemetry::InstrumentKind::Counter, "horo.world_streaming.bytes",
                                                                       "bytes", "World Streaming byte movement by closed flow category.",
                                                                       Core, {Dimension("flow", kByteFlowValues)}));
        BindHandles(handles.bytes, byteRoot, "flow", kByteFlowValues);

        auto queueRoot = Telemetry::Runtime::RegisterGauge(Descriptor(Telemetry::InstrumentKind::Gauge, "horo.world_streaming.queue.depth",
                                                                      "items", "Current bounded World Streaming queue depth.", Core,
                                                                      {Dimension("queue", kQueueValues)}));
        BindHandles(handles.queueDepth, queueRoot, "queue", kQueueValues);

        auto residencyRoot = Telemetry::Runtime::RegisterGauge(
            Descriptor(Telemetry::InstrumentKind::Gauge, "horo.world_streaming.residency.count", "cells",
                       "Current aggregate cell residency count.", Core, {Dimension("state", kResidencyValues)}));
        BindHandles(handles.residency, residencyRoot, "state", kResidencyValues);

        auto dropRoot = Telemetry::Runtime::RegisterCounter(Descriptor(Telemetry::InstrumentKind::Counter, "horo.world_streaming.drop",
                                                                       "events", "World Streaming observation and admission drops.", Core,
                                                                       {Dimension("reason", kDropValues)}));
        BindHandles(handles.drops, dropRoot, "reason", kDropValues);

        auto failureRoot = Telemetry::Runtime::RegisterCounter(
            Descriptor(Telemetry::InstrumentKind::Counter, "horo.world_streaming.failure", "events",
                       "Terminal World Streaming failures by closed cause.", Core, {Dimension("reason", kFailureValues)}));
        BindHandles(handles.failures, failureRoot, "reason", kFailureValues);

        if (level == Detailed) {
            auto stageRoot = Telemetry::Runtime::RegisterHistogram(
                Descriptor(Telemetry::InstrumentKind::Histogram, "horo.world_streaming.stage.duration", "seconds",
                           "World Streaming pipeline stage latency.", Detailed, {Dimension("stage", kStageValues)}));
            BindHandles(handles.stageLatencies, stageRoot, "stage", kStageValues);
        }
        return handles;
    }

    /** @copydoc WorldStreamingMetricBinding::WorldStreamingMetricBinding */
    WorldStreamingMetricBinding::WorldStreamingMetricBinding(WorldStreamingMetricBinding &&other) noexcept
        : owner_(other.owner_), bindingRevision_(other.bindingRevision_), ownerRevision_(other.ownerRevision_), bounds_(other.bounds_),
          availability_(other.availability_), level_(other.level_), handles_(std::move(other.handles_)), ownerThread_(other.ownerThread_),
          state_(other.state_), lastSampleRevision_(other.lastSampleRevision_) {
        other.state_ = StreamingMetricBindingState::Closed;
        other.lastSampleRevision_ = 0;
    }

    /** @copydoc WorldStreamingMetricBinding::Create */
    Result<WorldStreamingMetricBinding> WorldStreamingMetricBinding::Create(
        const StreamingRuntimeOwnerToken owner, const StreamingMetricBindingRevision bindingRevision,
        const StreamingRuntimeCompositionRevision ownerRevision, const StreamingMetricBounds &bounds,
        const StreamingMetricAvailability availability, const StreamingMetricRequirement requirement,
        const Telemetry::MetricCollectionLevel level, StreamingMetricHandles handles) {
        if (const auto valid =
                ValidateBindingPolicy(owner, bindingRevision, ownerRevision, bounds, availability, requirement, level, handles);
            valid.HasError())
            return Result<WorldStreamingMetricBinding>::Failure(valid.ErrorValue());
        return Result<WorldStreamingMetricBinding>::Success(
            WorldStreamingMetricBinding{owner, bindingRevision, ownerRevision, bounds, availability, level, std::move(handles)});
    }

    /** @copydoc WorldStreamingMetricBinding::Replace */
    Result<void> WorldStreamingMetricBinding::Replace(const StreamingMetricBindingRevision expectedRevision,
                                                      const StreamingMetricBindingRevision successorRevision,
                                                      const StreamingRuntimeCompositionRevision ownerRevision,
                                                      const StreamingMetricBounds &bounds, const StreamingMetricAvailability availability,
                                                      const StreamingMetricRequirement requirement,
                                                      const Telemetry::MetricCollectionLevel level, StreamingMetricHandles handles) {
        if (std::this_thread::get_id() != ownerThread_)
            return Internal::Failure<void>(WorldStreamingErrors::MetricThreadAffinityViolation);
        if (state_ != StreamingMetricBindingState::Active)
            return Internal::Failure<void>(WorldStreamingErrors::MetricLifecycleUnavailable);
        if (expectedRevision != bindingRevision_ || successorRevision.Value() <= bindingRevision_.Value() ||
            ownerRevision.Value() < ownerRevision_.Value())
            return Internal::Failure<void>(WorldStreamingErrors::MetricStale);
        if (const auto valid =
                ValidateBindingPolicy(owner_, successorRevision, ownerRevision, bounds, availability, requirement, level, handles);
            valid.HasError())
            return valid;

        bindingRevision_ = successorRevision;
        ownerRevision_ = ownerRevision;
        bounds_ = bounds;
        availability_ = availability;
        level_ = level;
        handles_ = std::move(handles);
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingMetricBinding::Publish */
    Result<StreamingMetricPublishDisposition> WorldStreamingMetricBinding::Publish(
        const StreamingMetricSample &sample, const StreamingMetricBindingRevision expectedBindingRevision) const {
        if (std::this_thread::get_id() != ownerThread_)
            return Internal::Failure<StreamingMetricPublishDisposition>(WorldStreamingErrors::MetricThreadAffinityViolation);
        if (state_ != StreamingMetricBindingState::Active)
            return Internal::Failure<StreamingMetricPublishDisposition>(WorldStreamingErrors::MetricLifecycleUnavailable);
        if (expectedBindingRevision != bindingRevision_)
            return Internal::Failure<StreamingMetricPublishDisposition>(WorldStreamingErrors::MetricStale);
        if (const auto valid = ValidateSample(sample, owner_, ownerRevision_, bounds_); valid.HasError())
            return Result<StreamingMetricPublishDisposition>::Failure(valid.ErrorValue());
        if (sample.sampleRevision.Value() <= lastSampleRevision_)
            return Internal::Failure<StreamingMetricPublishDisposition>(WorldStreamingErrors::MetricStale);
        lastSampleRevision_ = sample.sampleRevision.Value();
        if (availability_ == StreamingMetricAvailability::Off)
            return Result<StreamingMetricPublishDisposition>::Success(StreamingMetricPublishDisposition::SuppressedByPolicy);
        if (availability_ == StreamingMetricAvailability::Unavailable)
            return Result<StreamingMetricPublishDisposition>::Success(StreamingMetricPublishDisposition::SuppressedUnavailable);

        PublishSampleValues(sample, level_, handles_);
        return Result<StreamingMetricPublishDisposition>::Success(StreamingMetricPublishDisposition::Submitted);
    }

    /** @copydoc WorldStreamingMetricBinding::Cancel */
    Result<void> WorldStreamingMetricBinding::Cancel() {
        if (std::this_thread::get_id() != ownerThread_)
            return Internal::Failure<void>(WorldStreamingErrors::MetricThreadAffinityViolation);
        if (state_ == StreamingMetricBindingState::Active)
            state_ = StreamingMetricBindingState::Cancelled;
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingMetricBinding::Close */
    Result<void> WorldStreamingMetricBinding::Close() {
        if (std::this_thread::get_id() != ownerThread_)
            return Internal::Failure<void>(WorldStreamingErrors::MetricThreadAffinityViolation);
        state_ = StreamingMetricBindingState::Closed;
        handles_ = {};
        return Result<void>::Success();
    }

    /** @copydoc WorldStreamingMetricBinding::Owner */
    const StreamingRuntimeOwnerToken &WorldStreamingMetricBinding::Owner() const noexcept {
        return owner_;
    }

    /** @copydoc WorldStreamingMetricBinding::Revision */
    StreamingMetricBindingRevision WorldStreamingMetricBinding::Revision() const noexcept {
        return bindingRevision_;
    }

    /** @copydoc WorldStreamingMetricBinding::OwnerRevision */
    StreamingRuntimeCompositionRevision WorldStreamingMetricBinding::OwnerRevision() const noexcept {
        return ownerRevision_;
    }

    /** @copydoc WorldStreamingMetricBinding::State */
    StreamingMetricBindingState WorldStreamingMetricBinding::State() const noexcept {
        return state_;
    }

    /** @copydoc WorldStreamingMetricBinding::WorldStreamingMetricBinding */
    WorldStreamingMetricBinding::WorldStreamingMetricBinding(const StreamingRuntimeOwnerToken owner,
                                                             const StreamingMetricBindingRevision bindingRevision,
                                                             const StreamingRuntimeCompositionRevision ownerRevision,
                                                             const StreamingMetricBounds &bounds,
                                                             const StreamingMetricAvailability availability,
                                                             const Telemetry::MetricCollectionLevel level,
                                                             StreamingMetricHandles handles) noexcept
        : owner_(owner), bindingRevision_(bindingRevision), ownerRevision_(ownerRevision), bounds_(bounds), availability_(availability),
          level_(level), handles_(std::move(handles)), ownerThread_(std::this_thread::get_id()),
          state_(StreamingMetricBindingState::Active) {}
}  // namespace Horo::WorldStreaming
