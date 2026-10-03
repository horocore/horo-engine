#include "Horo/Audio/AudioCommandBuffer.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>
#include <utility>

namespace Horo::Audio {
    namespace {
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
        static_assert(std::atomic<bool>::is_always_lock_free);
        static_assert(alignof(AudioCommandRecord) <= AudioMemoryAlignment);
        static_assert(std::is_nothrow_default_constructible_v<AudioCommandRecord>);

        /** @brief Keep cursor subtraction unambiguous and modulo indexing valid through unsigned rollover. */
        bool ValidDimensions(const AudioCommandBufferDescriptor &descriptor) noexcept {
            return descriptor.epoch != 0 && descriptor.slots >= 2 && descriptor.slots <= MaximumAudioCommandSlots &&
                   std::has_single_bit(descriptor.slots) && descriptor.criticalSlots > 0 && descriptor.criticalSlots < descriptor.slots;
        }
    }  // namespace

    /** @brief Producer owns write/lastSequence; consumer owns read; release/acquire pairs protect ring payload reuse. */
    struct AudioCommandBuffer::State {
        AudioCommandBufferDescriptor descriptor;
        AudioMemoryPool storage;
        std::span<AudioCommandRecord> records;
        alignas(AudioMemoryAlignment) std::atomic<std::uint32_t> write{};
        alignas(AudioMemoryAlignment) std::atomic<std::uint32_t> read{};
        std::atomic<bool> closed{};
        std::uint64_t lastSequence{}; /**< Control-producer only; not read by the callback. */

        /** @brief A fresh one-slot pool must admit its single record array; initialize object lifetimes before publication. */
        State(const AudioCommandBufferDescriptor &description, AudioMemoryPool pool, const std::span<std::byte> allocation)
            : descriptor(description), storage(std::move(pool)),
              records(static_cast<AudioCommandRecord *>(static_cast<void *>(allocation.data())), description.slots) {
            std::uninitialized_value_construct_n(records.data(), records.size());
        }

        /** @brief End record object lifetimes only after the host has joined both SPSC participants. */
        ~State() {
            std::destroy_n(records.data(), records.size());
        }

        /** @brief Reject before touching a queue slot; normalization cannot consult mutable registries or leases. */
        AudioCommandPublishStatus Validate(const AudioCommandRecord &record, AudioCommand &normalized) const noexcept {
            using enum AudioCommandPublishStatus;
            if (closed.load(std::memory_order_relaxed)) {  // NOSONAR - the control producer owns both close and publication.
                return Closed;
            }
            if (record.sequence <= lastSequence) {
                return InvalidSequence;
            }
            if (record.command.scope.owner != descriptor.owner || record.command.scope.epoch != descriptor.epoch ||
                NormalizeAudioCommand(record.command, normalized) != AudioCommandStatus::Ok) {
                return InvalidCommand;
            }
            return Published;
        }
    };

