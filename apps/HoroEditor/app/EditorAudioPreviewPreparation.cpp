#include "EditorAudioPreviewPreparation.h"

#include <algorithm>
#include <array>

namespace Horo::Editor::AudioPreviewHostDetail {
    namespace {
        template <typename Id> Id Identity(const std::uint64_t value) {
            return Id::Create(value).Value();
        }

        constexpr std::uint32_t Frames = 256;
        constexpr std::uint32_t OutputRate = 48'000;
        using PlaybackResources = Result<Audio::Internal::AudioFrontendResources>;
        using VoiceResult = Result<std::unique_ptr<Audio::AudioVoiceRenderRuntime>>;

        /** @brief Describes the common one-voice editor preview conversion. */
        Audio::AudioResamplerDescriptor PreviewConversion(const Audio::AudioProcessingFormat &format) {
            return {.quality = Audio::AudioResamplerQuality::Linear,
                    .inputRate = format.sampleRate,
                    .outputRate = OutputRate,
                    .channels = static_cast<std::uint32_t>(format.layout.orderedChannels.size()),
                    .maximumOutputFrames = Frames};
        }

        /** @brief Keeps voice storage and parameter identities within this private one-voice lane. */
        Audio::AudioVoiceRenderDescriptor PreviewVoiceDescriptor(const Audio::AudioCommandScope &scope) {
            using namespace Audio;
            return {scope, Identity<AudioMemoryPoolId>(58), Identity<AudioParameterId>(59), Frames, 1U << 20U};
        }

        /** @brief Builds the real prepared resident voice; factory copies every sample before this borrow ends. */
        Result<std::unique_ptr<Audio::AudioVoiceRenderRuntime>> Voice(const CapturedSource &capture,
                                                                      const Audio::AudioCommandScope &scope) {
            using namespace Audio;
            const auto &decoded = capture.decoded;
            auto plan = AudioResamplerPlan::Prepare(PreviewConversion(decoded.format), {1U << 20U, 1U << 20U, 4096});
            if (plan.HasError())
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(plan.ErrorValue());
            auto registry = AudioVoiceStateMachine::Create({scope.owner, 1});
            if (registry.HasError())
                return Result<std::unique_ptr<AudioVoiceRenderRuntime>>::Failure(registry.ErrorValue());
            std::array<std::span<const float>, MaximumAudioChannels> planes;
            for (std::size_t i = 0; i < decoded.planes.size(); ++i)
                planes[i] = decoded.planes[i];
            return AudioVoiceRenderRuntime::CreateResident(std::make_shared<AudioVoiceStateMachine>(std::move(registry).Value()),
                                                           {std::span{planes}.first(decoded.planes.size()), decoded.frames, true},
                                                           {plan.Value(),
                                                            {capture.source.playback.loop, 0, decoded.frames},
                                                            64,
                                                            40U << 20U,
                                                            64U << 10U,
                                                            1.0F},
                                                           PreviewVoiceDescriptor(scope));
        }

