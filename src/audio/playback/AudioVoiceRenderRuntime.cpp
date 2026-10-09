#include "AudioVoiceRenderState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::Audio {
    /** @copydoc AudioVoiceRenderRuntime::State::Valid */
    bool AudioVoiceRenderRuntime::State::Valid(const AudioVoiceRenderDescriptor &descriptor, const AudioResamplerDescriptor &conversion,
                                               const std::uint64_t coefficientBytes) noexcept {
        return descriptor.scope.owner.IsValid() && descriptor.scope.epoch != 0 && descriptor.scope.scene.IsValid() &&
               descriptor.scope.scene.owner == descriptor.scope.owner && descriptor.storageIdentity.IsValid() &&
               descriptor.gainParameter.IsValid() && descriptor.maximumFrames != 0 && descriptor.maximumFrames <= 4096 &&
               descriptor.maximumFrames <= conversion.maximumOutputFrames && (conversion.channels == 1 || conversion.channels == 2) &&
               conversion.stage == AudioResamplerStage::ClipToMix && conversion.pitch == 1.0 && conversion.playbackSpeed == 1.0 &&
               descriptor.maximumStateBytes <= MaximumAudioMemoryBytes && descriptor.maximumStateBytes >= sizeof(State) &&
               coefficientBytes <= (descriptor.maximumStateBytes - sizeof(State)) / 2;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::~State */
    AudioVoiceRenderRuntime::State::~State() {
        if (streamVoice && registry) {
            (void)registry->TryCancel(voice);
            (void)registry->Release(voice);
        }
    }

    /** @copydoc AudioVoiceRenderRuntime::CreateResident */
    Result<std::unique_ptr<AudioVoiceRenderRuntime>> AudioVoiceRenderRuntime::CreateResident(
        std::shared_ptr<AudioVoiceStateMachine> registry, const AudioResamplerInput source, const AudioVoicePlaybackConfig &playback,
        const AudioVoiceRenderDescriptor &descriptor) {
        const auto &conversion = playback.plan.Descriptor();
        if (!registry || playback.gain != 1.0F || !State::Valid(descriptor, conversion, playback.maximumCoefficientBytes))
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        try {
            auto state = std::make_unique<State>();
            state->descriptor = descriptor;
            state->registry = std::move(registry);
            auto prepared = AudioVoicePlayback::Create(*state->registry, source, playback);
            if (prepared.HasError())
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(prepared.ErrorValue());
            state->resident = std::make_unique<AudioVoicePlayback>(std::move(prepared).Value());
            state->voice = state->resident->Voice();
            if (state->voice.owner != descriptor.scope.owner)
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
            state->conversion = conversion;
            state->residentPlan = playback.plan;
            state->coefficientBytes = playback.maximumCoefficientBytes;
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Success(
                std::make_unique<AudioVoiceRenderRuntime>(ConstructionKey{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioVoiceRenderRuntime::CreateStream */
    Result<std::unique_ptr<AudioVoiceRenderRuntime>> AudioVoiceRenderRuntime::CreateStream(std::shared_ptr<AudioVoiceStateMachine> registry,
                                                                                           std::shared_ptr<AudioStreamingService> service,
                                                                                           const AudioStreamHandle stream,
                                                                                           const AudioResamplerDescriptor &conversion,
                                                                                           const std::uint64_t maximumCoefficientBytes,
                                                                                           const AudioVoiceRenderDescriptor &descriptor) {
        if (!registry || !service || conversion.quality != AudioResamplerQuality::Linear ||
            !State::Valid(descriptor, conversion, maximumCoefficientBytes))
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        const auto facts = service->DecoderSpec(stream);
        if (facts.HasError())
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(facts.ErrorValue());
        if (!State::ValidStreamFormat(conversion, facts.Value()))
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::ResamplerInvalid));
        try {
            auto state = std::make_unique<State>();
            state->descriptor = descriptor;
            state->registry = std::move(registry);
            state->service = std::move(service);
            state->conversion = conversion;
            state->coefficientBytes = maximumCoefficientBytes;
            state->sourceFrames = facts.Value().frameCount;
            state->seekable = facts.Value().seekable;
            if (const auto prepared = state->PrepareStream(stream); prepared.HasError())
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(prepared.ErrorValue());
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Success(
                std::make_unique<AudioVoiceRenderRuntime>(ConstructionKey{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioVoiceRenderRuntime::State::ValidStreamFormat */
    bool AudioVoiceRenderRuntime::State::ValidStreamFormat(const AudioResamplerDescriptor &conversion,
                                                           const AudioStreamDecoderSpec &facts) noexcept {
        return conversion.inputRate == facts.outputFormat.sampleRate &&
               conversion.channels == facts.outputFormat.layout.orderedChannels.size() &&
               facts.outputFormat.layout ==
                   MakeAudioSpeakerLayout(conversion.channels == 1 ? AudioSpeakerPreset::Mono : AudioSpeakerPreset::Stereo);
    }

    /** @copydoc AudioVoiceRenderRuntime::State::PrepareStream */
    Result<void> AudioVoiceRenderRuntime::State::PrepareStream(const AudioStreamHandle streamHandle) {
        auto preparedSpatial = CoreStereoSpatialRenderer::Create(conversion, coefficientBytes);
        if (preparedSpatial.HasError())
            return Result<void>::Failure(preparedSpatial.ErrorValue());
        spatial.emplace(std::move(preparedSpatial).Value());
        auto createdVoice = registry->CreateVoice();
        if (createdVoice.HasError())
            return Result<void>::Failure(createdVoice.ErrorValue());
        voice = createdVoice.Value();
        streamVoice = true;
        if (voice.owner != descriptor.scope.owner)
            return Result<void>::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
        if (const auto ready = registry->Transition(voice, AudioVoiceState::Ready); ready.HasError())
            return ready;
        auto port = service->RetainedRenderPort(streamHandle);
        if (port.HasError())
            return Result<void>::Failure(port.ErrorValue());
        stream.emplace(std::move(port).Value());
        return Result<void>::Success();
    }

    /** @copydoc AudioVoiceRenderRuntime::AudioVoiceRenderRuntime */
    AudioVoiceRenderRuntime::AudioVoiceRenderRuntime(ConstructionKey, std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc AudioVoiceRenderRuntime::~AudioVoiceRenderRuntime */
    AudioVoiceRenderRuntime::~AudioVoiceRenderRuntime() = default;

    /** @copydoc AudioVoiceRenderRuntime::Voice */
    AudioVoiceHandle AudioVoiceRenderRuntime::Voice() const noexcept {
        return state_->voice;
    }

    /** @copydoc AudioVoiceRenderRuntime::Cursor */
    AudioVoiceCursor AudioVoiceRenderRuntime::Cursor() const noexcept {
        return state_->released ? AudioVoiceCursor{} : state_->resident ? state_->resident->Cursor() : state_->streamPlayback.cursor;
    }

    /** @copydoc AudioVoiceRenderRuntime::Publish */
    Result<AudioCommandAdmission> AudioVoiceRenderRuntime::Publish(const AudioVoiceRenderRequest &request, const MixerRenderPlan &graph,
                                                                   AudioCommandStaging &staging) {
        State &s = *state_;
        using enum AudioCommandStagingStatus;
        if (s.publications.closed.load())
            return Result<AudioCommandAdmission>::Success({Closed});
        if (s.publications.pendingSlot != -1)
            return Result<AudioCommandAdmission>::Success({Busy});
        const auto bus = graph.ResolveBus(request.bus);
        const bool threeD = request.source.playback.spatialMode == AudioSpatialMode::ThreeD;
        if (!s.ValidRoute(request, graph, bus))
            return Result<AudioCommandAdmission>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        const auto target =
            PrepareAudioStereoSpatialTarget(request.source, threeD ? &*request.listener : nullptr, request.spatial, s.conversion.channels);
        if (target.HasError())
            return Result<AudioCommandAdmission>::Failure(target.ErrorValue());
        const std::int32_t index = s.publications.retainedSlot == 0 ? 1 : 0;
        if (s.publications.generations[index] == std::numeric_limits<std::uint64_t>::max())
            return Result<AudioCommandAdmission>::Success({SequenceExhausted});
        auto prepared = s.PrepareSlot(request, graph, target.Value(), *bus, index);
        if (prepared.HasError())
            return Result<AudioCommandAdmission>::Failure(prepared.ErrorValue());
        auto &slot = s.publications.slots[index];
        slot = std::move(prepared).Value();
        const auto admitted = staging.Submit({s.descriptor.scope, AudioPublishVoiceStateCommand{s.voice, slot.handle}});
        if (admitted.status != Ok) {
            slot = {};
            return Result<AudioCommandAdmission>::Success(admitted);
        }
        slot.sequence = admitted.sequence;
        s.publications.pendingSlot = index;
        ++s.publications.generations[index];
        // Same control owner performs Pump only after all metadata and processing banks are published.
        s.publications.candidate.store(&slot);
        return Result<AudioCommandAdmission>::Success(admitted);
    }

    /** @copydoc AudioVoiceRenderRuntime::State::ValidRoute */
    bool AudioVoiceRenderRuntime::State::ValidRoute(const AudioVoiceRenderRequest &request, const MixerRenderPlan &graph,
                                                    const std::optional<std::uint32_t> bus) const noexcept {
        const bool threeD = request.source.playback.spatialMode == AudioSpatialMode::ThreeD;
        return request.spatial.smoothingFrames <= 16384 && bus && graph.Buses()[*bus].role != MixerBusRole::Return &&
               graph.Buses()[*bus].layout == layout && graph.SampleRate() == conversion.outputRate &&
               graph.MaximumFrames() >= descriptor.maximumFrames && graph.Identity().owner == descriptor.scope.owner &&
               graph.Identity().epoch == descriptor.scope.epoch && request.source.identity.context == descriptor.scope.scene &&
               request.source.identity.IsValid() &&
               (!threeD ||
                (request.listener && request.listener->identity.IsValid() && request.listener->identity.context == descriptor.scope.scene));
    }

    /** @copydoc AudioVoiceRenderRuntime::State::PrepareSlot */
    Result<VoiceRenderSlot> AudioVoiceRenderRuntime::State::PrepareSlot(const AudioVoiceRenderRequest &request,
                                                                        const MixerRenderPlan &graph,
                                                                        const AudioStereoSpatialTarget &target, const std::uint32_t bus,
                                                                        const std::int32_t index) const {
        const bool threeD = request.source.playback.spatialMode == AudioSpatialMode::ThreeD;
        VoiceRenderSlot prepared;
        prepared.target = target;
        prepared.graph = graph.Identity();
        prepared.busIndex = bus;
        prepared.source = request.source.identity;
        prepared.listener = threeD ? request.listener->identity : AudioSpatialIdentity{};
        prepared.sourceRevision = request.source.motion.discontinuityRevision;
        prepared.listenerRevision = threeD ? request.listener->motion.discontinuityRevision : 0;
        prepared.smoothingFrames = request.spatial.smoothingFrames;
        prepared.handle = {descriptor.scope.owner, descriptor.storageIdentity, static_cast<std::uint32_t>(index + 1),
                           publications.generations[index] + 1};
        if (const bool changedPitch = publications.retainedSlot == -1
                                          ? prepared.target.pitch != 1.0
                                          : prepared.target.pitch != publications.slots[publications.retainedSlot].target.pitch;
            resident && changedPitch) {
            auto pitchedConversion = conversion;
            pitchedConversion.pitch = prepared.target.pitch;
            auto plan = AudioResamplerPlan::Prepare(pitchedConversion, residentPlan->Requirements());
            if (plan.HasError())
                return Result<VoiceRenderSlot>::Failure(plan.ErrorValue());
            auto pitch = AudioResampler::Create(plan.Value(), coefficientBytes);
            if (pitch.HasError())
                return Result<VoiceRenderSlot>::Failure(pitch.ErrorValue());
            prepared.pitchBank.emplace(std::move(pitch).Value());
        }
        return Result<VoiceRenderSlot>::Success(std::move(prepared));
    }

    /** @copydoc AudioVoiceRenderRuntime::State::Control */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::Control(const AudioVoiceControlRequest &request) noexcept {
        if (request.voice != voice)
            return &AudioErrors::HandleStale;
        if (!ValidateAudioVoiceControlRequest(request))
            return &AudioErrors::PlaybackRequestInvalid;
        if (resident)
            return resident->Apply(request);
        AudioVoiceState actual{};
        if (const auto *error = registry->CheckState(voice, actual))
            return error;
        callback.streamState = actual;
        if (IsTerminalAudioVoiceState(actual))
            return &AudioErrors::VoiceInvalidTransition;
        if (request.control >= AudioVoiceControl::StartVirtual || request.control == AudioVoiceControl::Seek ||
            request.control == AudioVoiceControl::SetLoop)
            return StreamControl(request);
        return StreamLifecycle(request.control);
    }

    /** @copydoc AudioVoiceRenderRuntime::State::StreamLifecycle */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::StreamLifecycle(const AudioVoiceControl control) noexcept {
        using enum AudioVoiceControl;
        using enum AudioVoiceState;
        switch (control) {
            case Start:
            case Stop:
                return StartStopStream(control);
            case Pause:
            case Resume:
                return PauseResumeStream(control);
            case Cancel:
                if (const auto *error = registry->TryCancel(voice))
                    return error;
                callback.streamState = Cancelled;
                stream->SuspendFills(true);
                return nullptr;
            default:
                return &AudioErrors::OperationUnsupported;
        }
    }

    /** @copydoc AudioVoiceRenderRuntime::State::CommitStreamState */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::CommitStreamState(const AudioVoiceState next) noexcept {
        if (const auto *error = registry->TryTransition(voice, next))
            return error;
        callback.streamState = next;
        if (next == AudioVoiceState::Stopped)
            stream->SuspendFills(true);
        return nullptr;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::StartStopStream */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::StartStopStream(const AudioVoiceControl control) noexcept {
        using enum AudioVoiceState;
        const bool start = control == AudioVoiceControl::Start;
        if (start && callback.streamState != Ready)
            return &AudioErrors::VoiceInvalidTransition;
        if (callback.streamState == Ready) {
            if (const auto *error = registry->TryTransition(voice, Scheduled))
                return error;
        }
        if (!start) {
            if (const auto *error = registry->TryTransition(voice, Stopping))
                return error;
        }
        return CommitStreamState(start ? Playing : Stopped);
    }

    /** @copydoc AudioVoiceRenderRuntime::State::PauseResumeStream */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::PauseResumeStream(const AudioVoiceControl control) noexcept {
        using enum AudioVoiceState;
        if (control == AudioVoiceControl::Pause) {
            if (callback.streamState != Playing && callback.streamState != Virtual)
                return &AudioErrors::VoiceInvalidTransition;
            return CommitStreamState(Paused);
        }
        if (callback.streamState != Paused)
            return &AudioErrors::VoiceInvalidTransition;
        return CommitStreamState(streamPlayback.virtualMode ? Virtual : Playing);
    }

    /** @copydoc AudioVoiceRenderRuntime::Apply */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::Apply(const AudioCommandRecord &record) noexcept {
        State &s = *state_;
        if (s.released)
            return &AudioErrors::RuntimeInactive;
        if (record.sequence == 0 || record.sequence <= s.callback.callbackSequence)
            return &AudioErrors::CommandBufferInvalid;
        const bool reset = std::holds_alternative<AudioResetCommand>(record.command.payload);
        if (reset ? (record.command.scope.owner != s.descriptor.scope.owner || record.command.scope.epoch != s.descriptor.scope.epoch ||
                     record.command.scope.scene != AudioSceneContextHandle{})
                  : record.command.scope != s.descriptor.scope)
            return &AudioErrors::HandleOwnerMismatch;
        if (reset || std::holds_alternative<AudioSceneUnloadCommand>(record.command.payload)) {
            const auto *error = s.Control({s.voice, AudioVoiceControl::Cancel});
            s.callback.callbackSequence = record.sequence;
            return error;
        }
        if (const auto *publication = std::get_if<AudioPublishVoiceStateCommand>(&record.command.payload)) {
            return s.ApplyPublication(*publication, record.sequence);
        }
        if (const auto *parameter = std::get_if<AudioSetParameterCommand>(&record.command.payload)) {
            if (parameter->voice != s.voice || parameter->parameter != s.descriptor.gainParameter || !std::isfinite(parameter->value) ||
                parameter->value < 0.0F || parameter->value > 16.0F)
                return &AudioErrors::PlaybackRequestInvalid;
            s.callback.gain = parameter->value;
            s.callback.callbackSequence = record.sequence;
            return nullptr;
        }
        const auto *error = std::visit([&s]<typename Command>(const Command &command) noexcept -> const ErrorCodeDescriptor * {
            if constexpr (std::is_same_v<Command, AudioVoiceControlRequest>)
                return s.Control(command);
            else if constexpr (std::is_same_v<Command, AudioStartVoiceCommand>)
                return s.Control({command.voice, AudioVoiceControl::Start});
            else if constexpr (std::is_same_v<Command, AudioStopVoiceCommand>)
                return s.Control({command.voice, AudioVoiceControl::Stop});
            else
                return &AudioErrors::OperationUnsupported;
        }, record.command.payload);
        if (!error)
            s.callback.callbackSequence = record.sequence;
        return error;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::ApplyPublication */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::ApplyPublication(const AudioPublishVoiceStateCommand &publication,
                                                                                const std::uint64_t sequence) noexcept {
        auto *const pending = publications.candidate.load();
        if (!pending || publication.voice != voice || publication.storage != pending->handle || sequence != pending->sequence)
            return &AudioErrors::HandleStale;
        AudioVoiceState state;
        const auto *error = registry->CheckState(voice, state);
        if (!error && IsTerminalAudioVoiceState(state))
            error = &AudioErrors::VoiceInvalidTransition;
        if (!error && publications.closed.load())
            error = &AudioErrors::VoiceAdmissionClosed;
        if (!error && resident && pending->pitchBank)
            error = resident->SwapPitch(voice, *pending->pitchBank);
        if (!error && spatial) {
            const bool resetHistory = !callback.active || pending->source != callback.active->source ||
                                      pending->listener != callback.active->listener ||
                                      pending->sourceRevision != callback.active->sourceRevision ||
                                      pending->listenerRevision != callback.active->listenerRevision;
            if (!spatial->ApplyPreparedTarget(pending->target, pending->smoothingFrames, resetHistory))
                error = &AudioErrors::ResamplerInvalid;
        }
        if (!error) {
            if (resident) {
                callback.residentRamp = callback.active ? pending->smoothingFrames : 0;
                if (callback.residentRamp == 0)
                    callback.residentMatrix = pending->target.matrix;
            }
            callback.active = pending;
            callback.gain = 1.0F;
        }
        publications.applied.store(error == nullptr);
        publications.resolvedPublication.store(sequence);
        callback.callbackSequence = sequence;
        return error;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::RenderResident */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::State::RenderResident(const std::uint32_t frames) noexcept {
        std::array<std::span<float>, 2> planes{std::span{scratch.raw[0].samples}.first(frames),
                                               std::span{scratch.raw[1].samples}.first(frames)};
        const auto rendered = resident->Render({{planes.data(), conversion.channels}, frames});
        if (rendered.error)
            return {.error = rendered.error};
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            if (callback.residentRamp != 0) {
                for (std::size_t coefficient = 0; coefficient < callback.residentMatrix.size(); ++coefficient)
                    callback.residentMatrix[coefficient] +=
                        (callback.active->target.matrix[coefficient] - callback.residentMatrix[coefficient]) /
                        static_cast<float>(callback.residentRamp);
                --callback.residentRamp;
            }
            const float left = scratch.raw[0].samples[frame];
            const float right = conversion.channels == 2 ? scratch.raw[1].samples[frame] : 0.0F;
            scratch.output[0].samples[frame] = left * callback.residentMatrix[0] + right * callback.residentMatrix[1];
            scratch.output[1].samples[frame] = left * callback.residentMatrix[2] + right * callback.residentMatrix[3];
        }
        return {.terminal = rendered.terminal};
    }

    /** @copydoc AudioVoiceRenderRuntime::Render */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::Render(const std::uint32_t frames) noexcept {
        State &s = *state_;
        if (frames == 0 || frames > s.descriptor.maximumFrames)
            return {.error = &AudioErrors::ResamplerInvalid};
        for (auto &plane : s.scratch.output)
            std::fill_n(plane.samples.begin(), frames, 0.0F);
        if (!s.callback.active)
            return {};
        AudioVoiceMixRenderResult result;
        if (!s.publications.closed.load())
            result = s.resident ? s.RenderResident(frames) : s.RenderStream(frames);
        for (auto &plane : s.scratch.output)
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                const float value = plane.samples[frame] * s.callback.gain;
                plane.samples[frame] = std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL ? value : 0.0F;
            }
        if (result.error)
            for (auto &plane : s.scratch.output)
                std::fill_n(plane.samples.begin(), frames, 0.0F);
        result.input = {s.voice,
                        s.callback.active->graph.generation,
                        s.callback.active->busIndex,
                        {ViewAudioChannelLayout(s.layout), s.conversion.outputRate, s.scratch.outputPointers, frames,
                         s.descriptor.maximumFrames}};
        return result;
    }

    /** @copydoc AudioVoiceRenderRuntime::EndBlock */
    void AudioVoiceRenderRuntime::EndBlock() noexcept {
        state_->publications.completed.store(state_->callback.callbackSequence);
    }

    /** @copydoc AudioVoiceRenderRuntime::Reconcile */
    AudioVoiceStateAcknowledgement AudioVoiceRenderRuntime::Reconcile() noexcept {
        State &s = *state_;
        if (const auto completedSequence = s.publications.completed.load();
            s.publications.pendingSlot == -1 || completedSequence < s.publications.slots[s.publications.pendingSlot].sequence ||
            s.publications.resolvedPublication.load() != s.publications.slots[s.publications.pendingSlot].sequence)
            return {completedSequence, s.publications.applied.load()};
        const bool accepted = s.publications.applied.load();
        const auto sequence = s.publications.slots[s.publications.pendingSlot].sequence;
        if (accepted) {
            if (s.publications.retainedSlot != -1)
                s.publications.slots[s.publications.retainedSlot] = {};
            s.publications.retainedSlot = s.publications.pendingSlot;
            // SwapPitch left the previous processing bank here; its last callback use precedes EndBlock.
            s.publications.slots[s.publications.retainedSlot].pitchBank.reset();
        } else {
            s.publications.slots[s.publications.pendingSlot] = {};
        }
        s.publications.candidate.store(nullptr);
        s.publications.pendingSlot = -1;
        return {sequence, accepted};
    }

    /** @copydoc AudioVoiceRenderRuntime::Close */
    void AudioVoiceRenderRuntime::Close() noexcept {
        state_->publications.closed.store(true);
    }

    /** @copydoc AudioVoiceRenderRuntime::CompleteShutdown */
    bool AudioVoiceRenderRuntime::CompleteShutdown(const bool callbackDetached) noexcept {
        State &s = *state_;
        if (!s.publications.closed.load() || !callbackDetached)
            return false;
        if (s.released)
            return true;
        s.publications.candidate.store(nullptr);
        s.callback.active = nullptr;
        s.stream.reset();  // Releases the retirement pin on control, before the retained service can be destroyed.
        s.spatial.reset();
        s.resident.reset();
        if (s.streamVoice) {
            (void)s.registry->TryCancel(s.voice);
            (void)s.registry->Release(s.voice);
            s.streamVoice = false;
        }
        s.publications.slots = {};
        s.publications.pendingSlot = -1;
        s.publications.retainedSlot = -1;
        s.released = true;
        return true;
    }
}  // namespace Horo::Audio
