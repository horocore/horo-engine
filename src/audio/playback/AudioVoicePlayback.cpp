#include "Horo/Audio/AudioVoicePlayback.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <utility>
#include <vector>

namespace Horo::Audio {
    namespace {
        /** @brief Check a source-frame interval before touching processing state. */
        bool ValidLoop(const AudioVoiceLoop loop, const std::uint32_t frames) noexcept {
            return loop.enabled ? loop.begin < loop.end && loop.end <= frames : loop.begin == 0 && loop.end == 0;
        }

        /** @brief Logical playback controls remain available without a physical renderer. */
        bool Controllable(const AudioVoiceState state) noexcept {
            using enum AudioVoiceState;
            return state == Ready || state == Playing || state == Paused || state == Virtual;
        }

        /** @brief Keep one deferred discontinuity's target together until the zero boundary. */
        struct PendingTarget final {
            std::uint32_t frame{};
            AudioVoiceLoop loop;
        };

        /** @brief Keep actual output and its held discontinuity origin in one amplitude history. */
        struct FadeSamples final {
            float playbackGain{1.0F};
            std::array<float, 64> last{};
            std::array<float, 64> held{};
        };

        /** @brief Callback-owned fade counters share one admitted ramp duration; no cross-thread publication. */
        struct FadeProgress final {
            std::uint32_t rampFrames{};
            std::uint32_t remaining{};
            std::uint32_t fadeIn{};
        };

        /** @brief Check one output plane's extent and alignment before the overlap pass. */
        bool ValidPlane(const std::span<float> plane, const std::uint32_t capacity) noexcept {
            return plane.size() >= capacity &&
                   (capacity == 0 || (plane.data() != nullptr && reinterpret_cast<std::uintptr_t>(plane.data()) % 64 == 0));
        }

        /** @brief Admit only source dimensions and intervals supported by resident clip processing. */
        bool ValidSource(const AudioResamplerInput source, const AudioVoicePlaybackConfig &config) noexcept {
            const auto &descriptor = config.plan.Descriptor();
            return descriptor.stage == AudioResamplerStage::ClipToMix && source.planes.size() == descriptor.channels &&
                   ValidLoop(config.loop, source.frames) && config.rampFrames > 0 && config.rampFrames <= 16384 &&
                   std::isfinite(config.gain) && config.gain >= 0.0F && std::ranges::all_of(source.planes, [source](const auto plane) {
                return plane.size() >= source.frames && (source.frames == 0 || plane.data() != nullptr);
            });
        }

        /** @brief Pitch replacement preserves the retained PCM/layout and callback block contract. */
        bool CompatiblePitch(const AudioResamplerDescriptor &next, const AudioResamplerDescriptor &prior) noexcept {
            return next.stage == prior.stage && next.inputRate == prior.inputRate && next.outputRate == prior.outputRate &&
                   next.channels == prior.channels && next.quality == prior.quality &&
                   next.maximumOutputFrames == prior.maximumOutputFrames;
        }

        /** @brief One sample per channel starts aligned even when the destination frame is unaligned. */
        struct alignas(64) SampleCell final {
            std::array<float, 16> samples{};
        };

        /** @brief Fade products retain positive-zero silence without emitting subnormals. */
        float ApplyGain(const float sample, const float gain) noexcept {
            const float result = sample * gain;
            return std::abs(result) < std::numeric_limits<float>::min() ? 0.0F : result;
        }

        /** @brief Validate writable ranges before overwriting any output or advancing a voice. */
        bool ValidOutput(const AudioResamplerOutput output, const AudioResamplerDescriptor &descriptor) noexcept {
            if (output.planes.size() != descriptor.channels || output.capacity > descriptor.maximumOutputFrames)
                return false;
            for (std::size_t channel = 0; channel < output.planes.size(); ++channel) {
                const auto plane = output.planes[channel];
                if (!ValidPlane(plane, output.capacity))
                    return false;
                const auto address = reinterpret_cast<std::uintptr_t>(plane.data());
                for (std::size_t earlier = 0; earlier < channel; ++earlier) {
                    const auto other = reinterpret_cast<std::uintptr_t>(output.planes[earlier].data());
                    const auto distance = address > other ? address - other : other - address;
                    if (distance < static_cast<std::uint64_t>(output.capacity) * sizeof(float))
                        return false;
                }
            }
            return true;
        }
    }  // namespace

