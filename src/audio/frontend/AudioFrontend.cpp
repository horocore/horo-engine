#include "AudioFrontendState.h"

#include <array>
#include <atomic>
#include <exception>
#include <new>
#include <utility>

namespace Horo::Audio {
    namespace {
        /** @brief Preserves stable Audio failure identity at the control boundary. */
        Result<void> Failure(const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        }

        /** @brief Clears only the valid planar frames; padding belongs to the adapter. */
        void Silence(const AudioPlanarBlockView &block) noexcept {
            for (auto *plane : block.planes)
                for (std::uint32_t frame = 0; frame < block.validFrames; ++frame)
                    plane[frame] = 0.0F;
        }

        /** @brief Checks detached lane ownership before publishing any immutable generation. */
        bool ValidResources(const Internal::AudioFrontendResources &resources) noexcept {
            if (!resources.voice || !resources.mixer || !resources.plan)
                return false;
            if (resources.voice->Voice().owner != resources.scope.owner || resources.plan->Identity().owner != resources.scope.owner ||
                resources.plan->Identity().epoch != resources.scope.epoch)
                return false;
            return resources.streams ? resources.stream.IsValid() && resources.hostLease != nullptr : !resources.stream.IsValid();
        }

        /** @brief Validates explicitly selected output facts without probing or activating a peer. */
        bool ValidOutput(const Internal::AudioFrontendResources &resources, const Internal::AudioFrontendOutput &output) noexcept {
            return output.backend && output.clockDomain != 0 && output.backend->Owner() == resources.scope.owner &&
                   output.open.plannedEpoch.device.owner == resources.scope.owner && output.devices.backend == output.backend->Kind() &&
                   output.devices.owner == resources.scope.owner && ValidateAudioDeviceSnapshot(output.devices) &&
                   ValidateAudioDeviceFormatRequest(output.open.format) &&
                   resources.plan->SampleRate() == output.open.format.preferred.sampleRate;
        }
    }  // namespace

    /** @copydoc AudioFrontend::State::Process */
    Backend::RenderResult AudioFrontend::State::Process(void *context, const Backend::RenderInvocation &invocation) noexcept {
        auto &state = *static_cast<State *>(context);
        const auto &epoch = state.output->open.plannedEpoch;
        // The backend supplies its preparation-validated retained block; never repeat address-range
        // validation or discover/convert a format on the callback.
        if (invocation.epoch != epoch || invocation.output.validFrames > state.maximumFrames)
            return {Backend::RenderDisposition::Fault, AudioCallbackFaultCode::InvalidEpoch};
        Silence(invocation.output);
        if (invocation.phase == Backend::RenderPhase::Priming)
            return {Backend::RenderDisposition::Ready, AudioCallbackFaultCode::None};
        if (invocation.phase == Backend::RenderPhase::Quiescing)
            return {Backend::RenderDisposition::Quiesced, AudioCallbackFaultCode::None};
        if (invocation.phase != Backend::RenderPhase::Rendering)
            return {Backend::RenderDisposition::Fault, AudioCallbackFaultCode::InvalidEpoch};
        return state.RenderBlock(invocation);
    }

    /** @copydoc AudioFrontend::State::RenderBlock */
    Backend::RenderResult AudioFrontend::State::RenderBlock(const Backend::RenderInvocation &invocation) noexcept {
        auto &owned = *resources;
        AudioCommandRecord record;
        std::optional<AudioCommandRecord> swap;
        std::array<std::pair<std::uint64_t, const ErrorCodeDescriptor *>, MaximumAudioFrontendOperations> completed;
        std::size_t completedCount = 0;
        for (std::uint32_t count = 0; count < 64 && owned.staging.TryConsume(record); ++count) {
            if (std::holds_alternative<AudioSwapGraphCommand>(record.command.payload)) {
                swap = record;
            } else {
                completed[completedCount++] = {record.sequence, owned.voice->Apply(record)};
            }
        }
        const auto voice = owned.voice->Render(invocation.output.validFrames);
        std::array<MixerVoiceInput, 1> input{voice.input};
        const std::span<const MixerVoiceInput> inputs =
            voice.input.voice.IsValid() ? std::span<const MixerVoiceInput>{input} : std::span<const MixerVoiceInput>{};
        const auto mixed = owned.mixer->Render(owned.scope, swap.has_value() ? &*swap : nullptr, inputs, invocation.output);
        owned.voice->EndBlock();
        for (std::size_t index = 0; index < completedCount; ++index)
            CompleteOperation(completed[index].first, completed[index].second);
        if (voice.error != nullptr)
            callbackFailure.store(voice.error, std::memory_order_release);
        if (mixed.status != MixerRenderStatus::Rendered && mixed.status != MixerRenderStatus::Quiesced)
            return {Backend::RenderDisposition::Fault, AudioCallbackFaultCode::BackendFailure};
        return {Backend::RenderDisposition::Rendered, AudioCallbackFaultCode::None};
    }

