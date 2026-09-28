#include "AllocationProbe.h"
#include "Horo/Audio/AudioMetricExtraction.h"
#include "Horo/Audio/AudioMetrics.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>

namespace Horo::Audio {
    namespace {
        AudioDeviceEpoch Epoch() {
            return {.device = {.owner = AudioRuntimeId::Create(37).Value(), .slot = 1, .generation = 2},
                    .formatRevision = 3,
                    .callbackEpoch = 4};
        }

        AudioMetricExtractionQueue Queue(const std::uint32_t slots = 4, const std::uint64_t interval = 0) {
            auto result = AudioMetricExtractionQueue::Create(
                {.epoch = Epoch(), .slots = slots, .budgetBytes = 16'384, .diagnosticFrameInterval = interval});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        AudioExtractionRecord Record(const AudioExtractionKind kind, const std::uint64_t frame, const double seconds = 0.0) {
            return {.epoch = Epoch(), .sampleFrame = frame, .kind = kind, .seconds = seconds};
        }
    }  // namespace

    TEST_CASE("audio extraction rejects invalid preparation and stale callback observations", "[audio][metrics][extraction]") {
        auto descriptor = AudioExtractionDescriptor{.epoch = Epoch(), .slots = 3, .budgetBytes = 16'384};
        REQUIRE_FALSE(AudioMetricExtractionQueue::Create(descriptor).HasValue());
        descriptor.slots = 4;
        descriptor.budgetBytes = 1;
        REQUIRE_FALSE(AudioMetricExtractionQueue::Create(descriptor).HasValue());
        descriptor.budgetBytes = 16'384;
        descriptor.epoch = {};
        REQUIRE_FALSE(AudioMetricExtractionQueue::Create(descriptor).HasValue());

        auto queue = Queue();
        auto record = Record(AudioExtractionKind::CallbackDuration, 1, 0.1);
        record.epoch.callbackEpoch++;
        REQUIRE(queue.TryRecord(record) == AudioExtractionStatus::Invalid);
        record = Record(AudioExtractionKind::CallbackDuration, 1, std::numeric_limits<double>::infinity());
        REQUIRE(queue.TryRecord(record) == AudioExtractionStatus::Invalid);
        record = Record(AudioExtractionKind::Count, 1);
        REQUIRE(queue.TryRecord(record) == AudioExtractionStatus::Invalid);
        record = Record(AudioExtractionKind::LockAttempt, 1, 0.1);
        REQUIRE(queue.TryRecord(record) == AudioExtractionStatus::Invalid);
        REQUIRE(queue.Stats().depth == 0);
    }

    TEST_CASE("audio extraction coalesces one callback buffer and preserves worst duration", "[audio][metrics][extraction]") {
        auto queue = Queue();
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::CallbackDuration, 10, 0.002)) == AudioExtractionStatus::Queued);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::CallbackDuration, 12, 0.004)) == AudioExtractionStatus::Coalesced);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::CallbackDuration, 13, 0.003)) == AudioExtractionStatus::Coalesced);
        REQUIRE(queue.Flush() == 1);
        REQUIRE(queue.Stats().coalesced == 2);
        AudioExtractionRecord consumed;
        REQUIRE(queue.TryConsume(consumed));
        REQUIRE(consumed.epoch == Epoch());
        REQUIRE(consumed.sampleFrame == 13);
        REQUIRE(consumed.seconds == 0.004);
        REQUIRE(consumed.samples == 3);
        REQUIRE_FALSE(queue.TryConsume(consumed));
        REQUIRE_FALSE(queue.IsDrained());
        queue.Close();
        REQUIRE(queue.IsDrained());
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::CallbackDuration, 14)) == AudioExtractionStatus::Closed);
    }

    TEST_CASE("audio extraction callback path performs no heap allocation", "[audio][metrics][extraction]") {
        auto queue = Queue();
        const auto first = Record(AudioExtractionKind::CallbackDuration, 10, 0.002);
        const auto second = Record(AudioExtractionKind::CallbackDuration, 11, 0.003);
        const auto before = Tests::AllocationProbe::Count();
        const auto admission = queue.TryRecord(first);
        const auto coalesced = queue.TryRecord(second);
        const auto flushed = queue.Flush();
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(admission == AudioExtractionStatus::Queued);
        REQUIRE(coalesced == AudioExtractionStatus::Coalesced);
        REQUIRE(flushed == 1);
        REQUIRE(after == before);
    }

    TEST_CASE("audio extraction exposes full-ring drops and diagnostic rate limits", "[audio][metrics][extraction]") {
        auto queue = Queue(2, 100);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::DeadlineOverrun, 100, 0.01)) == AudioExtractionStatus::Queued);
        REQUIRE(queue.Flush() == 1);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::DeadlineOverrun, 110, 0.01)) == AudioExtractionStatus::RateLimited);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::MixerDuration, 111, 0.02)) == AudioExtractionStatus::Queued);
        REQUIRE(queue.Flush() == 1);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::EffectDuration, 120, 0.03)) == AudioExtractionStatus::Queued);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::EffectDuration, 121, 0.04)) == AudioExtractionStatus::Coalesced);
        REQUIRE(queue.Flush() == 0);
        const auto stats = queue.Stats();
        REQUIRE(stats.depth == 2);
        REQUIRE(stats.published == 2);
        REQUIRE(stats.dropped == 2);
        REQUIRE(stats.rateLimited == 1);
        REQUIRE(stats.coalesced == 1);
        AudioExtractionRecord consumed;
        REQUIRE(queue.TryConsume(consumed));
        REQUIRE(queue.TryConsume(consumed));
        queue.Close();
        REQUIRE(queue.IsDrained());
    }

    TEST_CASE("audio extraction rejects out-of-order pending diagnostics before rate limiting", "[audio][metrics][extraction]") {
        auto queue = Queue(4, 100);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::DeadlineOverrun, 100, 0.01)) == AudioExtractionStatus::Queued);
        REQUIRE(queue.Flush() == 1);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::DeadlineOverrun, 200, 0.02)) == AudioExtractionStatus::Queued);
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::DeadlineOverrun, 150, 0.02)) == AudioExtractionStatus::Invalid);
        REQUIRE(queue.Flush() == 1);
        AudioExtractionRecord diagnostic;
        REQUIRE(queue.TryConsume(diagnostic));
        REQUIRE(diagnostic.sampleFrame == 100);
        REQUIRE(queue.TryConsume(diagnostic));
        REQUIRE(diagnostic.sampleFrame == 200);
        REQUIRE(queue.Stats().rateLimited == 0);
    }

    TEST_CASE("audio extraction consumption belongs to the creating control thread", "[audio][metrics][extraction]") {
        auto queue = Queue();
        REQUIRE(queue.TryRecord(Record(AudioExtractionKind::AllocationAttempt, 1)) == AudioExtractionStatus::Queued);
        REQUIRE(queue.Flush() == 1);
        bool crossThreadConsumed = true;
        std::thread other{[&] {
            AudioExtractionRecord record;
            crossThreadConsumed = queue.TryConsume(record);
        }};
        other.join();
        REQUIRE_FALSE(crossThreadConsumed);
        AudioExtractionRecord record;
        REQUIRE(queue.TryConsume(record));
        REQUIRE(record.kind == AudioExtractionKind::AllocationAttempt);
    }

    TEST_CASE("audio extraction publishes immutable records across concurrent SPSC handoff", "[audio][metrics][extraction]") {
        constexpr std::uint64_t Observations = 512;
        auto queue = Queue(8);
        bool producerAccepted = true;
        std::thread callback{[&] {
            for (std::uint64_t frame = 1; frame <= Observations; ++frame) {
                producerAccepted &=
                    queue.TryRecord(Record(AudioExtractionKind::CallbackDuration, frame, 0.001)) == AudioExtractionStatus::Queued;
                static_cast<void>(queue.Flush());
            }
            queue.Close();
        }};
        std::uint64_t consumed{};
        std::uint64_t lastFrame{};
        bool recordsValid = true;
        while (!queue.IsDrained()) {
            AudioExtractionRecord record;
            if (!queue.TryConsume(record)) {
                std::this_thread::yield();
                continue;
            }
            recordsValid &= record.epoch == Epoch() && record.kind == AudioExtractionKind::CallbackDuration &&
                            record.sampleFrame > lastFrame && record.sampleFrame <= Observations && record.seconds == 0.001 &&
                            record.samples == 1;
            lastFrame = record.sampleFrame;
            ++consumed;
        }
        callback.join();
        const auto stats = queue.Stats();
        REQUIRE(producerAccepted);
        REQUIRE(recordsValid);
        REQUIRE(stats.depth == 0);
        REQUIRE(stats.published == consumed);
        REQUIRE(stats.published + stats.dropped == Observations);
    }

    TEST_CASE("audio metrics retain extraction pressure across queue generations", "[audio][metrics][extraction]") {
        AudioMetrics metrics{7, true};
        REQUIRE(metrics.ObserveExtractionQueue(1, {.depth = 2, .published = 3, .coalesced = 4, .dropped = 5, .rateLimited = 6}));
        REQUIRE(metrics.ObserveExtractionQueue(1, {.depth = 1, .published = 4, .coalesced = 5, .dropped = 7, .rateLimited = 9}));
        REQUIRE_FALSE(metrics.ObserveExtractionQueue(1, {.depth = 1, .published = 3, .coalesced = 5, .dropped = 7, .rateLimited = 9}));
        REQUIRE(metrics.ObserveExtractionQueue(2, {.depth = 0, .published = 1, .coalesced = 2, .dropped = 3, .rateLimited = 4}));
        REQUIRE_FALSE(metrics.ObserveExtractionQueue(1, {}));
        REQUIRE(metrics.ObserveExtractedRecord(Record(AudioExtractionKind::DeadlineOverrun, 100, 0.01)));
        REQUIRE(metrics.ObserveExtractedRecord(Record(AudioExtractionKind::AllocationAttempt, 101)));
        REQUIRE(metrics.ObserveExtractedRecord(Record(AudioExtractionKind::CallbackDuration, 102, 0.002)));
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.counters[static_cast<std::size_t>(AudioMetricCounter::ExtractionDrops)] == 10);
        REQUIRE(snapshot.counters[static_cast<std::size_t>(AudioMetricCounter::ExtractionCoalesced)] == 7);
        REQUIRE(snapshot.counters[static_cast<std::size_t>(AudioMetricCounter::ExtractionRateLimited)] == 13);
        REQUIRE(snapshot.counters[static_cast<std::size_t>(AudioMetricCounter::CallbackDeadlineOverruns)] == 1);
        REQUIRE(snapshot.counters[static_cast<std::size_t>(AudioMetricCounter::CallbackAllocationAttempts)] == 1);
        REQUIRE(snapshot.timings[static_cast<std::size_t>(AudioMetricTiming::CallbackDuration)] == 0.002);
        REQUIRE(snapshot.invalidObservations == 2);
    }
}  // namespace Horo::Audio