    /** @brief Retained PCM and DSP share one exclusive registry/processing lane; spans never escape a call. */
    struct AudioVoicePlayback::State final {
        AudioVoiceStateMachine &registry;
        AudioVoiceHandle voice;
        AudioResampler converter;
        AudioResamplerPlan plan;
        std::vector<float> pcm;
        std::uint32_t sourceFrames{};
        AudioVoiceLoop loop;
        AudioVoiceCursor cursor;
        std::uint32_t readFrame{};
        FadeProgress fade;
        AudioVoiceControl pending{AudioVoiceControl::Start};
        PendingTarget target;
        bool terminalReported{};
        bool virtualPlayback{}; /**< Retained while Paused so Resume restores the admitted execution mode. */
        FadeSamples amplitudes;
        std::array<SampleCell, 64> inputCells;
        std::array<SampleCell, 64> outputCells;
        std::array<std::span<const float>, 64> inputs;
        std::array<std::span<float>, 64> outputs;

        /** @brief Prepare storage and views before the registry acquires a slot. */
        State(AudioVoiceStateMachine &owner, AudioResampler prepared, const AudioVoicePlaybackConfig &config,
              const AudioResamplerInput source)
            : registry(owner), converter(std::move(prepared)), plan(config.plan),
              pcm(static_cast<std::size_t>(source.frames) * plan.Descriptor().channels), sourceFrames(source.frames), loop(config.loop),
              fade{.rampFrames = config.rampFrames}, amplitudes{.playbackGain = config.gain} {
            for (std::uint32_t channel = 0; channel < plan.Descriptor().channels; ++channel) {
                std::ranges::copy(source.planes[channel].first(source.frames),
                                  pcm.begin() + static_cast<std::size_t>(channel) * source.frames);
                inputs[channel] = std::span<const float>{inputCells[channel].samples}.first(1);
                outputs[channel] = std::span<float>{outputCells[channel].samples}.first(1);
            }
        }

        /** @brief Detached control teardown retains canonical terminal state until the slot is released. */
        ~State() {
            if (!voice.IsValid())
                return;
            AudioVoiceState current{};
            if (registry.CheckState(voice, current) == nullptr) {
                if (!IsTerminalAudioVoiceState(current))
                    (void)registry.TryCancel(voice);
                (void)registry.Release(voice);
            }
        }

        /** @brief Require the exact object and live registry generation, never a numerically equal foreign handle. */
        const ErrorCodeDescriptor *Check(const AudioVoiceHandle requested, AudioVoiceState &current) const noexcept {
            if (const auto *error = registry.CheckState(requested, current))
                return error;
            return requested == voice ? nullptr : &AudioErrors::HandleStale;
        }

        /** @brief Discard discontinuous filter state and start reading at the integer rendered cursor. */
        void Reset() noexcept {
            converter.Reset();
            cursor.fraction = 0.0;
            if (loop.enabled && cursor.frame >= loop.end)
                cursor.frame = loop.begin;
            readFrame = static_cast<std::uint32_t>(cursor.frame);
            amplitudes.last.fill(0.0F);
        }

        /** @brief Commit a bounded held-sample ramp's deferred state change exactly once. */
        bool Commit() noexcept {
            using enum AudioVoiceControl;
            using enum AudioVoiceState;
            if (pending == Stop) {
                (void)registry.TryTransition(voice, Stopped);
                converter.Reset();
                return true;
            }
            if (pending == Pause) {
                (void)registry.TryTransition(voice, Paused);
                return false;
            }
            if (pending == Seek)
                cursor.frame = target.frame;
            if (pending == Restart) {
                cursor.frame = 0;
                AudioVoiceState current{};
                (void)registry.CheckState(voice, current);
                if (current == Paused)
                    (void)registry.TryTransition(voice, virtualPlayback ? Virtual : Playing);
                if (current == Ready)
                    (void)StartOrResume(AudioVoiceControl::Start, current);
            }
            if (pending == SetLoop)
                loop = target.loop;
            Reset();
            fade.fadeIn = fade.rampFrames;
            return false;
        }

