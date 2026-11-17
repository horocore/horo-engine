#include "AudioStreamState.h"
#include "Horo/Audio/AudioStreamingService.h"

#include <algorithm>
#include <limits>

namespace Horo::Audio {
    namespace {
        /** @brief Checks callback arguments without touching worker-owned data. */
        bool ValidOutput(const AudioStreamState *state, const std::span<AudioSample *const> planes, const std::uint32_t frames) noexcept {
            return state != nullptr && frames != 0 && frames <= MaximumAudioCallbackFrames &&
                   planes.size() == state->request.decoder.outputFormat.layout.orderedChannels.size() &&
                   std::ranges::all_of(planes, [](const auto *plane) {
                return plane != nullptr;
            });
        }

        /** @brief Copies acquired ring samples and pads the remainder with positive zero. */
        void CopyOutput(const AudioStreamState &state, const std::span<AudioSample *const> planes, const std::uint64_t read,
                        const std::uint32_t available, const std::uint32_t frames) noexcept {
            for (std::uint32_t frame = 0; frame < available; ++frame) {
                const auto offset = static_cast<std::size_t>((read + frame) % state.request.ringFrames) * planes.size();
                for (std::size_t channel = 0; channel < planes.size(); ++channel)
                    planes[channel][frame] = state.ring[offset + channel];
            }
            for (auto *plane : planes)
                std::fill_n(plane + available, frames - available, 0.0F);
        }

        /** @brief Records saturating counters on the single callback consumer and applies its underrun policy. */
        void RecordUnderrun(AudioStreamState &state, const std::uint32_t missing) noexcept {
            const auto oldFrames = state.underrunFrames.load(std::memory_order_seq_cst);
            state.underrunFrames.store(oldFrames > std::numeric_limits<std::uint64_t>::max() - missing
                                           ? std::numeric_limits<std::uint64_t>::max()
                                           : oldFrames + missing,
                                       std::memory_order_seq_cst);
            const auto oldCallbacks = state.underrunCallbacks.load(std::memory_order_seq_cst);
            state.underrunCallbacks.store(oldCallbacks == std::numeric_limits<std::uint64_t>::max() ? oldCallbacks : oldCallbacks + 1,
                                          std::memory_order_seq_cst);
            if (state.request.underrunPolicy == AudioStreamUnderrunPolicy::StopWithSilence)
                state.stopped.store(true, std::memory_order_seq_cst);
        }
    }  // namespace

    /** @copydoc AudioStreamRenderPort::~AudioStreamRenderPort */
    AudioStreamRenderPort::~AudioStreamRenderPort() {
        if (retained_ && state_ != nullptr) {
            state_->retainedPort = false;
            state_->portIssued = false;
        }
    }

    /** @copydoc AudioStreamRenderPort::Render */
    AudioStreamRenderResult AudioStreamRenderPort::Render(const std::span<AudioSample *const> planes,
                                                          const std::uint32_t frames) const noexcept {
        AudioStreamRenderResult result;
        if (!ValidOutput(state_, planes, frames))
            return result;
        if (!state_->transport.positionReady.load()) {
            for (auto *plane : planes)
                std::fill_n(plane, frames, 0.0F);
            return {.silentFrames = frames, .stopped = state_->stopped.load()};
        }
        const bool stopped = state_->stopped.load(std::memory_order_seq_cst);
        const std::uint64_t read = state_->transport.consumed.load(std::memory_order_seq_cst);
        const std::uint64_t publication = state_->transport.produced.load(std::memory_order_seq_cst);
        const std::uint64_t written = publication & CursorMask;
        const auto available = stopped || written < read ? 0U : static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, written - read));
        CopyOutput(*state_, planes, read, available, frames);
        state_->transport.consumed.store(read + available, std::memory_order_seq_cst);

        const bool ended = (publication & EndMarker) != 0 && read + available == written;
        const std::uint32_t missing = frames - available;
        if (missing != 0 && !stopped && !ended)
            RecordUnderrun(*state_, missing);
        result.availableFrames = available;
        result.silentFrames = missing;
        result.ended = ended;
        result.stopped = state_->stopped.load(std::memory_order_seq_cst);
        return result;
    }
}  // namespace Horo::Audio
