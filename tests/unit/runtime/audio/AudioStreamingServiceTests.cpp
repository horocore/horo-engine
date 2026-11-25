#include "AllocationProbe.h"
#include "AudioStreamingTestFixture.h"

#include <limits>

namespace Horo::Audio::StreamingTests {
    namespace {
        /** @brief Checks ordered stereo output while the control lane concurrently reuses the ring. */
        void ConsumeStream(const AudioStreamRenderPort &port, std::atomic<bool> &completed, std::atomic<bool> &valid,
                           const std::stop_token stop) {
            alignas(64) std::array<AudioSample, 2> left{};
            alignas(64) std::array<AudioSample, 2> right{};
            const std::array<AudioSample *, 2> planes{left.data(), right.data()};
            std::uint32_t consumed{};
            while (!stop.stop_requested()) {
                const auto block = port.Render(planes, 2);
                for (std::uint32_t frame = 0; frame < block.availableFrames; ++frame) {
                    ++consumed;
                    if (left[frame] != static_cast<float>(consumed) || right[frame] != left[frame])
                        valid.store(false);
                }
                if (block.ended) {
                    if (consumed != 256)
                        valid.store(false);
                    completed.store(true);
                    return;
                }
                std::this_thread::yield();
            }
        }
    }  // namespace