        /** @brief Start and resume share fade-in while retaining distinct canonical preconditions. */
        const ErrorCodeDescriptor *StartOrResume(const AudioVoiceControl control, const AudioVoiceState current) noexcept {
            using enum AudioVoiceControl;
            using enum AudioVoiceState;
            const bool starting = control == Start || control == StartVirtual;
            if (current != (starting ? Ready : Paused))
                return &AudioErrors::VoiceInvalidTransition;
            if (starting) {
                if (const auto *error = registry.TryTransition(voice, Scheduled))
                    return error;
            }
            const bool nextVirtual = starting ? control == StartVirtual : virtualPlayback;
            if (const auto *error = registry.TryTransition(voice, nextVirtual ? Virtual : Playing))
                return error;
            virtualPlayback = nextVirtual;
            fade.fadeIn = fade.rampFrames;
            return nullptr;
        }

        /** @brief Validate deferred actions before their target or lifecycle changes. */
        const ErrorCodeDescriptor *ValidateDiscontinuity(const AudioVoiceControlRequest &request,
                                                         const AudioVoiceState current) const noexcept {
            using enum AudioVoiceControl;
            switch (request.control) {
                case Pause:
                    return current == AudioVoiceState::Playing || current == AudioVoiceState::Virtual
                               ? nullptr
                               : &AudioErrors::VoiceInvalidTransition;
                case Seek:
                    return request.seekFrame <= sourceFrames && (!loop.enabled || request.seekFrame < loop.end)
                               ? nullptr
                               : &AudioErrors::PlaybackRequestInvalid;
                case SetLoop:
                    return ValidLoop(request.loop, sourceFrames) ? nullptr : &AudioErrors::PlaybackRequestInvalid;
                default:
                    return nullptr;
            }
        }

        /** @brief Stop silent Ready voices through the same canonical scheduled/stopping path. */
        const ErrorCodeDescriptor *BeginStop(const AudioVoiceState current) const noexcept {
            using enum AudioVoiceState;
            if (current == Ready) {
                if (const auto *error = registry.TryTransition(voice, Scheduled))
                    return error;
            }
            return registry.TryTransition(voice, Stopping);
        }

        /** @brief Hold the current rendered amplitude until the deferred action's zero boundary. */
        void BeginDiscontinuity(const AudioVoiceControl control, const AudioVoiceState current, const PendingTarget next) noexcept {
            pending = control;
            target = next;
            amplitudes.held = amplitudes.last;
            fade.remaining = current == AudioVoiceState::Playing ? fade.rampFrames : 0;
            if (fade.remaining == 0)
                (void)Commit();
        }

        /** @brief Execute already structurally valid intent on a controllable voice. */
        const ErrorCodeDescriptor *ApplyControl(const AudioVoiceControlRequest &request, const AudioVoiceState current) noexcept {
            using enum AudioVoiceControl;
            switch (request.control) {
                case Start:
                case StartVirtual:
                case Resume:
                    return StartOrResume(request.control, current);
                case Virtualize:
                case Realize:
                    return ChangeExecution(request.control, current);
                case SetPlaybackSpeed:
                    return request.playbackSpeed == 1.0 ? nullptr : &AudioErrors::OperationUnsupported;
                default:
                    return ApplyDiscontinuity(request, current);
            }
        }

        /** @brief Commit the existing seek/loop/stop discontinuity policy after control dispatch. */
        const ErrorCodeDescriptor *ApplyDiscontinuity(const AudioVoiceControlRequest &request, const AudioVoiceState current) noexcept {
            if (const auto *error = ValidateDiscontinuity(request, current))
                return error;
            if (request.control == AudioVoiceControl::Stop) {
                if (const auto *error = BeginStop(current))
                    return error;
            }
            BeginDiscontinuity(request.control, current, {request.seekFrame, request.loop});
            return nullptr;
        }

