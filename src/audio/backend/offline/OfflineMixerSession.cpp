#include "Horo/Audio/Internal/OfflineMixerSession.h"

#include "Horo/Audio/AudioErrors.h"

#include <new>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Admit only the exact fresh audio epoch, zero-frame clock and ordered prepared voice owners. */
        const ErrorCodeDescriptor *ValidateSource(const OfflineAudioRenderRequest &request, const OfflineMixerSource &source) noexcept {
            const auto &clock = source.initialClock;
            const auto &scope = source.scope;
            const auto &command = source.initialGraph;
            if (!scope.scene.IsValid() || scope.scene.owner != scope.owner || scope.epoch == 0 ||
                !MatchesAudioDeviceEpoch(request.epoch, request.epoch) || request.epoch.device.owner != scope.owner ||
                clock.owner != scope.owner || clock.epoch != scope.epoch || clock.generation == 0 || clock.discontinuityRevision == 0 ||
                clock.sampleFrame != 0 || clock.state != AudioSampleClockState::Running || clock.sampleRate != request.format.sampleRate ||
                request.clockDomain == 0 || clock.observedAt.clockDomain != request.clockDomain || clock.observedAt.nanoseconds != 0 ||
                command.sequence == 0 || command.command.scope != scope ||
                !std::holds_alternative<AudioSwapGraphCommand>(command.command.payload) || source.voices.size() > MaximumAudioVoiceSlots)
                return &AudioErrors::CommandBufferInvalid;

            std::uint32_t previousSlot{};
            for (const auto *voice : source.voices) {
                if (!voice)
                    return &AudioErrors::CommandBufferInvalid;
                const auto handle = voice->Voice();
                if (!handle.IsValid() || handle.owner != scope.owner || handle.slot <= previousSlot)
                    return &AudioErrors::CommandBufferInvalid;
                if (!voice->HasResidentSource())
                    return &AudioErrors::SourceUnsupported;
                previousSlot = handle.slot;
            }
            return nullptr;
        }
    }  // namespace

    OfflineMixerSession::OfflineMixerSession(const OfflineAudioRenderRequest &request, const OfflineMixerSource &source)
        : source_(source), inputs_(source.voices.size()), epoch_(request.epoch), contentFrames_(request.contentFrames),
          pendingGraph_(&source.initialGraph) {}

    /** @copydoc OfflineMixerSession::Prepare */
    Result<OfflineMixerSession> OfflineMixerSession::Prepare(const OfflineAudioRenderRequest &request, const OfflineMixerSource &source) {
        if (request.cancellation.stop_requested())
            return Result<OfflineMixerSession>::Failure(MakeError(AudioErrors::OperationCancelled));
        if (const auto *error = ValidateSource(request, source))
            return Result<OfflineMixerSession>::Failure(MakeError(*error));
        try {
            return Result<OfflineMixerSession>::Success(OfflineMixerSession{request, source});
        } catch (const std::bad_alloc &) {
            return Result<OfflineMixerSession>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc OfflineMixerSession::Port */
    RenderPort OfflineMixerSession::Port() & noexcept {
        return {this, Process};
    }

    RenderResult OfflineMixerSession::Process(void *context, const RenderInvocation &invocation) noexcept {
        auto &session = *static_cast<OfflineMixerSession *>(context);
        if (invocation.epoch != session.epoch_)
            return {RenderDisposition::Fault, AudioCallbackFaultCode::InvalidEpoch};
        switch (invocation.phase) {
            case RenderPhase::Priming:
                return {RenderDisposition::Ready, AudioCallbackFaultCode::None};
            case RenderPhase::Rendering:
                return session.RenderBlock(invocation);
            case RenderPhase::Quiescing:
                return {RenderDisposition::Quiesced, AudioCallbackFaultCode::None};
        }
        return {RenderDisposition::Fault, AudioCallbackFaultCode::BackendFailure};
    }

    RenderResult OfflineMixerSession::RenderBlock(const RenderInvocation &invocation) noexcept {
        const auto voiced = invocation.sampleFrame < contentFrames_ ? source_.voices : source_.voices.first(0);
        std::size_t attempted{};
        std::size_t inputCount{};
        bool voiceFailed{};
        for (auto *voice : voiced) {
            const auto input = voice->Render(invocation.output.validFrames);
            ++attempted;
            if (input.error) {
                voiceFailed = true;
                break;
            }
            if (input.input.voice.IsValid())
                inputs_[inputCount++] = input.input;
        }

        MixerRenderResult mixed;
        if (!voiceFailed) {
            const auto voices = std::span{inputs_}.first(inputCount);
            auto clock = source_.initialClock;
            clock.sampleFrame = invocation.sampleFrame;
            clock.observedAt = invocation.startedAt;
            mixed = source_.automation ? source_.mixer.RenderAutomated(source_.scope, pendingGraph_, voices, invocation.output,
                                                                       {*source_.automation, clock})
                                       : source_.mixer.Render(source_.scope, pendingGraph_, voices, invocation.output);
            pendingGraph_ = nullptr;
        }
        for (auto *voice : voiced.first(attempted))
            voice->EndBlock();

        if (voiceFailed || mixed.status != MixerRenderStatus::Rendered || mixed.commandRejected)
            return {RenderDisposition::Fault, AudioCallbackFaultCode::BackendFailure};
        return {RenderDisposition::Rendered, AudioCallbackFaultCode::None};
    }

    /** @copydoc RenderOfflineAudio */
    Result<OfflineAudioRenderOutput> RenderOfflineAudio(const OfflineAudioRenderRequest &request, const OfflineMixerSource &source) {
        auto prepared = OfflineMixerSession::Prepare(request, source);
        if (prepared.HasError())
            return Result<OfflineAudioRenderOutput>::Failure(prepared.ErrorValue());
        auto session = std::move(prepared).Value();
        return RenderOfflineAudio(request, session.Port());
    }
}  // namespace Horo::Audio::Backend
