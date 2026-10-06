#include "Horo/Audio/AudioVoiceRenderRuntime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>

namespace Horo::Audio {
    /** @brief Complete retained slot; metadata is immutable between publication and completed-block acknowledgement.
     * A pitch bank is processing storage: callback exchanges it with resident playback, then control retires it.
     */
    struct VoiceRenderSlot final {
        AudioMemoryHandle handle;
        MixerPlanIdentity graph;
        AudioStereoSpatialTarget target;
        AudioSpatialIdentity source;
        AudioSpatialIdentity listener;
        std::uint64_t sourceRevision{};
        std::uint64_t listenerRevision{};
        std::uint64_t sequence{};
        std::uint32_t busIndex{};
        std::uint32_t smoothingFrames{};
        std::optional<AudioResampler> pitchBank;
    };

    /** @brief One control owner and one callback lane; only the named mailboxes cross lanes.
     * Control owns pending/retained slots. Callback owns playback, raw scratch, active pointer and gain.
     * SC completion follows the mixer's last sample read, never merely command consumption.
     */
    struct AudioVoiceRenderRuntime::State final {
        AudioVoiceRenderDescriptor descriptor;
        std::shared_ptr<AudioVoiceStateMachine> registry;
        std::unique_ptr<AudioVoicePlayback> resident;
        std::shared_ptr<AudioStreamingService> service;
        std::optional<AudioStreamRenderPort> stream;
        std::optional<CoreStereoSpatialRenderer> spatial;
        AudioVoiceHandle voice;
        AudioResamplerDescriptor conversion;
        std::optional<AudioResamplerPlan> residentPlan;
        std::uint64_t coefficientBytes{};
        std::array<VoiceRenderSlot, 2> slots;
        std::array<std::uint64_t, 2> generations{};
        std::int32_t retainedSlot{-1};
        std::int32_t pendingSlot{-1};
        std::atomic<VoiceRenderSlot *> candidate{};
        std::atomic<std::uint64_t> completed{};
        std::atomic<bool> applied{};
        std::atomic<std::uint64_t> resolvedPublication{};
        std::atomic<bool> closed{};
        VoiceRenderSlot *active{};
        std::uint64_t callbackSequence{};
        float gain{1.0F};
        std::array<float, 4> residentMatrix{};
        std::uint32_t residentRamp{};
        AudioVoiceState streamState{AudioVoiceState::Ready};
        std::uint32_t buffered{};
        bool sourceEnded{};
        bool released{};
        bool streamVoice{};
        AudioChannelLayout layout{MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)};

        struct alignas(64) Plane final {
            std::array<float, 4096> samples{};
        };

        std::array<Plane, 2> raw;
        std::array<Plane, 2> output;
        std::array<Plane, 2> converted;  // Aligned per-chunk destination; advancing final-output spans would violate DSP alignment.
        std::array<AudioSample *, 2> outputPointers{output[0].samples.data(), output[1].samples.data()};