        /** @brief Advance silent virtual time without reading PCM or draining a physical filter tail. */
        bool RenderVirtualFrame(AudioVoiceState &current) noexcept {
            if (!loop.enabled && cursor.frame == sourceFrames) {
                (void)registry.TryTransition(voice, AudioVoiceState::Finished);
                return false;
            }
            Advance();
            if (!loop.enabled && cursor.frame == sourceFrames)
                (void)registry.TryTransition(voice, AudioVoiceState::Finished);
            (void)registry.CheckState(voice, current);
            return true;
        }

        /** @brief Switch execution without changing logical phase or allocating a new processing owner. */
        const ErrorCodeDescriptor *ChangeExecution(const AudioVoiceControl control, const AudioVoiceState current) noexcept {
            using enum AudioVoiceState;
            const bool virtualizing = control == AudioVoiceControl::Virtualize;
            if (current != (virtualizing ? Playing : Virtual))
                return &AudioErrors::VoiceInvalidTransition;
            if (const auto *error = registry.TryTransition(voice, virtualizing ? Virtual : Playing))
                return error;
            virtualPlayback = virtualizing;
            // Virtual time has no filter history. Rebuild at the integer cursor, retaining logical
            // fraction: rendered signal phase differs by less than one source frame on realization.
            converter.Reset();
            readFrame = static_cast<std::uint32_t>(cursor.frame);
            amplitudes.last.fill(0.0F);
            fade.fadeIn = fade.rampFrames;
            return nullptr;
        }

        /** @brief Advance audible source time; look-ahead reads are a separate cursor. */
        void Advance() noexcept {
            const double next = cursor.fraction + plan.InputStep();
            const auto whole = static_cast<std::uint32_t>(next);
            cursor.fraction = next - whole;
            const auto frame = static_cast<std::uint64_t>(cursor.frame) + whole;
            if (loop.enabled && frame >= loop.end) {
                cursor.frame = loop.begin + static_cast<std::uint32_t>((frame - loop.begin) % (loop.end - loop.begin));
            } else if (frame >= sourceFrames) {
                cursor = {.frame = sourceFrames};
            } else {
                cursor.frame = static_cast<std::uint32_t>(frame);
            }
        }

        /** @brief Offer one aligned resident frame and advance only the converter's consumed look-ahead. */
        AudioResamplerProgress Feed() noexcept {
            const auto channels = plan.Descriptor().channels;
            const bool available = readFrame < sourceFrames;
            if (available) {
                for (std::uint32_t channel = 0; channel < channels; ++channel)
                    inputCells[channel].samples[0] = pcm[static_cast<std::size_t>(channel) * sourceFrames + readFrame];
            }
            const auto progress = converter.Process({std::span{inputs}.first(channels), available ? 1U : 0U,
                                                     !loop.enabled && (!available || readFrame + 1 == sourceFrames)},
                                                    {std::span{outputs}.first(channels), 1});
            if (progress.consumed != 0) {
                ++readFrame;
                if (loop.enabled && readFrame == loop.end)
                    readFrame = loop.begin;
            }
            return progress;
        }

        /** @brief Feed at most half the admitted kernel plus 65 source frames for one rendered sample. */
        AudioResamplerProgress Sample() noexcept {
            AudioResamplerProgress total{};
            for (std::uint32_t attempt = 0; attempt <= plan.Taps() / 2 + 65; ++attempt) {
                const auto progress = Feed();
                total.sanitizedSamples += progress.sanitizedSamples;
                if (progress.produced != 0 || progress.status != AudioResamplerStatus::InputNeeded) {
                    total.status = progress.status;
                    total.produced = progress.produced;
                    return total;
                }
            }
            total.status = AudioResamplerStatus::InvalidState;
            return total;
        }

        /** @brief Emit one held-sample ramp frame, committing its deferred action only at zero. */
        void RenderFade(const AudioResamplerOutput output, const std::uint32_t frame, AudioVoiceState &current) noexcept {
            const auto gain = static_cast<float>(--fade.remaining) / static_cast<float>(fade.rampFrames);
            for (std::size_t channel = 0; channel < output.planes.size(); ++channel) {
                amplitudes.last[channel] = ApplyGain(amplitudes.held[channel], gain);
                output.planes[channel][frame] = amplitudes.last[channel];
            }
            if (fade.remaining == 0) {
                (void)Commit();
                (void)registry.CheckState(voice, current);
            }
        }

