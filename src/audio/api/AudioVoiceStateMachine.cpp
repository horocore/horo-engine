#include "Horo/Audio/AudioVoiceStateMachine.h"

#include "AudioHandleRegistry.h"
#include "Horo/Audio/AudioErrors.h"

#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace Horo::Audio {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnownState(const AudioVoiceState state) noexcept {
            using enum AudioVoiceState;
            switch (state) {
                case Created:
                case Ready:
                case Scheduled:
                case Playing:
                case Paused:
                case Virtual:
                case Stopping:
                case Stopped:
                case Finished:
                case Cancelled:
                case Failed:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool IsLegalTransition(const AudioVoiceState current, const AudioVoiceState next) noexcept {
            using enum AudioVoiceState;
            if (next == Failed && !IsTerminalAudioVoiceState(current))
                return true;

            switch (current) {
                case Created:
                    return next == Ready;
                case Ready:
                    return next == Scheduled;
                case Scheduled:
                    return next == Playing || next == Stopping || next == Stopped;
                case Playing:
                    return next == Paused || next == Virtual || next == Stopping || next == Stopped || next == Finished;
                case Paused:
                    return next == Playing || next == Stopping;
                case Virtual:
                    return next == Playing || next == Stopping;
                case Stopping:
                    return next == Stopped;
                case Stopped:
                case Finished:
                case Cancelled:
                case Failed:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool ValidConfig(const AudioVoiceStateMachineConfig &config) noexcept {
            return config.owner.IsValid() && config.maximumVoices > 0 && config.maximumVoices <= MaximumAudioVoiceSlots &&
                   config.maximumGeneration > 0;
        }

        struct VoiceSlot final {
            AudioVoiceState state{AudioVoiceState::Created};
            bool active{};
        };
    }  // namespace

    bool IsTerminalAudioVoiceState(const AudioVoiceState state) noexcept {
        using enum AudioVoiceState;
        return state == Stopped || state == Finished || state == Cancelled || state == Failed;
    }

    std::optional<AudioVoiceTerminalReason> AudioVoiceTerminalReasonForState(const AudioVoiceState state) noexcept {
        switch (state) {
            case AudioVoiceState::Stopped:
                return AudioVoiceTerminalReason::Stopped;
            case AudioVoiceState::Finished:
                return AudioVoiceTerminalReason::Finished;
            case AudioVoiceState::Cancelled:
                return AudioVoiceTerminalReason::Cancelled;
            case AudioVoiceState::Failed:
                return AudioVoiceTerminalReason::Failed;
            case AudioVoiceState::Created:
            case AudioVoiceState::Ready:
            case AudioVoiceState::Scheduled:
            case AudioVoiceState::Playing:
            case AudioVoiceState::Paused:
            case AudioVoiceState::Virtual:
            case AudioVoiceState::Stopping:
                return std::nullopt;
        }
        return std::nullopt;
    }

    struct AudioVoiceStateMachine::Implementation final {
        using Registry = Detail::AudioHandleRegistry<AudioVoiceHandle>;

        AudioVoiceStateMachineConfig config;
        Registry registry;
        std::vector<VoiceSlot> slots;
        bool admissionOpen{true};

        Implementation(const AudioVoiceStateMachineConfig &description, Registry preparedRegistry)
            : config(description), registry(std::move(preparedRegistry)), slots(static_cast<std::size_t>(description.maximumVoices) + 1) {}
    };

    /** @copydoc AudioVoiceStateMachine::Create */
    Result<AudioVoiceStateMachine> AudioVoiceStateMachine::Create(const AudioVoiceStateMachineConfig &config) {
        if (!ValidConfig(config)) {
            const auto &descriptor =
                config.maximumVoices > MaximumAudioVoiceSlots ? AudioErrors::HandleCapacityExhausted : AudioErrors::IdentityInvalid;
            return Failure<AudioVoiceStateMachine>(descriptor);
        }

        auto registry = Implementation::Registry::Create(config.owner, {.maximumSlots = config.maximumVoices,
                                                                        .maximumGeneration = config.maximumGeneration});
        if (registry.HasError())
            return Result<AudioVoiceStateMachine>::Failure(registry.ErrorValue());

        try {
            return Result<AudioVoiceStateMachine>::Success(
                AudioVoiceStateMachine{std::make_unique<Implementation>(config, std::move(registry).Value())});
        } catch (const std::bad_alloc &) {
            return Failure<AudioVoiceStateMachine>(AudioErrors::MemoryAllocationFailed);
        }
    }

    /** @copydoc AudioVoiceStateMachine::AudioVoiceStateMachine */
    AudioVoiceStateMachine::AudioVoiceStateMachine(std::unique_ptr<Implementation> implementation) noexcept
        : implementation_(std::move(implementation)) {}

    Result<std::uint32_t> AudioVoiceStateMachine::ResolveSlot(const AudioVoiceHandle &voice) const {
        if (!implementation_)
            return Failure<std::uint32_t>(AudioErrors::RuntimeInactive);
        return implementation_->registry.Resolve(voice);
    }

    /** @copydoc AudioVoiceStateMachine::~AudioVoiceStateMachine */
    AudioVoiceStateMachine::~AudioVoiceStateMachine() = default;

    /** @copydoc AudioVoiceStateMachine::AudioVoiceStateMachine */
    AudioVoiceStateMachine::AudioVoiceStateMachine(AudioVoiceStateMachine &&other) noexcept = default;

    /** @copydoc AudioVoiceStateMachine::operator= */
    AudioVoiceStateMachine &AudioVoiceStateMachine::operator=(AudioVoiceStateMachine &&other) noexcept = default;

    /** @copydoc AudioVoiceStateMachine::CreateVoice */
    Result<AudioVoiceHandle> AudioVoiceStateMachine::CreateVoice() {
        if (!implementation_)
            return Failure<AudioVoiceHandle>(AudioErrors::RuntimeInactive);
        if (!implementation_->admissionOpen)
            return Failure<AudioVoiceHandle>(AudioErrors::VoiceAdmissionClosed);

        auto created = implementation_->registry.Acquire();
        if (created.HasError())
            return Result<AudioVoiceHandle>::Failure(created.ErrorValue());

        const AudioVoiceHandle voice = created.Value();
        VoiceSlot &slot = implementation_->slots[voice.slot];
        slot.state = AudioVoiceState::Created;
        slot.active = true;
        return created;
    }

    /** @copydoc AudioVoiceStateMachine::State */
    Result<AudioVoiceState> AudioVoiceStateMachine::State(const AudioVoiceHandle &voice) const {
        const auto resolved = ResolveSlot(voice);
        if (resolved.HasError())
            return Result<AudioVoiceState>::Failure(resolved.ErrorValue());
        return Result<AudioVoiceState>::Success(implementation_->slots[resolved.Value()].state);
    }

    /** @copydoc AudioVoiceStateMachine::Snapshot */
    Result<AudioVoiceSnapshot> AudioVoiceStateMachine::Snapshot(const AudioVoiceHandle &voice) const {
        const auto resolved = ResolveSlot(voice);
        if (resolved.HasError())
            return Result<AudioVoiceSnapshot>::Failure(resolved.ErrorValue());
        const AudioVoiceState state = implementation_->slots[resolved.Value()].state;
        return Result<AudioVoiceSnapshot>::Success(
            AudioVoiceSnapshot{.voice = voice, .state = state, .terminalReason = AudioVoiceTerminalReasonForState(state)});
    }

    /** @copydoc AudioVoiceStateMachine::Transition */
    Result<void> AudioVoiceStateMachine::Transition(const AudioVoiceHandle &voice, const AudioVoiceState state) const {
        const auto resolved = ResolveSlot(voice);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        if (!IsKnownState(state))
            return Failure<void>(AudioErrors::VoiceInvalidTransition);

        VoiceSlot &slot = implementation_->slots[resolved.Value()];
        if (IsTerminalAudioVoiceState(slot.state))
            return Failure<void>(AudioErrors::VoiceInvalidTransition);
        if (!implementation_->admissionOpen)
            return Failure<void>(AudioErrors::VoiceAdmissionClosed);
        if (!IsLegalTransition(slot.state, state))
            return Failure<void>(AudioErrors::VoiceInvalidTransition);

        slot.state = state;
        return Result<void>::Success();
    }

    /** @copydoc AudioVoiceStateMachine::Cancel */
    Result<void> AudioVoiceStateMachine::Cancel(const AudioVoiceHandle &voice) const {
        const auto resolved = ResolveSlot(voice);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());

        VoiceSlot &slot = implementation_->slots[resolved.Value()];
        if (IsTerminalAudioVoiceState(slot.state))
            return Failure<void>(AudioErrors::VoiceInvalidTransition);
        if (!implementation_->admissionOpen)
            return Failure<void>(AudioErrors::VoiceAdmissionClosed);

        slot.state = AudioVoiceState::Cancelled;
        return Result<void>::Success();
    }

    /** @copydoc AudioVoiceStateMachine::Release */
    Result<void> AudioVoiceStateMachine::Release(const AudioVoiceHandle &voice) {
        const auto resolved = ResolveSlot(voice);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        if (!IsTerminalAudioVoiceState(implementation_->slots[resolved.Value()].state))
            return Failure<void>(AudioErrors::VoiceInvalidTransition);

        if (const auto released = implementation_->registry.Release(voice); released.HasError())
            return released;
        implementation_->slots[resolved.Value()].active = false;
        return Result<void>::Success();
    }

    /** @copydoc AudioVoiceStateMachine::BeginShutdown */
    Result<void> AudioVoiceStateMachine::BeginShutdown() {
        if (!implementation_)
            return Failure<void>(AudioErrors::RuntimeInactive);
        if (!implementation_->admissionOpen)
            return Result<void>::Success();

        implementation_->admissionOpen = false;
        for (std::uint32_t slotIndex = 1; slotIndex <= implementation_->config.maximumVoices; ++slotIndex) {
            VoiceSlot &slot = implementation_->slots[slotIndex];
            if (slot.active && !IsTerminalAudioVoiceState(slot.state))
                slot.state = AudioVoiceState::Cancelled;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Audio
