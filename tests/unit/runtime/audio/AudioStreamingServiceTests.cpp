#include "AllocationProbe.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioStreamingService.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <utility>

namespace Horo::Audio {
    namespace {
        struct PackageFixture final {
            std::atomic<std::uint32_t> opens{};
            std::atomic<std::uint32_t> releases{};
            std::atomic<std::uint32_t> decodes{};
            std::atomic<std::uint32_t> firstOpenedAsset{};
            std::atomic<bool> opening{};
            std::atomic<bool> holdOpen{};
            std::atomic<bool> finalDecodeEntered{};
            std::atomic<bool> holdFinalDecode{};
            std::atomic<bool> ignoreCancellation{};
            std::atomic<bool> holdFirstDecode{};
            std::atomic<bool> firstDecodeEntered{};
            std::atomic<bool> privateCancellationObserved{};
            std::atomic<bool> privateCancellationTimedOut{};
            bool openAfterCancellation{};
            bool decodeFailure{};
            bool wrongSpec{};
        };

        /** @brief Stable stereo callback storage shared by ring and cancellation regressions. */
        struct CallbackBlock final {
            std::array<AudioSample, 4> left{};
            std::array<AudioSample, 4> right{};
            std::array<AudioSample *, 2> planes{left.data(), right.data()};

            AudioStreamRenderResult Render(const AudioStreamRenderPort &port, const std::uint32_t frames) {
                return port.Render(planes, frames);
            }
        };

        struct DecoderContext final {
            PackageFixture *fixture{};
            std::uint64_t frameCount{};
        };

        Assets::AssetId Asset(const std::uint8_t suffix) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = suffix;
            return Assets::AssetId::FromBytes(bytes);
        }

