#include "AudioVoiceRenderTestFixture.h"

namespace Horo::Tests::VoiceRenderFixture {
    TEST_CASE("Voice render publication copies source state and executes actual resident PCM and mixer", "[audio][voice_render]") {
        Rig rig;
        auto request = Request();
        float expectedLeft = std::sqrt(0.5F);
        float expectedRight = expectedLeft;
        SECTION("copied 2D source") {}
        SECTION("copied 3D source and listener") {
            request.source.playback.spatialMode = AudioSpatialMode::ThreeD;
            request.source.motion.current.position = {1.0F, 0.0F, 0.0F};
            request.listener.emplace();
            request.listener->identity = {Scope.scene, 2, 1};
            expectedLeft = 0.0F;
            expectedRight = 1.0F;
        }
        const auto publication = rig.Publish(request);
        request.source.playback.gain = 0.0F;
        request.bus = IdOf<AudioBusId>(3);
        request.source.motion.current.position = {-40.0F, 0.0F, 0.0F};
        if (request.listener)
            request.listener->motion.current.position = {40.0F, 0.0F, 0.0F};
        rig.samples.pcm.fill(0.0F);  // Resident preparation owns its PCM; neither producer memory survives as a borrow.
        REQUIRE(rig.voice->Apply(publication) == nullptr);
        rig.Start();
        auto mixer = Runtime();
        const auto graphRecord = Publish(*mixer, rig.staging, rig.plan);
        Block output;
        const auto rendered = rig.voice->Render(16);
        REQUIRE(rendered.error == nullptr);
        const std::array inputs{rendered.input};
        REQUIRE(mixer->Render(Scope, &graphRecord, inputs, output.View()).status == MixerRenderStatus::Rendered);
        CHECK(std::abs(output.left[8] - expectedLeft) < 2e-6F);
        CHECK(std::abs(output.right[8] - expectedRight) < 2e-6F);
        const auto waiting = rig.voice->Reconcile();
        CHECK(waiting.sequence == 0);
        rig.voice->EndBlock();
        CHECK(rig.voice->Reconcile().applied);
        ShutDown(*mixer, output);
    }

    TEST_CASE("Voice state cannot be reclaimed on consumption before the mixer's completed block", "[audio][voice_render]") {
        Rig rig;
        REQUIRE(rig.voice->Apply(rig.Publish()) == nullptr);
        rig.Start();
        const auto old = rig.voice->Render(16);
        REQUIRE(old.error == nullptr);
        CHECK(rig.voice->Publish(Request(3), *rig.plan, rig.staging).Value().status == AudioCommandStagingStatus::Busy);
        (void)rig.voice->Reconcile();
        CHECK(old.input.samples.planes[0][8] > 0.0F);
        rig.voice->EndBlock();
        REQUIRE(rig.voice->Reconcile().applied);
        const auto changed = rig.Publish(Request(3));
        REQUIRE(rig.voice->Apply(changed) == nullptr);
        const auto next = rig.voice->Render(16);
        CHECK(next.input.busIndex == *rig.plan->ResolveBus(IdOf<AudioBusId>(3)));
        CHECK(next.input.samples.planes[0][0] > 0.0F);  // A route-only change does not reset/fade resident conversion.
        rig.voice->EndBlock();
        CHECK(rig.voice->Reconcile().sequence == changed.sequence);
    }

