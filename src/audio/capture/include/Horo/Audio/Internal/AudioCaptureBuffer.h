#pragma once

/** @file AudioCaptureBuffer.h @brief Bounded timestamped PCM transport within one admitted recording generation. */
#include "Horo/Audio/AudioDeviceTiming.h"
#include "Horo/Audio/AudioFormat.h"

#include <memory>
#include <optional>
#include <span>

namespace Horo::Audio::Backend {
    /** @brief Measures the largest finite magnitude in borrowed PCM, clamped to the UI's 0–1 display range. */
    [[nodiscard]] AudioSample MeasureCapturePeak(std::span<const AudioSample> samples) noexcept;

    /** @brief Immutable capture generation and bounded storage selected on Audio control. */
    struct AudioCaptureBufferDescriptor final {
        AudioCaptureDeviceId device;
        std::uint64_t generation{};
        std::uint64_t clockDomain{};
        AudioProcessingFormat format;
        std::uint32_t slots{};
        std::uint32_t maximumFrames{};
        std::size_t budgetBytes{};
    };

    /** @brief Metadata for interleaved binary32 PCM in the descriptor's semantic channel order. */
    struct AudioCapturedBlock final {
        AudioMonotonicTimestamp start;
        std::uint64_t firstFrame{};
        std::uint64_t sequence{};
        std::uint64_t droppedFramesBefore{}; /**< Cumulative frames discarded by ring overflow before this block. */
        std::uint32_t frames{};
        std::uint64_t missingSourceFramesBefore{}; /**< Cumulative source gaps after the first observed frame, independent of overflow. */
    };

    /** @brief Callback admission outcome; a full ring consumes the source sequence and drops the new block. */
    enum class AudioCaptureWrite : std::uint8_t {
        Accepted,
        Full,
        Invalid,
        Cancelled,
        Exhausted
    };

    /** @brief Copy result; insufficient space leaves the block queued. */
    enum class AudioCaptureRead : std::uint8_t {
        Copied,
        Empty,
        InsufficientSpace,
        Cancelled
    };

    /**
     * @brief Audio-owned SPSC ring prepared before a backend attaches its capture callback.
     * @details The producer owns source timing and write position. The consumer owns read position.
     * Publishes and copies use fixed storage and never wait. Audio control can close admission from
     * either thread or its own thread. Calls already in progress may finish. Before destruction,
     * control must detach/join producer and consumer. One ring serves one consumer and generation.
     */
    class AudioCaptureBuffer final {
    public:
        /** @brief Creates a ring and reserves PCM and slot metadata within the caller's byte budget.
         * @param descriptor Exact recording identity, interleaved format and power-of-two slot count (2–256).
         * @return Owned ring, or a typed descriptor, budget or allocation failure.
         */
        [[nodiscard]] static Result<AudioCaptureBuffer> Create(const AudioCaptureBufferDescriptor &descriptor);
        ~AudioCaptureBuffer();
        AudioCaptureBuffer(AudioCaptureBuffer &&) noexcept;
        AudioCaptureBuffer &operator=(AudioCaptureBuffer &&) noexcept;
        AudioCaptureBuffer(const AudioCaptureBuffer &) = delete;
        AudioCaptureBuffer &operator=(const AudioCaptureBuffer &) = delete;

        /** @brief Copies a complete source block or counts its frames when the ring is full.
         * @param start First sample time on the admitted host clock domain.
         * @param firstFrame Source sample-frame index. Gaps are allowed and remain observable.
         * @param frames Positive number of frames, at most maximumFrames.
         * @param pcm Interleaved samples in the exact negotiated channel order.
         * @return Admission result. Invalid data has no effect on ordering or counters.
         */
        [[nodiscard]] AudioCaptureWrite TryWrite(AudioMonotonicTimestamp start, std::uint64_t firstFrame, std::uint32_t frames,
                                                 std::span<const AudioSample> pcm) noexcept;
        /** @brief Copies the oldest block into consumer-owned storage.
         * @param block Metadata destination, changed only on success.
         * @param pcm Destination for the valid interleaved sample prefix, unchanged on failure.
         * @return Copy status. No internal storage escapes.
         */
        [[nodiscard]] AudioCaptureRead TryRead(AudioCapturedBlock &block, std::span<AudioSample> pcm) noexcept;
        /** @brief Borrows the exact admitted identity and format for every block. Null on a moved-from ring. */
        [[nodiscard]] const AudioCaptureBufferDescriptor *Descriptor() const noexcept;
        /** @brief Returns the cumulative saturating overflow frame count, including drops after the last queued block. */
        [[nodiscard]] std::uint64_t DroppedFrames() const noexcept;
        /** @brief Reads the last discarded block's timing without copying PCM or waiting for the producer.
         * @return Coherent timing from the latest observed overflow, or empty before overflow or during a concurrent update.
         * @details The metadata remains readable after Cancel while the control owner retains the buffer.
         */
        [[nodiscard]] std::optional<AudioCapturedBlock> LatestDroppedBlock() const noexcept;
        /** @brief Permanently closes reads and writes to this generation. Storage retires after owners detach. */
        void Cancel() noexcept;

    private:
        struct State;
        /** @brief Adopts prepared storage. */
        explicit AudioCaptureBuffer(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Audio::Backend
