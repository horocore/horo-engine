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
        if (conversion.inputRate != facts.Value().outputFormat.sampleRate ||
            conversion.channels != facts.Value().outputFormat.layout.orderedChannels.size() ||
            facts.Value().outputFormat.layout !=
                MakeAudioSpeakerLayout(conversion.channels == 1 ? AudioSpeakerPreset::Mono : AudioSpeakerPreset::Stereo))
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::ResamplerInvalid));
        try {
            auto state = std::make_unique<State>();
            state->descriptor = descriptor;
            state->registry = std::move(registry);
            state->service = std::move(service);
            state->conversion = conversion;
            state->coefficientBytes = maximumCoefficientBytes;
            auto spatial = CoreStereoSpatialRenderer::Create(conversion, maximumCoefficientBytes);
            if (spatial.HasError())
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(spatial.ErrorValue());
            state->spatial.emplace(std::move(spatial).Value());
            auto voice = state->registry->CreateVoice();
            if (voice.HasError())
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(voice.ErrorValue());
            state->voice = voice.Value();
            state->streamVoice = true;
            if (state->voice.owner != descriptor.scope.owner)
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
            if (const auto ready = state->registry->Transition(state->voice, AudioVoiceState::Ready); ready.HasError())
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(ready.ErrorValue());
            auto port = state->service->RetainedRenderPort(stream);
            if (port.HasError())
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(port.ErrorValue());
            state->stream.emplace(std::move(port).Value());
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Success(
                std::make_unique<AudioVoiceRenderRuntime>(ConstructionKey{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioVoiceRenderRuntime::AudioVoiceRenderRuntime */
    AudioVoiceRenderRuntime::AudioVoiceRenderRuntime(ConstructionKey, std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc AudioVoiceRenderRuntime::~AudioVoiceRenderRuntime */
    AudioVoiceRenderRuntime::~AudioVoiceRenderRuntime() = default;

    /** @copydoc AudioVoiceRenderRuntime::Voice */
    AudioVoiceHandle AudioVoiceRenderRuntime::Voice() const noexcept {
        return state_->voice;
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
        using enum AudioVoiceControl;
        using enum AudioVoiceState;
        AudioVoiceState next;
        switch (request.control) {
            case Start:
                if (callback.streamState != Ready)
                    return &AudioErrors::VoiceInvalidTransition;
                if (const auto *error = registry->TryTransition(voice, Scheduled))
                    return error;
                next = Playing;
                break;
            case Pause:
                if (callback.streamState != Playing)
                    return &AudioErrors::VoiceInvalidTransition;
                next = Paused;
                break;
            case Resume:
                if (callback.streamState != Paused)
                    return &AudioErrors::VoiceInvalidTransition;
                next = Playing;
                break;
            case Stop:
                if (callback.streamState == Ready) {
                    if (const auto *error = registry->TryTransition(voice, Scheduled))
                        return error;
                }
                if (const auto *error = registry->TryTransition(voice, Stopping))
                    return error;
                next = Stopped;
                break;
            case Cancel:
                if (const auto *error = registry->TryCancel(voice))
                    return error;
                callback.streamState = Cancelled;
                return nullptr;
            default:
                return &AudioErrors::OperationUnsupported;
        }
        if (const auto *error = registry->TryTransition(voice, next))
            return error;
        callback.streamState = next;
        return nullptr;
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

    /** @copydoc AudioVoiceRenderRuntime::State::RenderStream */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::State::RenderStream(const std::uint32_t frames) noexcept {
        if (callback.streamState != AudioVoiceState::Playing)
            return {};
        std::array<AudioSample *, 2> rawPointers{scratch.raw[0].samples.data(), scratch.raw[1].samples.data()};
        std::uint32_t produced{};
        // Linear conversion admits at most 64 input frames per output frame. Each full chunk covers
        // maximumFrames >= frames; 64 chunks plus two history/end-marker visits bound even high-ratio
        // conversion without the short-block truncation of a frames+1 visit budget.
        std::uint32_t visit{};
        while (visit < 66 && produced < frames) {
            ++visit;
            if (callback.buffered == 0 && !callback.sourceEnded) {
                const auto read = stream->Render({rawPointers.data(), conversion.channels}, descriptor.maximumFrames);
                callback.buffered = read.availableFrames;
                callback.sourceEnded = read.ended || read.stopped;
                if (read.stopped) {
                    (void)registry->TryCancel(voice);
                    callback.streamState = AudioVoiceState::Cancelled;
                    return {.terminal = true};
                }
            }
            std::array<std::span<const float>, 2> inputs{std::span<const float>{scratch.raw[0].samples}.first(callback.buffered),
                                                         std::span<const float>{scratch.raw[1].samples}.first(callback.buffered)};
            std::array<std::span<float>, 2> outputs{std::span{scratch.converted[0].samples}.first(frames - produced),
                                                    std::span{scratch.converted[1].samples}.first(frames - produced)};
            const auto progress = spatial->Process({{inputs.data(), conversion.channels}, callback.buffered, callback.sourceEnded},
                                                   {outputs, frames - produced});
            if (progress.status == AudioResamplerStatus::InvalidBuffer || progress.status == AudioResamplerStatus::InvalidState)
                return {.error = &AudioErrors::ResamplerInvalid};
            for (std::uint32_t channel = 0; channel < 2; ++channel)
                std::copy_n(scratch.converted[channel].samples.begin(), progress.produced,
                            scratch.output[channel].samples.begin() + produced);
            produced += progress.produced;
            callback.buffered -= progress.consumed;
            for (std::uint32_t channel = 0; channel < conversion.channels; ++channel)
                std::move(scratch.raw[channel].samples.begin() + progress.consumed,
                          scratch.raw[channel].samples.begin() + progress.consumed + callback.buffered,
                          scratch.raw[channel].samples.begin());
            if (progress.status == AudioResamplerStatus::Complete) {
                (void)registry->TryTransition(voice, AudioVoiceState::Finished);
                callback.streamState = AudioVoiceState::Finished;
                return {.terminal = true};
            }
            if (progress.produced == 0 && progress.consumed == 0)
                break;  // Silence is already prepared; starvation never waits for a fill job.
        }
        return {};
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
