#pragma once

/** @file AudioStreamState.h @brief AudioApi-private retained ring ownership and callback/worker mailboxes. */
#include "Horo/Audio/AudioStreamingService.h"

#include <limits>

namespace Horo::Audio {
    inline constexpr std::uint64_t EndMarker = 1ULL << 63U;
    inline constexpr std::uint64_t CursorMask = EndMarker - 1;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<AudioStreamDecoder *>::is_always_lock_free);

    /** @brief Control-owned cursor for coalesced underrun reporting, never accessed by the callback. */
    struct UnderrunReporting final {
        bool reported{};
        std::uint64_t frames{};
        std::uint64_t callbacks{};
        std::uint64_t sampleFrame{};
    };

    /** @brief Ring publication and position handoff between the sole callback and serialized worker.
     * The callback ends ring reads before publishing seekTarget. The worker resets produced after Seek
     * and acknowledges reuse with positionReady. All mailboxes retain sequentially consistent ordering;
     * fillsSuspended controls future admission, not cancellation or the lifetime of an existing fill.
     */
    struct AudioStreamTransport final {
        std::atomic<std::uint64_t> produced{};
        std::atomic<std::uint64_t> consumed{};
        std::atomic<std::uint64_t> seekTarget{std::numeric_limits<std::uint64_t>::max()};
        std::atomic<bool> positionReady{true};
        std::atomic<bool> fillsSuspended{};
        std::atomic<std::uint64_t> requestedLoop{};
    };

    /** @brief Control retains storage through callback detachment and serialized worker completion.
     * Only atomic mailboxes cross lanes; decoder/workerLoop belong to the worker, reporting/failure/pins to control.
     */
    struct AudioStreamState final {
        AudioStreamRequest request;
        std::vector<AudioSample> ring;
        std::vector<AudioSample> decodeBuffer;
        std::vector<std::byte> scratch;
        std::unique_ptr<AudioStreamDecoder> decoder;
        std::atomic<AudioStreamDecoder *> publishedDecoder{nullptr};
        CancellationSource cancellation;
        AudioStreamTransport transport;
        AudioVoiceLoop workerLoop;
        std::atomic<std::uint64_t> underrunFrames{};
        std::atomic<std::uint64_t> underrunCallbacks{};
        std::atomic<bool> stopped{false};
        bool failed{};
        bool cancelled{};
        bool sourceOpenReturned{}; /**< Worker writes; control reads only after proved completion. */
        std::optional<Error> failure;
        bool portIssued{};
        bool retainedPort{};
        UnderrunReporting reporting;
        std::size_t chargedBytes{};

        explicit AudioStreamState(AudioStreamRequest value, const std::size_t bytes)
            : request(std::move(value)),
              ring(static_cast<std::size_t>(request.ringFrames) * request.decoder.outputFormat.layout.orderedChannels.size()),
              decodeBuffer(static_cast<std::size_t>(request.decoder.maximumFramesPerDecode) *
                           request.decoder.outputFormat.layout.orderedChannels.size()),
              scratch(request.decoder.requiredWorkingBytes), chargedBytes(bytes) {}

        /** @brief Control-only eligibility: terminal, cancelled or virtually idle streams do not admit future jobs. */
        bool FillAdmitted() const noexcept {
            return !failed && !cancelled && !stopped.load() && !cancellation.Token().IsCancellationRequested() &&
                   !transport.fillsSuspended.load();
        }
    };
}  // namespace Horo::Audio
