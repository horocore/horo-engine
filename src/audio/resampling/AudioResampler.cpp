#include "Horo/Audio/AudioResampler.h"

#include "ResamplerKernel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <ranges>
#include <utility>

namespace Horo::Audio {
    namespace {
        /** @brief Check the declared readable/writable prefix without accessing its samples. */
        template <typename Sample> bool ValidPlanes(const std::span<const std::span<Sample>> planes, const std::uint32_t frames) {
            return std::ranges::all_of(planes, [frames](const std::span<Sample> plane) {
                const auto address = reinterpret_cast<std::uintptr_t>(plane.data());
                const std::array requirements{
                    plane.size() >= frames,
                    frames == 0 || plane.data() != nullptr,
                    frames == 0 || address % 64 == 0,
                };
                return std::ranges::all_of(requirements, std::identity{});
            });
        }

        /** @brief Detect intersecting bounded sample prefixes using a total address representation. */
        bool Overlap(const std::span<const float> left, const std::uint32_t leftFrames, const std::span<const float> right,
                     const std::uint32_t rightFrames) {
            if (const std::array hasFrames{leftFrames != 0, rightFrames != 0}; !std::ranges::all_of(hasFrames, std::identity{})) {
                return false;
            }
            const auto a = reinterpret_cast<std::uintptr_t>(left.data());
            const auto b = reinterpret_cast<std::uintptr_t>(right.data());
            return a <= b ? b - a < static_cast<std::uint64_t>(leftFrames) * sizeof(float)
                          : a - b < static_cast<std::uint64_t>(rightFrames) * sizeof(float);
        }

        /** @brief Reject aliasing that could overwrite later input or another output channel. */
        bool SeparatePlanes(const AudioResamplerInput input, const AudioResamplerOutput output) {
            return std::ranges::none_of(std::views::iota(std::size_t{0}, output.planes.size()), [&](const std::size_t channel) {
                const bool aliasesInput = std::ranges::any_of(input.planes, [&](const auto source) {
                    return Overlap(source, input.frames, output.planes[channel], output.capacity);
                });
                const bool aliasesOutput = std::ranges::any_of(std::views::iota(std::size_t{0}, channel), [&](const std::size_t earlier) {
                    return Overlap(output.planes[earlier], output.capacity, output.planes[channel], output.capacity);
                });
                const std::array aliases{aliasesInput, aliasesOutput};
                return std::ranges::any_of(aliases, std::identity{});
            });
        }

        /** @brief Apply finite/subnormal safety without clipping ordinary internal headroom. */
        float SafeSample(const double value, std::uint32_t &faults) {
            if (const std::array invalid{!std::isfinite(value), std::abs(value) > std::numeric_limits<float>::max()};
                std::ranges::any_of(invalid, std::identity{})) {
                ++faults;
                return 0.0F;
            }
            return std::abs(value) < std::numeric_limits<float>::min() ? 0.0F : static_cast<float>(value);
        }
    }  // namespace

    /** @brief One owner mutates bounded relative phase and ring history; no ever-growing sample counter exists. */
    struct AudioResampler::State {
        AudioResamplerPlan plan;
        Detail::ResamplerKernel kernel;
        std::vector<float> history;
        std::uint32_t oldest{};
        std::uint32_t needed{};
        std::uint32_t padding{};
        double fraction{};
        double step{};
        double targetStep{};
        std::uint32_t pitchRamp{};
        bool draining{};
        bool seenInput{};

        /** @brief Prepare retained history outside the callback. */
        State(const AudioResamplerPlan &value, Detail::ResamplerKernel prepared)
            : plan(value), kernel(std::move(prepared)), history(static_cast<std::size_t>(plan.Descriptor().channels) * plan.Taps()) {
            Reset();
        }

        /** @brief Reset the timeline without changing the admitted rate, layout or quality. */
        void Reset() noexcept {
            std::ranges::fill(history, 0.0F);
            oldest = 0;
            needed = plan.Taps() / 2 + 1;
            padding = plan.Taps();
            fraction = 0.0;
            step = targetStep = plan.InputStep();
            pitchRamp = 0;
            draining = false;
            seenInput = false;
        }

        /** @brief Validate the entire call's shape before any state or output mutation. */
        bool ValidCall(const AudioResamplerInput input, const AudioResamplerOutput output) const {
            const auto &descriptor = plan.Descriptor();
            const std::array requirements{
                input.planes.size() == descriptor.channels,
                output.planes.size() == descriptor.channels,
                input.frames <= descriptor.maximumOutputFrames * 64U + plan.Taps(),
                output.capacity <= descriptor.maximumOutputFrames,
                ValidPlanes(input.planes, input.frames),
                ValidPlanes(output.planes, output.capacity),
                SeparatePlanes(input, output),
            };
            return std::ranges::all_of(requirements, std::identity{});
        }

        /** @brief Consume one real or padded frame into every channel's ring at the same phase. */
        void Push(const AudioResamplerInput input, AudioResamplerProgress &progress) {
            const bool real = progress.consumed < input.frames;
            std::ranges::for_each(std::views::iota(std::size_t{0}, input.planes.size()), [&](const std::size_t channel) {
                const float sample = real ? input.planes[channel][progress.consumed] : 0.0F;
                history[channel * plan.Taps() + oldest] = SafeSample(sample, progress.sanitizedSamples);
            });
            if (++oldest == plan.Taps()) {
                oldest = 0;
            }
            --needed;
            if (real) {
                ++progress.consumed;
                seenInput = true;
            } else {
                --padding;
            }
        }