        /** @brief Write new PCM through fade-in and advance audible time once. */
        void RenderPcm(const AudioResamplerOutput output, const std::uint32_t frame) noexcept {
            const float gain =
                fade.fadeIn == 0 ? 1.0F : static_cast<float>(fade.rampFrames - --fade.fadeIn) / static_cast<float>(fade.rampFrames);
            for (std::size_t channel = 0; channel < output.planes.size(); ++channel) {
                amplitudes.last[channel] = ApplyGain(ApplyGain(outputCells[channel].samples[0], amplitudes.playbackGain), gain);
                output.planes[channel][frame] = amplitudes.last[channel];
            }
            Advance();
        }

        /** @brief Process one output frame; false leaves the precleared remainder silent. */
        bool RenderFrame(const AudioResamplerOutput output, const std::uint32_t frame, AudioVoiceRenderResult &result,
                         AudioVoiceState &current) noexcept {
            using enum AudioVoiceState;
            if (IsTerminalAudioVoiceState(current) || current == Ready || current == Paused)
                return false;
            if (fade.remaining != 0) {
                RenderFade(output, frame, current);
                ++result.produced;
                return true;
            }
            if (current != Playing) {
                if (current == Virtual)
                    return RenderVirtualFrame(current);
                result.error = &AudioErrors::VoiceInvalidTransition;
                return false;
            }
            const auto progress = Sample();
            result.sanitizedSamples += progress.sanitizedSamples;
            if (progress.status == AudioResamplerStatus::Complete) {
                (void)registry.TryTransition(voice, Finished);
                return false;
            }
            if (progress.produced == 0) {
                result.error = &AudioErrors::CallbackFault;
                (void)registry.TryTransition(voice, Failed);
                return false;
            }
            RenderPcm(output, frame);
            ++result.produced;
            return true;
        }
    };

