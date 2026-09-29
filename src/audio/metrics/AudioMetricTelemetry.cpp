#include "Horo/Audio/AudioMetrics.h"

#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Audio {
    namespace {
        struct MetricSpec final {
            std::string_view name;
            Telemetry::MetricUnit unit;
        };

        constexpr std::array<MetricSpec, AudioMetricCounterCount> CounterSpecs{{
            {"audio.callback.underruns", Telemetry::MetricUnit::Count},
            {"audio.command_queue.ordinary_rejections", Telemetry::MetricUnit::Count},
            {"audio.command_queue.critical_retries", Telemetry::MetricUnit::Count},
            {"audio.command_queue.output_stalls", Telemetry::MetricUnit::Count},
            {"audio.event_queue.telemetry_drops", Telemetry::MetricUnit::Count},
            {"audio.event_queue.critical_retries", Telemetry::MetricUnit::Count},
            {"audio.event_queue.duplicate_terminals", Telemetry::MetricUnit::Count},
            {"audio.memory.allocation_failures", Telemetry::MetricUnit::Count},
            {"audio.voices.rejected", Telemetry::MetricUnit::Count},
            {"audio.spatial.fallbacks", Telemetry::MetricUnit::Count},
            {"audio.callback.faults", Telemetry::MetricUnit::Count},
            {"audio.device.backend_failures", Telemetry::MetricUnit::Count},
            {"audio.parameter.lookup_failures", Telemetry::MetricUnit::Count},
            {"audio.event.lookup_failures", Telemetry::MetricUnit::Count},
            {"audio.extraction.drops", Telemetry::MetricUnit::Count},
            {"audio.extraction.coalesced", Telemetry::MetricUnit::Count},
            {"audio.extraction.rate_limited", Telemetry::MetricUnit::Count},
            {"audio.callback.deadline_overruns", Telemetry::MetricUnit::Count},
            {"audio.callback.allocation_attempts", Telemetry::MetricUnit::Count},
            {"audio.callback.lock_attempts", Telemetry::MetricUnit::Count},
        }};

        constexpr std::array<MetricSpec, AudioMetricGaugeCount> GaugeSpecs{{
            {"audio.command_queue.depth", Telemetry::MetricUnit::Count},
            {"audio.event_queue.depth", Telemetry::MetricUnit::Count},
            {"audio.voices.active", Telemetry::MetricUnit::Count},
            {"audio.voices.virtualized", Telemetry::MetricUnit::Count},
            {"audio.stream.buffer_fill", Telemetry::MetricUnit::Ratio},
            {"audio.memory.reserved", Telemetry::MetricUnit::Bytes},
            {"audio.memory.used", Telemetry::MetricUnit::Bytes},
            {"audio.memory.peak", Telemetry::MetricUnit::Bytes},
            {"audio.device.sample_rate", Telemetry::MetricUnit::Hertz},
            {"audio.device.buffer_frames", Telemetry::MetricUnit::Count},
            {"audio.callback.budget_utilization", Telemetry::MetricUnit::Ratio},
            {"audio.occlusion.staleness", Telemetry::MetricUnit::Seconds},
            {"audio.bus.peak", Telemetry::MetricUnit::Ratio},
            {"audio.bus.rms", Telemetry::MetricUnit::Ratio},
            {"audio.extraction.depth", Telemetry::MetricUnit::Count},
        }};

        constexpr std::array<MetricSpec, AudioMetricTimingCount> TimingSpecs{{
            {"audio.callback.duration", Telemetry::MetricUnit::Seconds},
            {"audio.mixer.duration", Telemetry::MetricUnit::Seconds},
            {"audio.effect.duration", Telemetry::MetricUnit::Seconds},
            {"audio.spatial.duration", Telemetry::MetricUnit::Seconds},
        }};

        [[nodiscard]] Telemetry::InstrumentDescriptor Descriptor(const MetricSpec &spec, const Telemetry::InstrumentKind kind) {
            return {.kind = kind,
                    .name = std::string{spec.name},
                    .subsystem = "audio",
                    .unit = spec.unit,
                    .description = "Bounded owner-observed audio measurement",
                    .maxSeries = 1,
                    .minimumCollectionLevel = Telemetry::MetricCollectionLevel::Core};
        }

        [[nodiscard]] bool ValidCounters(const AudioMetricSnapshot &next, const AudioMetricSnapshot &previous) noexcept {
            for (std::size_t index = 0; index < AudioMetricCounterCount; ++index) {
                if (next.counters[index] < previous.counters[index])
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidGauges(const AudioMetricSnapshot &next) noexcept {
            for (std::size_t index = 0; index < AudioMetricGaugeCount; ++index) {
                if (next.gaugeAvailable[index] && (!std::isfinite(next.gauges[index]) || next.gauges[index] < 0.0))
                    return false;
                if (next.closed && next.gaugeAvailable[index] && next.gauges[index] != 0.0)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidTimings(const AudioMetricSnapshot &next, const AudioMetricSnapshot &previous) noexcept {
            for (std::size_t index = 0; index < AudioMetricTimingCount; ++index) {
                if (next.timingSequence[index] < previous.timingSequence[index] ||
                    (next.timingSequence[index] != 0 && (!std::isfinite(next.timings[index]) || next.timings[index] < 0.0)))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool ValidSuccessor(const AudioMetricSnapshot &next, const AudioMetricSnapshot &previous,
                                          const std::uint64_t generation) noexcept {
            return next.schemaVersion == 1 && next.ownerGeneration == generation && generation != 0 && next.revision != 0 &&
                   next.revision > previous.revision && !previous.closed && (previous.revision == 0 || next.enabled == previous.enabled) &&
                   (!previous.saturated || next.saturated) && next.invalidObservations >= previous.invalidObservations &&
                   ValidCounters(next, previous) && ValidGauges(next) && ValidTimings(next, previous);
        }
    }  // namespace

    /** @copydoc RegisterAudioMetricHandles */
    AudioMetricHandles RegisterAudioMetricHandles(const Telemetry::MetricCollectionLevel level) {
        using enum Telemetry::InstrumentKind;
        AudioMetricHandles handles{};
        if (level == Telemetry::MetricCollectionLevel::Off)
            return handles;
        for (std::size_t index = 0; index < AudioMetricCounterCount; ++index)
            handles.counters[index] = Telemetry::Runtime::RegisterCounter(Descriptor(CounterSpecs[index], Counter));
        for (std::size_t index = 0; index < AudioMetricGaugeCount; ++index)
            handles.gauges[index] = Telemetry::Runtime::RegisterGauge(Descriptor(GaugeSpecs[index], Gauge));
        for (std::size_t index = 0; index < AudioMetricTimingCount; ++index)
            handles.timings[index] = Telemetry::Runtime::RegisterHistogram(Descriptor(TimingSpecs[index], Histogram));
        return handles;
    }

    /** @copydoc AudioMetricPublisher::AudioMetricPublisher */
    AudioMetricPublisher::AudioMetricPublisher(const std::uint64_t generation, AudioMetricHandles handles) noexcept
        : ownerThread_(std::this_thread::get_id()), generation_(generation), handles_(std::move(handles)) {
        previous_.ownerGeneration = generation;
    }

    /** @copydoc AudioMetricPublisher::Publish */
    bool AudioMetricPublisher::Publish(const AudioMetricSnapshot &snapshot) noexcept {
        if (std::this_thread::get_id() != ownerThread_ || closed_ || !ValidSuccessor(snapshot, previous_, generation_))
            return false;
        if (snapshot.enabled) {
            for (std::size_t index = 0; index < AudioMetricCounterCount; ++index) {
                const auto delta = snapshot.counters[index] - previous_.counters[index];
                if (delta != 0)
                    handles_.counters[index].Add(delta);
            }
            for (std::size_t index = 0; index < AudioMetricGaugeCount; ++index) {
                if (snapshot.gaugeAvailable[index])
                    handles_.gauges[index].Set(snapshot.gauges[index]);
            }
            for (std::size_t index = 0; index < AudioMetricTimingCount; ++index) {
                if (snapshot.timingSequence[index] > previous_.timingSequence[index])
                    handles_.timings[index].Observe(snapshot.timings[index]);
            }
        }
        previous_ = snapshot;
        if (snapshot.closed)
            closed_ = true;
        return true;
    }

    /** @copydoc AudioMetricPublisher::Close */
    void AudioMetricPublisher::Close() noexcept {
        if (std::this_thread::get_id() == ownerThread_)
            closed_ = true;
    }
}  // namespace Horo::Audio