    /** @copydoc AudioCommandBuffer::Create */
    Result<AudioCommandBuffer> AudioCommandBuffer::Create(const AudioCommandBufferDescriptor &descriptor) {
        using enum AudioMemoryStatus;
        if (!ValidDimensions(descriptor)) {
            return Result<AudioCommandBuffer>::Failure(MakeError(AudioErrors::CommandBufferInvalid));
        }
        auto pool = AudioMemoryPool::Create({.owner = descriptor.owner,
                                             .identity = descriptor.storageIdentity,
                                             .purpose = AudioMemoryPurpose::CommandStorage,
                                             .slots = 1,
                                             .blockBytes = sizeof(AudioCommandRecord) * descriptor.slots,
                                             .budgetBytes = descriptor.budgetBytes});
        if (!pool.HasValue()) {
            return Result<AudioCommandBuffer>::Failure(pool.ErrorValue());
        }
        auto storage = std::move(pool).Value();
        const auto allocation = storage.Acquire();
        if (const auto requiredBytes = sizeof(AudioCommandRecord) * descriptor.slots;
            allocation.status != Ok || allocation.bytes.size() < requiredBytes) {
            return Result<AudioCommandBuffer>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
        try {
            return Result<AudioCommandBuffer>::Success(
                AudioCommandBuffer{std::make_unique<State>(descriptor, std::move(storage), allocation.bytes)});
        } catch (const std::bad_alloc &) {
            return Result<AudioCommandBuffer>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioCommandBuffer::AudioCommandBuffer */
    AudioCommandBuffer::AudioCommandBuffer(std::unique_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc AudioCommandBuffer::~AudioCommandBuffer */
    AudioCommandBuffer::~AudioCommandBuffer() = default;
    /** @copydoc AudioCommandBuffer::AudioCommandBuffer */
    AudioCommandBuffer::AudioCommandBuffer(AudioCommandBuffer &&) noexcept = default;
    /** @copydoc AudioCommandBuffer::operator= */
    AudioCommandBuffer &AudioCommandBuffer::operator=(AudioCommandBuffer &&) noexcept = default;

    /** @copydoc AudioCommandBuffer::TryPublish */
    AudioCommandPublishStatus AudioCommandBuffer::TryPublish(const AudioCommandRecord &record) noexcept {
        using enum AudioCommandPublishStatus;
        if (!state_) {
            return Inactive;
        }
        auto &state = *state_;
        AudioCommand normalized;
        if (const auto validation = state.Validate(record, normalized); validation != Published) {
            return validation;
        }
        const auto write = state.write.load(std::memory_order_relaxed);  // NOSONAR - write is producer-owned.
        const auto read = state.read.load(std::memory_order_acquire);    // NOSONAR - pairs with consumer slot release.
        const bool critical = ClassifyAudioCommand(normalized) == AudioCommandClass::Critical;
        if (const auto limit = critical ? state.descriptor.slots : state.descriptor.slots - state.descriptor.criticalSlots;
            write - read >= limit) {
            return critical ? CriticalRetry : OrdinaryFull;
        }
        state.records[write & (state.descriptor.slots - 1)] = {.sequence = record.sequence, .command = normalized};
        state.lastSequence = record.sequence;
        state.write.store(write + 1, std::memory_order_release);  // NOSONAR - publishes the initialized record.
        return Published;
    }

    /** @copydoc AudioCommandBuffer::TryConsume */
    bool AudioCommandBuffer::TryConsume(AudioCommandRecord &record) noexcept {
        if (!state_) {
            return false;
        }
        auto &state = *state_;
        const auto read = state.read.load(std::memory_order_relaxed);  // NOSONAR - read is consumer-owned.
        if (read == state.write.load(std::memory_order_acquire)) {     // NOSONAR - pairs with producer publication.
            return false;
        }
        record = state.records[read & (state.descriptor.slots - 1)];
        state.read.store(read + 1, std::memory_order_release);  // NOSONAR - releases the copied slot for reuse.
        return true;
    }

    /** @copydoc AudioCommandBuffer::Close */
    void AudioCommandBuffer::Close() noexcept {
        if (state_) {
            state_->closed.store(true, std::memory_order_release);  // NOSONAR - publishes closure to the consumer.
        }
    }

    /** @copydoc AudioCommandBuffer::IsDrained */
    bool AudioCommandBuffer::IsDrained() const noexcept {
        return !state_ || (state_->closed.load(std::memory_order_acquire) &&    // NOSONAR - observes producer closure.
                           state_->read.load(std::memory_order_relaxed) ==      // NOSONAR - read is consumer-owned.
                               state_->write.load(std::memory_order_acquire));  // NOSONAR - observes the last published record.
    }

    /** @copydoc AudioCommandBuffer::Depth */
    std::uint32_t AudioCommandBuffer::Depth() const noexcept {
        if (!state_)
            return 0;
        const auto consumed = state_->read.load(std::memory_order_acquire);
        const auto published = state_->write.load(std::memory_order_acquire);
        return std::min(published - consumed, state_->descriptor.slots);
    }
}  // namespace Horo::Audio