    /** @copydoc AudioVoicePlayback::Create */
    Result<AudioVoicePlayback> AudioVoicePlayback::Create(AudioVoiceStateMachine &registry, const AudioResamplerInput source,
                                                          const AudioVoicePlaybackConfig &config) {
        if (!ValidSource(source, config))
            return Result<AudioVoicePlayback>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        if (const auto bytes =
                static_cast<std::uint64_t>(source.frames) * config.plan.Descriptor().channels * sizeof(float) + sizeof(State);
            bytes > config.maximumStorageBytes || bytes > std::numeric_limits<std::size_t>::max())
            return Result<AudioVoicePlayback>::Failure(MakeError(AudioErrors::MemoryBudgetExceeded));
        try {
            auto prepared = AudioResampler::Create(config.plan, config.maximumCoefficientBytes);
            if (prepared.HasError())
                return Result<AudioVoicePlayback>::Failure(prepared.ErrorValue());
            auto state = std::make_unique<State>(registry, std::move(prepared).Value(), config, source);
            const auto acquired = registry.CreateVoice();
            if (acquired.HasError())
                return Result<AudioVoicePlayback>::Failure(acquired.ErrorValue());
            state->voice = acquired.Value();
            if (const auto ready = registry.Transition(state->voice, AudioVoiceState::Ready); ready.HasError())
                return Result<AudioVoicePlayback>::Failure(ready.ErrorValue());
            return Result<AudioVoicePlayback>::Success(AudioVoicePlayback{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Result<AudioVoicePlayback>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioVoicePlayback::AudioVoicePlayback */
    AudioVoicePlayback::AudioVoicePlayback(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc AudioVoicePlayback::~AudioVoicePlayback */
    AudioVoicePlayback::~AudioVoicePlayback() = default;
    /** @copydoc AudioVoicePlayback::AudioVoicePlayback */
    AudioVoicePlayback::AudioVoicePlayback(AudioVoicePlayback &&other) noexcept = default;
    /** @copydoc AudioVoicePlayback::operator= */
    AudioVoicePlayback &AudioVoicePlayback::operator=(AudioVoicePlayback &&other) noexcept = default;

    /** @copydoc AudioVoicePlayback::Voice */
    AudioVoiceHandle AudioVoicePlayback::Voice() const noexcept {
        return state_ ? state_->voice : AudioVoiceHandle{};
    }

    /** @copydoc AudioVoicePlayback::Cursor */
    AudioVoiceCursor AudioVoicePlayback::Cursor() const noexcept {
        return state_ ? state_->cursor : AudioVoiceCursor{};
    }

    /** @copydoc AudioVoicePlayback::Apply */
    const ErrorCodeDescriptor *AudioVoicePlayback::Apply(const AudioVoiceControlRequest &request) noexcept {
        if (!state_)
            return &AudioErrors::RuntimeInactive;
        AudioVoiceState current{};
        if (const auto *error = state_->Check(request.voice, current))
            return error;
        if (!ValidateAudioVoiceControlRequest(request))
            return &AudioErrors::PlaybackRequestInvalid;
        if (request.control == AudioVoiceControl::Cancel)
            return state_->registry.TryCancel(request.voice);
        if (!Controllable(current))
            return &AudioErrors::VoiceInvalidTransition;
        if (state_->fade.remaining != 0 && request.control != AudioVoiceControl::Stop)
            return &AudioErrors::VoiceInvalidTransition;
        return state_->ApplyControl(request, current);
    }

    /** @copydoc AudioVoicePlayback::CheckRestart */
    const ErrorCodeDescriptor *AudioVoicePlayback::CheckRestart(const AudioVoiceHandle voice) const noexcept {
        if (!state_)
            return &AudioErrors::RuntimeInactive;
        AudioVoiceState current{};
        if (const auto *error = state_->Check(voice, current))
            return error;
        return Controllable(current) && state_->fade.remaining == 0 ? nullptr : &AudioErrors::VoiceInvalidTransition;
    }

    /** @copydoc AudioVoicePlayback::SwapPitch */
    const ErrorCodeDescriptor *AudioVoicePlayback::SwapPitch(const AudioVoiceHandle voice, AudioResampler &replacement) noexcept {
        if (!state_)
            return &AudioErrors::RuntimeInactive;
        AudioVoiceState current{};
        if (const auto *error = state_->Check(voice, current))
            return error;
        if (!Controllable(current) || state_->fade.remaining != 0)
            return &AudioErrors::VoiceInvalidTransition;
        if (!replacement.IsFresh() || !replacement.Plan())
            return &AudioErrors::ResamplerInvalid;
        const auto candidate = *replacement.Plan();
        if (const auto &next = candidate.Descriptor(); !CompatiblePitch(next, state_->plan.Descriptor()))
            return &AudioErrors::ResamplerInvalid;
        std::swap(state_->converter, replacement);
        state_->plan = candidate;
        state_->BeginDiscontinuity(AudioVoiceControl::Seek, current, {static_cast<std::uint32_t>(state_->cursor.frame), {}});
        return nullptr;
    }

    /** @copydoc AudioVoicePlayback::Render */
    AudioVoiceRenderResult AudioVoicePlayback::Render(const AudioResamplerOutput output) noexcept {
        if (!state_)
            return {.error = &AudioErrors::RuntimeInactive};
        if (!ValidOutput(output, state_->plan.Descriptor()))
            return {.error = &AudioErrors::ResamplerInvalid};
        AudioVoiceState current{};
        if (const auto *error = state_->Check(state_->voice, current))
            return {.error = error};
        for (const auto plane : output.planes)
            std::ranges::fill(plane.first(output.capacity), 0.0F);
        AudioVoiceRenderResult result;
        for (std::uint32_t frame = 0; frame < output.capacity; ++frame) {
            if (!state_->RenderFrame(output, frame, result, current))
                break;
        }
        (void)state_->registry.CheckState(state_->voice, current);
        result.terminal = output.capacity != 0 && IsTerminalAudioVoiceState(current) && !state_->terminalReported;
        state_->terminalReported = state_->terminalReported || result.terminal;
        return result;
    }
}  // namespace Horo::Audio
