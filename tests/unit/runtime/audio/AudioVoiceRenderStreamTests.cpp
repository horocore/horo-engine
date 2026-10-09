#include "AudioStreamingTestFixture.h"
#include "AudioVoiceRenderTestFixture.h"

namespace Horo::Tests::VoiceRenderFixture {
    /** @brief Own the real stream's jobs/package/service in dependency order, sharing admission and detached cleanup assertions. */
    struct StreamRig final {
        JobSystem jobs{{.workerCount = 1}};
        Audio::StreamingTests::PackageFixture package;
        std::shared_ptr<AudioStreamingService> service{Audio::StreamingTests::Service(jobs, package)};
        AudioStreamHandle stream;

        explicit StreamRig(const bool seekable = false) {
            auto request = Audio::StreamingTests::Request();
            request.decoder.seekable = seekable;
            auto admitted = service->Admit(request);
            REQUIRE(admitted.HasValue());
            stream = admitted.Value();
        }

        /** @brief Match real stereo decoder facts; negative admission tests deliberately modify these owned values. */
        static AudioResamplerDescriptor Conversion() {
            auto conversion = Playback().plan.Descriptor();
            conversion.channels = 2;
            return conversion;
        }

        /** @brief Assert successful real preparation before exposing the retained owner. */
        std::unique_ptr<AudioVoiceRenderRuntime> Create() const {
            auto created = AudioVoiceRenderRuntime::CreateStream(Registry(), service, stream, Conversion(), CoefficientBytes, Descriptor());
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        /** @brief Dispatch the retained state through actual FIFO staging without a worker fill. */
        static void Publish(AudioVoiceRenderRuntime &runtime, const MixerRenderPlan &plan, AudioCommandStaging &staging) {
            REQUIRE(runtime.Publish(Request(), plan, staging).Value().status == AudioCommandStagingStatus::Ok);
            REQUIRE(runtime.Apply(ConsumePublished(staging)) == nullptr);
        }

        /** @brief Exercise the compact start command rather than bypassing lifecycle dispatch. */
        static void Start(AudioVoiceRenderRuntime &runtime, AudioCommandStaging &staging) {
            REQUIRE(staging.Submit({Scope, AudioStartVoiceCommand{runtime.Voice()}}).status == AudioCommandStagingStatus::Ok);
            REQUIRE(runtime.Apply(ConsumePublished(staging)) == nullptr);
        }

        /** @brief Exercise typed lifecycle requests through the same real transport. */
        static void Control(AudioVoiceRenderRuntime &runtime, AudioCommandStaging &staging, const AudioVoiceControl operation) {
            REQUIRE(staging.Submit({Scope, AudioVoiceControlRequest{runtime.Voice(), operation}}).status == AudioCommandStagingStatus::Ok);
            REQUIRE(runtime.Apply(ConsumePublished(staging)) == nullptr);
        }

        /** @brief Prepare a seekable logical voice through real publication and FIFO start, without worker PCM. */
        std::unique_ptr<AudioVoiceRenderRuntime> CreateVirtual(const MixerRenderPlan &plan, AudioCommandStaging &staging) const {
            auto runtime = Create();
            Publish(*runtime, plan, staging);
            Control(*runtime, staging, AudioVoiceControl::StartVirtual);
            return runtime;
        }

        /** @brief Detach before retirement and verify whether this case actually acquired a provider source. */
        void Finish(AudioVoiceRenderRuntime &runtime, const std::uint32_t expectedOpens = 1) const {
            runtime.Close();
            REQUIRE(runtime.CompleteShutdown(true));
            REQUIRE(service->Retire(stream).HasValue());
            REQUIRE(service->Shutdown().HasValue());
            CHECK(package.opens.load() == expectedOpens);
            CHECK(package.releases.load() == expectedOpens);
        }
    };