    /** @copydoc AudioFrontend::State::CompleteOperation */
    void AudioFrontend::State::CompleteOperation(const std::uint64_t sequence, const ErrorCodeDescriptor *error) noexcept {
        for (auto &receipt : receipts) {
            if (receipt.sequence.load(std::memory_order_acquire) != sequence)
                continue;
            receipt.error = error;
            receipt.status.store(error == nullptr ? OperationStatus::Applied : OperationStatus::Rejected, std::memory_order_release);
            return;
        }
        if (error != nullptr)
            callbackFailure.store(error, std::memory_order_release);  // Initial owner publication, not a producer operation.
    }

    /** @copydoc AudioFrontend::AudioFrontend */
    AudioFrontend::AudioFrontend(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc AudioFrontend::~AudioFrontend */
    AudioFrontend::~AudioFrontend() {
        if (!state_->resources.has_value())
            return;  // Factory rejected before ownership transfer; the caller retains its candidate.
        if (!state_->opened && !state_->started && !state_->pending.has_value() && state_->detached &&
            state_->phase != AudioFrontendPhase::Retained && state_->phase != AudioFrontendPhase::Closed) {
            state_->closing = true;
            (void)state_->Release();
        }
        if (state_->phase != AudioFrontendPhase::Closed) {
            // Same fatal lifetime guard as AudioStreamingService: a host must retain and pump the
            // island or terminate. Never silently leak a playing device or free unjoined callbacks.
            std::terminate();
        }
    }

    /** @copydoc AudioFrontend::Start */
    Result<void> AudioFrontend::Start(const AudioMonotonicTimestamp deadline) {
        if (state_->phase != AudioFrontendPhase::Prepared || state_->closing)
            return Failure(AudioErrors::RuntimeInactive);
        const auto begun = state_->Begin(State::Operation::Open, state_->output->open, deadline);
        if (begun.HasValue()) {
            state_->phase = AudioFrontendPhase::Opening;
            state_->failure.reset();
        } else {
            state_->failure = begun.ErrorValue();
        }
        return begun;
    }

    /** @copydoc AudioFrontend::Pump */
    Result<void> AudioFrontend::Pump(const AudioMonotonicTimestamp deadline) {
        if (state_->phase == AudioFrontendPhase::Closed)
            return Result<void>::Success();
        if (state_->phase == AudioFrontendPhase::Retained)
            return state_->failure.has_value() ? Result<void>::Failure(*state_->failure) : Failure(AudioErrors::RuntimeInactive);
        if (state_->resources->streams)
            state_->resources->streams->Pump();
        if (state_->ReconcileOutput().HasError())
            Close();
        if (state_->closing) {
            const auto advanced = state_->AdvanceClose(deadline);
            return advanced.HasError() || !state_->failure.has_value() ? advanced : Result<void>::Failure(*state_->failure);
        }
        return state_->AdvanceActive(deadline);
    }

    /** @copydoc AudioFrontend::State::ReconcileOutput */
    Result<void> AudioFrontend::State::ReconcileOutput() {
        const auto polled = Poll();
        const auto drained = Drain();
        if (polled.HasValue() && drained.HasValue())
            return Result<void>::Success();
        if (!failure.has_value())
            failure = polled.HasError() ? polled.ErrorValue() : drained.ErrorValue();
        return Result<void>::Failure(*failure);
    }

    /** @copydoc AudioFrontend::State::AdvanceActive */
    Result<void> AudioFrontend::State::AdvanceActive(const AudioMonotonicTimestamp deadline) {
        if (opened && !started && !pending.has_value()) {
            const auto begun = Begin(State::Operation::Start, Backend::Start{output->open.plannedEpoch, {this, &State::Process}}, deadline);
            if (begun.HasError()) {
                failure = begun.ErrorValue();
                closing = true;
                resources->voice->Close();
                resources->mixer->Close();
                phase = AudioFrontendPhase::Closing;
                return begun;
            }
            phase = AudioFrontendPhase::Starting;
        }
        (void)resources->staging.Pump(64);
        (void)resources->voice->Reconcile();
        (void)resources->mixer->Reconcile();
        if (const auto *error = callbackFailure.exchange(nullptr, std::memory_order_acq_rel))
            return Failure(*error);
        return Result<void>::Success();
    }

    /** @copydoc AudioFrontend::Transport */
    Result<AudioCommandAdmission> AudioFrontend::Transport(const AudioVoiceControlRequest control) {
        if (state_->phase != AudioFrontendPhase::Active || state_->closing)
            return Result<AudioCommandAdmission>::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (control.voice != state_->resources->voice->Voice() || !ValidateAudioVoiceControlRequest(control))
            return Result<AudioCommandAdmission>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        for (auto &receipt : state_->receipts) {
            if (receipt.sequence.load(std::memory_order_acquire) != 0)
                continue;
            const auto admitted = state_->resources->staging.Submit({state_->resources->scope, control});
            if (admitted.status == AudioCommandStagingStatus::Ok) {
                receipt.error = nullptr;
                receipt.status.store(State::OperationStatus::Pending, std::memory_order_relaxed);
                receipt.sequence.store(admitted.sequence, std::memory_order_release);
            }
            return Result<AudioCommandAdmission>::Success(admitted);
        }
        return Result<AudioCommandAdmission>::Success({AudioCommandStagingStatus::Busy, 0, 0});
    }

    /** @copydoc AudioFrontend::DrainTransportResults */
    std::size_t AudioFrontend::DrainTransportResults(const std::span<AudioFrontendOperationResult> output) noexcept {
        std::size_t count = 0;
        for (auto &receipt : state_->receipts) {
            if (count == output.size())
                break;
            const auto sequence = receipt.sequence.load(std::memory_order_acquire);
            const auto status = receipt.status.load(std::memory_order_acquire);
            if (sequence == 0 || status == State::OperationStatus::Pending)
                continue;
            AudioFrontendOperationDisposition disposition = AudioFrontendOperationDisposition::Cancelled;
            if (status == State::OperationStatus::Applied)
                disposition = AudioFrontendOperationDisposition::Applied;
            else if (status == State::OperationStatus::Rejected)
                disposition = AudioFrontendOperationDisposition::Rejected;
            output[count++] = {state_->resources->scope.owner, sequence, disposition, receipt.error};
            receipt.sequence.store(0, std::memory_order_release);
        }
        return count;
    }

    /** @copydoc AudioFrontend::Close */
    void AudioFrontend::Close() noexcept {
        if (state_->phase == AudioFrontendPhase::Closed)
            return;
        state_->closing = true;
        state_->resources->voice->Close();
        state_->resources->mixer->Close();
        if (state_->phase != AudioFrontendPhase::Retained)
            state_->phase = AudioFrontendPhase::Closing;
    }

    /** @copydoc AudioFrontend::Snapshot */
    AudioFrontendSnapshot AudioFrontend::Snapshot() const {
        AudioFrontendSnapshot snapshot{state_->phase, state_->resources->scope.owner, state_->voiceIdentity,
                                       state_->output->open.plannedEpoch, state_->failure};
        for (const auto &receipt : state_->receipts) {
            if (receipt.sequence.load(std::memory_order_acquire) == 0)
                continue;
            if (receipt.status.load(std::memory_order_acquire) == State::OperationStatus::Pending)
                ++snapshot.pendingOperations;
            else
                ++snapshot.retainedOperationResults;
        }
        return snapshot;
    }

    /** @copydoc Internal::AudioFrontendComposition::Create */
    Result<std::unique_ptr<AudioFrontend>> Internal::AudioFrontendComposition::Create(AudioFrontendResources &resources,
                                                                                      AudioFrontendOutput &output) {
        if (!ValidResources(resources) || !ValidOutput(resources, output))
            return Result<std::unique_ptr<AudioFrontend>>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        bool masterMatches = false;
        for (const auto &bus : resources.plan->Buses())
            if (bus.role == MixerBusRole::MasterOutput)
                masterMatches = bus.layout == output.open.format.preferred.layout;
        if (!masterMatches)
            return Result<std::unique_ptr<AudioFrontend>>::Failure(MakeError(AudioErrors::DeviceFormatUnsupported));
        try {
            auto state = std::make_unique<AudioFrontend::State>();
            std::unique_ptr<AudioFrontend> owner(new AudioFrontend(std::move(state)));
            owner->state_->maximumFrames = resources.plan->MaximumFrames();
            owner->state_->voiceIdentity = resources.voice->Voice();
            const auto published = resources.voice->Publish(resources.initialVoice, *resources.plan, resources.staging);
            if (published.HasError())
                return Result<std::unique_ptr<AudioFrontend>>::Failure(published.ErrorValue());
            if (published.Value().status != AudioCommandStagingStatus::Ok)
                return Result<std::unique_ptr<AudioFrontend>>::Failure(MakeError(AudioErrors::QueueSaturated));
            const auto identity = resources.plan->Identity();
            if (resources.mixer->Publish(resources.plan, identity, resources.staging).status != AudioCommandStagingStatus::Ok)
                return Result<std::unique_ptr<AudioFrontend>>::Failure(MakeError(AudioErrors::QueueSaturated));
            owner->state_->resources.emplace(std::move(resources));
            owner->state_->output.emplace(std::move(output));
            return Result<std::unique_ptr<AudioFrontend>>::Success(std::move(owner));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<AudioFrontend>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }
}  // namespace Horo::Audio
