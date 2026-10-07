#pragma once

#include "AudioPlaybackTestSupport.h"
#include "Horo/Audio/AudioVoiceRenderRuntime.h"
#include "MixerTestFixture.h"

namespace Horo::Tests::VoiceRenderFixture {
    using MixerFixture::Asset;
    using MixerFixture::Block;
    using MixerFixture::ConsumePublished;
    using MixerFixture::Identity;
    using MixerFixture::IdOf;
    using MixerFixture::Owner;
    using MixerFixture::Plan;
    using MixerFixture::Publish;
    using MixerFixture::Runtime;
    using MixerFixture::Scope;
    using MixerFixture::ShutDown;
    using MixerFixture::Staging;

    // Linear preparation retains 1,025 rows of two binary32 coefficients (8,200 bytes).
    inline constexpr std::uint64_t CoefficientBytes = 16U * 1024U;

    inline std::shared_ptr<AudioVoiceStateMachine> Registry() {
        auto prepared = AudioVoiceStateMachine::Create({Owner, 8});
        REQUIRE(prepared.HasValue());
        return std::make_shared<AudioVoiceStateMachine>(std::move(prepared).Value());
    }

    inline AudioVoiceRenderDescriptor Descriptor() {
        return {Scope, IdOf<AudioMemoryPoolId>(58), IdOf<AudioParameterId>(59), 16, 1U << 20U};
    }

    inline AudioVoicePlaybackConfig Playback() {
        const auto plan = AudioResamplerPlan::Prepare({.quality = AudioResamplerQuality::Linear,
                                                       .inputRate = 48'000,
                                                       .outputRate = 48'000,
                                                       .channels = 1,
                                                       .maximumOutputFrames = 16},
                                                      {1'000'000, 1'000'000, 4096});
        REQUIRE(plan.HasValue());
        return {plan.Value(), {true, 0, 512}, 1, 1'000'000, CoefficientBytes, 1.0F};
    }

    inline AudioVoiceRenderRequest Request(const std::uint64_t bus = 2) {
        AudioVoiceRenderRequest request;
        request.bus = IdOf<AudioBusId>(bus);
        request.source.identity = {Scope.scene, 1, 1};
        request.source.playback.spatialMode = AudioSpatialMode::TwoD;
        request.source.playback.gain = 1.0F;
        request.source.playback.pitch = 1.0F;
        request.spatial.smoothingFrames = 0;
        return request;
    }

    /** @brief Own every prepared borrow and detach before teardown, including failed assertions. */
    struct Rig final {
        std::shared_ptr<AudioVoiceStateMachine> registry = Registry();
        Audio::PlaybackTest::SampleBuffers samples;
        std::unique_ptr<MixerRenderPlan> plan = Plan(Asset());
        AudioCommandStaging staging = Staging();
        std::unique_ptr<AudioVoiceRenderRuntime> voice;

        Rig() {
            auto created = AudioVoiceRenderRuntime::CreateResident(registry, samples.Source(), Playback(), Descriptor());
            REQUIRE(created.HasValue());
            voice = std::move(created).Value();
        }

        ~Rig() {
            voice->Close();
            (void)voice->CompleteShutdown(true);  // Fixture owns the synchronous callback lane; no background reader exists.
        }

        Rig(const Rig &) = delete;
        Rig &operator=(const Rig &) = delete;

        AudioCommandRecord Publish(const AudioVoiceRenderRequest &request = Request()) {
            const auto submitted = voice->Publish(request, *plan, staging);
            REQUIRE(submitted.HasValue());
            REQUIRE(submitted.Value().status == AudioCommandStagingStatus::Ok);
            return ConsumePublished(staging);
        }

        void Apply(const AudioCommandPayload &payload) {
            const auto admitted = staging.Submit({Scope, payload});
            REQUIRE(admitted.status == AudioCommandStagingStatus::Ok);
            const auto record = ConsumePublished(staging);
            REQUIRE(voice->Apply(record) == nullptr);
        }

        void Start() {
            Apply(AudioStartVoiceCommand{voice->Voice()});
        }

        /** @brief Adopt and acknowledge the initial generation before testing a later parameter/route update. */
        void BeginAcknowledged() {
            REQUIRE(voice->Apply(Publish()) == nullptr);
            Start();
            voice->EndBlock();
            REQUIRE(voice->Reconcile().applied);
        }
    };
}  // namespace Horo::Tests::VoiceRenderFixture
