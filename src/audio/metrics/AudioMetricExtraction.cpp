#include "Horo/Audio/AudioMetricExtraction.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Audio {
    namespace {
        constexpr auto KindCount = static_cast<std::size_t>(AudioExtractionKind::Count);
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert(std::atomic<bool>::is_always_lock_free);
        static_assert(std::is_trivially_copyable_v<AudioExtractionRecord>);

        /** @brief Adds callback-owned totals without wrapping at exhaustion. */
        void Increment(std::uint64_t &counter, const std::uint64_t amount = 1) noexcept {
            counter += std::min(amount, std::numeric_limits<std::uint64_t>::max() - counter);
        }

        /** @brief Separates sparse diagnostic facts from per-buffer cost samples. */
        bool IsDiagnostic(const AudioExtractionKind kind) noexcept {
            return kind >= AudioExtractionKind::DeadlineOverrun && kind < AudioExtractionKind::Count;
        }

        /** @brief Producer-owned cumulative values, published together at each callback boundary. */
        struct ProducerTotals final {
            std::uint64_t published{};
            std::uint64_t coalesced{};
            std::uint64_t dropped{};
            std::uint64_t rateLimited{};
        };
    }  // namespace

    /** @brief Each side owns its plain cursor; sequentially consistent shared cursors publish ring-slot handoff. */
    struct AudioMetricExtractionQueue::State final {
        AudioExtractionDescriptor descriptor;
        std::vector<AudioExtractionRecord> records;
        std::array<AudioExtractionRecord, KindCount> pending{};
        std::array<bool, KindCount> hasPending{};
        std::array<std::uint64_t, KindCount> lastDiagnosticFrame{};
        std::array<bool, KindCount> hasPublishedDiagnostic{};
        std::uint32_t producerCursor{};
        alignas(64) std::atomic<std::uint32_t> write{};
        std::uint32_t consumerCursor{};
        alignas(64) std::atomic<std::uint32_t> read{};
        std::atomic<bool> closed{};
        bool producerClosed{};
        ProducerTotals producerTotals;
        std::atomic<std::uint64_t> published{};
        std::atomic<std::uint64_t> coalesced{};
        std::atomic<std::uint64_t> dropped{};
        std::atomic<std::uint64_t> rateLimited{};
        const std::thread::id ownerThread{std::this_thread::get_id()};

        State(const AudioExtractionDescriptor &value, std::vector<AudioExtractionRecord> storage) noexcept
            : descriptor(value), records(std::move(storage)) {}

        /** @brief Makes producer-owned totals visible only at a completed callback boundary. */
        void PublishStats() noexcept {
            published.store(producerTotals.published);
            coalesced.store(producerTotals.coalesced);
            dropped.store(producerTotals.dropped);
            rateLimited.store(producerTotals.rateLimited);
        }
    };

    /** @copydoc AudioMetricExtractionQueue::Create */
    Result<AudioMetricExtractionQueue> AudioMetricExtractionQueue::Create(const AudioExtractionDescriptor &descriptor) {
        if (!MatchesAudioDeviceEpoch(descriptor.epoch, descriptor.epoch) || descriptor.slots < 2 ||
            descriptor.slots > MaximumAudioExtractionSlots || !std::has_single_bit(descriptor.slots))
            return Result<AudioMetricExtractionQueue>::Failure(MakeError(AudioErrors::EventQueueInvalid));
        if (descriptor.budgetBytes < static_cast<std::size_t>(descriptor.slots) * sizeof(AudioExtractionRecord))
            return Result<AudioMetricExtractionQueue>::Failure(MakeError(AudioErrors::MemoryBudgetExceeded));
        try {
            std::vector<AudioExtractionRecord> records(descriptor.slots);
            return Result<AudioMetricExtractionQueue>::Success(
                AudioMetricExtractionQueue{std::make_unique<State>(descriptor, std::move(records))});
        } catch (const std::bad_alloc &) {
            return Result<AudioMetricExtractionQueue>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioMetricExtractionQueue::AudioMetricExtractionQueue */
    AudioMetricExtractionQueue::AudioMetricExtractionQueue(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc AudioMetricExtractionQueue::~AudioMetricExtractionQueue */
    AudioMetricExtractionQueue::~AudioMetricExtractionQueue() = default;
    /** @copydoc AudioMetricExtractionQueue::AudioMetricExtractionQueue */
    AudioMetricExtractionQueue::AudioMetricExtractionQueue(AudioMetricExtractionQueue &&) noexcept = default;
    /** @copydoc AudioMetricExtractionQueue::operator= */
    AudioMetricExtractionQueue &AudioMetricExtractionQueue::operator=(AudioMetricExtractionQueue &&) noexcept = default;

    /** @copydoc AudioMetricExtractionQueue::TryRecord */
    AudioExtractionStatus AudioMetricExtractionQueue::TryRecord(const AudioExtractionRecord &record) noexcept {
        using enum AudioExtractionStatus;
        if (!state_)
            return Inactive;
        auto &state = *state_;
        if (state.producerClosed)
            return Closed;
        const auto kind = static_cast<std::size_t>(record.kind);
        if (kind >= KindCount || !MatchesAudioDeviceEpoch(state.descriptor.epoch, record.epoch) || record.samples != 1 ||
            !std::isfinite(record.seconds) || record.seconds < 0.0 ||
            (record.kind == AudioExtractionKind::AllocationAttempt || record.kind == AudioExtractionKind::LockAttempt) &&
                record.seconds != 0.0)
            return Invalid;
        if (state.hasPending[kind] && record.sampleFrame < state.pending[kind].sampleFrame)
            return Invalid;
        if (IsDiagnostic(record.kind) && state.hasPublishedDiagnostic[kind]) {
            if (record.sampleFrame < state.lastDiagnosticFrame[kind])
                return Invalid;
            if (record.sampleFrame - state.lastDiagnosticFrame[kind] < state.descriptor.diagnosticFrameInterval) {
                Increment(state.producerTotals.rateLimited);
                return RateLimited;
            }
        }
        if (state.hasPending[kind]) {
            auto &pending = state.pending[kind];
            if (pending.samples == std::numeric_limits<std::uint32_t>::max()) {
                Increment(state.producerTotals.dropped);
                return Dropped;
            }
            pending.sampleFrame = record.sampleFrame;
            pending.seconds = std::max(pending.seconds, record.seconds);
            ++pending.samples;
            Increment(state.producerTotals.coalesced);
            return Coalesced;
        }
        state.pending[kind] = record;
        state.hasPending[kind] = true;
        return Queued;
    }

    /** @copydoc AudioMetricExtractionQueue::Flush */
    std::uint32_t AudioMetricExtractionQueue::Flush() noexcept {
        if (!state_ || state_->producerClosed)
            return 0;
        auto &state = *state_;
        std::uint32_t count{};
        for (std::size_t kind = 0; kind < KindCount; ++kind) {
            if (!state.hasPending[kind])
                continue;
            const auto producer = state.producerCursor;
            const auto consumer = state.read.load();
            const auto &record = state.pending[kind];
            if (producer - consumer >= state.descriptor.slots) {
                Increment(state.producerTotals.dropped, record.samples);
            } else {
                state.records[producer & (state.descriptor.slots - 1)] = record;
                state.producerCursor = producer + 1;
                state.write.store(state.producerCursor);
                Increment(state.producerTotals.published);
                if (IsDiagnostic(record.kind)) {
                    state.lastDiagnosticFrame[kind] = record.sampleFrame;
                    state.hasPublishedDiagnostic[kind] = true;
                }
                ++count;
            }
            state.hasPending[kind] = false;
        }
        state.PublishStats();
        return count;
    }

    /** @copydoc AudioMetricExtractionQueue::Close */
    void AudioMetricExtractionQueue::Close() noexcept {
        if (state_) {
            static_cast<void>(Flush());
            state_->producerClosed = true;
            state_->closed.store(true);
        }
    }

    /** @copydoc AudioMetricExtractionQueue::TryConsume */
    bool AudioMetricExtractionQueue::TryConsume(AudioExtractionRecord &record) noexcept {
        if (!state_ || std::this_thread::get_id() != state_->ownerThread)
            return false;
        auto &state = *state_;
        const auto consumer = state.consumerCursor;
        if (consumer == state.write.load())
            return false;
        record = state.records[consumer & (state.descriptor.slots - 1)];
        state.consumerCursor = consumer + 1;
        state.read.store(state.consumerCursor);
        return true;
    }

    /** @copydoc AudioMetricExtractionQueue::Stats */
    AudioExtractionStats AudioMetricExtractionQueue::Stats() const noexcept {
        if (!state_ || std::this_thread::get_id() != state_->ownerThread)
            return {};
        const auto consumed = state_->read.load();
        const auto produced = state_->write.load();
        return {.depth = std::min(produced - consumed, state_->descriptor.slots),
                .published = state_->published.load(),
                .coalesced = state_->coalesced.load(),
                .dropped = state_->dropped.load(),
                .rateLimited = state_->rateLimited.load()};
    }

    /** @copydoc AudioMetricExtractionQueue::IsDrained */
    bool AudioMetricExtractionQueue::IsDrained() const noexcept {
        if (!state_)
            return true;
        if (std::this_thread::get_id() != state_->ownerThread)
            return false;
        return state_->closed.load() && state_->read.load() == state_->write.load();
    }
}  // namespace Horo::Audio