    TEST_CASE("Virtual stream realization positions the worker at the logical cursor", "[audio][virtualization][stream]") {
        StreamRig rig{true};
        auto plan = Plan(Asset());
        auto staging = Staging();
        auto runtime = rig.CreateVirtual(*plan, staging);
        const auto silent = runtime->Render(3);
        REQUIRE(silent.error == nullptr);
        CHECK(runtime->Cursor().frame == 3);
        for (std::uint32_t pump = 0; pump < 8; ++pump)
            rig.service->Pump();
        CHECK(rig.package.opens.load() == 0);
        CHECK(rig.package.decodes.load() == 0);
        CHECK(silent.input.samples.planes[0][0] == 0.0F);
        StreamRig::Control(*runtime, staging, AudioVoiceControl::Realize);
        REQUIRE(Audio::StreamingTests::PumpUntil(*rig.service, rig.stream, 4));
        const auto physical = runtime->Render(1);
        REQUIRE(physical.error == nullptr);
        CHECK(physical.input.samples.planes[0][0] == 4.0F);
        CHECK(runtime->Cursor().frame == 4);
        StreamRig::Control(*runtime, staging, AudioVoiceControl::Virtualize);
        CHECK(runtime->Render(4).terminal);
        CHECK(runtime->Cursor().frame == 8);
        CHECK_FALSE(runtime->Render(1).terminal);
        rig.Finish(*runtime);
    }

    TEST_CASE("Virtual stream advances during preparation and catches up without callback decode", "[audio][virtualization][stream]") {
        StreamRig rig{true};
        auto plan = Plan(Asset());
        auto staging = Staging();
        auto runtime = rig.CreateVirtual(*plan, staging);
        REQUIRE(runtime->Render(1).error == nullptr);
        StreamRig::Control(*runtime, staging, AudioVoiceControl::Realize);
        // No Pump: the accepted seek cannot have executed. Virtual time still advances.
        REQUIRE(runtime->Render(2).error == nullptr);
        CHECK(runtime->Cursor().frame == 3);
        CHECK(rig.package.decodes.load() == 0);
        REQUIRE(Audio::StreamingTests::PumpUntil(*rig.service, rig.stream, 4));
        REQUIRE(runtime->Render(1).error == nullptr);
        CHECK(runtime->Cursor().frame == 4);
        rig.Finish(*runtime);
    }

    TEST_CASE("Nonseekable streams reject virtualization transactionally", "[audio][virtualization][stream]") {
        StreamRig rig;
        auto runtime = rig.Create();
        auto plan = Plan(Asset());
        auto staging = Staging();
        StreamRig::Publish(*runtime, *plan, staging);
        REQUIRE(staging.Submit({Scope, AudioVoiceControlRequest{runtime->Voice(), AudioVoiceControl::StartVirtual}}).status ==
                AudioCommandStagingStatus::Ok);
        CHECK(runtime->Apply(ConsumePublished(staging)) == &AudioErrors::OperationUnsupported);
        CHECK(runtime->Cursor().frame == 0);
        StreamRig::Start(*runtime, staging);
        rig.Finish(*runtime, 0);
    }

    TEST_CASE("Virtual stream observes service stop during pending realization exactly once", "[audio][virtualization][stream]") {
        StreamRig rig{true};
        auto plan = Plan(Asset());
        auto staging = Staging();
        auto runtime = rig.CreateVirtual(*plan, staging);
        REQUIRE(runtime->Render(2).error == nullptr);
        StreamRig::Control(*runtime, staging, AudioVoiceControl::Realize);
        REQUIRE(rig.service->Stop(rig.stream).HasValue());
        const auto allocations = AllocationProbe::Count();
        const auto frees = AllocationProbe::FreeCount();
        const auto stopped = runtime->Render(4);
        const auto repeated = runtime->Render(4);
        const auto allocationEnd = AllocationProbe::Count();
        const auto freeEnd = AllocationProbe::FreeCount();
        CHECK(stopped.terminal);
        CHECK_FALSE(repeated.terminal);
        CHECK(runtime->Cursor().frame == 2);
        CHECK(allocationEnd == allocations);
        CHECK(freeEnd == frees);
        rig.Finish(*runtime, 0);
    }