    TEST_CASE("Adjacent gain coalescing preserves lifecycle and retained state publication order", "[audio][voice_render]") {
        Rig rig;
        rig.BeginAcknowledged();
        const AudioSetParameterCommand first{rig.voice->Voice(), Descriptor().gainParameter, 0.25F};
        auto later = first;
        later.value = 0.5F;
        const auto a = rig.staging.Submit({Scope, first});
        const auto b = rig.staging.Submit({Scope, later});
        REQUIRE(b.status == AudioCommandStagingStatus::Coalesced);
        CHECK(b.replacedSequence == a.sequence);
        REQUIRE(rig.voice->Apply(ConsumePublished(rig.staging)) == nullptr);
        const auto rendered = rig.voice->Render(16);
        CHECK(std::abs(rendered.input.samples.planes[0][8] - std::sqrt(0.5F) * 0.5F) < 2e-6F);
        const auto stop = rig.staging.Submit({Scope, AudioStopVoiceCommand{rig.voice->Voice()}});
        REQUIRE(stop.status == AudioCommandStagingStatus::Ok);
        const auto after = rig.staging.Submit({Scope, first});
        CHECK(after.status == AudioCommandStagingStatus::Ok);
        CHECK(after.replacedSequence == 0);
        REQUIRE(rig.voice->Apply(ConsumePublished(rig.staging)) == nullptr);
        REQUIRE(rig.voice->Apply(ConsumePublished(rig.staging)) == nullptr);
        CHECK(rig.voice->Render(16).terminal);
        rig.voice->EndBlock();
    }

    TEST_CASE("Unsupported voice payloads preserve sequence and callback storage", "[audio][voice_render]") {
        Rig rig;
        rig.BeginAcknowledged();
        const std::array<AudioCommandPayload, 6> unsupported{AudioCreateVoiceCommand{},      AudioAutomateParameterCommand{},
                                                             AudioCancelAutomationCommand{}, AudioSwapGraphCommand{},
                                                             AudioReleaseResourceCommand{},  AudioScheduledBatchCommand{}};
        std::array<const ErrorCodeDescriptor *, unsupported.size()> errors{};
        const auto allocations = AllocationProbe::Count();
        const auto frees = AllocationProbe::FreeCount();
        for (std::size_t index = 0; index < unsupported.size(); ++index)
            errors[index] = rig.voice->Apply({100, {Scope, unsupported[index]}});
        const auto allocationEnd = AllocationProbe::Count();
        const auto freeEnd = AllocationProbe::FreeCount();
        for (const auto *error : errors)
            CHECK(error == &AudioErrors::OperationUnsupported);
        CHECK(allocationEnd == allocations);
        CHECK(freeEnd == frees);
        const AudioCommandRecord stop{100, {Scope, AudioStopVoiceCommand{rig.voice->Voice()}}};
        REQUIRE(rig.voice->Apply(stop) == nullptr);
        CHECK(rig.voice->Apply(stop) == &AudioErrors::CommandBufferInvalid);
        CHECK(rig.voice->Render(16).terminal);
        rig.voice->EndBlock();
    }

    TEST_CASE("Rejected voice state queue admission and malformed routes preserve retry ownership", "[audio][voice_render]") {
        Rig rig;
        for (std::uint32_t index = 0; index < 3; ++index)
            REQUIRE(rig.staging.Submit({Scope, AudioStartVoiceCommand{rig.voice->Voice()}}).status == AudioCommandStagingStatus::Ok);
        CHECK(rig.voice->Publish(Request(), *rig.plan, rig.staging).Value().status == AudioCommandStagingStatus::OrdinaryFull);
        for (std::uint32_t index = 0; index < 3; ++index)
            (void)ConsumePublished(rig.staging);  // No playback start is executed; only the rejected-publication retry is under test.
        auto foreign = Request();
        ++foreign.source.identity.context.generation;
        CHECK(rig.voice->Publish(foreign, *rig.plan, rig.staging).HasError());
        CHECK(rig.voice->Publish(Request(999), *rig.plan, rig.staging).HasError());
        auto excessiveRamp = Request();
        excessiveRamp.spatial.smoothingFrames = 16385;
        CHECK(rig.voice->Publish(excessiveRamp, *rig.plan, rig.staging).HasError());
        REQUIRE(rig.voice->Apply(rig.Publish()) == nullptr);
        rig.voice->EndBlock();
        REQUIRE(rig.voice->Reconcile().applied);
    }