        /** @brief Shares detached FIFO and mixer preparation between asset previews and the local monitor. */
        template <class PrepareVoice>
        PlaybackResources PrepareLane(const Audio::MixerAssetSchema &asset, Audio::AudioVoiceRenderRequest initial,
                                      const Audio::AudioRuntimeId owner, PrepareVoice prepareVoice) {
            using namespace Audio;
            const AudioCommandScope scope{owner, 1, {owner, 1, 1}};
            const MixerCompileProfile profile{.sampleRate = OutputRate,
                                              .outputLayout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo),
                                              .maximumFrames = Frames,
                                              .maximumVoices = 1};
            auto plan = CompileMixerGraph(asset, {owner, scope.epoch, 1, 1, 1, 1}, profile);
            if (plan.HasError())
                return PlaybackResources::Failure(plan.ErrorValue());
            if (!plan.Value()->ResolveBus(initial.bus).has_value())
                return PlaybackResources::Failure(MakeError(AudioComponentPreviewErrors::MissingBus));
            auto staging = AudioCommandStaging::Create(
                {{owner, Identity<AudioMemoryPoolId>(31), 1, 128, 8, 256U << 10U}, Identity<AudioMemoryPoolId>(32), 128, 8, 1,
                 256U << 10U});
            if (staging.HasError())
                return PlaybackResources::Failure(staging.ErrorValue());
            auto commands = std::move(staging).Value();
            if (commands.RegisterScene(scope.scene) != AudioCommandStagingStatus::Ok)
                return PlaybackResources::Failure(MakeError(AudioErrors::RuntimeInactive));
            auto mixer = MixerGraphRuntime::Create({scope, Identity<AudioMemoryPoolId>(33), profile});
            if (mixer.HasError())
                return PlaybackResources::Failure(mixer.ErrorValue());
            auto voice = prepareVoice(scope);
            if (voice.HasError())
                return PlaybackResources::Failure(voice.ErrorValue());
            initial.source.identity.context = scope.scene;
            if (initial.listener)
                initial.listener->identity.context = scope.scene;
            return PlaybackResources::Success({scope, std::move(commands), std::move(voice).Value(), std::move(mixer).Value(),
                                               std::move(plan).Value(), std::move(initial)});
        }
    }  // namespace

    /** @copydoc RetireDetached */
    void RetireDetached(Audio::Internal::AudioFrontendResources &resources) noexcept {
        if (resources.voice) {
            resources.voice->Close();
            (void)resources.voice->CompleteShutdown(true);  // Composition has never attached a callback.
        }
        if (resources.mixer) {
            resources.mixer->Close();
            (void)resources.mixer->CompleteShutdown(resources.scope, true);
        }
        (void)resources.staging.Close();
        Audio::AudioCommandRecord record;
        for (std::size_t pass = 0; pass < 4; ++pass) {
            (void)resources.staging.Pump(64);
            while (resources.staging.TryConsume(record)) {
                // Detached preparation never publishes these retired staging records.
            }
        }
    }

    /** @copydoc Prepare */
    Result<Audio::Internal::AudioFrontendResources> Prepare(const CapturedSource &capture, const Audio::AudioRuntimeId owner) {
        using namespace Audio;
        const auto bus = capture.source.playback.bus.value_or(Identity<AudioBusId>(3));
        return PrepareLane(MakeDefaultMixerAsset(), {bus, capture.source, capture.listener, {}}, owner,
                           [&capture](const AudioCommandScope &scope) { return Voice(capture, scope); });
    }

    /** @copydoc PrepareMonitor */
    Result<PreparedMonitor> PrepareMonitor(const std::shared_ptr<Audio::AudioStreamingService> &service,
                                           const Audio::AudioStreamHandle stream, const Audio::AudioRuntimeId owner,
                                           std::shared_ptr<const void> hostLease) {
        using namespace Audio;
        using Output = Result<PreparedMonitor>;
        if (!service || !hostLease)
            return Output::Failure(MakeError(AudioErrors::RuntimeInactive));
        const auto facts = service->PlaybackFacts(stream);
        if (facts.HasError())
            return Output::Failure(facts.ErrorValue());
        if (facts.Value().frameCount)
            return Output::Failure(MakeError(AudioErrors::OperationUnsupported));
        const auto state = service->Snapshot(stream);
        if (state.HasError())
            return Output::Failure(state.ErrorValue());
        if (state.Value().stopped || state.Value().bufferedFrames != 0)
            return Output::Failure(MakeError(AudioErrors::RuntimeInactive));

        // This isolated graph contains only the monitor source and its local master output.
        const auto master = Identity<AudioBusId>(1);
        const auto monitor = Identity<AudioBusId>(2);
        const auto stereo = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo);
        const MixerAssetSchema graph{.buses = {{master, MixerBusRole::MasterOutput, "Master", stereo, {}, {}},
                                               {monitor, MixerBusRole::Bus, "Microphone Monitor", stereo, {}, {}}},
                                     .routes = {{Identity<AudioRouteId>(1), monitor, master, MixerRouteKind::Primary, MixerSendTap::PostFader,
                                                 0.0F, true}}};
        AudioVoiceRenderRequest initial{.bus = monitor, .source = {.identity = {.slot = 1, .generation = 1}}};
        initial.source.playback.spatialMode = AudioSpatialMode::TwoD;
        auto playback = PrepareLane(graph, std::move(initial), owner, [&](const AudioCommandScope &scope) -> VoiceResult {
            auto registry = AudioVoiceStateMachine::Create({scope.owner, 1});
            if (registry.HasError())
                return VoiceResult::Failure(registry.ErrorValue());
            return AudioVoiceRenderRuntime::CreateStream(std::make_shared<AudioVoiceStateMachine>(std::move(registry).Value()), service,
                                                          stream, PreviewConversion(facts.Value().format), 16U << 10U,
                                                          PreviewVoiceDescriptor(scope));
        });
        if (playback.HasError())
            return Output::Failure(playback.ErrorValue());
        auto resources = std::move(playback).Value();
        const AudioBusHandle bus{owner, resources.plan->ResolveBus(monitor).value() + 1, 1};
        resources.streams = service;
        resources.stream = stream;
        resources.hostLease = std::move(hostLease);
        return Output::Success({std::move(resources), bus});
    }
}  // namespace Horo::Editor::AudioPreviewHostDetail
