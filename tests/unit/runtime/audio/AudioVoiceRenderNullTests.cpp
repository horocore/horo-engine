#include "AudioVoiceRenderTestFixture.h"

namespace Horo::Tests::VoiceRenderFixture {
    /** @brief Host composition uses the production staging, voice executor and mixer in the actual Null callback. */
    struct NullVoicePort final {
        AudioVoiceRenderRuntime &voice;
        MixerGraphRuntime &mixer;
        AudioCommandStaging &staging;
        bool rendered{};

        static Audio::Backend::RenderResult Process(void *context, const Audio::Backend::RenderInvocation &invocation) noexcept {
            using enum Audio::Backend::RenderDisposition;
            if (invocation.phase != Audio::Backend::RenderPhase::Rendering)
                return Audio::PlaybackTest::SilentPhase(invocation);
            auto &port = *static_cast<NullVoicePort *>(context);
            AudioCommandRecord record;
            std::optional<AudioCommandRecord> graph;
            for (std::uint32_t count = 0; count < 4 && port.staging.TryConsume(record); ++count) {
                if (std::holds_alternative<AudioSwapGraphCommand>(record.command.payload))
                    graph = record;
                else if (port.voice.Apply(record))
                    return {};
            }
            const auto output = port.voice.Render(invocation.output.validFrames);
            if (output.error)
                return {};
            const std::array inputs{output.input};
            const auto mixed = port.mixer.Render(Scope, graph ? &*graph : nullptr, inputs, invocation.output);
            port.voice.EndBlock();  // Acknowledge only after the mixer's final access, not TryConsume.
            port.rendered = mixed.status == MixerRenderStatus::Rendered;
            return port.rendered ? Audio::Backend::RenderResult{Rendered, AudioCallbackFaultCode::None} : Audio::Backend::RenderResult{};
        }
    };

    TEST_CASE("Actual stereo Null callbacks execute retained voice state staging and production mixer", "[audio][voice_render][null]") {
        using namespace Audio;
        Rig rig;
        auto mixer = Runtime();
        REQUIRE(rig.voice->Publish(Request(), *rig.plan, rig.staging).Value().status == AudioCommandStagingStatus::Ok);
        REQUIRE(mixer->Publish(rig.plan, Identity(), rig.staging).status == AudioCommandStagingStatus::Ok);
        REQUIRE(rig.staging.Submit({Scope, AudioStartVoiceCommand{rig.voice->Voice()}}).status == AudioCommandStagingStatus::Ok);
        REQUIRE(rig.staging.Pump(4).published == 3);
        auto backend = std::move(Backend::CreateNullAudioBackend({Owner, 1, 1, 1}).Value());
        const AudioDeviceEpoch epoch{{Owner, 1, 1}, 1, Scope.epoch};
        NullVoicePort port{*rig.voice, *mixer, rig.staging};
        PlaybackTest::Complete(*backend, Backend::Enumerate{});
        PlaybackTest::Complete(*backend, Backend::Open{epoch,
                                                       {epoch.device,
                                                        {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                                                        {},
                                                        {16, 16, 16}}});
        PlaybackTest::Complete(*backend, Backend::Start{epoch, {&port, NullVoicePort::Process}});
        REQUIRE(backend->AdvanceCallback().HasValue());
        REQUIRE(backend->CommitRendering(epoch).HasValue());
        const auto allocations = AllocationProbe::Count();
        const auto frees = AllocationProbe::FreeCount();
        const auto rendered = backend->AdvanceCallback();
        const auto allocationEnd = AllocationProbe::Count();
        const auto freeEnd = AllocationProbe::FreeCount();
        REQUIRE(rendered.HasValue());
        CHECK(allocationEnd == allocations);
        CHECK(freeEnd == frees);
        CHECK(port.rendered);
        REQUIRE(rig.voice->Reconcile().applied);
        CHECK(mixer->Reconcile().generation == 1);
        const auto quiesce = backend->Begin(Backend::Quiesce{epoch}, {1, 1'000'000'000}).Value();
        REQUIRE(backend->AdvanceControl().HasValue());
        REQUIRE(backend->AdvanceCallback().HasValue());
        REQUIRE(backend->AcknowledgeCompletion(quiesce).HasValue());
        PlaybackTest::Complete(*backend, Backend::Stop{epoch});
        PlaybackTest::Complete(*backend, Backend::Close{});
        rig.voice->Close();
        REQUIRE(rig.voice->CompleteShutdown(true));
        mixer->Close();
        REQUIRE(mixer->CompleteShutdown(Scope, true) == MixerRenderStatus::Quiesced);
    }
}  // namespace Horo::Tests::VoiceRenderFixture
