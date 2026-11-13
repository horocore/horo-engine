#include "AudioRepeatedPlaybackTestFixture.h"

namespace Horo::Audio {
    namespace {
        using namespace RepeatedPlaybackTest;

        TEST_CASE("Persistent playback requires an explicitly admitted runtime-owned host context",
                  "[audio][repeated_playback][lifecycle]") {
            auto binding = Binding();
            binding.prototype.sceneLifecycle = AudioSceneLifecyclePolicy::KeepAliveInHostContext;
            auto ordinary = Owner();
            const auto unsupported = ordinary.Bind(binding, {});
            REQUIRE(unsupported.HasError());
            CHECK(unsupported.ErrorValue().code.Value() == AudioErrors::OperationUnsupported.code.Value());
            auto config = Config();
            config.hostContext = AudioSceneContextHandle{};
            CHECK(AudioRepeatedPlayback::Create(config).HasError());
            config.hostContext = AudioSceneContextHandle{AudioRuntimeId::Create(999).Value(), 9, 1};
            CHECK(AudioRepeatedPlayback::Create(config).HasError());
            config.hostContext = AudioSceneContextHandle{Runtime(), 9, 1};
            auto created = AudioRepeatedPlayback::Create(config);
            REQUIRE(created.HasValue());
            auto owner = std::move(created).Value();
            CHECK(owner.Bind(binding, {}).HasValue());
        }

        TEST_CASE("Host-owned pending starts survive source unload and terminate at host retirement",
                  "[audio][repeated_playback][lifecycle][qualification]") {
            auto config = Config();
            const AudioSceneContextHandle host{Runtime(), 9, 1};
            config.hostContext = host;
            auto created = AudioRepeatedPlayback::Create(config);
            REQUIRE(created.HasValue());
            auto owner = std::move(created).Value();
            auto binding = Binding();
            binding.prototype.sceneLifecycle = AudioSceneLifecyclePolicy::KeepAliveInHostContext;
            const auto lane = owner.Bind(binding, {}).Value();
            Samples samples;
            auto request = Request(lane, binding, 1);
            request.target = {AudioCommandTargetKind::ExactSampleFrame, 100, 1, 1};
            const auto receipt = Submit(owner, request, samples);
            REQUIRE(receipt.disposition == AudioRepeatedPlaybackDisposition::Admitted);
            CHECK(receipt.commands.commands[0].scope.scene == host);
            const auto ordinaryBinding = Binding(false, 2, 2);
            const auto ordinaryLane = owner.Bind(ordinaryBinding, {}).Value();
            const auto ordinary = Submit(owner, Request(ordinaryLane, ordinaryBinding, 1), samples);
            REQUIRE(owner.RetireScene(binding.prototype.sceneContext).HasValue());
            CHECK(owner.Snapshot(ordinary.voice).Value().state == AudioVoiceState::Cancelled);
            CHECK(owner.Snapshot(receipt.voice).Value().state == AudioVoiceState::Ready);
            CHECK(owner.Submit(Request(lane, binding, 2), samples.Resolved(false), {1, 0}).HasError());
            auto forged = receipt.commands.commands[0];
            forged.scope.scene = binding.prototype.sceneContext;
            CHECK(owner.Apply(forged, {1, 100}) == &AudioErrors::PlaybackRequestInvalid);
            Apply(owner, receipt, 100);
            const auto rendered = owner.Render(receipt.voice, samples.Output(16));
            REQUIRE(rendered.error == nullptr);
            CHECK(rendered.produced == 16);
            CHECK(samples.output[15] > 0.0F);
            REQUIRE(owner.RetireScene(host).HasValue());
            CHECK(owner.Snapshot(receipt.voice).Value().terminalReason == AudioVoiceTerminalReason::Cancelled);
            CHECK(owner.Render(receipt.voice, samples.Output(1)).terminal);
            CHECK_FALSE(owner.Render(receipt.voice, samples.Output(1)).terminal);
            auto fresh = Binding(false, 3, 3, 3);
            fresh.prototype.sceneLifecycle = AudioSceneLifecyclePolicy::KeepAliveInHostContext;
            CHECK(owner.Bind(fresh, {}).HasError());
            const auto closedHost = owner.Bind(Binding(false, 4, 4, 9), {});
            REQUIRE(closedHost.HasError());
            CHECK(closedHost.ErrorValue().code.Value() == AudioErrors::VoiceAdmissionClosed.code.Value());
            REQUIRE(owner.Release(receipt.voice).HasValue());
            REQUIRE(owner.Release(ordinary.voice).HasValue());
            REQUIRE(owner.Reset(2, 2, 2).HasValue());
            CHECK(owner.Bind(fresh, {}).HasError());
        }

        TEST_CASE("Active looping ambience retains owned PCM across source retirement and cancels on reset",
                  "[audio][repeated_playback][lifecycle][qualification]") {
            auto config = Config();
            const AudioSceneContextHandle host{Runtime(), 9, 1};
            config.hostContext = host;
            auto created = AudioRepeatedPlayback::Create(config);
            REQUIRE(created.HasValue());
            auto owner = std::move(created).Value();
            auto binding = Binding();
            binding.prototype.playback.loop = true;
            binding.prototype.sceneLifecycle = AudioSceneLifecyclePolicy::KeepAliveInHostContext;
            const auto lane = owner.Bind(binding, {}).Value();
            Samples samples;
            const auto receipt = Submit(owner, Request(lane, binding, 1), samples);
            Apply(owner, receipt);
            samples.pcm.fill(0.0F);
            REQUIRE(owner.Render(receipt.voice, samples.Output(256)).error == nullptr);
            REQUIRE(owner.RetireScene(binding.prototype.sceneContext).HasValue());
            for (std::uint32_t block = 0; block < 4; ++block) {
                const auto rendered = owner.Render(receipt.voice, samples.Output(256));
                REQUIRE(rendered.error == nullptr);
                CHECK_FALSE(rendered.terminal);
                CHECK(rendered.produced == 256);
                CHECK(samples.output[255] == 1.0F);
            }
            const AudioCommand pause{{Runtime(), 1, host}, AudioVoiceControlRequest{receipt.voice, AudioVoiceControl::Pause}};
            REQUIRE(owner.Apply(pause, {1, 0}) == nullptr);
            REQUIRE(owner.Render(receipt.voice, samples.Output(16)).error == nullptr);
            CHECK(owner.Snapshot(receipt.voice).Value().state == AudioVoiceState::Paused);
            auto resume = pause;
            std::get<AudioVoiceControlRequest>(resume.payload).control = AudioVoiceControl::Resume;
            REQUIRE(owner.Apply(resume, {1, 0}) == nullptr);
            CHECK(owner.Render(receipt.voice, samples.Output(16)).produced == 16);
            REQUIRE(owner.Reset(2, 2, 2).HasValue());
            CHECK(owner.Snapshot(receipt.voice).Value().state == AudioVoiceState::Cancelled);
            CHECK(owner.Apply(receipt.commands.commands[0], {2, 0}) == &AudioErrors::HandleStale);
            REQUIRE(owner.Release(receipt.voice).HasValue());
        }

    }  // namespace
}  // namespace Horo::Audio