        AudioStreamRequest Request(const std::uint8_t assetSuffix = 1) {
            AudioStreamRequest request;
            request.asset = Asset(assetSuffix);
            request.decoder = {AudioCodecIds::Pcm, {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)}, 8, 2, 16, false};
            request.ringFrames = 4;
            request.lookaheadFrames = 4;
            request.maximumPackageBytes = 1'024;
            return request;
        }

        bool Until(const auto &condition) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!condition()) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::yield();
            }
            return true;
        }

        /** @brief Models a provider blocked on only its private decoder cancellation, never the parent token. */
        void AwaitPrivateCancellation(PackageFixture &fixture, const std::uint64_t firstFrame, const std::atomic<bool> &cancelled) {
            if (firstFrame != 0 || !fixture.holdFirstDecode.load())
                return;
            fixture.firstDecodeEntered.store(true);
            const bool observed = Until([&cancelled] {
                return cancelled.load();
            });
            fixture.privateCancellationObserved.store(observed);
            fixture.privateCancellationTimedOut.store(!observed);
        }

        Result<AudioStreamDecodeProgress> Decode(void *opaque, const std::uint64_t firstFrame, const std::span<AudioSample> output,
                                                 const std::span<std::byte> scratch, const std::atomic<bool> &cancelled) {
            auto &context = *static_cast<DecoderContext *>(opaque);
            auto &fixture = *context.fixture;
            fixture.decodes.fetch_add(1);
            AwaitPrivateCancellation(fixture, firstFrame, cancelled);
            if (firstFrame == context.frameCount - 2) {
                fixture.finalDecodeEntered.store(true);
                while (fixture.holdFinalDecode.load() && (fixture.ignoreCancellation.load() || !cancelled.load()))
                    std::this_thread::yield();
            }
            if (cancelled.load())
                return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioErrors::OperationCancelled));
            if (fixture.decodeFailure)
                return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioErrors::StreamReadFailed));
            if (scratch.size() != 16 || output.size() != 4)
                return Result<AudioStreamDecodeProgress>::Failure(MakeError(AudioErrors::StreamReadFailed));
            for (std::size_t index = 0; index < output.size(); ++index)
                output[index] = static_cast<float>(firstFrame + index / 2 + 1);
            return Result<AudioStreamDecodeProgress>::Success({2, firstFrame + 2 == context.frameCount});
        }

        void Release(void *opaque) noexcept {
            const std::unique_ptr<DecoderContext> context(static_cast<DecoderContext *>(opaque));
            context->fixture->releases.fetch_add(1);
        }

        Result<AudioStreamDecoder> Open(void *opaque, const Assets::AssetId asset, const AudioStreamDecoderSpec &expected,
                                        const std::size_t maximumPackageBytes, const CancellationToken &cancelled) {
            auto &fixture = *static_cast<PackageFixture *>(opaque);
            fixture.opening.store(true);
            while (fixture.holdOpen.load() && (fixture.ignoreCancellation.load() || !cancelled.IsCancellationRequested()))
                std::this_thread::yield();
            if (cancelled.IsCancellationRequested() && !fixture.openAfterCancellation)
                return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::OperationCancelled));
            if (maximumPackageBytes < 32)
                return Result<AudioStreamDecoder>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
            fixture.opens.fetch_add(1);
            std::uint32_t firstExpected = 0;
            (void)fixture.firstOpenedAsset.compare_exchange_strong(firstExpected, asset.Bytes().back());
            auto spec = expected;
            if (fixture.wrongSpec)
                ++spec.frameCount;
            auto context = std::make_unique<DecoderContext>(DecoderContext{&fixture, spec.frameCount});
            auto opened = AudioStreamDecoder::Create(spec, {context.get(), &Decode, nullptr, &Release});
            if (opened.HasValue())
                (void)context.release();
            return opened;
        }

        std::unique_ptr<AudioStreamingService> Service(JobSystem &jobs, PackageFixture &fixture,
                                                       AudioStreamingLimits limits = {2, 1, 1U << 20U}) {
            auto created = AudioStreamingService::Create(jobs, {&fixture, &Open}, limits);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        bool PumpUntil(AudioStreamingService &service, const AudioStreamHandle handle, const std::uint32_t buffered) {
            return Until([&service, handle, buffered] {
                service.Pump();
                const auto snapshot = service.Snapshot(handle);
                return snapshot.HasValue() && snapshot.Value().bufferedFrames >= buffered;
            });
        }

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

    TEST_CASE("Streaming service reports underrun and obeys stop-with-silence policy", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdOpen.store(true);
        auto service = Service(jobs, fixture);
        auto request = Request();
        request.underrunPolicy = AudioStreamUnderrunPolicy::StopWithSilence;
        auto admitted = service->Admit(std::move(request));
        REQUIRE(admitted.HasValue());
        auto portResult = service->RenderPort(admitted.Value());
        REQUIRE(portResult.HasValue());
        auto port = std::move(portResult).Value();
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opening.load();
        }));
        std::array<AudioSample, 2> left{9.0F, 9.0F};
        std::array<AudioSample, 2> right{9.0F, 9.0F};
        std::array<AudioSample *, 2> planes{left.data(), right.data()};
        const auto rendered = port.Render(planes, 2);
        CHECK(rendered.availableFrames == 0);
        CHECK(rendered.silentFrames == 2);
        CHECK(rendered.stopped);
        CHECK(left[0] == 0.0F);
        CHECK(right[1] == 0.0F);
        const auto snapshot = service->Snapshot(admitted.Value());
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().underrunFrames == 2);
        CHECK(snapshot.Value().underrunCallbacks == 1);
        REQUIRE(Until([&] {
            service->Pump();
            return service->Snapshot(admitted.Value()).Value().cancelled;
        }));
        REQUIRE(service->Retire(admitted.Value()).HasValue());
        CHECK(fixture.opens.load() == 0);
    }

    TEST_CASE("Streaming service orders package opens by priority and rejects mismatched generations", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        auto service = Service(jobs, fixture);
        auto low = Request(1);
        low.priority = 1;
        auto high = Request(2);
        high.priority = 9;
        auto lowHandle = service->Admit(std::move(low));
        auto highHandle = service->Admit(std::move(high));
        REQUIRE(lowHandle.HasValue());
        REQUIRE(highHandle.HasValue());
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opens.load() != 0;
        }));
        CHECK(fixture.firstOpenedAsset.load() == 2);
        CHECK(service->Snapshot(lowHandle.Value()).Value().bufferedFrames == 0);
        REQUIRE(service->Retire(highHandle.Value()).HasValue());
        REQUIRE(service->Retire(lowHandle.Value()).HasValue());

        fixture.wrongSpec = true;
        auto next = service->Admit(Request(3));
        REQUIRE(next.HasValue());
        CHECK(next.Value() != lowHandle.Value());
        REQUIRE(Until([&] {
            service->Pump();
            auto snapshot = service->Snapshot(next.Value());
            return snapshot.HasValue() && snapshot.Value().failed;
        }));
        CHECK(fixture.releases.load() >= 2);
        REQUIRE(service->Retire(next.Value()).HasValue());
    }

    TEST_CASE("Streaming service cancellation joins package open and releases worker ownership", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdOpen.store(true);
        auto service = Service(jobs, fixture);
        auto admitted = service->Admit(Request());
        REQUIRE(admitted.HasValue());
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opening.load();
        }));
        REQUIRE(service->Stop(admitted.Value()).HasValue());
        REQUIRE(service->Retire(admitted.Value()).HasValue());
        CHECK(fixture.opens.load() == 0);
        CHECK(fixture.releases.load() == 0);
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
        service->Pump();
        REQUIRE(Until([&] {
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

    TEST_CASE("Streaming cancellation discards and releases a decoder returned by a late open", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdOpen.store(true);
        fixture.ignoreCancellation.store(true);
        fixture.openAfterCancellation = true;
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opening.load();
        }));
        CHECK(service->Stop(handle).HasValue());
        fixture.holdOpen.store(false);
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.opens.load() == 1);
        CHECK(fixture.releases.load() == 1);
        CHECK(fixture.decodes.load() == 0);
    }

    TEST_CASE("Streaming stop cancels a published decoder blocked on its private flag", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdFirstDecode.store(true);
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        service->Pump();
        REQUIRE(Until([&fixture] {
            return fixture.firstDecodeEntered.load();
        }));
        REQUIRE(service->Stop(handle).HasValue());
        CallbackBlock output;
        CHECK(output.Render(port, 4).silentFrames == 4);
        REQUIRE(Until([&fixture] {
            return fixture.privateCancellationObserved.load();
        }));
        REQUIRE(service->Retire(handle).HasValue());
        CHECK_FALSE(fixture.privateCancellationTimedOut.load());
        CHECK(fixture.decodes.load() == 1);
        CHECK(fixture.releases.load() == 1);
    }

    TEST_CASE("Streaming concurrent late open and stop cannot strand a private decoder cancellation", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        for (std::uint32_t iteration = 0; iteration < 32; ++iteration) {
            PackageFixture fixture;
            fixture.holdOpen.store(true);
            fixture.ignoreCancellation.store(true);
            fixture.openAfterCancellation = true;
            fixture.holdFirstDecode.store(true);
            auto service = Service(jobs, fixture);
            const auto handle = service->Admit(Request()).Value();
            service->Pump();
            REQUIRE(Until([&fixture] {
                return fixture.opening.load();
            }));
            std::atomic<bool> race{};
            std::jthread opener([&fixture, &race] {
                while (!race.load())
                    std::this_thread::yield();
                fixture.holdOpen.store(false);
            });
            race.store(true);
            REQUIRE(service->Stop(handle).HasValue());
            opener.join();
            REQUIRE(service->Retire(handle).HasValue());
            CHECK_FALSE(fixture.privateCancellationTimedOut.load());
            CHECK(fixture.releases.load() == 1);
            CHECK((fixture.decodes.load() == 0 || fixture.privateCancellationObserved.load()));
        }
    }

    TEST_CASE("Streaming shutdown timeout retains storage and closes new admission until retry", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdOpen.store(true);
        fixture.ignoreCancellation.store(true);
        auto service = Service(jobs, fixture, {2, 1, 1U << 20U, 1});
        const auto handle = service->Admit(Request()).Value();
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opening.load();
        }));
        const auto first = service->Shutdown();
        CHECK(first.HasError());
        CHECK(service->Snapshot(handle).HasValue());
        CHECK(service->Admit(Request(2)).HasError());
        fixture.holdOpen.store(false);
        REQUIRE(Until([&] {
            return service->Shutdown().HasValue();
        }));
        CHECK(service->Snapshot(handle).HasError());
        CHECK(service->Shutdown().HasValue());
    }

    TEST_CASE("Streaming decoder failure retains its typed reason and callback stays silent", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.decodeFailure = true;
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        REQUIRE(Until([&] {
            service->Pump();
            return service->Snapshot(handle).Value().failed;
        }));
        const auto failure = service->Snapshot(handle).Value().failure;
        REQUIRE(failure.has_value());
        CHECK(failure->code.Value() == AudioErrors::StreamReadFailed.code.Value());
        std::array<AudioSample, 2> left{1.0F, 1.0F};
        std::array<AudioSample, 2> right{1.0F, 1.0F};
        std::array<AudioSample *, 2> planes{left.data(), right.data()};
        CHECK(port.Render(planes, 2).silentFrames == 2);
        CHECK(left[0] == 0.0F);
        CHECK(right[1] == 0.0F);
        CHECK(service->TakeUnderrunReport(handle, 0, 48'000).Value().has_value());
        CHECK_FALSE(service->TakeUnderrunReport(handle, 1, 48'000).Value().has_value());
        CHECK(port.Render(planes, 2).silentFrames == 2);
        CHECK_FALSE(service->TakeUnderrunReport(handle, 2, 48'000).Value().has_value());
        CHECK(service->TakeUnderrunReport(handle, 48'000, 48'000).Value()->missingFrames == 2);
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.releases.load() == 1);
    }

    TEST_CASE("Streaming limits reject unbounded rings and release the byte reservation on retirement", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        auto service = Service(jobs, fixture, {1, 1, 1'088});
        auto invalid = Request();
        invalid.ringFrames = 1;
        CHECK(service->Admit(invalid).HasError());
        invalid = Request();
        invalid.lookaheadFrames = 5;
        CHECK(service->Admit(invalid).HasError());
        const auto first = service->Admit(Request());
        REQUIRE(first.HasValue());
        CHECK(service->Admit(Request(2)).HasError());
        REQUIRE(service->Retire(first.Value()).HasValue());
        const auto second = service->Admit(Request(2));
        REQUIRE(second.HasValue());
        CHECK(second.Value().generation > first.Value().generation);
        CHECK(service->Stop(first.Value()).HasError());
        REQUIRE(service->Retire(second.Value()).HasValue());
    }

    TEST_CASE("Streaming starvation recovers and cancellation interrupts an active decoder before release", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.holdOpen.store(true);
        fixture.holdFinalDecode.store(true);
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.opening.load();
        }));
        CallbackBlock output;
        CHECK(output.Render(port, 4).silentFrames == 4);
        fixture.holdOpen.store(false);
        REQUIRE(PumpUntil(*service, handle, 4));
        CHECK(output.Render(port, 4).availableFrames == 4);
        CHECK(output.left[0] == 1.0F);
        CHECK(output.right[3] == 4.0F);
        REQUIRE(PumpUntil(*service, handle, 2));
        service->Pump();
        REQUIRE(Until([&] {
            return fixture.finalDecodeEntered.load();
        }));
        CHECK(fixture.releases.load() == 0);
        REQUIRE(service->Stop(handle).HasValue());
        CHECK(output.Render(port, 4).silentFrames == 4);
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.releases.load() == 1);
    }

    TEST_CASE("Streaming preparation allocation failure accepts no stream or reservation", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        const auto creation = [&] {
            Tests::AllocationProbe::ScopedFailure failure;
            return AudioStreamingService::Create(jobs, {&fixture, &Open});
        }();
        CHECK(creation.HasError());
        auto service = Service(jobs, fixture);
        auto request = Request();
        const auto admission = [&] {
            Tests::AllocationProbe::ScopedFailure failure;
            return service->Admit(std::move(request));
        }();
        CHECK(admission.HasError());
        const auto admitted = service->Admit(Request());
        REQUIRE(admitted.HasValue());
        CHECK(fixture.opens.load() == 0);
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
}  // namespace Horo::Audio
