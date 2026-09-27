#include "AlignedAudioStorage.h"
#include "Horo/Audio/AudioMemory.h"
#include "Horo/Audio/Internal/AudioCallbackSafetyHooks.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <ranges>
#include <utility>

namespace Horo::Audio {
    /** @brief The processing owner exclusively mutates epoch/cursor/metrics; no cross-thread access is implied. */
    struct AudioScratchArena::State {
        AudioRuntimeId owner;
        Detail::AlignedAudioStorage storage;
        AudioMemoryStats stats;
        std::uint64_t epoch{};

        /** @brief Construct only from a validated size; allocation failure rolls back the unpublished owner. */
        State(const AudioRuntimeId runtime, const std::size_t capacity)
            : owner(runtime), storage(Detail::AllocateAudioStorage(capacity)), stats{.reservedBytes = capacity} {}

        /** @brief Record a failed admission without wrapping long-lived pressure telemetry. */
        AudioScratchAllocation Fail(const AudioMemoryStatus status) noexcept {
            stats.failedAllocations += static_cast<std::uint64_t>(stats.failedAllocations != std::numeric_limits<std::uint64_t>::max());
            return {.status = status, .bytes = {}, .epoch = epoch};
        }
    };

    /** @copydoc AudioScratchArena::Create */
    Result<AudioScratchArena> AudioScratchArena::Create(const AudioRuntimeId owner, const std::size_t capacityBytes) {
        if (const std::array invalidRequest{!owner.IsValid(), capacityBytes == 0, capacityBytes % AudioMemoryAlignment != 0};
            std::ranges::any_of(invalidRequest, std::identity{})) {
            return Result<AudioScratchArena>::Failure(MakeError(AudioErrors::MemoryInvalid));
        }
        if (capacityBytes > MaximumAudioMemoryBytes) {
            return Result<AudioScratchArena>::Failure(MakeError(AudioErrors::MemoryBudgetExceeded));
        }
        Safety::OnAudioAllocationAttempt();
        try {
            return Result<AudioScratchArena>::Success(AudioScratchArena{std::make_unique<State>(owner, capacityBytes)});
        } catch (const std::bad_alloc &) {
            return Result<AudioScratchArena>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioScratchArena::AudioScratchArena */
    AudioScratchArena::AudioScratchArena(std::unique_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc AudioScratchArena::~AudioScratchArena */
    AudioScratchArena::~AudioScratchArena() = default;
    /** @copydoc AudioScratchArena::AudioScratchArena */
    AudioScratchArena::AudioScratchArena(AudioScratchArena &&) noexcept = default;
    /** @copydoc AudioScratchArena::operator= */
    AudioScratchArena &AudioScratchArena::operator=(AudioScratchArena &&) noexcept = default;

    /** @copydoc AudioScratchArena::BeginEpoch */
    AudioMemoryStatus AudioScratchArena::BeginEpoch(const AudioRuntimeId owner, const std::uint64_t epoch) noexcept {
        using enum AudioMemoryStatus;
        if (!state_) {
            return Inactive;
        }
        if (const std::array invalidEpoch{owner != state_->owner, epoch <= state_->epoch};
            std::ranges::any_of(invalidEpoch, std::identity{})) {
            return InvalidEpoch;
        }
        state_->epoch = epoch;
        state_->stats.usedBytes = 0;
        return Ok;
    }

    /** @copydoc AudioScratchArena::Allocate */
    AudioScratchAllocation AudioScratchArena::Allocate(const std::size_t bytes) noexcept {
        using enum AudioMemoryStatus;
        if (!state_) {
            return {};
        }
        auto &stats = state_->stats;

        struct Rejection {
            bool rejected;
            AudioMemoryStatus status;
        };

        const std::array rejections{Rejection{state_->epoch == 0, Inactive}, Rejection{bytes == 0, InvalidRequest},
                                    Rejection{bytes > stats.reservedBytes - stats.usedBytes, Exhausted}};
        if (const auto rejected = std::ranges::find_if(rejections, &Rejection::rejected); rejected != rejections.end()) {
            return state_->Fail(rejected->status);
        }
        const auto stride = Detail::AudioMemoryStride(bytes);
        auto *start = state_->storage.get() + stats.usedBytes;
        std::fill_n(start, stride, std::byte{0});
        stats.usedBytes += stride;
        stats.peakBytes = std::max(stats.peakBytes, stats.usedBytes);
        return {.status = Ok, .bytes = {start, bytes}, .epoch = state_->epoch};
    }

    /** @copydoc AudioScratchArena::Stats */
    AudioMemoryStats AudioScratchArena::Stats() const noexcept {
        return state_ ? state_->stats : AudioMemoryStats{};
    }
}  // namespace Horo::Audio