    TEST_CASE("Cancelled voice rejects a new route deterministically and acknowledgement preserves old generation",
              "[audio][voice_render]") {
        Rig rig;
        REQUIRE(rig.voice->Apply(rig.Publish()) == nullptr);
        rig.voice->EndBlock();
        REQUIRE(rig.voice->Reconcile().applied);
        rig.Apply(AudioVoiceControlRequest{rig.voice->Voice(), AudioVoiceControl::Cancel});
        const auto publication = rig.Publish(Request(3));
        CHECK(rig.voice->Apply(publication) == &AudioErrors::VoiceInvalidTransition);
        CHECK(rig.voice->Render(16).input.busIndex == *rig.plan->ResolveBus(IdOf<AudioBusId>(2)));
        rig.voice->EndBlock();
        const auto resolved = rig.voice->Reconcile();
        CHECK(resolved.sequence == publication.sequence);
        CHECK_FALSE(resolved.applied);
    }

    TEST_CASE("Voice render hot paths allocate and reclaim no general storage even on rejection", "[audio][voice_render]") {
        Rig rig;
        const auto publication = rig.Publish();
        const auto allocations = AllocationProbe::Count();
        const auto frees = AllocationProbe::FreeCount();
        const auto *error = rig.voice->Apply(publication);
        const auto rendered = rig.voice->Render(16);
        const auto *rejected = rig.voice->Apply(publication);
        rig.voice->EndBlock();
        const auto allocationEnd = AllocationProbe::Count();
        const auto freeEnd = AllocationProbe::FreeCount();
        CHECK(error == nullptr);
        CHECK(rendered.error == nullptr);
        CHECK(rejected == &AudioErrors::CommandBufferInvalid);
        CHECK(allocationEnd == allocations);
        CHECK(freeEnd == frees);
        REQUIRE(rig.voice->Reconcile().applied);
    }

    TEST_CASE("Voice epoch shutdown retains ownership until detachment and rejects foreign barriers", "[audio][voice_render]") {
        Rig rig;
        REQUIRE(rig.voice->Apply(rig.Publish()) == nullptr);
        auto foreign = Scope;
        ++foreign.epoch;
        CHECK(rig.voice->Apply({2, {foreign, AudioSceneUnloadCommand{}}}) == &AudioErrors::HandleOwnerMismatch);
        rig.voice->Close();
        CHECK_FALSE(rig.voice->CompleteShutdown(false));
        CHECK(rig.voice->Publish(Request(), *rig.plan, rig.staging).Value().status == AudioCommandStagingStatus::Closed);
        REQUIRE(rig.voice->CompleteShutdown(true));
        CHECK(rig.voice->CompleteShutdown(true));
        CHECK(rig.voice->Apply({3, {Scope, AudioStartVoiceCommand{rig.voice->Voice()}}}) == &AudioErrors::RuntimeInactive);
    }

    TEST_CASE("Later control completion cannot acknowledge an unexecuted state publication", "[audio][voice_render]") {
        Rig rig;
        const auto publication = rig.Publish();
        // Deliberately violate dispatch order: malformed host dispatch must not reclaim a still-pending slot.
        rig.Start();
        rig.voice->EndBlock();
        (void)rig.voice->Reconcile();
        CHECK(rig.voice->Publish(Request(3), *rig.plan, rig.staging).Value().status == AudioCommandStagingStatus::Busy);
        CHECK(rig.voice->Apply(publication) == &AudioErrors::CommandBufferInvalid);
        CHECK(rig.voice->Render(16).input.voice == AudioVoiceHandle{});
        rig.voice->Close();
        CHECK_FALSE(rig.voice->CompleteShutdown(false));
        REQUIRE(rig.voice->CompleteShutdown(true));
    }

