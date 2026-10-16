#include "EditorAudioPreviewPreparation.h"

#include <array>

namespace Horo::Editor::AudioPreviewHostDetail {
    namespace {
        template <typename Id> Id Identity(const std::uint64_t value) {
            return Id::Create(value).Value();
        }

        constexpr std::uint32_t Frames = 256;

        /** @brief Builds the real prepared resident voice; factory copies every sample before this borrow ends. */
        Result<std::unique_ptr<Audio::AudioVoiceRenderRuntime>> Voice(const AudioComponentPreviewRequest &request,
                                                                      const DecodedSource &decoded, const Audio::AudioCommandScope &scope) {
            using namespace Audio;
            auto plan = AudioResamplerPlan::Prepare({.quality = AudioResamplerQuality::Linear,
                                                     .inputRate = decoded.format.sampleRate,
                                                     .outputRate = 48'000,
                                                     .channels = static_cast<std::uint32_t>(decoded.planes.size()),
                                                     .maximumOutputFrames = Frames},
                                                    {1U << 20U, 1U << 20U, 4096});
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
                                                            {request.source.playback.loop, 0, decoded.frames},
                                                            64,
                                                            40U << 20U,
                                                            64U << 10U,
                                                            1.0F},
                                                           {scope, Identity<AudioMemoryPoolId>(58), Identity<AudioParameterId>(59), Frames,
                                                            1U << 20U});
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
            }
        }
    }

    /** @copydoc Prepare */
    Result<Audio::Internal::AudioFrontendResources> Prepare(const AudioComponentPreviewRequest &request, const DecodedSource &decoded,
                                                            const Audio::AudioRuntimeId owner) {
        using namespace Audio;
        const AudioCommandScope scope{owner, 1, {owner, 1, 1}};
        const MixerCompileProfile profile{.sampleRate = 48'000,
                                          .outputLayout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo),
                                          .maximumFrames = Frames,
                                          .maximumVoices = 1};
        auto plan = CompileMixerGraph(MakeDefaultMixerAsset(), {owner, 1, 1, 1, 1, 1}, profile);
        if (plan.HasError())
            return Result<Internal::AudioFrontendResources>::Failure(plan.ErrorValue());
        const auto bus = request.source.playback.bus.value_or(Identity<AudioBusId>(3));
        if (!plan.Value()->ResolveBus(bus))
            return Result<Internal::AudioFrontendResources>::Failure(MakeError(AudioComponentPreviewErrors::MissingBus));
        auto stagingResult = AudioCommandStaging::Create(
            {{owner, Identity<AudioMemoryPoolId>(31), 1, 128, 8, 256U << 10U}, Identity<AudioMemoryPoolId>(32), 128, 8, 1, 256U << 10U});
        if (stagingResult.HasError())
            return Result<Internal::AudioFrontendResources>::Failure(stagingResult.ErrorValue());
        auto staging = std::move(stagingResult).Value();
        if (staging.RegisterScene(scope.scene) != AudioCommandStagingStatus::Ok)
            return Result<Internal::AudioFrontendResources>::Failure(MakeError(AudioErrors::RuntimeInactive));
        auto mixer = MixerGraphRuntime::Create({scope, Identity<AudioMemoryPoolId>(33), profile});
        if (mixer.HasError())
            return Result<Internal::AudioFrontendResources>::Failure(mixer.ErrorValue());
        auto voice = Voice(request, decoded, scope);
        if (voice.HasError())
            return Result<Internal::AudioFrontendResources>::Failure(voice.ErrorValue());
        AudioVoiceRenderRequest initial{bus, request.source, request.listener, {}};
        initial.source.identity.context = scope.scene;
        if (initial.listener)
            initial.listener->identity.context = scope.scene;
        return Result<Internal::AudioFrontendResources>::Success(
            {scope, std::move(staging), std::move(voice).Value(), std::move(mixer).Value(), std::move(plan).Value(), std::move(initial)});
    }
}  // namespace Horo::Editor::AudioPreviewHostDetail
