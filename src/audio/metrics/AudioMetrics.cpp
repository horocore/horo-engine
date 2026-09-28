#include "Horo/Audio/AudioMetrics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace Horo::Audio {
    namespace {
        template <typename Enum> [[nodiscard]] constexpr std::size_t Index(const Enum value) noexcept {
            return static_cast<std::size_t>(value);
        }

        template <typename Enum> [[nodiscard]] constexpr bool Known(const Enum value) noexcept {
            return Index(value) < Index(Enum::Count);
        }

        [[nodiscard]] bool Monotonic(const AudioEventQueueStats &next, const AudioEventQueueStats &previous) noexcept {
            return next.terminalEvents >= previous.terminalEvents && next.deviceEvents >= previous.deviceEvents &&
                   next.droppedTelemetry >= previous.droppedTelemetry && next.criticalRetries >= previous.criticalRetries &&
                   next.duplicateTerminals >= previous.duplicateTerminals;
        }

        [[nodiscard]] bool Monotonic(const AudioExtractionStats &next, const AudioExtractionStats &previous) noexcept {
            return next.published >= previous.published && next.coalesced >= previous.coalesced && next.dropped >= previous.dropped &&
                   next.rateLimited >= previous.rateLimited && next.depth <= MaximumAudioExtractionSlots;
        }
    }  // namespace

    /** @copydoc AudioMetrics::AudioMetrics */
    AudioMetrics::AudioMetrics(const std::uint64_t generation, const bool enabled) noexcept : ownerThread_(std::this_thread::get_id()) {
        current_.ownerGeneration = generation;
        current_.enabled = enabled && generation != 0;
        published_ = current_;
    }

    /** @copydoc AudioMetrics::IsCollecting */
    bool AudioMetrics::IsCollecting() const noexcept {
        return std::this_thread::get_id() == ownerThread_ && current_.enabled && !current_.closed;
    }

    void AudioMetrics::AddSaturating(std::uint64_t &target, const std::uint64_t delta, bool &saturated) noexcept {
        if (delta > std::numeric_limits<std::uint64_t>::max() - target) {
            target = std::numeric_limits<std::uint64_t>::max();
            saturated = true;
        } else {
            target += delta;
        }
    }

    bool AudioMetrics::Invalid() noexcept {
        AddSaturating(current_.invalidObservations, 1, current_.saturated);
        return false;
    }

    /** @copydoc AudioMetrics::ObserveEventQueue */
    bool AudioMetrics::ObserveEventQueue(const std::uint64_t sourceGeneration, const AudioEventQueueStats &stats) noexcept {
        if (!IsCollecting())
            return false;
        if (sourceGeneration == 0 || sourceGeneration < eventSourceGeneration_)
            return Invalid();
        const AudioEventQueueStats previous = sourceGeneration == eventSourceGeneration_ ? lastEventStats_ : AudioEventQueueStats{};
        if (!Monotonic(stats, previous))
            return Invalid();
        AddSaturating(current_.counters[Index(AudioMetricCounter::EventTelemetryDrops)], stats.droppedTelemetry - previous.droppedTelemetry,
                      current_.saturated);
        AddSaturating(current_.counters[Index(AudioMetricCounter::EventCriticalRetries)], stats.criticalRetries - previous.criticalRetries,
                      current_.saturated);
        AddSaturating(current_.counters[Index(AudioMetricCounter::EventDuplicateTerminals)],
                      stats.duplicateTerminals - previous.duplicateTerminals, current_.saturated);
        current_.gauges[Index(AudioMetricGauge::EventQueueDepth)] = stats.depth;
        current_.gaugeAvailable[Index(AudioMetricGauge::EventQueueDepth)] = true;
        lastEventStats_ = stats;
        eventSourceGeneration_ = sourceGeneration;
        return true;
    }

    /** @copydoc AudioMetrics::ObserveExtractionQueue */
    bool AudioMetrics::ObserveExtractionQueue(const std::uint64_t sourceGeneration, const AudioExtractionStats &stats) noexcept {
        if (!IsCollecting())
            return false;
        if (sourceGeneration == 0 || sourceGeneration < extractionSourceGeneration_)
            return Invalid();
        const auto previous = sourceGeneration == extractionSourceGeneration_ ? lastExtractionStats_ : AudioExtractionStats{};
        if (!Monotonic(stats, previous))
            return Invalid();
        const std::array deltas{stats.dropped - previous.dropped, stats.coalesced - previous.coalesced,
                                stats.rateLimited - previous.rateLimited};
        const std::array kinds{AudioMetricCounter::ExtractionDrops, AudioMetricCounter::ExtractionCoalesced,
                               AudioMetricCounter::ExtractionRateLimited};
        for (std::size_t index = 0; index < deltas.size(); ++index)
            AddSaturating(current_.counters[Index(kinds[index])], deltas[index], current_.saturated);
        current_.gauges[Index(AudioMetricGauge::ExtractionQueueDepth)] = stats.depth;
        current_.gaugeAvailable[Index(AudioMetricGauge::ExtractionQueueDepth)] = true;
        lastExtractionStats_ = stats;
        extractionSourceGeneration_ = sourceGeneration;
        return true;
    }

    /** @copydoc AudioMetrics::ObserveExtractedRecord */
    bool AudioMetrics::ObserveExtractedRecord(const AudioExtractionRecord &record) noexcept {
        if (!IsCollecting())
            return false;
        if (record.samples == 0 || !std::isfinite(record.seconds) || record.seconds < 0.0)
            return Invalid();
        if (const auto value = static_cast<std::size_t>(record.kind); value < AudioMetricTimingCount)
            return ObserveTiming(static_cast<AudioMetricTiming>(value), record.seconds);
        using enum AudioExtractionKind;
        AudioMetricCounter counter;
        switch (record.kind) {
            case DeadlineOverrun:
                counter = AudioMetricCounter::CallbackDeadlineOverruns;
                break;
            case AllocationAttempt:
                if (record.seconds != 0.0)
                    return Invalid();
                counter = AudioMetricCounter::CallbackAllocationAttempts;
                break;
            case LockAttempt:
                if (record.seconds != 0.0)
                    return Invalid();
                counter = AudioMetricCounter::CallbackLockAttempts;
                break;
            default:
                return Invalid();
        }
        AddSaturating(current_.counters[Index(counter)], record.samples, current_.saturated);
        return true;
    }

    /** @copydoc AudioMetrics::ObserveCommandQueue */
    bool AudioMetrics::ObserveCommandQueue(const AudioCommandStagingStats &stats) noexcept {
        if (!IsCollecting())
            return false;
        const std::uint64_t depth = static_cast<std::uint64_t>(stats.ingressDepth) + stats.callbackDepth;
        if (depth > MaximumAudioCommandSlots * 2ULL)
            return Invalid();
        current_.gauges[Index(AudioMetricGauge::CommandQueueDepth)] = static_cast<double>(depth);
        current_.gaugeAvailable[Index(AudioMetricGauge::CommandQueueDepth)] = true;
        return true;
    }

    /** @copydoc AudioMetrics::ObserveCommandAdmission */
    bool AudioMetrics::ObserveCommandAdmission(const AudioCommandStagingStatus status) noexcept {
        using enum AudioCommandStagingStatus;
        if (!IsCollecting())
            return false;
        switch (status) {
            case OrdinaryFull:
                AddSaturating(current_.counters[Index(AudioMetricCounter::CommandOrdinaryRejections)], 1, current_.saturated);
                return true;
            case CriticalRetry:
                AddSaturating(current_.counters[Index(AudioMetricCounter::CommandCriticalRetries)], 1, current_.saturated);
                return true;
            case Ok:
            case Coalesced:
            case Busy:
            case InvalidCommand:
            case InvalidScene:
            case Closed:
            case SequenceExhausted:
            case AlreadyStaged:
            case OutputFull:
            case Inactive:
            case ProtocolError:
                return true;
        }
        return Invalid();
    }

    /** @copydoc AudioMetrics::ObserveCommandPump */
    bool AudioMetrics::ObserveCommandPump(const AudioCommandStagingStatus status) noexcept {
        using enum AudioCommandStagingStatus;
        if (!IsCollecting())
            return false;
        if (status == OutputFull) {
            AddSaturating(current_.counters[Index(AudioMetricCounter::CommandOutputStalls)], 1, current_.saturated);
            return true;
        }
        if (status == Ok || status == Busy || status == ProtocolError || status == Inactive)
            return true;
        return Invalid();
    }

    /** @copydoc AudioMetrics::ObserveControlEvent */
    bool AudioMetrics::ObserveControlEvent(const AudioControlEventRecord &record) noexcept {
        using enum AudioMetricCounter;
        if (!IsCollecting())
            return false;
        const auto *event = std::get_if<AudioCallbackEvent>(&record.event);
        if (!event)
            return true;
        if (std::holds_alternative<AudioCallbackUnderrun>(event->fact))
            AddSaturating(current_.counters[Index(Underruns)], 1, current_.saturated);
        if (const auto *fault = std::get_if<AudioCallbackFault>(&event->fact)) {
            using enum AudioCallbackFaultCode;
            if (fault->code == None || fault->code > BackendFailure)
                return Invalid();
            AddSaturating(current_.counters[Index(CallbackFaults)], 1, current_.saturated);
            if (fault->code == BackendFailure)
                AddSaturating(current_.counters[Index(BackendFailures)], 1, current_.saturated);
        }
        return true;
    }

    /** @brief Resolves one source-local failure delta without treating replacement as a counter reset. */
    std::optional<std::uint64_t> AudioMetrics::FailureDeltaForSource(const AudioMetricMemorySource &source) const noexcept {
        for (std::size_t index = 0; index < memoryCursorCount_; ++index) {
            const auto &cursor = memoryCursors_[index];
            if (cursor.sourceGeneration == source.sourceGeneration) {
                if (source.stats.failedAllocations < cursor.failedAllocations)
                    return std::nullopt;
                return source.stats.failedAllocations - cursor.failedAllocations;
            }
        }
        if (source.sourceGeneration <= highestMemorySourceGeneration_)
            return std::nullopt;
        return source.stats.failedAllocations;
    }

    /** @copydoc AudioMetrics::ObserveMemory */
    bool AudioMetrics::ObserveMemory(const std::span<const AudioMetricMemorySource> sources) noexcept {
        if (!IsCollecting())
            return false;
        if (sources.size() > MaximumAudioMetricMemorySources)
            return Invalid();
        std::uint64_t reserved{};
        std::uint64_t used{};
        std::uint64_t peak{};
        std::uint64_t failureDelta{};
        bool aggregateSaturated{};
        std::array<MemoryCursor, MaximumAudioMetricMemorySources> nextCursors{};
        auto nextHighest = highestMemorySourceGeneration_;
        std::uint64_t priorInputGeneration{};
        std::size_t nextCount{};
        for (const auto &source : sources) {
            if (source.sourceGeneration == 0 || source.sourceGeneration <= priorInputGeneration ||
                source.stats.usedBytes > source.stats.reservedBytes || source.stats.peakBytes > source.stats.reservedBytes)
                return Invalid();
            priorInputGeneration = source.sourceGeneration;
            const auto delta = FailureDeltaForSource(source);
            if (!delta.has_value())
                return Invalid();
            nextHighest = std::max(nextHighest, source.sourceGeneration);
            AddSaturating(failureDelta, *delta, aggregateSaturated);
            AddSaturating(reserved, source.stats.reservedBytes, aggregateSaturated);
            AddSaturating(used, source.stats.usedBytes, aggregateSaturated);
            AddSaturating(peak, source.stats.peakBytes, aggregateSaturated);
            nextCursors[nextCount++] = {.sourceGeneration = source.sourceGeneration, .failedAllocations = source.stats.failedAllocations};
        }
        current_.saturated |= aggregateSaturated;
        AddSaturating(current_.counters[Index(AudioMetricCounter::MemoryAllocationFailures)], failureDelta, current_.saturated);
        memoryCursors_ = nextCursors;
        memoryCursorCount_ = nextCount;
        highestMemorySourceGeneration_ = nextHighest;
        const std::array values{reserved, used, peak};
        const std::array kinds{AudioMetricGauge::MemoryReservedBytes, AudioMetricGauge::MemoryUsedBytes, AudioMetricGauge::MemoryPeakBytes};
        for (std::size_t index = 0; index < values.size(); ++index) {
            current_.gauges[Index(kinds[index])] = static_cast<double>(values[index]);
            current_.gaugeAvailable[Index(kinds[index])] = true;
        }
        return true;
    }

    /** @copydoc AudioMetrics::ObserveTiming */
    bool AudioMetrics::ObserveTiming(const AudioMetricTiming kind, const double seconds) noexcept {
        if (!IsCollecting())
            return false;
        if (!Known(kind) || !std::isfinite(seconds) || seconds < 0.0 ||
            current_.timingSequence[Index(kind)] == std::numeric_limits<std::uint64_t>::max())
            return Invalid();
        current_.timings[Index(kind)] = seconds;
        ++current_.timingSequence[Index(kind)];
        return true;
    }

    /** @copydoc AudioMetrics::ObserveGauge */
    bool AudioMetrics::ObserveGauge(const AudioMetricGauge kind, const double value) noexcept {
        using enum AudioMetricGauge;
        if (!IsCollecting())
            return false;
        if (!Known(kind) || !std::isfinite(value) || value < 0.0)
            return Invalid();
        if ((kind == StreamFillRatio || kind == BusPeakRatio || kind == BusRmsRatio) && value > 1.0)
            return Invalid();
        current_.gauges[Index(kind)] = value;
        current_.gaugeAvailable[Index(kind)] = true;
        return true;
    }

    /** @copydoc AudioMetrics::ObserveFailure */
    bool AudioMetrics::ObserveFailure(const AudioMetricCounter kind, const std::uint64_t count) noexcept {
        using enum AudioMetricCounter;
        if (!IsCollecting())
            return false;
        switch (kind) {
            case VoiceRejections:
            case SpatialFallbacks:
            case ParameterLookupFailures:
            case EventLookupFailures:
                AddSaturating(current_.counters[Index(kind)], count, current_.saturated);
                return true;
            default:
                return Invalid();
        }
    }

    /** @copydoc AudioMetrics::Publish */
    bool AudioMetrics::Publish() noexcept {
        if (std::this_thread::get_id() != ownerThread_ || current_.closed || current_.revision == std::numeric_limits<std::uint64_t>::max())
            return false;
        ++current_.revision;
        const std::scoped_lock lock{publishedMutex_};
        published_ = current_;
        return true;
    }

    /** @copydoc AudioMetrics::Snapshot */
    AudioMetricSnapshot AudioMetrics::Snapshot() const noexcept {
        const std::scoped_lock lock{publishedMutex_};
        return published_;
    }

    /** @copydoc AudioMetrics::Close */
    bool AudioMetrics::Close() noexcept {
        if (std::this_thread::get_id() != ownerThread_)
            return false;
        if (current_.closed)
            return true;
        if (current_.revision == std::numeric_limits<std::uint64_t>::max())
            return false;
        current_.closed = true;
        for (std::size_t index = 0; index < AudioMetricGaugeCount; ++index) {
            if (current_.gaugeAvailable[index])
                current_.gauges[index] = 0.0;
        }
        ++current_.revision;
        const std::scoped_lock lock{publishedMutex_};
        published_ = current_;
        return true;
    }
}  // namespace Horo::Audio
