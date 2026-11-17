#include "Horo/Audio/Internal/AudioDeviceSwitch.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Check runtime ownership and strictly advancing callback/format identities independently of device policy. */
        bool ValidMigrationIdentity(const DeviceSwitchRequest &request, const AudioRuntimeId owner) noexcept {
            const auto &next = request.replacement;
            return MatchesAudioDeviceEpoch(request.previous, request.previous) && request.previous.device.owner == owner &&
                   next.callbackEpoch > request.previous.callbackEpoch && next.formatRevision > request.previous.formatRevision;
        }

        /** @brief Validate selection ownership and known policy without requiring discovery on the failed old connection. */
        bool ValidMigrationTarget(const DeviceSwitchRequest &request, const AudioBackend &backend) noexcept {
            const auto &target = request.replacement;
            const auto *explicitDevice = std::get_if<AudioDeviceId>(&target.format.device);
            const bool knownAccess = target.access == AccessMode::Shared || target.access == AccessMode::Exclusive;
            const bool knownLatency = target.latency == LatencyClass::Interactive || target.latency == LatencyClass::Balanced ||
                                      target.latency == LatencyClass::PowerSaving;
            return ValidateAudioProcessingFormat(request.previousFormat) && ValidateAudioDeviceFormatRequest(target.format) &&
                   (!explicitDevice || explicitDevice->owner == backend.Owner()) && knownAccess && knownLatency;
        }

        /** @brief Keep deadline identity and expiration distinct from sample clocks. */
        bool BeforeDeadline(const AudioMonotonicTimestamp deadline, const AudioMonotonicTimestamp now) noexcept {
            return now.clockDomain != 0 && now.clockDomain == deadline.clockDomain && now.nanoseconds < deadline.nanoseconds;
        }

        /** @brief Reuse the exact epoch comparison for the protocol's three acknowledgement types. */
        template <class Outcome> bool MatchesOutcome(const Backend::Completion &completion, const AudioDeviceEpoch &epoch) noexcept {
            const auto *value = std::get_if<Outcome>(&completion.outcome);
            return value && value->epoch == epoch;
        }

        /** @brief Return a stable rejection without changing transaction/backend state. */
        Result<void> Reject(const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        }

        /** @brief Keep unsupported format preservation distinct from malformed playback policy. */
        Result<void> AdmitPlayback(const DeviceSwitchRequest &request) {
            if (request.playback != SwitchPlaybackPolicy::Preserve && request.playback != SwitchPlaybackPolicy::Restart)
                return Reject(AudioErrors::IdentityInvalid);
            const auto &format = request.replacement.format;
            if (request.playback == SwitchPlaybackPolicy::Preserve && request.previousFormat != format.preferred &&
                std::ranges::find(format.allowedAlternatives, request.previousFormat) == format.allowedAlternatives.end())
                return Reject(AudioErrors::OperationUnsupported);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc AudioDeviceSwitch::AudioDeviceSwitch */
    AudioDeviceSwitch::AudioDeviceSwitch(AudioBackend &backend) noexcept : backend_(backend) {}

    /** @copydoc AudioDeviceSwitch::Begin */
    Result<OperationId> AudioDeviceSwitch::Begin(const DeviceSwitchRequest &request, const AudioMonotonicTimestamp now) {
        if (stage_ != DeviceSwitchStage::Idle || sequence_ == std::numeric_limits<std::uint64_t>::max())
            return Result<OperationId>::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (!ValidMigrationIdentity(request, backend_.Owner()) || !ValidMigrationTarget(request, backend_) ||
            !BeforeDeadline(request.deadline, now))
            return Result<OperationId>::Failure(MakeError(AudioErrors::IdentityInvalid));
        if (const auto playback = AdmitPlayback(request); playback.HasError())
            return Result<OperationId>::Failure(playback.ErrorValue());
        try {
            request_ = request;
        } catch (const std::bad_alloc &) {
            request_.reset();
            return Result<OperationId>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
        ++sequence_;
        detached_ = false;
        ready_.reset();
        cancelled_ = false;
        cancellationCause_.reset();
        render_ = {};
        stage_ = DeviceSwitchStage::Quiescing;
        return Result<OperationId>::Success({backend_.Owner(), sequence_});
    }

    /** @copydoc AudioDeviceSwitch::Cancel */
    Result<CancelDisposition> AudioDeviceSwitch::Cancel(const OperationId operation) {
        if (operation != OperationId{backend_.Owner(), sequence_} || stage_ == DeviceSwitchStage::Idle)
            return Result<CancelDisposition>::Failure(MakeError(AudioErrors::IdentityInvalid));
        if (stage_ == DeviceSwitchStage::Terminal)
            return Result<CancelDisposition>::Success(CancelDisposition::AlreadyTerminal);
        cancelled_ = true;
        return Result<CancelDisposition>::Success(CancelDisposition::Requested);
    }

    /** @copydoc AudioDeviceSwitch::FinishCancelled */
    Result<void> AudioDeviceSwitch::FinishCancelled() {
        auto error = MakeError(AudioErrors::OperationCancelled);
        if (cancellationCause_)
            error = WithCause(std::move(error), *cancellationCause_);
        completion_ = DeviceSwitchCompletion{{backend_.Owner(), sequence_},
                                             request_->previous,
                                             replacement_ ? std::optional(replacement_->plannedEpoch) : std::nullopt,
                                             request_->playback,
                                             false,
                                             detached_,
                                             ResourceDisposition::Closed,
                                             std::move(error)};
        stage_ = DeviceSwitchStage::Terminal;
        return Result<void>::Failure(*completion_->error);
    }

    /** @copydoc AudioDeviceSwitch::AdvanceCancellation */
    void AudioDeviceSwitch::AdvanceCancellation() noexcept {
        if (!cancelled_)
            return;
        switch (stage_) {
            case DeviceSwitchStage::Opening:  // No operation is pending: replacement acquisition has not begun.
            case DeviceSwitchStage::AwaitPlayback:
            case DeviceSwitchStage::Starting:  // No operation is pending: replacement attachment has not begun.
                stage_ = DeviceSwitchStage::ClosingReplacement;
                break;
            case DeviceSwitchStage::AwaitReady:
                stage_ = DeviceSwitchStage::QuiescingReplacement;
                break;
            default:
                break;
        }
    }

    /** @copydoc AudioDeviceSwitch::Fail */
    Result<void> AudioDeviceSwitch::Fail(const Error &error, const ResourceDisposition resources) {
        if (!completion_) {
            completion_ = DeviceSwitchCompletion{{backend_.Owner(), sequence_},
                                                 request_->previous,
                                                 replacement_ ? std::optional(replacement_->plannedEpoch) : std::nullopt,
                                                 request_->playback,
                                                 false,
                                                 detached_,
                                                 resources,
                                                 error};
            stage_ = DeviceSwitchStage::Terminal;
        }
        return Result<void>::Failure(error);
    }

    /** @copydoc AudioDeviceSwitch::Apply */
    Result<void> AudioDeviceSwitch::Apply(const Backend::Completion &completion) {
        if (cancelled_ && (stage_ == DeviceSwitchStage::Opening || stage_ == DeviceSwitchStage::Starting)) {
            if (const auto *failed = std::get_if<Failed>(&completion.outcome)) {
                cancellationCause_ = failed->error;
                stage_ = stage_ == DeviceSwitchStage::Starting && failed->resources == ResourceDisposition::Retained
                             ? DeviceSwitchStage::QuiescingReplacement
                             : DeviceSwitchStage::ClosingReplacement;
                return Result<void>::Success();
            }
            if (stage_ == DeviceSwitchStage::Opening && std::holds_alternative<Opened>(completion.outcome)) {
                stage_ = DeviceSwitchStage::ClosingReplacement;
                return Result<void>::Success();
            }
        }
        if (const auto *failed = std::get_if<Failed>(&completion.outcome))
            return Fail(failed->error, failed->resources);
        if (stage_ == DeviceSwitchStage::Opening) {
            if (const auto *value = std::get_if<Opened>(&completion.outcome))
                return AdmitCandidate(*value);
            return Fail(MakeError(AudioErrors::EventQueueInvalid), ResourceDisposition::Retained);
        }
        if (stage_ == DeviceSwitchStage::Starting) {
            if (MatchesOutcome<Started>(completion, replacement_->plannedEpoch)) {
                stage_ = DeviceSwitchStage::AwaitReady;
                return Result<void>::Success();
            }
            return Fail(MakeError(AudioErrors::EventQueueInvalid), ResourceDisposition::Retained);
        }
        return ApplyRetirement(completion);
    }

    /** @copydoc AudioDeviceSwitch::AdmitDiscovery */
    Result<void> AudioDeviceSwitch::AdmitDiscovery(const AudioDeviceSnapshot &devices) {
        if (!ValidateAudioDeviceSnapshot(devices) || devices.owner != backend_.Owner() || devices.backend != backend_.Kind())
            return Fail(MakeError(AudioErrors::IdentityInvalid), ResourceDisposition::Retained);
        const auto resolved = ResolveAudioDevice(devices, request_->replacement.format.device);
        if (resolved.status != AudioDeviceResolutionStatus::Resolved)
            return Fail(MakeError(AudioErrors::DeviceUnavailable), ResourceDisposition::Retained);
        const auto &intent = request_->replacement;
        devices_ = devices;
        replacement_ = Open{{resolved.device, intent.formatRevision, intent.callbackEpoch}, intent.format, intent.access, intent.latency};
        stage_ = DeviceSwitchStage::Opening;
        return Result<void>::Success();
    }

    /** @copydoc AudioDeviceSwitch::SupplyDiscovery */
    Result<void> AudioDeviceSwitch::SupplyDiscovery(const OperationId operation, const OperationId discovery,
                                                    const AudioMonotonicTimestamp now) {
        if (operation != OperationId{backend_.Owner(), sequence_} || stage_ != DeviceSwitchStage::AwaitDiscovery ||
            discovery.owner != backend_.Owner() || discovery.sequence <= lastBackendSequence_)
            return Reject(AudioErrors::IdentityInvalid);
        if (!BeforeDeadline(request_->deadline, now))
            return Fail(MakeError(AudioErrors::BackendFailed), ResourceDisposition::Retained);
        try {
            const auto polled = backend_.Poll(discovery);
            if (polled.HasError())
                return Result<void>::Failure(polled.ErrorValue());
            if (!polled.Value())
                return Reject(AudioErrors::RuntimeInactive);
            const auto &completion = *polled.Value();
            if (completion.operation != discovery)
                return Reject(AudioErrors::EventQueueInvalid);
            Result<void> applied = Result<void>::Success();
            if (cancelled_) {
                if (const auto *failed = std::get_if<Failed>(&completion.outcome))
                    cancellationCause_ = failed->error;
                else if (!std::holds_alternative<AudioDeviceSnapshot>(completion.outcome) &&
                         !std::holds_alternative<Cancelled>(completion.outcome))
                    return Reject(AudioErrors::EventQueueInvalid);
                stage_ = DeviceSwitchStage::ClosingReplacement;
            } else if (const auto *devices = std::get_if<AudioDeviceSnapshot>(&completion.outcome))
                applied = AdmitDiscovery(*devices);
            else if (const auto *failed = std::get_if<Failed>(&completion.outcome))
                applied = Fail(failed->error, failed->resources);
            else if (const auto *cancelled = std::get_if<Cancelled>(&completion.outcome))
                applied =
                    Fail(MakeError(AudioErrors::OperationCancelled),
                         cancelled->resources == ResourceDisposition::Unchanged ? ResourceDisposition::Retained : cancelled->resources);
            else
                return Reject(AudioErrors::EventQueueInvalid);
            const auto acknowledged = backend_.AcknowledgeCompletion(discovery);
            if (acknowledged.HasError()) {
                completion_.reset();
                return Fail(applied.HasError() ? WithCause(acknowledged.ErrorValue(), applied.ErrorValue()) : acknowledged.ErrorValue(),
                            ResourceDisposition::Retained);
            }
            lastBackendSequence_ = discovery.sequence;
            return applied;
        } catch (const std::bad_alloc &) {
            return Fail(MakeError(AudioErrors::MemoryAllocationFailed), ResourceDisposition::Retained);
        }
    }

    /** @copydoc AudioDeviceSwitch::AdmitCandidate */
    Result<void> AudioDeviceSwitch::AdmitCandidate(const Opened &value) {
        const auto &next = *replacement_;
        if (ValidateAudioDeviceNegotiation(next.format, *devices_, value.format).status != AudioDeviceNegotiationStatus::Accepted ||
            value.access != next.access || value.format.formatRevision != next.plannedEpoch.formatRevision ||
            value.timing.capturedAt.clockDomain != request_->deadline.clockDomain ||
            !ValidateAudioDeviceTimingReport(value.timing, next.plannedEpoch, backend_.Kind()) ||
            (request_->playback == SwitchPlaybackPolicy::Preserve && value.format.effective != request_->previousFormat))
            return Fail(MakeError(AudioErrors::DeviceFormatUnsupported), ResourceDisposition::Retained);
        candidate_ = value;
        stage_ = DeviceSwitchStage::AwaitPlayback;
        return Result<void>::Success();
    }

    /** @copydoc AudioDeviceSwitch::ApplyRetirement */
    Result<void> AudioDeviceSwitch::ApplyRetirement(const Backend::Completion &completion) {
        switch (stage_) {
            case DeviceSwitchStage::Quiescing:
                if (MatchesOutcome<Quiesced>(completion, request_->previous)) {
                    stage_ = DeviceSwitchStage::Stopping;
                    return Result<void>::Success();
                }
                break;
            case DeviceSwitchStage::Stopping:
                if (MatchesOutcome<Stopped>(completion, request_->previous)) {
                    detached_ = true;
                    stage_ = DeviceSwitchStage::Closing;
                    return Result<void>::Success();
                }
                break;
            case DeviceSwitchStage::Closing:
                if (std::holds_alternative<Closed>(completion.outcome)) {
                    if (cancelled_)
                        return FinishCancelled();
                    stage_ = DeviceSwitchStage::AwaitDiscovery;
                    return Result<void>::Success();
                }
                break;
            case DeviceSwitchStage::QuiescingReplacement:
                if (MatchesOutcome<Quiesced>(completion, replacement_->plannedEpoch)) {
                    stage_ = DeviceSwitchStage::StoppingReplacement;
                    return Result<void>::Success();
                }
                break;
            case DeviceSwitchStage::StoppingReplacement:
                if (MatchesOutcome<Stopped>(completion, replacement_->plannedEpoch)) {
                    stage_ = DeviceSwitchStage::ClosingReplacement;
                    return Result<void>::Success();
                }
                break;
            case DeviceSwitchStage::ClosingReplacement:
                if (std::holds_alternative<Closed>(completion.outcome))
                    return FinishCancelled();
                break;
            default:
                break;
        }
        return Fail(MakeError(AudioErrors::EventQueueInvalid), ResourceDisposition::Retained);
    }

    /** @copydoc AudioDeviceSwitch::Submit */
    Result<void> AudioDeviceSwitch::Submit() {
        Request operation;
        switch (stage_) {
            case DeviceSwitchStage::Quiescing:
                operation = Quiesce{request_->previous};
                break;
            case DeviceSwitchStage::Stopping:
                operation = Stop{request_->previous};
                break;
            case DeviceSwitchStage::Closing:
                operation = Close{};
                break;
            case DeviceSwitchStage::Opening:
                operation = *replacement_;
                break;
            case DeviceSwitchStage::Starting:
                operation = Start{replacement_->plannedEpoch, render_};
                break;
            case DeviceSwitchStage::QuiescingReplacement:
                operation = Quiesce{replacement_->plannedEpoch};
                break;
            case DeviceSwitchStage::StoppingReplacement:
                operation = Stop{replacement_->plannedEpoch};
                break;
            case DeviceSwitchStage::ClosingReplacement:
                operation = Close{};
                break;
            default:
                return Result<void>::Success();
        }
        auto admitted = backend_.Begin(operation, request_->deadline);
        if (admitted.HasError())
            return Fail(admitted.ErrorValue(), ResourceDisposition::Retained);
        pending_ = admitted.Value();
        return Result<void>::Success();
    }

    /** @copydoc AudioDeviceSwitch::Pump */
    Result<void> AudioDeviceSwitch::Pump(const AudioMonotonicTimestamp now) {
        try {
            return PumpImpl(now);
        } catch (const std::bad_alloc &) {
            const auto failure = MakeError(AudioErrors::MemoryAllocationFailed);
            return request_ ? Fail(failure, ResourceDisposition::Retained) : Result<void>::Failure(failure);
        }
    }

    /** @copydoc AudioDeviceSwitch::PumpImpl */
    Result<void> AudioDeviceSwitch::PumpImpl(const AudioMonotonicTimestamp now) {
        if (stage_ == DeviceSwitchStage::Terminal)
            return completion_->error ? Result<void>::Failure(*completion_->error) : Result<void>::Success();
        if (stage_ == DeviceSwitchStage::Idle)
            return Reject(AudioErrors::RuntimeInactive);
        if (now.clockDomain != request_->deadline.clockDomain || now.nanoseconds >= request_->deadline.nanoseconds)
            return Fail(MakeError(AudioErrors::BackendFailed), ResourceDisposition::Retained);
        if (const auto polled = PollPending(); polled.HasError())
            return polled;
        if (pending_)
            return Result<void>::Success();
        AdvanceCancellation();
        if (stage_ == DeviceSwitchStage::AwaitReady && ready_)
            return ActivateReplacement();
        return Submit();
    }

    /** @copydoc AudioDeviceSwitch::PollPending */
    Result<void> AudioDeviceSwitch::PollPending() {
        if (!pending_)
            return Result<void>::Success();
        auto polled = backend_.Poll(*pending_);
        if (polled.HasError())
            return Fail(polled.ErrorValue(), ResourceDisposition::Retained);
        if (!polled.Value())
            return Result<void>::Success();
        const auto &completion = *polled.Value();
        if (completion.operation != *pending_)
            return Fail(MakeError(AudioErrors::EventQueueInvalid), ResourceDisposition::Retained);
        const auto applied = Apply(completion);
        const auto acknowledged = backend_.AcknowledgeCompletion(*pending_);
        if (acknowledged.HasError()) {
            // A retained backend slot still owns native state; never allow coordinator reuse.
            completion_.reset();
            return Fail(applied.HasError() ? WithCause(acknowledged.ErrorValue(), applied.ErrorValue()) : acknowledged.ErrorValue(),
                        ResourceDisposition::Retained);
        }
        lastBackendSequence_ = pending_->sequence;
        pending_.reset();
        return applied;
    }

    /** @copydoc AudioDeviceSwitch::ActivateReplacement */
    Result<void> AudioDeviceSwitch::ActivateReplacement() {
        if (!ready_)
            return Reject(AudioErrors::RuntimeInactive);
        if (const auto committed = backend_.CommitRendering(replacement_->plannedEpoch); committed.HasError())
            return Fail(committed.ErrorValue(), ResourceDisposition::Retained);
        completion_ = DeviceSwitchCompletion{{backend_.Owner(), sequence_},
                                             request_->previous,
                                             replacement_->plannedEpoch,
                                             request_->playback,
                                             true,
                                             detached_,
                                             ResourceDisposition::Unchanged,
                                             std::nullopt,
                                             ready_};
        stage_ = DeviceSwitchStage::Terminal;
        return Result<void>::Success();
    }

    /** @copydoc AudioDeviceSwitch::Observe */
    bool AudioDeviceSwitch::Observe(const AudioCallbackEvent &event) noexcept {
        if (!request_ || cancelled_ || ready_ || (stage_ != DeviceSwitchStage::Starting && stage_ != DeviceSwitchStage::AwaitReady) ||
            !ValidateAudioCallbackEvent(event, replacement_->plannedEpoch, request_->deadline.clockDomain) ||
            !std::holds_alternative<AudioCallbackReady>(event.fact))
            return false;
        ready_ = event;
        return true;
    }

    /** @copydoc AudioDeviceSwitch::SupplyPlayback */
    Result<void> AudioDeviceSwitch::SupplyPlayback(const OperationId operation, const SwitchPlaybackPolicy applied,
                                                   const RenderPort render) {
        if (operation != OperationId{backend_.Owner(), sequence_} || cancelled_ || stage_ != DeviceSwitchStage::AwaitPlayback ||
            applied != request_->playback || render.process == nullptr)
            return Reject(AudioErrors::IdentityInvalid);
        render_ = render;
        stage_ = DeviceSwitchStage::Starting;
        return Result<void>::Success();
    }

    /** @copydoc AudioDeviceSwitch::Candidate */
    const Opened *AudioDeviceSwitch::Candidate() const noexcept {
        return candidate_ ? &*candidate_ : nullptr;
    }

    /** @copydoc AudioDeviceSwitch::Completion */
    const DeviceSwitchCompletion *AudioDeviceSwitch::Completion() const noexcept {
        return completion_ ? &*completion_ : nullptr;
    }

    /** @copydoc AudioDeviceSwitch::Stage */
    DeviceSwitchStage AudioDeviceSwitch::Stage() const noexcept {
        return stage_;
    }

    /** @copydoc AudioDeviceSwitch::Acknowledge */
    Result<void> AudioDeviceSwitch::Acknowledge(const OperationId operation) {
        if (!completion_ || completion_->operation != operation || completion_->resources == ResourceDisposition::Retained)
            return Reject(AudioErrors::RuntimeInactive);
        completion_.reset();
        candidate_.reset();
        replacement_.reset();
        devices_.reset();
        request_.reset();
        ready_.reset();
        cancellationCause_.reset();
        cancelled_ = false;
        render_ = {};
        stage_ = DeviceSwitchStage::Idle;
        return Result<void>::Success();
    }
}  // namespace Horo::Audio::Backend