    TEST_CASE("Malformed voice storage references preserve the active route and closed output stays mixable silence",
              "[audio][voice_render]") {
        Rig rig;
        rig.BeginAcknowledged();
        const auto publication = rig.Publish(Request(3));
        auto malformed = publication;
        ++std::get<AudioPublishVoiceStateCommand>(malformed.command.payload).storage.generation;
        CHECK(rig.voice->Apply(malformed) == &AudioErrors::HandleStale);
        CHECK(rig.voice->Render(16).input.busIndex == *rig.plan->ResolveBus(IdOf<AudioBusId>(2)));
        rig.voice->EndBlock();
        (void)rig.voice->Reconcile();
        CHECK(rig.voice->Publish(Request(), *rig.plan, rig.staging).Value().status == AudioCommandStagingStatus::Busy);
        REQUIRE(rig.voice->Apply(publication) == nullptr);
        rig.voice->Close();
        const auto closed = rig.voice->Render(16);
        REQUIRE(closed.input.voice == rig.voice->Voice());
        CHECK(closed.input.busIndex == *rig.plan->ResolveBus(IdOf<AudioBusId>(3)));
        for (const auto *plane : closed.input.samples.planes)
            CHECK(std::all_of(plane, plane + 16, [](const float sample) {
                return sample == 0.0F;
            }));
        auto mixer = Runtime();
        const auto graphRecord = Publish(*mixer, rig.staging, rig.plan);
        Block output;
        const std::array inputs{closed.input};
        REQUIRE(mixer->Render(Scope, &graphRecord, inputs, output.View()).status == MixerRenderStatus::Rendered);
        rig.voice->EndBlock();
        REQUIRE(rig.voice->Reconcile().applied);
        ShutDown(*mixer, output);
    }

    TEST_CASE("Resident immutable gain generations smooth without borrowing changed producer state", "[audio][voice_render]") {
        Rig rig;
        REQUIRE(rig.voice->Apply(rig.Publish()) == nullptr);
        rig.Start();
        (void)rig.voice->Render(16);
        rig.voice->EndBlock();
        REQUIRE(rig.voice->Reconcile().applied);
        auto request = Request();
        request.source.playback.gain = 0.0F;
        request.spatial.smoothingFrames = 4;
        const auto publication = rig.Publish(request);
        request.source.playback.gain = 1.0F;
        REQUIRE(rig.voice->Apply(publication) == nullptr);
        const auto rendered = rig.voice->Render(16);
        REQUIRE(rendered.error == nullptr);
        const auto *left = rendered.input.samples.planes[0];
        CHECK(left[0] > left[1]);
        CHECK(left[1] > left[2]);
        CHECK(left[2] > left[3]);
        CHECK(left[3] == 0.0F);
        CHECK(left[15] == 0.0F);
        rig.voice->EndBlock();
        REQUIRE(rig.voice->Reconcile().applied);
    }

    TEST_CASE("Voice preparation allocation failures leave canonical slots reusable", "[audio][voice_render][allocation]") {
        Audio::PlaybackTest::SampleBuffers samples;
        const auto playback = Playback();
        bool sawFailure{};
        bool sawSuccess{};
        for (std::size_t failureIndex = 0; failureIndex < 16; ++failureIndex) {
            auto registry = Registry();
            {
                AllocationProbe::ScopedFailure failure(failureIndex);
                auto created = AudioVoiceRenderRuntime::CreateResident(registry, samples.Source(), playback, Descriptor());
                if (created.HasValue()) {
                    created.Value()->Close();
                    (void)created.Value()->CompleteShutdown(true);
                    sawSuccess = true;
                } else {
                    sawFailure = true;
                }
            }
            std::array<AudioVoiceHandle, 8> admitted;
            for (auto &voice : admitted) {
                const auto created = registry->CreateVoice();
                REQUIRE(created.HasValue());
                voice = created.Value();
            }
            CHECK(registry->CreateVoice().HasError());
            REQUIRE(registry->BeginShutdown().HasValue());
            for (const auto voice : admitted)
                REQUIRE(registry->Release(voice).HasValue());
        }
        CHECK(sawFailure);
        CHECK(sawSuccess);
    }
}  // namespace Horo::Tests::VoiceRenderFixture