    TEST_CASE("Virtual stream loop phase realizes through worker wraps without source reopening", "[audio][virtualization][stream]") {
        StreamRig rig{true};
        auto plan = Plan(Asset());
        auto staging = Staging();
        auto runtime = rig.CreateVirtual(*plan, staging);
        const AudioVoiceControlRequest loop{.voice = runtime->Voice(), .control = AudioVoiceControl::SetLoop, .loop = {true, 1, 4}};
        REQUIRE(staging.Submit({Scope, loop}).status == AudioCommandStagingStatus::Ok);
        REQUIRE(runtime->Apply(ConsumePublished(staging)) == nullptr);
        REQUIRE(runtime->Render(13).error == nullptr);
        CHECK(runtime->Cursor().frame == 1);
        StreamRig::Control(*runtime, staging, AudioVoiceControl::Realize);
        for (std::uint32_t frame = 0; frame < 9; ++frame) {
            REQUIRE(Audio::StreamingTests::PumpUntil(*rig.service, rig.stream, 4));
            const auto rendered = runtime->Render(1);
            REQUIRE(rendered.error == nullptr);
            CHECK_FALSE(rendered.terminal);
            CHECK(rendered.input.samples.planes[0][0] == static_cast<float>(2 + frame % 3));
        }
        CHECK(runtime->Cursor().frame == 1);
        rig.Finish(*runtime);
    }

    TEST_CASE("Retained voice stream port prevents retirement until explicit detached cleanup", "[audio][voice_render][stream]") {
        StreamRig rig;
        auto runtime = rig.Create();
        CHECK(rig.service->Retire(rig.stream).HasError());
        CHECK(rig.service->Snapshot(rig.stream).HasValue());
        CHECK(rig.service->RenderPort(rig.stream).HasError());
        auto plan = Plan(Asset());
        auto staging = Staging();
        StreamRig::Publish(*runtime, *plan, staging);
        StreamRig::Start(*runtime, staging);
        REQUIRE(Audio::StreamingTests::PumpUntil(*rig.service, rig.stream, 4));
        // Buffered PCM does not prove worker callback destruction. Quiesce this test-owned
        // scheduler before process-wide counters; retain the actual port/ring for rendering.
        rig.jobs.Shutdown(ShutdownPolicy::Drain);
        const auto allocations = AllocationProbe::Count();
        const auto frees = AllocationProbe::FreeCount();
        const auto output = runtime->Render(2);
        runtime->EndBlock();
        const auto allocationEnd = AllocationProbe::Count();
        const auto freeEnd = AllocationProbe::FreeCount();
        REQUIRE(output.error == nullptr);
        CHECK(output.input.samples.planes[0][0] == 1.0F);
        CHECK(output.input.samples.planes[1][0] == 1.0F);
        CHECK(allocationEnd == allocations);
        CHECK(freeEnd == frees);
        REQUIRE(runtime->Reconcile().applied);
        runtime->Close();
        CHECK_FALSE(runtime->CompleteShutdown(false));
        CHECK(rig.service->Retire(rig.stream).HasError());
        rig.Finish(*runtime);
    }

    TEST_CASE("Stream render admission uses actual decoder facts rather than supplied rate metadata", "[audio][voice_render][stream]") {
        StreamRig rig;
        auto conversion = StreamRig::Conversion();
        conversion.inputRate = 24'000;
        CHECK(AudioVoiceRenderRuntime::CreateStream(Registry(), rig.service, rig.stream, conversion, 4096, Descriptor()).HasError());
        conversion.inputRate = 48'000;
        conversion.channels = 1;
        const auto channelFailure =
            AudioVoiceRenderRuntime::CreateStream(Registry(), rig.service, rig.stream, conversion, CoefficientBytes, Descriptor());
        REQUIRE(channelFailure.HasError());
        CHECK(channelFailure.ErrorValue().code.Value() == AudioErrors::ResamplerInvalid.code.Value());
        conversion.channels = 2;
        const auto insufficientCoefficients =
            AudioVoiceRenderRuntime::CreateStream(Registry(), rig.service, rig.stream, conversion, 4096, Descriptor());
        REQUIRE(insufficientCoefficients.HasError());
        CHECK(insufficientCoefficients.ErrorValue().code.Value() == AudioErrors::ResamplerBudgetExceeded.code.Value());
        auto insufficientState = Descriptor();
        insufficientState.maximumStateBytes = 1;
        const auto stateFailure =
            AudioVoiceRenderRuntime::CreateStream(Registry(), rig.service, rig.stream, conversion, CoefficientBytes, insufficientState);
        REQUIRE(stateFailure.HasError());
        CHECK(stateFailure.ErrorValue().code.Value() == AudioErrors::PlaybackRequestInvalid.code.Value());
        CHECK(rig.service->Retire(rig.stream).HasValue());  // A failed factory never acquires a lasting render-port pin.
        REQUIRE(rig.service->Shutdown().HasValue());
    }

