#include "Horo/Audio/Internal/AudioCaptureBuffer.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Security/SecureMemory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Audio::Backend {
    namespace {
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        static_assert(std::atomic<bool>::is_always_lock_free);
        constexpr std::uint32_t MaximumSlots = 256;
        constexpr auto MaximumFrameIndex = std::numeric_limits<std::uint64_t>::max();

        /** @brief Checks timing identity and storage bounds before computing allocation sizes. */
        bool ValidDescriptor(const AudioCaptureBufferDescriptor &descriptor) noexcept {
            return descriptor.device.IsValid() && descriptor.generation != 0 && descriptor.clockDomain != 0 &&
                   descriptor.maximumFrames > 0 && descriptor.maximumFrames <= MaximumAudioCallbackFrames && descriptor.slots >= 2 &&
                   descriptor.slots <= MaximumSlots && std::has_single_bit(descriptor.slots) &&
                   ValidateAudioProcessingFormat(descriptor.format);
        }
    }  // namespace

    /** @copydoc MeasureCapturePeak */
    AudioSample MeasureCapturePeak(const std::span<const AudioSample> samples) noexcept {
        AudioSample peak{};
        for (const auto sample : samples) {
            if (std::isfinite(sample))
                peak = std::max(peak, std::abs(sample));
        }
        return std::min(peak, 1.0F);
    }

    /** @brief Producer and consumer each advance only their own ring cursor. */
    struct AudioCaptureBuffer::State final {
        const AudioCaptureBufferDescriptor descriptor;
        const std::size_t channels;
        const std::size_t samplesPerSlot;
        std::vector<AudioCapturedBlock> blocks;
        std::vector<AudioSample> samples;
        alignas(64) std::atomic<std::uint32_t> write{};
        alignas(64) std::atomic<std::uint32_t> read{};
        std::atomic<bool> cancelled{};
        std::atomic<std::uint64_t> dropped{};

        // Only the producer writes overflow metadata. Readers attempt one coherent snapshot and never wait.
        struct OverflowPublication final {
            std::atomic<bool> updating{};
            std::atomic<std::uint64_t> sequence{};
            std::atomic<std::uint64_t> nanoseconds{};
            std::atomic<std::uint64_t> firstFrame{};
            std::atomic<std::uint64_t> droppedBefore{};
            std::atomic<std::uint32_t> frames{};

            /** @brief Publishes the latest discarded block without retaining its PCM. */
            void Publish(const AudioCapturedBlock &block) noexcept {
                updating.store(true);
                nanoseconds.store(block.start.nanoseconds);
                firstFrame.store(block.firstFrame);
                droppedBefore.store(block.droppedFramesBefore);
                frames.store(block.frames);
                sequence.store(block.sequence);
                updating.store(false);
            }

            /** @brief Observes all fields from one nonrepeating callback sequence, or discards a racing read. */
            [[nodiscard]] std::optional<AudioCapturedBlock> Observe(const std::uint64_t clockDomain) const noexcept {
                const auto observed = sequence.load();
                if (observed == 0 || updating.load())
                    return std::nullopt;
                const AudioCapturedBlock snapshot{{clockDomain, nanoseconds.load()},
                                                  firstFrame.load(),
                                                  observed,
                                                  droppedBefore.load(),
                                                  frames.load()};
                if (updating.load() || sequence.load() != observed)
                    return std::nullopt;
                return snapshot;
            }
        };

        OverflowPublication overflow;

        // Only the producer reads or writes source chronology.
        std::uint64_t sequence{};
        std::uint64_t nextFrame{};
        std::uint64_t previousStart{};

        /** @brief Reserves the generation's descriptors and all sample slots before attachment. */
        explicit State(const AudioCaptureBufferDescriptor &description)
            : descriptor(description), channels(description.format.layout.orderedChannels.size()),
              samplesPerSlot(channels * description.maximumFrames), blocks(description.slots), samples(description.slots * samplesPerSlot) {
        }

        /** @brief Scrubs queued or abandoned samples after both endpoints detach. */
        ~State() {
            Security::SecureZero(std::as_writable_bytes(std::span{samples}));
        }

        /** @brief Tests source chronology and exact interleaved sample shape. */
        bool ValidBlock(const AudioMonotonicTimestamp start, const std::uint64_t firstFrame, const std::uint32_t frames,
                        const std::size_t sampleCount) const noexcept {
            return start.clockDomain == descriptor.clockDomain && start.nanoseconds >= previousStart && firstFrame >= nextFrame &&
                   frames > 0 && frames <= descriptor.maximumFrames && firstFrame <= MaximumFrameIndex - frames &&
                   sampleCount == frames * channels;
        }

        /** @brief One producer serializes overflow; the observer only reads the resulting cumulative value. */
        void CountDrop(const AudioCapturedBlock &block) noexcept {
            const auto previous = block.droppedFramesBefore;
            dropped.store(previous + std::min<std::uint64_t>(block.frames, MaximumFrameIndex - previous));
            overflow.Publish(block);
        }
    };

    /** @copydoc AudioCaptureBuffer::Create */
    Result<AudioCaptureBuffer> AudioCaptureBuffer::Create(const AudioCaptureBufferDescriptor &descriptor) {
        using Output = Result<AudioCaptureBuffer>;
        if (!ValidDescriptor(descriptor))
            return Output::Failure(MakeError(AudioErrors::MemoryInvalid));
        const auto &channels = descriptor.format.layout.orderedChannels;
        const auto sampleBytes = descriptor.maximumFrames * channels.size() * sizeof(AudioSample);
        if (const auto required =
                sizeof(State) + channels.size() * sizeof(channels.front()) + descriptor.slots * (sizeof(AudioCapturedBlock) + sampleBytes);
            required > descriptor.budgetBytes)
            return Output::Failure(MakeError(AudioErrors::MemoryBudgetExceeded));
        try {
            return Output::Success(AudioCaptureBuffer{std::make_unique<State>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioCaptureBuffer::AudioCaptureBuffer */
    AudioCaptureBuffer::AudioCaptureBuffer(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    AudioCaptureBuffer::~AudioCaptureBuffer() = default;
    AudioCaptureBuffer::AudioCaptureBuffer(AudioCaptureBuffer &&) noexcept = default;
    AudioCaptureBuffer &AudioCaptureBuffer::operator=(AudioCaptureBuffer &&) noexcept = default;

    /** @copydoc AudioCaptureBuffer::TryWrite */
    AudioCaptureWrite AudioCaptureBuffer::TryWrite(const AudioMonotonicTimestamp start, const std::uint64_t firstFrame,
                                                   const std::uint32_t frames, const std::span<const AudioSample> pcm) noexcept {
        using enum AudioCaptureWrite;
        if (!state_ || state_->cancelled.load())
            return Cancelled;
        auto &state = *state_;
        if (!state.ValidBlock(start, firstFrame, frames, pcm.size()))
            return Invalid;
        if (state.sequence == MaximumFrameIndex)
            return Exhausted;

        ++state.sequence;
        state.nextFrame = firstFrame + frames;
        state.previousStart = start.nanoseconds;
        const auto write = state.write.load();
        const AudioCapturedBlock block{start, firstFrame, state.sequence, state.dropped.load(), frames};
        if (write - state.read.load() >= state.descriptor.slots) {
            state.CountDrop(block);
            return Full;
        }
        const auto slot = write & (state.descriptor.slots - 1);
        std::ranges::copy(pcm, state.samples.data() + slot * state.samplesPerSlot);
        state.blocks[slot] = block;
        state.write.store(write + 1);
        return Accepted;
    }

    /** @copydoc AudioCaptureBuffer::TryRead */
    AudioCaptureRead AudioCaptureBuffer::TryRead(AudioCapturedBlock &block, const std::span<AudioSample> pcm) noexcept {
        using enum AudioCaptureRead;
        if (!state_ || state_->cancelled.load())
            return Cancelled;
        auto &state = *state_;
        const auto read = state.read.load();
        if (read == state.write.load())
            return Empty;
        const auto slot = read & (state.descriptor.slots - 1);
        const auto &source = state.blocks[slot];
        const auto sampleCount = source.frames * state.channels;
        if (sampleCount > pcm.size())
            return InsufficientSpace;
        const std::span sourcePcm{state.samples.data() + slot * state.samplesPerSlot, sampleCount};
        std::ranges::copy(sourcePcm, pcm.begin());
        Security::SecureZero(std::as_writable_bytes(sourcePcm));
        block = source;
        state.read.store(read + 1);
        return Copied;
    }

    /** @copydoc AudioCaptureBuffer::Descriptor */
    const AudioCaptureBufferDescriptor *AudioCaptureBuffer::Descriptor() const noexcept {
        return state_ ? &state_->descriptor : nullptr;
    }

    /** @copydoc AudioCaptureBuffer::DroppedFrames */
    std::uint64_t AudioCaptureBuffer::DroppedFrames() const noexcept {
        return state_ ? state_->dropped.load() : 0;
    }

    /** @copydoc AudioCaptureBuffer::LatestDroppedBlock */
    std::optional<AudioCapturedBlock> AudioCaptureBuffer::LatestDroppedBlock() const noexcept {
        return state_ ? state_->overflow.Observe(state_->descriptor.clockDomain) : std::nullopt;
    }

    /** @copydoc AudioCaptureBuffer::Cancel */
    void AudioCaptureBuffer::Cancel() noexcept {
        if (state_)
            state_->cancelled.store(true);
    }
}  // namespace Horo::Audio::Backend
