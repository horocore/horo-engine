#include "AudioFrontendState.h"

namespace Horo::Audio {
    namespace {
        /** @brief Preserves stable Audio failure identity at the lifecycle boundary. */
        Result<void> Failure(const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        }
    }  // namespace

    /** @copydoc AudioFrontend::State::Begin */
    Result<void> AudioFrontend::State::Begin(const Operation next, const Backend::Request &request,
                                             const AudioMonotonicTimestamp deadline) {
        if (deadline.clockDomain != output->clockDomain)
            return Failure(AudioErrors::IdentityInvalid);
        auto admitted = output->backend->Begin(request, deadline);
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        pending = admitted.Value();
        operation = next;
        return Result<void>::Success();
    }

    /** @copydoc AudioFrontend::State::AdmitOpened */
    Result<void> AudioFrontend::State::AdmitOpened(const Backend::Opened &value) {
        opened = true;
        const auto &request = output->open;
        if (ValidateAudioDeviceNegotiation(request.format, output->devices, value.format).status !=
                AudioDeviceNegotiationStatus::Accepted ||
            value.access != request.access || value.timing.epoch != request.plannedEpoch ||
            !ValidateAudioDeviceTimingReport(value.timing, request.plannedEpoch, output->backend->Kind()) ||
            value.format.formatRevision != request.plannedEpoch.formatRevision ||
            value.format.effective.sampleRate != request.format.preferred.sampleRate ||
            value.format.effective.layout != request.format.preferred.layout || value.format.callbackFrames > maximumFrames)
            return Failure(AudioErrors::DeviceFormatUnsupported);
        return Result<void>::Success();
    }

    /** @copydoc AudioFrontend::State::Poll */
    Result<void> AudioFrontend::State::Poll() {
        if (!pending.has_value())
            return Result<void>::Success();
        auto result = output->backend->Poll(*pending);
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (!result.Value().has_value())
            return Result<void>::Success();
        const auto &completion = *result.Value();
        if (completion.operation != *pending)
            return Failure(AudioErrors::EventQueueInvalid);
        const auto outcome = ApplyCompletion(completion);
        const auto acknowledged = output->backend->AcknowledgeCompletion(*pending);
        if (acknowledged.HasError()) {
            phase = AudioFrontendPhase::Retained;
            return acknowledged;
        }
        pending.reset();
        return outcome;
    }

    /** @copydoc AudioFrontend::State::InvalidCompletion */
    Result<void> AudioFrontend::State::InvalidCompletion() {
        phase = AudioFrontendPhase::Retained;
        return Failure(AudioErrors::EventQueueInvalid);
    }

    /** @copydoc AudioFrontend::State::ApplyFailure */
    Result<void> AudioFrontend::State::ApplyFailure(const Backend::Failed &failed) {
        if (!failure.has_value())
            failure = failed.error;
        closing = true;
        switch (failed.resources) {
            case Backend::ResourceDisposition::Retained:
                phase = AudioFrontendPhase::Retained;
                break;
            case Backend::ResourceDisposition::Closed:
                opened = false;
                started = false;
                detached = true;
                break;
            case Backend::ResourceDisposition::CallbackDetached:
                started = false;
                detached = true;
                break;
            case Backend::ResourceDisposition::Unchanged:
                break;
            default:
                return InvalidCompletion();
        }
        return Result<void>::Failure(failed.error);
    }

    /** @copydoc AudioFrontend::State::ApplyCompletion */
    Result<void> AudioFrontend::State::ApplyCompletion(const Backend::Completion &completion) {
        if (const auto *failed = std::get_if<Backend::Failed>(&completion.outcome))
            return ApplyFailure(*failed);
        switch (operation) {
            case Operation::Open:
                if (const auto *value = std::get_if<Backend::Opened>(&completion.outcome))
                    return AdmitOpened(*value);
                return InvalidCompletion();
            case Operation::Start:
            case Operation::Quiesce:
            case Operation::Stop:
                return ApplyPlaybackCompletion(completion);
            case Operation::Close:
                if (std::holds_alternative<Backend::Closed>(completion.outcome)) {
                    opened = false;
                    return Result<void>::Success();
                }
                return InvalidCompletion();
        }
        return InvalidCompletion();
    }

    /** @copydoc AudioFrontend::State::ApplyPlaybackCompletion */
    Result<void> AudioFrontend::State::ApplyPlaybackCompletion(const Backend::Completion &completion) {
        switch (operation) {
            case Operation::Start:
                if (const auto *value = std::get_if<Backend::Started>(&completion.outcome);
                    value != nullptr && value->epoch == output->open.plannedEpoch) {
                    started = true;
                    detached = false;
                    return Result<void>::Success();
                }
                return InvalidCompletion();
            case Operation::Quiesce:
                if (const auto *value = std::get_if<Backend::Quiesced>(&completion.outcome);
                    value != nullptr && value->epoch == output->open.plannedEpoch) {
                    quiesced = true;
                    return Result<void>::Success();
                }
                return InvalidCompletion();
            case Operation::Stop:
                if (const auto *value = std::get_if<Backend::Stopped>(&completion.outcome);
                    value != nullptr && value->epoch == output->open.plannedEpoch) {
                    detached = true;
                    started = false;
                    return Result<void>::Success();
                }
                return InvalidCompletion();
            default:
                return InvalidCompletion();
        }
    }