        /** @brief Adopt the end marker only after its entire supplied input has been consumed. */
        void ObserveEnd(const AudioResamplerInput input, const AudioResamplerProgress &progress) {
            if (const std::array reachedEnd{input.endOfStream, progress.consumed == input.frames};
                std::ranges::all_of(reachedEnd, std::identity{})) {
                draining = true;
                if (!seenInput) {
                    padding = 0;
                }
            }
        }

        /** @brief Report the latched terminal state after all bounded tail padding has been consumed. */
        [[nodiscard]] bool Complete() const noexcept {
            const std::array complete{draining, padding == 0};
            return std::ranges::all_of(complete, std::identity{});
        }

        /** @brief Fill only the history needed for one output; starvation preserves all fractional state. */
        bool Fill(const AudioResamplerInput input, AudioResamplerProgress &progress) {
            ObserveEnd(input, progress);
            while (needed != 0) {
                const std::array drainUnavailable{!draining, padding == 0};
                if (const std::array cannotPush{progress.consumed == input.frames, std::ranges::any_of(drainUnavailable, std::identity{})};
                    std::ranges::all_of(cannotPush, std::identity{})) {
                    return false;
                }
                Push(input, progress);
                ObserveEnd(input, progress);
            }
            const std::array canContinue{!draining, padding != 0};
            return std::ranges::any_of(canContinue, std::identity{});
        }

        /** @brief Produce one shared-phase output frame and advance only the bounded relative input position. */
        void Emit(const AudioResamplerOutput output, AudioResamplerProgress &progress) {
            std::ranges::for_each(std::views::iota(std::size_t{0}, output.planes.size()), [&](const std::size_t channel) {
                const auto samples = std::span<const float>{history}.subspan(channel * plan.Taps(), plan.Taps());
                output.planes[channel][progress.produced] =
                    SafeSample(kernel.Evaluate(samples, oldest, fraction), progress.sanitizedSamples);
            });
            ++progress.produced;
            if (pitchRamp != 0) {
                step += (targetStep - step) / pitchRamp;
                --pitchRamp;
            }
            const double advanced = fraction + step;
            needed = static_cast<std::uint32_t>(advanced);
            fraction = advanced - needed;
        }
    };

    /** @copydoc AudioResampler::Create */
    Result<AudioResampler> AudioResampler::Create(const AudioResamplerPlan &plan, const std::uint64_t maximumCoefficientBytes,
                                                  const AudioResamplerExecution execution) {
        auto kernel = Detail::ResamplerKernel::Prepare(plan, maximumCoefficientBytes, execution);
        if (kernel.HasError()) {
            return Result<AudioResampler>::Failure(kernel.ErrorValue());
        }
        return Result<AudioResampler>::Success(AudioResampler{std::make_unique<State>(plan, std::move(kernel).Value())});
    }

    /** @copydoc AudioResampler::SupportsSimd */
    bool AudioResampler::SupportsSimd() noexcept {
        return Detail::NativeResamplerEvaluator() != nullptr;
    }

    /** @copydoc AudioResampler::AudioResampler */
    AudioResampler::AudioResampler(std::unique_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc AudioResampler::~AudioResampler */
    AudioResampler::~AudioResampler() = default;
    /** @copydoc AudioResampler::AudioResampler */
    AudioResampler::AudioResampler(AudioResampler &&) noexcept = default;
    /** @copydoc AudioResampler::operator= */
    AudioResampler &AudioResampler::operator=(AudioResampler &&) noexcept = default;

    /** @copydoc AudioResampler::SetLinearPitch */
    bool AudioResampler::SetLinearPitch(const double pitch, const std::uint32_t rampFrames) noexcept {
        if (!state_ || !std::isfinite(pitch) || pitch < 0.125 || pitch > 8.0 || rampFrames > 16384)
            return false;
        const auto &descriptor = state_->plan.Descriptor();
        const double step = static_cast<double>(descriptor.inputRate) / descriptor.outputRate * pitch;
        if (descriptor.stage != AudioResamplerStage::ClipToMix || descriptor.quality != AudioResamplerQuality::Linear ||
            step < 1.0 / 64.0 || step > 64.0)
            return false;
        state_->targetStep = step;
        state_->pitchRamp = rampFrames;
        if (rampFrames == 0)
            state_->step = step;
        return true;
    }

    /** @copydoc AudioResampler::Reset */
    void AudioResampler::Reset() noexcept {
        if (state_) {
            state_->Reset();
        }
    }

    /** @copydoc AudioResampler::Plan */
    const AudioResamplerPlan *AudioResampler::Plan() const noexcept {
        return state_ ? &state_->plan : nullptr;
    }

    /** @copydoc AudioResampler::IsFresh */
    bool AudioResampler::IsFresh() const noexcept {
        return state_ && !state_->seenInput && !state_->draining;
    }

    /** @copydoc AudioResampler::Process */
    AudioResamplerProgress AudioResampler::Process(const AudioResamplerInput input, const AudioResamplerOutput output) noexcept {
        using enum AudioResamplerStatus;
        if (!state_) {
            return {.status = InvalidState};
        }
        if (const std::array validState{!state_->draining, input.frames == 0}; !std::ranges::any_of(validState, std::identity{})) {
            return {.status = InvalidState};
        }
        if (!state_->ValidCall(input, output)) {
            return {.status = InvalidBuffer};
        }
        if (state_->Complete()) {
            return {.status = Complete};
        }
        AudioResamplerProgress progress{.status = OutputFull};
        while (progress.produced < output.capacity) {
            if (!state_->Fill(input, progress)) {
                progress.status = state_->Complete() ? Complete : InputNeeded;
                break;
            }
            state_->Emit(output, progress);
        }
        return progress;
    }
}  // namespace Horo::Audio
