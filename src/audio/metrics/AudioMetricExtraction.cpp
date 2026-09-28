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

namespace Horo::Audio {
    namespace {
        constexpr auto KindCount = static_cast<std::size_t>(AudioExtractionKind::Count);
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert(std::atomic<bool>::is_always_lock_free);
        static_assert(std::is_trivially_copyable_v<AudioExtractionRecord>);

        /** @brief Adds callback-owned totals without wrapping at exhaustion. */
        void Increment(std::atomic<std::uint64_t> &counter, const std::uint64_t amount = 1) noexcept {
            const auto current = counter.load(std::memory_order_relaxed);
            counter.store(current + std::min(amount, std::numeric_limits<std::uint64_t>::max() - current), std::memory_order_relaxed);
        }

        /** @brief Separates sparse diagnostic facts from per-buffer cost samples. */
        bool IsDiagnostic(const AudioExtractionKind kind) noexcept {
            return kind >= AudioExtractionKind::DeadlineOverrun && kind < AudioExtractionKind::Count;
        }
    }  // namespace

    /** @brief Producer owns pending slots and write cursor; consumer owns read cursor and record copies. */
    struct AudioMetricExtractionQueue::State final {
        AudioExtractionDescriptor descriptor;
        std::unique_ptr<AudioExtractionRecord[]> records;
        std::array<AudioExtractionRecord, KindCount> pending{};
        std::array<bool, KindCount> hasPending{};
        std::array<std::uint64_t, KindCount> lastDiagnosticFrame{};
        std::array<bool, KindCount> hasPublishedDiagnostic{};
        alignas(64) std::atomic<std::uint32_t> write{};
        alignas(64) std::atomic<std::uint32_t> read{};
        std::atomic<bool> closed{};
        std::atomic<std::uint64_t> published{};
        std::atomic<std::uint64_t> coalesced{};
        std::atomic<std::uint64_t> dropped{};
        std::atomic<std::uint64_t> rateLimited{};
        const std::thread::id ownerThread{std::this_thread::get_id()};

        State(const AudioExtractionDescriptor &value, std::unique_ptr<AudioExtractionRecord[]> storage) noexcept
            : descriptor(value), records(std::move(storage)) {}
    };

    /** @copydoc AudioMetricExtractionQueue::Create */
    Result<AudioMetricExtractionQueue> AudioMetricExtractionQueue::Create(const AudioExtractionDescriptor &descriptor) {
        if (!MatchesAudioDeviceEpoch(descriptor.epoch, descriptor.epoch) || descriptor.slots < 2 ||
            descriptor.slots > MaximumAudioExtractionSlots || !std::has_single_bit(descriptor.slots))
            return Result<AudioMetricExtractionQueue>::Failure(MakeError(AudioErrors::EventQueueInvalid));
        if (descriptor.budgetBytes < static_cast<std::size_t>(descriptor.slots) * sizeof(AudioExtractionRecord))
            return Result<AudioMetricExtractionQueue>::Failure(MakeError(AudioErrors::MemoryBudgetExceeded));
        try {
            auto records = std::make_unique<AudioExtractionRecord[]>(descriptor.slots);
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
        if (state.closed.load(std::memory_order_relaxed))
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
                Increment(state.rateLimited);
                return RateLimited;
            }
        }
        if (state.hasPending[kind]) {
            auto &pending = state.pending[kind];
            if (pending.samples == std::numeric_limits<std::uint32_t>::max()) {
                Increment(state.dropped);
                return Dropped;
            }
            pending.sampleFrame = record.sampleFrame;
            pending.seconds = std::max(pending.seconds, record.seconds);
            ++pending.samples;
            Increment(state.coalesced);
            return Coalesced;
        }
        state.pending[kind] = record;
        state.hasPending[kind] = true;
        return Queued;
    }

    /** @copydoc AudioMetricExtractionQueue::Flush */
    std::uint32_t AudioMetricExtractionQueue::Flush() noexcept {
        if (!state_ || state_->closed.load(std::memory_order_relaxed))
            return 0;
        auto &state = *state_;
        std::uint32_t count{};
        for (std::size_t kind = 0; kind < KindCount; ++kind) {
            if (!state.hasPending[kind])
                continue;
            const auto producer = state.write.load(std::memory_order_relaxed);
            const auto consumer = state.read.load(std::memory_order_acquire);
            const auto &record = state.pending[kind];
            if (producer - consumer >= state.descriptor.slots) {
                Increment(state.dropped, record.samples);
            } else {
                state.records[producer & (state.descriptor.slots - 1)] = record;
                state.write.store(producer + 1, std::memory_order_release);
                Increment(state.published);
                if (IsDiagnostic(record.kind)) {
                    state.lastDiagnosticFrame[kind] = record.sampleFrame;
                    state.hasPublishedDiagnostic[kind] = true;
                }
                ++count;
            }
            state.hasPending[kind] = false;
        }
        return count;
    }

    /** @copydoc AudioMetricExtractionQueue::Close */
    void AudioMetricExtractionQueue::Close() noexcept {
        if (state_) {
            static_cast<void>(Flush());
            state_->closed.store(true, std::memory_order_release);
        }
    }

    /** @copydoc AudioMetricExtractionQueue::TryConsume */
    bool AudioMetricExtractionQueue::TryConsume(AudioExtractionRecord &record) noexcept {
        if (!state_ || std::this_thread::get_id() != state_->ownerThread)
            return false;
        auto &state = *state_;
        const auto consumer = state.read.load(std::memory_order_relaxed);
        if (consumer == state.write.load(std::memory_order_acquire))
            return false;
        record = state.records[consumer & (state.descriptor.slots - 1)];
        state.read.store(consumer + 1, std::memory_order_release);
        return true;
    }

    /** @copydoc AudioMetricExtractionQueue::Stats */
    AudioExtractionStats AudioMetricExtractionQueue::Stats() const noexcept {
        if (!state_ || std::this_thread::get_id() != state_->ownerThread)
            return {};
        const auto consumed = state_->read.load(std::memory_order_relaxed);
        const auto produced = state_->write.load(std::memory_order_acquire);
        return {.depth = std::min(produced - consumed, state_->descriptor.slots),
                .published = state_->published.load(std::memory_order_relaxed),
                .coalesced = state_->coalesced.load(std::memory_order_relaxed),
                .dropped = state_->dropped.load(std::memory_order_relaxed),
                .rateLimited = state_->rateLimited.load(std::memory_order_relaxed)};
    }

    /** @copydoc AudioMetricExtractionQueue::IsDrained */
    bool AudioMetricExtractionQueue::IsDrained() const noexcept {
        if (!state_)
            return true;
        if (std::this_thread::get_id() != state_->ownerThread)
            return false;
        return state_->closed.load(std::memory_order_acquire) &&
               state_->read.load(std::memory_order_relaxed) == state_->write.load(std::memory_order_acquire);
    }
}  // namespace Horo::Audio