        /** @brief Validate fixed composition and charge both worst-case prepared pitch banks without overflow. */
        static bool Valid(const AudioVoiceRenderDescriptor &descriptor, const AudioResamplerDescriptor &conversion,
                          std::uint64_t coefficientBytes) noexcept;
        /** @brief Validate the captured route, graph format and spatial identities before preparing storage. */
        bool ValidRoute(const AudioVoiceRenderRequest &request, const MixerRenderPlan &graph,
                        std::optional<std::uint32_t> bus) const noexcept;
        /** @brief Own one immutable generation and its optional replacement pitch bank before queue admission. */
        Result<VoiceRenderSlot> PrepareSlot(const AudioVoiceRenderRequest &request, const MixerRenderPlan &graph,
                                            const AudioStereoSpatialTarget &target, std::uint32_t bus, std::int32_t index) const;
        /** @brief Resolve an exact retained publication on callback, acknowledging success or rejection without reclamation. */
        const ErrorCodeDescriptor *ApplyPublication(const AudioPublishVoiceStateCommand &publication, std::uint64_t sequence) noexcept;
        /** @brief Apply resident or stream lifecycle without reclamation or worker cancellation on callback. */
        const ErrorCodeDescriptor *Control(const AudioVoiceControlRequest &request) noexcept;
        /** @brief Render owned resident PCM once, then apply the prepared spatial matrix without double rate conversion. */
        AudioVoiceMixRenderResult RenderResident(std::uint32_t frames) noexcept;
        /** @brief Feed retained stream PCM to its sole spatial converter, retaining every unconsumed input frame. */
        AudioVoiceMixRenderResult RenderStream(std::uint32_t frames) noexcept;
        /** @brief Release a stream-only canonical slot on detached control, never from callback. */
        ~State();
    };

    static_assert(std::atomic<VoiceRenderSlot *>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

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
                std::unique_ptr<AudioVoiceRenderRuntime>(new AudioVoiceRenderRuntime(std::move(state))));
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
                std::unique_ptr<AudioVoiceRenderRuntime>(new AudioVoiceRenderRuntime(std::move(state))));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioVoiceRenderRuntime::AudioVoiceRenderRuntime */
    AudioVoiceRenderRuntime::AudioVoiceRenderRuntime(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

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
        if (s.closed.load())
            return Result<AudioCommandAdmission>::Success({Closed});
        if (s.pendingSlot != -1)
            return Result<AudioCommandAdmission>::Success({Busy});
        const auto bus = graph.ResolveBus(request.bus);
        const bool threeD = request.source.playback.spatialMode == AudioSpatialMode::ThreeD;
        if (!s.ValidRoute(request, graph, bus))
            return Result<AudioCommandAdmission>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        const auto target =
            PrepareAudioStereoSpatialTarget(request.source, threeD ? &*request.listener : nullptr, request.spatial, s.conversion.channels);
        if (target.HasError())
            return Result<AudioCommandAdmission>::Failure(target.ErrorValue());
        const std::int32_t index = s.retainedSlot == 0 ? 1 : 0;
        if (s.generations[index] == std::numeric_limits<std::uint64_t>::max())
            return Result<AudioCommandAdmission>::Success({SequenceExhausted});
        auto prepared = s.PrepareSlot(request, graph, target.Value(), *bus, index);
        if (prepared.HasError())
            return Result<AudioCommandAdmission>::Failure(prepared.ErrorValue());
        auto &slot = s.slots[index];
        slot = std::move(prepared).Value();
        const auto admitted = staging.Submit({s.descriptor.scope, AudioPublishVoiceStateCommand{s.voice, slot.handle}});
        if (admitted.status != Ok) {
            slot = {};
            return Result<AudioCommandAdmission>::Success(admitted);
        }
        slot.sequence = admitted.sequence;
        s.pendingSlot = index;
        ++s.generations[index];
        // Same control owner performs Pump only after all metadata and processing banks are published.
        s.candidate.store(&slot);
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
                           generations[index] + 1};
        const bool changedPitch =
            retainedSlot == -1 ? prepared.target.pitch != 1.0 : prepared.target.pitch != slots[retainedSlot].target.pitch;
        if (resident && changedPitch) {
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
        AudioVoiceState next;
        switch (request.control) {
            case Start:
                if (streamState != AudioVoiceState::Ready)
                    return &AudioErrors::VoiceInvalidTransition;
                if (const auto *error = registry->TryTransition(voice, AudioVoiceState::Scheduled))
                    return error;
                next = AudioVoiceState::Playing;
                break;
            case Pause:
                if (streamState != AudioVoiceState::Playing)
                    return &AudioErrors::VoiceInvalidTransition;
                next = AudioVoiceState::Paused;
                break;
            case Resume:
                if (streamState != AudioVoiceState::Paused)
                    return &AudioErrors::VoiceInvalidTransition;
                next = AudioVoiceState::Playing;
                break;
            case Stop:
                if (streamState == AudioVoiceState::Ready) {
                    if (const auto *error = registry->TryTransition(voice, AudioVoiceState::Scheduled))
                        return error;
                }
                if (const auto *error = registry->TryTransition(voice, AudioVoiceState::Stopping))
                    return error;
                next = AudioVoiceState::Stopped;
                break;
            case Cancel:
                if (const auto *error = registry->TryCancel(voice))
                    return error;
                streamState = AudioVoiceState::Cancelled;
                return nullptr;
            default:
                return &AudioErrors::OperationUnsupported;
        }
        if (const auto *error = registry->TryTransition(voice, next))
            return error;
        streamState = next;
        return nullptr;
    }

    /** @copydoc AudioVoiceRenderRuntime::Apply */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::Apply(const AudioCommandRecord &record) noexcept {
        State &s = *state_;
        if (s.released)
            return &AudioErrors::RuntimeInactive;
        if (record.sequence == 0 || record.sequence <= s.callbackSequence)
            return &AudioErrors::CommandBufferInvalid;
        const bool reset = std::holds_alternative<AudioResetCommand>(record.command.payload);
        if (reset ? (record.command.scope.owner != s.descriptor.scope.owner || record.command.scope.epoch != s.descriptor.scope.epoch ||
                     record.command.scope.scene != AudioSceneContextHandle{})
                  : record.command.scope != s.descriptor.scope)
            return &AudioErrors::HandleOwnerMismatch;
        if (reset || std::holds_alternative<AudioSceneUnloadCommand>(record.command.payload)) {
            const auto *error = s.Control({s.voice, AudioVoiceControl::Cancel});
            s.callbackSequence = record.sequence;
            return error;
        }
        if (const auto *publication = std::get_if<AudioPublishVoiceStateCommand>(&record.command.payload)) {
            return s.ApplyPublication(*publication, record.sequence);
        }
        if (const auto *parameter = std::get_if<AudioSetParameterCommand>(&record.command.payload)) {
            if (parameter->voice != s.voice || parameter->parameter != s.descriptor.gainParameter || !std::isfinite(parameter->value) ||
                parameter->value < 0.0F || parameter->value > 16.0F)
                return &AudioErrors::PlaybackRequestInvalid;
            s.gain = parameter->value;
            s.callbackSequence = record.sequence;
            return nullptr;
        }
        AudioVoiceControlRequest control;
        if (const auto *request = std::get_if<AudioVoiceControlRequest>(&record.command.payload))
            control = *request;
        else if (const auto *start = std::get_if<AudioStartVoiceCommand>(&record.command.payload))
            control = {start->voice, AudioVoiceControl::Start};
        else if (const auto *stop = std::get_if<AudioStopVoiceCommand>(&record.command.payload))
            control = {stop->voice, AudioVoiceControl::Stop};
        else
            return &AudioErrors::OperationUnsupported;
        const auto *error = s.Control(control);
        if (!error)
            s.callbackSequence = record.sequence;
        return error;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::ApplyPublication */
    const ErrorCodeDescriptor *AudioVoiceRenderRuntime::State::ApplyPublication(const AudioPublishVoiceStateCommand &publication,
                                                                                const std::uint64_t sequence) noexcept {
        auto *const pending = candidate.load();
        if (!pending || publication.voice != voice || publication.storage != pending->handle || sequence != pending->sequence)
            return &AudioErrors::HandleStale;
        AudioVoiceState state;
        const auto *error = registry->CheckState(voice, state);
        if (!error && IsTerminalAudioVoiceState(state))
            error = &AudioErrors::VoiceInvalidTransition;
        if (!error && closed.load())
            error = &AudioErrors::VoiceAdmissionClosed;
        if (!error && resident && pending->pitchBank)
            error = resident->SwapPitch(voice, *pending->pitchBank);
        if (!error && spatial) {
            const bool resetHistory = !active || pending->source != active->source || pending->listener != active->listener ||
                                      pending->sourceRevision != active->sourceRevision ||
                                      pending->listenerRevision != active->listenerRevision;
            if (!spatial->ApplyPreparedTarget(pending->target, pending->smoothingFrames, resetHistory))
                error = &AudioErrors::ResamplerInvalid;
        }
        if (!error) {
            if (resident) {
                residentRamp = active ? pending->smoothingFrames : 0;
                if (residentRamp == 0)
                    residentMatrix = pending->target.matrix;
            }
            active = pending;
            gain = 1.0F;
        }
        applied.store(error == nullptr);
        resolvedPublication.store(sequence);
        callbackSequence = sequence;
        return error;
    }

    /** @copydoc AudioVoiceRenderRuntime::State::RenderResident */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::State::RenderResident(const std::uint32_t frames) noexcept {
        std::array<std::span<float>, 2> planes{std::span{raw[0].samples}.first(frames), std::span{raw[1].samples}.first(frames)};
        const auto rendered = resident->Render({{planes.data(), conversion.channels}, frames});
        if (rendered.error)
            return {.error = rendered.error};
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            if (residentRamp != 0) {
                for (std::size_t coefficient = 0; coefficient < residentMatrix.size(); ++coefficient)
                    residentMatrix[coefficient] +=
                        (active->target.matrix[coefficient] - residentMatrix[coefficient]) / static_cast<float>(residentRamp);
                --residentRamp;
            }
            const float left = raw[0].samples[frame];
            const float right = conversion.channels == 2 ? raw[1].samples[frame] : 0.0F;
            output[0].samples[frame] = left * residentMatrix[0] + right * residentMatrix[1];
            output[1].samples[frame] = left * residentMatrix[2] + right * residentMatrix[3];
        }
        return {.terminal = rendered.terminal};
    }

    /** @copydoc AudioVoiceRenderRuntime::State::RenderStream */
    AudioVoiceMixRenderResult AudioVoiceRenderRuntime::State::RenderStream(const std::uint32_t frames) noexcept {
        if (streamState != AudioVoiceState::Playing)
            return {};
        std::array<AudioSample *, 2> rawPointers{raw[0].samples.data(), raw[1].samples.data()};
        std::uint32_t produced{};
        // Linear conversion admits at most 64 input frames per output frame. Each full chunk covers
        // maximumFrames >= frames; 64 chunks plus two history/end-marker visits bound even high-ratio
        // conversion without the short-block truncation of a frames+1 visit budget.
        for (std::uint32_t visit = 0; visit < 66 && produced < frames; ++visit) {
            if (buffered == 0 && !sourceEnded) {
                const auto read = stream->Render({rawPointers.data(), conversion.channels}, descriptor.maximumFrames);
                buffered = read.availableFrames;
                sourceEnded = read.ended || read.stopped;
                if (read.stopped) {
                    (void)registry->TryCancel(voice);
                    streamState = AudioVoiceState::Cancelled;
                    return {.terminal = true};
                }
            }
            std::array<std::span<const float>, 2> inputs{std::span<const float>{raw[0].samples}.first(buffered),
                                                         std::span<const float>{raw[1].samples}.first(buffered)};
            std::array<std::span<float>, 2> outputs{std::span{converted[0].samples}.first(frames - produced),
                                                    std::span{converted[1].samples}.first(frames - produced)};
            const auto progress =
                spatial->Process({{inputs.data(), conversion.channels}, buffered, sourceEnded}, {outputs, frames - produced});
            if (progress.status == AudioResamplerStatus::InvalidBuffer || progress.status == AudioResamplerStatus::InvalidState)
                return {.error = &AudioErrors::ResamplerInvalid};
            for (std::uint32_t channel = 0; channel < 2; ++channel)
                std::copy_n(converted[channel].samples.begin(), progress.produced, output[channel].samples.begin() + produced);
            produced += progress.produced;
            buffered -= progress.consumed;
            for (std::uint32_t channel = 0; channel < conversion.channels; ++channel)
                std::move(raw[channel].samples.begin() + progress.consumed, raw[channel].samples.begin() + progress.consumed + buffered,
                          raw[channel].samples.begin());
            if (progress.status == AudioResamplerStatus::Complete) {
                (void)registry->TryTransition(voice, AudioVoiceState::Finished);
                streamState = AudioVoiceState::Finished;
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
        for (auto &plane : s.output)
            std::fill_n(plane.samples.begin(), frames, 0.0F);
        if (!s.active)
            return {};
        auto result = s.closed.load() ? AudioVoiceMixRenderResult{} : (s.resident ? s.RenderResident(frames) : s.RenderStream(frames));
        for (auto &plane : s.output)
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                const float value = plane.samples[frame] * s.gain;
                plane.samples[frame] = std::isfinite(value) && std::fpclassify(value) != FP_SUBNORMAL ? value : 0.0F;
            }
        if (result.error)
            for (auto &plane : s.output)
                std::fill_n(plane.samples.begin(), frames, 0.0F);
        result.input = {s.voice,
                        s.active->graph.generation,
                        s.active->busIndex,
                        {ViewAudioChannelLayout(s.layout), s.conversion.outputRate, s.outputPointers, frames, s.descriptor.maximumFrames}};
        return result;
    }

    /** @copydoc AudioVoiceRenderRuntime::EndBlock */
    void AudioVoiceRenderRuntime::EndBlock() noexcept {
        state_->completed.store(state_->callbackSequence);
    }

    /** @copydoc AudioVoiceRenderRuntime::Reconcile */
    AudioVoiceStateAcknowledgement AudioVoiceRenderRuntime::Reconcile() noexcept {
        State &s = *state_;
        const auto completed = s.completed.load();
        if (s.pendingSlot == -1 || completed < s.slots[s.pendingSlot].sequence ||
            s.resolvedPublication.load() != s.slots[s.pendingSlot].sequence)
            return {completed, s.applied.load()};
        const bool accepted = s.applied.load();
        const auto sequence = s.slots[s.pendingSlot].sequence;
        if (accepted) {
            if (s.retainedSlot != -1)
                s.slots[s.retainedSlot] = {};
            s.retainedSlot = s.pendingSlot;
            // SwapPitch left the previous processing bank here; its last callback use precedes EndBlock.
            s.slots[s.retainedSlot].pitchBank.reset();
        } else {
            s.slots[s.pendingSlot] = {};
        }
        s.candidate.store(nullptr);
        s.pendingSlot = -1;
        return {sequence, accepted};
    }

    /** @copydoc AudioVoiceRenderRuntime::Close */
    void AudioVoiceRenderRuntime::Close() noexcept {
        state_->closed.store(true);
    }

    /** @copydoc AudioVoiceRenderRuntime::CompleteShutdown */
    bool AudioVoiceRenderRuntime::CompleteShutdown(const bool callbackDetached) noexcept {
        State &s = *state_;
        if (!s.closed.load() || !callbackDetached)
            return false;
        if (s.released)
            return true;
        s.candidate.store(nullptr);
        s.active = nullptr;
        s.stream.reset();  // Releases the retirement pin on control, before the retained service can be destroyed.
        s.spatial.reset();
        s.resident.reset();
        if (s.streamVoice) {
            (void)s.registry->TryCancel(s.voice);
            (void)s.registry->Release(s.voice);
            s.streamVoice = false;
        }
        s.slots = {};
        s.pendingSlot = -1;
        s.retainedSlot = -1;
        s.released = true;
        return true;
    }
}  // namespace Horo::Audio