    TEST_CASE("Live PCM shares bounded slots and never publishes a rejected block", "[unit][audio][streaming][live-pcm]") {
        JobSystem jobs({.workerCount = 1});
        auto created = AudioStreamingService::CreateLive(jobs, {.maximumStreams = 1, .maximumConcurrentFills = 1, .maximumBytes = 32});
        REQUIRE(created.HasValue());
        auto service = std::move(created).Value();
        const auto format = Request().decoder.outputFormat;
        CHECK(service->Admit(Request()).HasError());
        CHECK(service->AdmitLivePcm({format, 5}).HasError());
        const auto admitted = service->AdmitLivePcm({format, 4});
        REQUIRE(admitted.HasValue());
        const auto handle = admitted.Value();
        CHECK(service->Snapshot().reservedBytes == 32);
        CHECK(service->DecoderSpec(handle).HasError());
        CHECK(service->PlaybackFacts(handle).Value().format == format);
        CHECK_FALSE(service->PlaybackFacts(handle).Value().frameCount);
        service->Pump();
        CHECK(service->Snapshot(handle).Value().starvationCause == AudioStreamStarvationCause::AwaitingLiveInput);

        CallbackBlock output;
        auto port = std::move(service->RenderPort(handle)).Value();
        const std::array initial{1.0F, -1.0F, 2.0F, -2.0F, 3.0F, -3.0F};
        REQUIRE(service->SubmitLivePcm(handle, initial).HasValue());
        CHECK(service->SubmitLivePcm(handle, initial).HasError());
        CHECK(service->SubmitLivePcm(handle, std::array{4.0F}).HasError());
        CHECK(service->SubmitLivePcm(handle, std::array{4.0F, std::numeric_limits<float>::infinity()}).HasError());
        REQUIRE(output.Render(port, 2).availableFrames == 2);
        CHECK(output.left[1] == 2.0F);
        CHECK(output.right[1] == -2.0F);
        REQUIRE(service->SubmitLivePcm(handle, std::array{4.0F, -4.0F, 5.0F, -5.0F}).HasValue());
        REQUIRE(output.Render(port, 4).availableFrames == 3);
        CHECK((output.left == std::array{3.0F, 4.0F, 5.0F, 0.0F}));
        CHECK((output.right == std::array{-3.0F, -4.0F, -5.0F, 0.0F}));
        REQUIRE(service->SubmitLivePcm(handle, initial).HasValue());
        REQUIRE(service->Stop(handle).HasValue());
        CHECK(output.Render(port, 4).stopped);
        CHECK((output.left == std::array{0.0F, 0.0F, 0.0F, 0.0F}));
        CHECK(service->SubmitLivePcm(handle, initial).HasError());
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(service->Snapshot().reservedBytes == 0);
        const auto replacement = service->AdmitLivePcm({format, 4});
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value() != handle);
        CHECK(service->SubmitLivePcm(handle, initial).HasError());
        CHECK(service->Snapshot(replacement.Value()).Value().bufferedFrames == 0);
    }

    TEST_CASE("Streaming service fills from a package worker and callback consumes a bounded wraparound ring", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        auto service = Service(jobs, fixture);
        auto admitted = service->Admit(Request());
        REQUIRE(admitted.HasValue());
        const auto handle = admitted.Value();
        auto portResult = service->RenderPort(handle);
        REQUIRE(portResult.HasValue());
        auto port = std::move(portResult).Value();
        CHECK(service->RenderPort(handle).HasError());
        REQUIRE(PumpUntil(*service, handle, 4));

        CallbackBlock output;
        const auto allocationsBefore = Tests::AllocationProbe::Count();
        const auto first = output.Render(port, 3);
        const auto allocationsAfter = Tests::AllocationProbe::Count();
        CHECK(allocationsAfter == allocationsBefore);
        CHECK(first.availableFrames == 3);
        CHECK(first.silentFrames == 0);
        CHECK(output.left[0] == 1.0F);
        CHECK(output.right[2] == 3.0F);

        REQUIRE(PumpUntil(*service, handle, 3));
        const auto second = output.Render(port, 3);
        CHECK(second.availableFrames == 3);
        CHECK(output.left[0] == 4.0F);
        CHECK(output.left[2] == 6.0F);
        REQUIRE(PumpUntil(*service, handle, 2));
        const auto third = output.Render(port, 3);
        CHECK(third.availableFrames == 2);
        CHECK(third.silentFrames == 1);
        CHECK(third.ended);
        CHECK(output.left[0] == 7.0F);
        CHECK(output.left[2] == 0.0F);
        CHECK(service->Snapshot(handle).Value().underrunFrames == 0);
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.releases.load() == 1);
        CHECK(service->Snapshot(handle).HasError());
    }

    TEST_CASE("Streaming service refills one free frame without changing exact PCM order", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        REQUIRE(PumpUntil(*service, handle, 4));
        CallbackBlock output;
        REQUIRE(output.Render(port, 1).availableFrames == 1);
        CHECK(output.left[0] == 1.0F);
        REQUIRE(PumpUntil(*service, handle, 4));
        const auto middle = output.Render(port, 4);
        CHECK(middle.availableFrames == 4);
        CHECK_FALSE(middle.ended);
        for (std::size_t frame = 0; frame < 4; ++frame) {
            CHECK(output.left[frame] == static_cast<float>(frame + 2));
            CHECK(output.right[frame] == output.left[frame]);
        }
        REQUIRE(PumpUntil(*service, handle, 3));
        const auto terminalBlock = output.Render(port, 4);
        CHECK(terminalBlock.availableFrames == 3);
        CHECK(terminalBlock.silentFrames == 1);
        CHECK(terminalBlock.ended);
        CHECK(output.left[0] == 6.0F);
        CHECK(output.right[2] == 8.0F);
        CHECK(output.left[3] == 0.0F);
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.releases.load() == 1);
    }

    TEST_CASE("Streaming service terminal publication never hides pending final frames", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdFinalDecode.store(true);
        auto service = Service(jobs, fixture);
        auto admitted = service->Admit(Request());
        REQUIRE(admitted.HasValue());
        auto portResult = service->RenderPort(admitted.Value());
        REQUIRE(portResult.HasValue());
        auto port = std::move(portResult).Value();
        REQUIRE(PumpUntil(*service, admitted.Value(), 4));
        CallbackBlock output;
        CHECK(output.Render(port, 4).availableFrames == 4);
        REQUIRE(PumpUntil(*service, admitted.Value(), 2));
        REQUIRE(Until([&] {
            // Published samples do not prove the prior fill job is terminal; keep
            // the control lane reaping and scheduling until the final decode enters.
            service->Pump();
            return fixture.finalDecodeEntered.load();
        }));
        const auto beforeFinal = output.Render(port, 2);
        CHECK_FALSE(beforeFinal.ended);
        fixture.holdFinalDecode.store(false);
        REQUIRE(Until([&] {
            service->Pump();
            return service->Snapshot(admitted.Value()).Value().sourceEnded;
        }));
        const auto terminalBlock = output.Render(port, 4);
        CHECK(terminalBlock.availableFrames == 2);
        CHECK(terminalBlock.silentFrames == 2);
        CHECK(terminalBlock.ended);
        CHECK_FALSE(terminalBlock.stopped);
        CHECK(output.left[0] == 7.0F);
        CHECK(output.left[1] == 8.0F);
        CHECK(output.left[2] == 0.0F);
        CHECK(service->Snapshot(admitted.Value()).Value().underrunFrames == 0);
        REQUIRE(service->Retire(admitted.Value()).HasValue());
    }

    TEST_CASE("Streaming callback and worker preserve sample order through concurrent ring reuse", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        auto service = Service(jobs, fixture);
        auto request = Request();
        request.decoder.frameCount = 256;
        const auto handle = service->Admit(request).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        std::atomic<bool> completed{};
        std::atomic<bool> valid{true};
        std::jthread callback([port = std::move(port), &completed, &valid](const std::stop_token stop) {
            ConsumeStream(port, completed, valid, stop);
        });
        const bool reachedEnd = Until([&service, handle, ringFrames = request.ringFrames, &valid, &completed] {
            service->Pump();
            if (const auto buffered = service->Snapshot(handle).Value().bufferedFrames; buffered > ringFrames)
                valid.store(false);
            return completed.load();
        });
        callback.request_stop();
        callback.join();
        CHECK(reachedEnd);
        CHECK(valid.load());
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.releases.load() == 1);
    }
}  // namespace Horo::Audio::StreamingTests