    TEST_CASE("Stream starvation renders bounded silence and callback cancellation leaves worker ownership on control",
              "[audio][voice_render][stream]") {
        StreamRig rig;
        auto runtime = rig.Create();
        auto plan = Plan(Asset());
        auto staging = Staging();
        StreamRig::Publish(*runtime, *plan, staging);
        StreamRig::Start(*runtime, staging);
        const auto empty = runtime->Render(16);  // No fill has been scheduled: deterministic starvation, not a timed sleep.
        REQUIRE(empty.error == nullptr);
        for (auto *plane : empty.input.samples.planes)
            CHECK(std::all_of(plane, plane + 16, [](const float sample) {
                return sample == 0.0F;
            }));
        REQUIRE(staging.StageSceneUnload(Scope.scene).status == AudioCommandStagingStatus::Ok);
        REQUIRE(runtime->Apply(ConsumePublished(staging)) == nullptr);
        runtime->EndBlock();
        (void)runtime->Reconcile();
        CHECK(rig.service->Retire(rig.stream).HasError());
        rig.Finish(*runtime, 0);  // No fill/open was scheduled; cancellation must not release an unacquired provider source.
    }

    TEST_CASE("Partitioned streamed voice keeps unconsumed PCM through pause and reaches exact end of source",
              "[audio][voice_render][stream]") {
        StreamRig rig;
        auto runtime = rig.Create();
        auto plan = Plan(Asset());
        auto staging = Staging();
        StreamRig::Publish(*runtime, *plan, staging);
        const auto control = [&runtime, &staging](const AudioVoiceControl operation) {
            StreamRig::Control(*runtime, staging, operation);
        };
        control(AudioVoiceControl::Start);
        bool terminal{};
        std::uint32_t actualFrames{};
        for (std::uint32_t block = 0; block < 16; ++block) {
            REQUIRE(Audio::StreamingTests::Until([&rig] {
                rig.service->Pump();
                const auto snapshot = rig.service->Snapshot(rig.stream);
                return snapshot.HasValue() && (snapshot.Value().bufferedFrames >= 4 || snapshot.Value().sourceEnded);
            }));
            const auto rendered = runtime->Render(1);
            REQUIRE(rendered.error == nullptr);
            if (const float sample = rendered.input.samples.planes[0][0]; sample != 0.0F) {
                ++actualFrames;
                CHECK(sample == static_cast<float>(actualFrames));
                CHECK(rendered.input.samples.planes[1][0] == sample);
            }
            terminal = rendered.terminal;
            runtime->EndBlock();
            (void)runtime->Reconcile();
            if (block == 0) {
                control(AudioVoiceControl::Pause);
                const auto paused = runtime->Render(1);
                REQUIRE(paused.error == nullptr);
                CHECK(paused.input.samples.planes[0][0] == 0.0F);
                CHECK(paused.input.samples.planes[1][0] == 0.0F);
                control(AudioVoiceControl::Resume);
            }
            if (terminal)
                break;
        }
        CHECK(actualFrames == 8);
        CHECK(terminal);
        CHECK(runtime->Render(1).input.samples.planes[0][0] == 0.0F);
        rig.Finish(*runtime);
    }
}  // namespace Horo::Tests::VoiceRenderFixture