    /** @copydoc AudioFrontend::State::Observe */
    Result<void> AudioFrontend::State::Observe(const Backend::Event &event) {
        if (event.owner != resources->scope.owner)
            return Failure(AudioErrors::HandleOwnerMismatch);
        if (const auto *callback = std::get_if<AudioCallbackEvent>(&event.fact)) {
            if (!ValidateAudioCallbackEvent(*callback, output->open.plannedEpoch, output->clockDomain))
                return Failure(AudioErrors::EventQueueInvalid);
            if (std::holds_alternative<AudioCallbackReady>(callback->fact))
                ready = true;  // Ready may arrive before Start's retained terminal is polled.
            else if (std::holds_alternative<AudioCallbackFault>(callback->fact))
                return Failure(AudioErrors::CallbackFault);
        } else if (std::holds_alternative<Backend::DeviceLost>(event.fact)) {
            return Failure(AudioErrors::DeviceLost);
        } else if (const auto *interruption = std::get_if<Backend::DeviceInterruption>(&event.fact);
                   interruption != nullptr && interruption->state == Backend::InterruptionState::Began) {
            return Failure(AudioErrors::DeviceUnavailable);
        }
        return Result<void>::Success();
    }

    /** @copydoc AudioFrontend::State::Drain */
    Result<void> AudioFrontend::State::Drain() {
        std::array<Backend::Event, 64> events;
        const auto count = output->backend->DrainEvents(events);
        if (count > events.size())
            return Failure(AudioErrors::EventQueueInvalid);
        for (std::size_t index = 0; index < count; ++index)
            if (const auto observed = Observe(events[index]); observed.HasError())
                return observed;
        if (ready && started && !rendering) {
            const auto committed = output->backend->CommitRendering(output->open.plannedEpoch);
            if (committed.HasError())
                return committed;
            rendering = true;
            phase = closing ? AudioFrontendPhase::Closing : AudioFrontendPhase::Active;
        }
        return Result<void>::Success();
    }

    /** @copydoc AudioFrontend::State::Release */
    Result<void> AudioFrontend::State::Release() {
        auto &owned = *resources;
        owned.voice->Close();
        owned.mixer->Close();
        if (!detached || !owned.voice->CompleteShutdown(true) ||
            owned.mixer->CompleteShutdown(owned.scope, true) != MixerRenderStatus::Quiesced)
            return Failure(AudioErrors::RuntimeInactive);
        if (const auto drained = DrainCancelled(); drained.HasError())
            return drained;
        if (owned.streams) {
            const auto stopped = owned.streams->Stop(owned.stream);
            if (stopped.HasError())
                return stopped;
            const auto retired = owned.streams->Retire(owned.stream);
            if (retired.HasError())
                return retired;
            owned.streams.reset();
        }
        owned.voice.reset();
        owned.mixer.reset();
        owned.hostLease.reset();
        phase = AudioFrontendPhase::Closed;
        return Result<void>::Success();
    }

    /** @copydoc AudioFrontend::State::DrainCancelled */
    Result<void> AudioFrontend::State::DrainCancelled() {
        auto &owned = *resources;
        // Stop proved that this lane is the sole consumer. Cancelled FIFO records no longer borrow owners.
        (void)owned.staging.Close();
        AudioCommandRecord cancelled;
        for (std::size_t count = 0; count <= MaximumAudioCommandSlots; ++count) {
            (void)owned.staging.Pump(64);
            while (owned.staging.TryConsume(cancelled)) {
            }
            if (owned.staging.IsDrained())
                break;
        }
        if (!owned.staging.IsDrained())
            return Failure(AudioErrors::RuntimeInactive);
        for (auto &receipt : receipts)
            if (receipt.sequence.load() != 0 && receipt.status.load() == OperationStatus::Pending)
                receipt.status.store(OperationStatus::Cancelled);  // Native Stop proved the callback cannot still publish this result.
        return Result<void>::Success();
    }

    /** @copydoc AudioFrontend::State::AdvanceClose */
    Result<void> AudioFrontend::State::AdvanceClose(const AudioMonotonicTimestamp deadline) {
        if (pending.has_value() || phase == AudioFrontendPhase::Retained)
            return Result<void>::Success();
        if (started) {
            if (!rendering)
                return Result<void>::Success();  // Wait for the exact priming Ready fact before Quiesce.
            if (!quiesced)
                return Begin(Operation::Quiesce, Backend::Quiesce{output->open.plannedEpoch}, deadline);
            return Begin(Operation::Stop, Backend::Stop{output->open.plannedEpoch}, deadline);
        }
        if (opened)
            return Begin(Operation::Close, Backend::Close{}, deadline);
        return Release();
    }

}  // namespace Horo::Audio
