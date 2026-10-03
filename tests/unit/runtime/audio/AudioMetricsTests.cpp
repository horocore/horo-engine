#include "Horo/Audio/AudioMetrics.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Audio {
    namespace {
        template <typename Enum> constexpr std::size_t Index(const Enum value) {
            return static_cast<std::size_t>(value);
        }

        AudioRuntimeId Owner() {
            return AudioRuntimeId::Create(37).Value();
        }

        AudioDeviceEpoch CallbackEpoch() {
            return {.device = {.owner = Owner(), .slot = 1, .generation = 2}, .formatRevision = 3, .callbackEpoch = 4};
        }

        AudioCallbackEvent Callback(const AudioCallbackFact fact) {
            return {.epoch = CallbackEpoch(), .sampleFrame = 0, .timestamp = {.clockDomain = 9, .nanoseconds = 0}, .fact = fact};
        }

        AudioEventQueue EventQueue() {
            auto result = AudioEventQueue::Create({.owner = Owner(),
                                                   .storageIdentity = AudioMemoryPoolId::Create(41).Value(),
                                                   .commandEpoch = 5,
                                                   .callbackEpoch = CallbackEpoch(),
                                                   .clockDomain = 9,
                                                   .slots = 4,
                                                   .criticalSlots = 2,
                                                   .budgetBytes = 64 * 1024});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        AudioCommandStaging Staging() {
            auto result = AudioCommandStaging::Create({.callback = {.owner = Owner(),
                                                                    .storageIdentity = AudioMemoryPoolId::Create(43).Value(),
                                                                    .epoch = 5,
                                                                    .slots = 4,
                                                                    .criticalSlots = 2,
                                                                    .budgetBytes = 4096},
                                                       .ingressIdentity = AudioMemoryPoolId::Create(44).Value(),
                                                       .ingressSlots = 4,
                                                       .criticalSlots = 2,
                                                       .sceneSlots = 2,
                                                       .ingressBudgetBytes = 4096});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        AudioCommand Start(const std::uint32_t slot) {
            return {.scope = {.owner = Owner(), .epoch = 5, .scene = {.owner = Owner(), .slot = 1, .generation = 1}},
                    .payload = AudioStartVoiceCommand{.voice = {.owner = Owner(), .slot = slot, .generation = 1}}};
        }

        class MetricSink final : public Telemetry::ISink {
        public:
            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override {
                if (record.Kind() != Telemetry::RecordKind::Metric || !descriptor)
                    return;
                std::scoped_lock lock{mutex};
                names.push_back(descriptor->name);
                bounded &= descriptor->maxSeries == 1 && descriptor->dimensions.empty();
                if (descriptor->name == "audio.device.sample_rate")
                    hertz &= descriptor->unit == Telemetry::MetricUnit::Hertz;
            }

            void Flush() override {}

            std::mutex mutex;
            std::vector<std::string> names;
            bool bounded{true};
            bool hertz{true};
        };

        bool HasCoreSamples(MetricSink &sink) {
            std::scoped_lock lock{sink.mutex};
            for (const auto *name : {"audio.event_queue.telemetry_drops", "audio.device.sample_rate", "audio.callback.duration"}) {
                if (std::ranges::find(sink.names, name) == sink.names.end())
                    return false;
            }
            return sink.bounded && sink.hertz;
        }

        void PublishObservedSample(AudioMetrics &metrics, AudioMetricPublisher &publisher, const std::uint32_t sequence) {
            REQUIRE(metrics.ObserveEventQueue(1, {.depth = 3, .droppedTelemetry = sequence}));
            REQUIRE(metrics.ObserveGauge(AudioMetricGauge::DeviceSampleRate, 48'000 + sequence));
            REQUIRE(metrics.ObserveTiming(AudioMetricTiming::CallbackDuration, 0.003));
            REQUIRE(metrics.Publish());
            REQUIRE(publisher.Publish(metrics.Snapshot()));
            REQUIRE(Telemetry::Runtime::Flush());
        }
    }  // namespace

    TEST_CASE("audio metrics bridge measures real event queue drops and consumed callback failures", "[audio][metrics]") {
        auto queue = EventQueue();
        REQUIRE(queue.TryPublishDevice(Callback(AudioCallbackUnderrun{64})) == AudioEventPublishStatus::Published);
        REQUIRE(queue.TryPublishDevice(Callback(AudioCallbackUnderrun{32})) == AudioEventPublishStatus::Published);
        REQUIRE(queue.TryPublishDevice(Callback(AudioCallbackUnderrun{16})) == AudioEventPublishStatus::TelemetryDropped);
        REQUIRE(queue.TryPublishDevice(Callback(AudioCallbackReady{})) == AudioEventPublishStatus::Published);
        REQUIRE(queue.TryPublishDevice(Callback(AudioCallbackQuiesced{})) == AudioEventPublishStatus::Published);
        REQUIRE(queue.TryPublishDevice(Callback(AudioCallbackFault{AudioCallbackFaultCode::BackendFailure})) ==
                AudioEventPublishStatus::CriticalRetry);
        AudioMetrics metrics{37, true};
        REQUIRE(metrics.ObserveEventQueue(1, queue.Stats()));
        REQUIRE(queue.Stats().depth == 4);
        AudioControlEventRecord record;
        REQUIRE(queue.TryConsume(record));
        REQUIRE(metrics.ObserveControlEvent(record));
        REQUIRE(queue.TryPublishDevice(Callback(AudioCallbackFault{AudioCallbackFaultCode::BackendFailure})) ==
                AudioEventPublishStatus::Published);
        while (queue.TryConsume(record))
            REQUIRE(metrics.ObserveControlEvent(record));
        REQUIRE(metrics.ObserveEventQueue(1, queue.Stats()));
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.counters[Index(AudioMetricCounter::Underruns)] == 2);
        REQUIRE(snapshot.counters[Index(AudioMetricCounter::EventTelemetryDrops)] == 1);
        REQUIRE(snapshot.counters[Index(AudioMetricCounter::EventCriticalRetries)] == 1);
        REQUIRE(snapshot.counters[Index(AudioMetricCounter::CallbackFaults)] == 1);
        REQUIRE(snapshot.counters[Index(AudioMetricCounter::BackendFailures)] == 1);
        REQUIRE(snapshot.gauges[Index(AudioMetricGauge::EventQueueDepth)] == 0);
    }

    TEST_CASE("audio metrics bridge samples actual command queue depth and classified admission pressure", "[audio][metrics]") {
        auto staging = Staging();
        REQUIRE(staging.RegisterScene({.owner = Owner(), .slot = 1, .generation = 1}) == AudioCommandStagingStatus::Ok);
        REQUIRE(staging.Submit(Start(2)).status == AudioCommandStagingStatus::Ok);
        REQUIRE(staging.Submit(Start(3)).status == AudioCommandStagingStatus::Ok);
        const auto rejected = staging.Submit(Start(4));
        REQUIRE(rejected.status == AudioCommandStagingStatus::OrdinaryFull);
        AudioMetrics metrics{37, true};
        REQUIRE(metrics.ObserveCommandAdmission(rejected.status));
        REQUIRE(staging.Stats().has_value());
        REQUIRE(metrics.ObserveCommandQueue(*staging.Stats()));
        REQUIRE(metrics.Publish());
        const auto first = metrics.Snapshot();
        REQUIRE(first.gauges[Index(AudioMetricGauge::CommandQueueDepth)] == 2);
        REQUIRE(first.counters[Index(AudioMetricCounter::CommandOrdinaryRejections)] == 1);
        REQUIRE(staging.Pump(2).published == 2);
        REQUIRE(metrics.ObserveCommandQueue(*staging.Stats()));
        REQUIRE(metrics.Publish());
        REQUIRE(metrics.Snapshot().gauges[Index(AudioMetricGauge::CommandQueueDepth)] == 2);
        AudioCommandRecord record;
        REQUIRE(staging.TryConsume(record));
        REQUIRE(metrics.ObserveCommandQueue(*staging.Stats()));
        REQUIRE(metrics.Publish());
        REQUIRE(metrics.Snapshot().gauges[Index(AudioMetricGauge::CommandQueueDepth)] == 1);
    }

    TEST_CASE("audio metrics bridge samples real pool pressure without recounting old failures", "[audio][metrics]") {
        auto prepared = AudioMemoryPool::Create({.owner = Owner(),
                                                 .identity = AudioMemoryPoolId::Create(47).Value(),
                                                 .purpose = AudioMemoryPurpose::VoiceState,
                                                 .slots = 1,
                                                 .blockBytes = 64,
                                                 .budgetBytes = 4096});
        REQUIRE(prepared.HasValue());
        auto pool = std::move(prepared).Value();
        REQUIRE(pool.Acquire().status == AudioMemoryStatus::Ok);
        REQUIRE(pool.Acquire().status == AudioMemoryStatus::Exhausted);
        AudioMetrics metrics{37, true};
        const std::array sources{AudioMetricMemorySource{.sourceGeneration = 1, .stats = pool.Stats()}};
        REQUIRE(metrics.ObserveMemory(sources));
        REQUIRE(metrics.ObserveMemory(sources));
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.counters[Index(AudioMetricCounter::MemoryAllocationFailures)] == 1);
        REQUIRE(snapshot.gaugeAvailable[Index(AudioMetricGauge::MemoryUsedBytes)]);
        REQUIRE(snapshot.gauges[Index(AudioMetricGauge::MemoryUsedBytes)] >= 64);
        REQUIRE_FALSE(snapshot.gaugeAvailable[Index(AudioMetricGauge::StreamFillRatio)]);
    }

    TEST_CASE("audio memory replacement retains lifetime failures and rejects stale source replay", "[audio][metrics]") {
        AudioMetrics metrics{37, true};
        AudioMemoryStats retired{};
        {
            auto prepared = AudioMemoryPool::Create({.owner = Owner(),
                                                     .identity = AudioMemoryPoolId::Create(47).Value(),
                                                     .purpose = AudioMemoryPurpose::VoiceState,
                                                     .slots = 1,
                                                     .blockBytes = 64,
                                                     .budgetBytes = 4096});
            REQUIRE(prepared.HasValue());
            auto pool = std::move(prepared).Value();
            REQUIRE(pool.Acquire().status == AudioMemoryStatus::Ok);
            REQUIRE(pool.Acquire().status == AudioMemoryStatus::Exhausted);
            retired = pool.Stats();
            const std::array source{AudioMetricMemorySource{.sourceGeneration = 1, .stats = retired}};
            REQUIRE(metrics.ObserveMemory(source));
            REQUIRE(metrics.Publish());
        }
        auto prepared = AudioMemoryPool::Create({.owner = Owner(),
                                                 .identity = AudioMemoryPoolId::Create(48).Value(),
                                                 .purpose = AudioMemoryPurpose::VoiceState,
                                                 .slots = 1,
                                                 .blockBytes = 64,
                                                 .budgetBytes = 4096});
        REQUIRE(prepared.HasValue());
        auto replacement = std::move(prepared).Value();
        REQUIRE(replacement.Acquire().status == AudioMemoryStatus::Ok);
        REQUIRE(replacement.Acquire().status == AudioMemoryStatus::Exhausted);
        const std::array current{AudioMetricMemorySource{.sourceGeneration = 2, .stats = replacement.Stats()}};
        REQUIRE(metrics.ObserveMemory(current));
        REQUIRE(metrics.Publish());
        REQUIRE(metrics.Snapshot().counters[Index(AudioMetricCounter::MemoryAllocationFailures)] == 2);
        const std::array stale{AudioMetricMemorySource{.sourceGeneration = 1, .stats = retired}};
        REQUIRE_FALSE(metrics.ObserveMemory(stale));
        auto rolledBack = current;
        rolledBack[0].stats.failedAllocations = 0;
        REQUIRE_FALSE(metrics.ObserveMemory(rolledBack));
        REQUIRE(metrics.Snapshot().counters[Index(AudioMetricCounter::MemoryAllocationFailures)] == 2);
    }

    TEST_CASE("audio event queue replacement preserves prior drops and rejects same-generation resets", "[audio][metrics]") {
        AudioMetrics metrics{37, true};
        AudioEventQueueStats retired{};
        {
            auto first = EventQueue();
            for (int index = 0; index < 2; ++index)
                REQUIRE(first.TryPublishDevice(Callback(AudioCallbackUnderrun{1})) == AudioEventPublishStatus::Published);
            REQUIRE(first.TryPublishDevice(Callback(AudioCallbackUnderrun{1})) == AudioEventPublishStatus::TelemetryDropped);
            retired = first.Stats();
            REQUIRE(metrics.ObserveEventQueue(1, retired));
            REQUIRE(metrics.Publish());
            first.Close();
            AudioControlEventRecord drained;
            while (first.TryConsume(drained)) {
            }
            REQUIRE(first.IsDrained());
        }
        auto second = EventQueue();
        REQUIRE_FALSE(metrics.ObserveEventQueue(1, second.Stats()));
        for (int index = 0; index < 2; ++index)
            REQUIRE(second.TryPublishDevice(Callback(AudioCallbackUnderrun{1})) == AudioEventPublishStatus::Published);
        REQUIRE(second.TryPublishDevice(Callback(AudioCallbackUnderrun{1})) == AudioEventPublishStatus::TelemetryDropped);
        REQUIRE(metrics.ObserveEventQueue(2, second.Stats()));
        REQUIRE(metrics.Publish());
        REQUIRE(metrics.Snapshot().counters[Index(AudioMetricCounter::EventTelemetryDrops)] == 2);
        REQUIRE_FALSE(metrics.ObserveEventQueue(1, retired));
        second.Close();
        AudioControlEventRecord drained;
        while (second.TryConsume(drained)) {
        }
        REQUIRE(second.IsDrained());
    }

    TEST_CASE("audio metrics reject hostile values and preserve disabled and owner lifetime boundaries", "[audio][metrics]") {
        AudioMetrics disabled{37, false};
        REQUIRE_FALSE(disabled.ObserveFailure(AudioMetricCounter::VoiceRejections));
        REQUIRE(disabled.Publish());
        REQUIRE_FALSE(disabled.Snapshot().enabled);
        AudioMetrics metrics{38, true};
        REQUIRE_FALSE(metrics.ObserveGauge(static_cast<AudioMetricGauge>(255), 1.0));
        REQUIRE_FALSE(metrics.ObserveTiming(AudioMetricTiming::CallbackDuration, std::numeric_limits<double>::infinity()));
        REQUIRE_FALSE(metrics.ObserveGauge(AudioMetricGauge::StreamFillRatio, 2.0));
        REQUIRE_FALSE(metrics.ObserveFailure(AudioMetricCounter::EventTelemetryDrops));
        REQUIRE(metrics.ObserveFailure(AudioMetricCounter::VoiceRejections, std::numeric_limits<std::uint64_t>::max()));
        REQUIRE(metrics.ObserveFailure(AudioMetricCounter::VoiceRejections));
        REQUIRE(metrics.Publish());
        REQUIRE(metrics.Snapshot().saturated);
        REQUIRE(metrics.Snapshot().invalidObservations == 4);
        bool wrongThreadAccepted{};
        std::thread worker{[&] {
            wrongThreadAccepted = metrics.ObserveGauge(AudioMetricGauge::DeviceSampleRate, 48'000);
        }};
        worker.join();
        REQUIRE_FALSE(wrongThreadAccepted);
        REQUIRE(metrics.Close());
        REQUIRE(metrics.Close());
        REQUIRE_FALSE(metrics.ObserveFailure(AudioMetricCounter::VoiceRejections));
        REQUIRE_FALSE(metrics.Publish());
        REQUIRE(metrics.Snapshot().closed);
    }

    TEST_CASE("audio metric retirement clears previously measured gauges without inventing absent gauges", "[audio][metrics]") {
        AudioMetrics metrics{37, true};
        AudioMetricPublisher publisher{37, {}};
        REQUIRE(metrics.ObserveGauge(AudioMetricGauge::DeviceSampleRate, 48'000));
        REQUIRE(metrics.Publish());
        REQUIRE(publisher.Publish(metrics.Snapshot()));
        REQUIRE(metrics.Close());
        const auto final = metrics.Snapshot();
        REQUIRE(final.closed);
        REQUIRE(final.gaugeAvailable[Index(AudioMetricGauge::DeviceSampleRate)]);
        REQUIRE(final.gauges[Index(AudioMetricGauge::DeviceSampleRate)] == 0.0);
        REQUIRE_FALSE(final.gaugeAvailable[Index(AudioMetricGauge::ActiveVoices)]);
        REQUIRE(publisher.Publish(final));
        REQUIRE_FALSE(publisher.Publish(final));
    }

    TEST_CASE("audio metrics publisher rejects stale owner snapshots and never invents unavailable costs", "[audio][metrics]") {
        AudioMetricPublisher publisher{37, {}};
        AudioMetrics metrics{37, true};
        REQUIRE(metrics.ObserveGauge(AudioMetricGauge::DeviceSampleRate, 48'000));
        REQUIRE(metrics.Publish());
        const auto first = metrics.Snapshot();
        REQUIRE(first.gaugeAvailable[Index(AudioMetricGauge::DeviceSampleRate)]);
        REQUIRE(first.timingSequence[Index(AudioMetricTiming::MixerDuration)] == 0);
        REQUIRE(publisher.Publish(first));
        REQUIRE_FALSE(publisher.Publish(first));
        auto rollback = first;
        rollback.revision = 2;
        rollback.counters[Index(AudioMetricCounter::Underruns)] = 2;
        REQUIRE(publisher.Publish(rollback));
        auto decreasing = rollback;
        decreasing.revision = 3;
        decreasing.counters[Index(AudioMetricCounter::Underruns)] = 1;
        REQUIRE_FALSE(publisher.Publish(decreasing));
        auto foreign = rollback;
        foreign.revision = 3;
        foreign.ownerGeneration = 38;
        REQUIRE_FALSE(publisher.Publish(foreign));
        REQUIRE(metrics.Close());
        REQUIRE_FALSE(publisher.Publish(metrics.Snapshot()));
    }

    TEST_CASE("audio OBS bridge exports fixed single-series names and correct Hertz unit", "[audio][metrics][telemetry]") {
        Telemetry::Runtime::Shutdown();
        auto sink = std::make_shared<MetricSink>();
        REQUIRE(
            Telemetry::Runtime::Initialize({.queueCapacity = 256, .metricCollectionLevel = Telemetry::MetricCollectionLevel::Core}, sink));
        auto handles = RegisterAudioMetricHandles(Telemetry::MetricCollectionLevel::Core);
        REQUIRE(handles.gauges[Index(AudioMetricGauge::DeviceSampleRate)]);
        REQUIRE(handles.counters[Index(AudioMetricCounter::EventTelemetryDrops)]);
        REQUIRE(Telemetry::Runtime::GetStatistics().invalidInstrumentRegistrations == 0);
        AudioMetricPublisher publisher{37, std::move(handles)};
        AudioMetrics metrics{37, true};
        for (std::uint32_t sequence = 1; sequence <= 16 && !HasCoreSamples(*sink); ++sequence)
            PublishObservedSample(metrics, publisher, sequence);
        const auto statistics = Telemetry::Runtime::GetStatistics();
        CAPTURE(statistics.acceptedRecords, statistics.exportedRecords, statistics.contentionDrops);
        REQUIRE(HasCoreSamples(*sink));
        REQUIRE(Telemetry::Runtime::Shutdown());
    }
}  // namespace Horo::Audio
