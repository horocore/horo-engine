#pragma once

/**
 * @file AudioStreamDecoder.h
 * @brief Bounded worker-side runtime stream decode session and provider contract.
 */

#include "Horo/Audio/AudioMediaFormatRegistry.h"
#include "Horo/Foundation/BorrowedCallbackContext.h"
#include "Horo/Foundation/Result.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::Audio {
    /** @brief Immutable decoder capability and exact decoded-frame contract for one source generation. */
    struct AudioStreamDecoderSpec final {
        AudioCodecId codec;
        AudioProcessingFormat outputFormat;
        std::uint64_t frameCount{};
        std::uint32_t maximumFramesPerDecode{};
        std::size_t requiredWorkingBytes{};
        bool seekable{};
    };

    /** @brief Host-admitted upper bounds, independent of a codec provider's advertised requirements. */
    struct AudioStreamDecoderLimits final {
        std::uint64_t maximumFrames{1ULL << 40U};
        std::uint32_t maximumFramesPerDecode{4'096};
        std::uint32_t maximumChannels{MaximumAudioChannels};
        std::size_t maximumWorkingBytes{8U << 20U};
    };

    /** @brief Successful provider progress; endOfStream must match the exact declared frame count. */
    struct AudioStreamDecodeProgress final {
        std::uint32_t frames{};
        bool endOfStream{};
    };

    /** @brief Accepted decoded block location and terminal status. */
    struct AudioStreamDecodedBlock final {
        std::uint64_t firstFrame{};
        std::uint32_t frames{};
        bool endOfStream{};
    };

    /** @brief Session lifecycle as observed by its serialized worker caller. */
    enum class AudioStreamDecoderState : std::uint8_t {
        Ready,
        Ended,
        Cancelled,
        Failed,
        Closed,
    };

    /**
     * @brief Provider operations bound to one owned source generation.
     * @details The provider owns source I/O and codec internals. Decode and seek run only on a worker, use at most the supplied
     * output and scratch spans, and poll cancellation during bounded work. The caller must join an active worker before release.
     * A successful session takes responsibility for invoking release exactly once; a rejected session takes none.
     * Each operation resolves its exact private state type with context.Get<T>() and rejects a mismatch before dereferencing.
     * The context does not extend object/code lifetime and must not cross independently rebuilt ABI boundaries.
     */
    struct AudioStreamDecoderProvider final {
        using DecodeFunction = Result<AudioStreamDecodeProgress> (*)(const BorrowedCallbackContext &context, std::uint64_t firstFrame,
                                                                     std::span<AudioSample> interleavedOutput,
                                                                     std::span<std::byte> workingMemory,
                                                                     const std::atomic<bool> &cancelled);
        using SeekFunction = Result<void> (*)(const BorrowedCallbackContext &context, std::uint64_t targetFrame,
                                              const std::atomic<bool> &cancelled);
        using ReleaseFunction = void (*)(const BorrowedCallbackContext &context) noexcept;

        BorrowedCallbackContext context{}; /**< Type-checked provider state; release responsibility transfers only on success. */
        DecodeFunction decode{};   /**< Bounded synchronous decode; output is discarded when an error or cancellation is returned. */
        SeekFunction seek{};       /**< Required exactly when the specification advertises seekable. */
        ReleaseFunction release{}; /**< Called after the worker has stopped, including failure and cancellation. */
    };

    /**
     * @brief Move-only worker-side decode owner; never passed to or invoked from the audio callback.
     * @details Decode and Seek are serialized by one worker. Cancel may be called concurrently from control. Close/destruction
     * follows worker completion; it does not wait or perform hidden scheduling. Decoded blocks are candidates until the
     * audio control owner publishes them through the normal ADR-062 epoch/ring boundary.
     */
    class AudioStreamDecoder final {
    public:
        /**
         * @brief Validates capabilities, budgets and provider operations before accepting ownership.
         * @param spec Exact decoded format, frame count, block limit, scratch requirement and seek capability.
         * @param provider Provider context and operations transferred only on success.
         * @param limits Host-admitted finite resource limits.
         * @return Session or typed invalid/capacity error; failure does not call release.
         */
        [[nodiscard]] static Result<AudioStreamDecoder> Create(AudioStreamDecoderSpec spec, const AudioStreamDecoderProvider &provider,
                                                               const AudioStreamDecoderLimits &limits = {});

        AudioStreamDecoder(const AudioStreamDecoder &) = delete;
        AudioStreamDecoder &operator=(const AudioStreamDecoder &) = delete;
        AudioStreamDecoder(AudioStreamDecoder &&other) noexcept;
        AudioStreamDecoder &operator=(AudioStreamDecoder &&) = delete;
        ~AudioStreamDecoder();

        /**
         * @brief Decodes the next bounded block into caller-owned interleaved binary32 storage.
         * @param output Capacity for at least requestedFrames times the declared channel count.
         * @param workingMemory Capacity for at least the declared scratch requirement.
         * @param requestedFrames Positive count within the declared per-call maximum.
         * @return Accepted block, including an idempotent empty terminal block at end, or a typed failure.
         * @note Invalid call arguments leave the cursor unchanged. Provider error, malformed progress or cancellation
         * closes admission and the caller must discard any output written during that call.
         */
        [[nodiscard]] Result<AudioStreamDecodedBlock> Decode(std::span<AudioSample> output, std::span<std::byte> workingMemory,
                                                             std::uint32_t requestedFrames);

        /**
         * @brief Repositions a seek-capable provider at an exact source frame, including the terminal frame.
         * @param targetFrame Frame within [0, frameCount].
         * @return Success or typed unsupported/invalid/provider/cancelled failure.
         * @note A provider failure latches Failed because its private cursor may have changed.
         */
        [[nodiscard]] Result<void> Seek(std::uint64_t targetFrame);

        /** @brief Requests cooperative cancellation from any thread without waiting or releasing provider state. */
        void Cancel() noexcept;

        /** @brief Releases the provider once after worker completion; repeated calls are harmless. */
        void Close() noexcept;

        /** @brief Reports worker-observed state. @return Current session state; call only after worker synchronization. */
        [[nodiscard]] AudioStreamDecoderState State() const noexcept;
        /** @brief Reports the next source frame. @return Committed cursor; call only after worker synchronization. */
        [[nodiscard]] std::uint64_t CursorFrame() const noexcept;
        /** @brief Borrows the immutable session specification. @return Specification valid until Close or destruction. */
        [[nodiscard]] const AudioStreamDecoderSpec &Spec() const noexcept;

    private:
        AudioStreamDecoder(AudioStreamDecoderSpec spec, const AudioStreamDecoderProvider &provider) noexcept;

        AudioStreamDecoderSpec spec_;
        AudioStreamDecoderProvider provider_;
        std::atomic<bool> cancelled_{false};
        std::uint64_t cursor_{};
        AudioStreamDecoderState state_{AudioStreamDecoderState::Ready};
    };
}  // namespace Horo::Audio
