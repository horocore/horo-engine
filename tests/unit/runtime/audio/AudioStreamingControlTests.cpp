#include "AllocationProbe.h"
#include "AudioStreamState.h"
#include "AudioStreamingTestFixture.h"

#include <limits>

namespace Horo::Audio::StreamingTests {
    TEST_CASE("Rate-limited stream reports retain late callback facts and saturated frame totals", "[unit][audio][streaming]") {
        UnderrunReporting reporting;
        const auto cause = AudioStreamStarvationCause::WorkerPending;
        const auto framesOnly = reporting.Take(2, 0, 100, 10, cause);
        REQUIRE(framesOnly.has_value());
        CHECK(framesOnly->missingFrames == 2);
        CHECK_FALSE(reporting.Take(2, 1, 105, 10, cause).has_value());
        const auto callbackOnly = reporting.Take(2, 1, 110, 10, cause);
        REQUIRE(callbackOnly.has_value());
        CHECK(callbackOnly->missingFrames == 0);
        CHECK(callbackOnly->callbackCount == 1);
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        REQUIRE(reporting.Take(maximum, 2, 120, 10, cause).has_value());
        const auto saturated = reporting.Take(maximum, 3, 130, 10, cause);
        REQUIRE(saturated.has_value());
        CHECK(saturated->missingFrames == 0);
        CHECK(saturated->callbackCount == 1);
        CHECK_FALSE(reporting.Take(maximum, 3, 140, 10, cause).has_value());
    }

    TEST_CASE("Scheduler rejection is retained separately from budget rejection and coalesced starvation", "[unit][audio][streaming]") {
        JobSystemConfig configuration;
        configuration.workerCount = 1;
        configuration.priorityQueues[static_cast<std::size_t>(JobPriority::Normal)].capacity = 0;
        JobSystem jobs(configuration);
        PackageFixture fixture;
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        service->Pump();
        const auto snapshot = service->Snapshot(handle).Value();
        CHECK(snapshot.starvationCause == AudioStreamStarvationCause::SchedulerRejected);
        CHECK(snapshot.fillSubmissionRejections == 1);
        CHECK(service->Snapshot().budgetRejections == 0);
        CallbackBlock output;
        CHECK(output.Render(port, 2).silentFrames == 2);
        const auto first = service->TakeUnderrunReport(handle, 100, 20).Value();
        REQUIRE(first.has_value());
        CHECK(first->cause == AudioStreamStarvationCause::SchedulerRejected);
        CHECK(first->missingFrames == 2);
        CHECK(output.Render(port, 2).silentFrames == 2);
        CHECK_FALSE(service->TakeUnderrunReport(handle, 110, 20).Value().has_value());
        const auto due = service->TakeUnderrunReport(handle, 120, 20).Value();
        REQUIRE(due.has_value());
        CHECK(due->missingFrames == 2);
        CHECK(due->callbackCount == 1);
        REQUIRE(service->Retire(handle).HasValue());
    }

    TEST_CASE("Worker seeks and half-open loops match exact PCM reference across ring wraps", "[unit][audio][streaming][gapless]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        auto service = Service(jobs, fixture);
        auto request = Request();
        request.decoder.seekable = true;
        const auto handle = service->Admit(request).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        REQUIRE(PumpUntil(*service, handle, 4));
        REQUIRE(port.RequestPosition(2, {true, 2, 5}));
        CallbackBlock block;
        CHECK(block.Render(port, 2).silentFrames == 2);
        CHECK(service->Snapshot(handle).Value().underrunCallbacks == 0);
        for (std::uint32_t index = 0; index < 12; ++index) {
            REQUIRE(PumpUntil(*service, handle, 1));
            CHECK(block.Render(port, 1).availableFrames == 1);
            const auto expected = static_cast<float>(3 + index % 3);
            CHECK(block.left[0] == expected);
            CHECK(block.right[0] == expected);
        }
        REQUIRE(port.RequestPosition(7));
        REQUIRE(PumpUntil(*service, handle, 1));
        const auto terminal = block.Render(port, 2);
        CHECK(terminal.availableFrames == 1);
        CHECK(terminal.ended);
        CHECK(block.left[0] == 8.0F);
        CHECK(block.left[1] == 0.0F);
        CHECK(service->Snapshot(handle).Value().underrunCallbacks == 0);
        REQUIRE(service->Retire(handle).HasValue());
    }

    TEST_CASE("Streaming telemetry separates reservation rejection from callback starvation", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        auto service = Service(jobs, fixture, {1, 1, 2'048});
        const auto handle = service->Admit(Request()).Value();
        CHECK(service->Snapshot().activeStreams == 1);
        CHECK(service->Snapshot().reservedBytes == 1'088);
        CHECK(service->Admit(Request()).HasError());
        CHECK(service->Snapshot().budgetRejections == 1);
        auto invalid = Request();
        invalid.lookaheadFrames = 0;
        CHECK(service->Admit(invalid).HasError());
        CHECK(service->Snapshot().invalidRequests == 1);
        auto port = std::move(service->RenderPort(handle)).Value();
        CallbackBlock block;
        CHECK(block.Render(port, 2).silentFrames == 2);
        const auto starved = service->Snapshot(handle).Value();
        CHECK(starved.starvationCause == AudioStreamStarvationCause::AwaitingPump);
        CHECK(starved.underrunCallbacks == 1);
        CHECK(starved.reservedBytes == service->Snapshot().reservedBytes);
        REQUIRE(PumpUntil(*service, handle, 4));
        const auto filled = service->Snapshot(handle).Value();
        CHECK(filled.capacityFrames == 4);
        CHECK(filled.decodeNanoseconds > 0);
        CHECK(filled.starvationCause == AudioStreamStarvationCause::None);
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(service->Snapshot().reservedBytes == 0);
        CHECK(service->Snapshot().budgetRejections == 1);
    }

    TEST_CASE("Stream replacement preserves active old media through prepare failure and publication", "[unit][audio][streaming][reload]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture old;
        auto service = Service(jobs, old);
        const auto previous = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(previous)).Value();
        REQUIRE(PumpUntil(*service, previous, 4));
        auto generation = std::make_shared<PackageFixture>();
        AudioStreamPackageSource binding{BorrowedCallbackContext{generation.get()}, &Open, generation};
        auto request = Request();
        SECTION("Incompatible layout fails before admission") {
            request.decoder.outputFormat.sampleRate = 44'100;
            CHECK(service->StageReplacement(previous, binding, request).HasError());
            CHECK(service->Snapshot().activeStreams == 1);
        }
        SECTION("Replacement budget exhaustion preserves the previous reservation") {
            request.maximumPackageBytes = 2U << 20U;
            const auto reserved = service->Snapshot().reservedBytes;
            CHECK(service->StageReplacement(previous, binding, request).HasError());
            CHECK(service->Snapshot().reservedBytes == reserved);
            CHECK(service->Snapshot().budgetRejections == 1);
        }
        SECTION("Invalid replacement retains the old ring and lease") {
            generation->openFailure = OpenFailure::TypedFailure;
            const auto candidate = service->StageReplacement(previous, binding, request).Value();
            REQUIRE(PumpUntilFailure(*service, candidate));
            CHECK(service->CommitReplacement(previous, candidate).HasError());
            CHECK(service->RenderPort(candidate).HasError());
            REQUIRE(service->Retire(candidate).HasValue());
        }
        SECTION("Candidate cancellation leaves the old generation playable") {
            const auto candidate = service->StageReplacement(previous, binding, request).Value();
            REQUIRE(service->Retire(candidate).HasValue());
        }
        SECTION("Prefetched replacement publishes a new port without retargeting the old one") {
            const auto candidate = service->StageReplacement(previous, binding, request).Value();
            CHECK(service->RenderPort(candidate).HasError());
            CHECK(service->CommitReplacement(previous, candidate).HasError());
            REQUIRE(PumpUntil(*service, candidate, 4));
            REQUIRE(service->CommitReplacement(previous, candidate).HasValue());
            auto replacement = std::move(service->RenderPort(candidate)).Value();
            CallbackBlock output;
            CHECK(output.Render(replacement, 2).availableFrames == 2);
            CHECK(old.releases.load() == 0);
            REQUIRE(service->Retire(candidate).HasValue());
        }
        CallbackBlock output;
        CHECK(output.Render(port, 2).availableFrames == 2);
        CHECK(output.left[0] == 1.0F);
        CHECK_FALSE(service->Snapshot(previous).Value().stopped);
        CHECK(old.releases.load() == 0);
        REQUIRE(service->Retire(previous).HasValue());
        CHECK(old.releases.load() == 1);
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
        // The open counter advances before the fixture records the selected asset.
        // Wait for that identity, then assert priority rather than waiting for the expected ID.
        REQUIRE(Until([&] {
            return fixture.firstOpenedAsset.load() != 0;
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

    TEST_CASE("Streaming decoder failure retains its typed reason and callback stays silent", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        fixture.decodeFailure = true;
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        REQUIRE(PumpUntilFailure(*service, handle));
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

    TEST_CASE("Streaming source mismatch fails on worker without accessing foreign state", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        CHECK(AudioStreamingService::Create(jobs, {{}, &Open}).HasError());
        std::uint32_t foreignState{37};
        auto created = AudioStreamingService::Create(jobs, {BorrowedCallbackContext{&foreignState}, &Open});
        REQUIRE(created.HasValue());
        auto service = std::move(created).Value();
        const auto handle = service->Admit(Request()).Value();
        REQUIRE(PumpUntilFailure(*service, handle));
        const auto failure = service->Snapshot(handle).Value().failure;
        REQUIRE(failure.has_value());
        CHECK(failure->code.Value() == AudioErrors::StreamReadFailed.code.Value());
        CHECK_FALSE(failure->cause);
        CHECK(foreignState == 37);
        REQUIRE(service->Retire(handle).HasValue());
    }

    TEST_CASE("Streaming preparation allocation failure accepts no stream or reservation", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        const auto creation = [&] {
            Tests::AllocationProbe::ScopedFailure failure;
            return AudioStreamingService::Create(jobs, {BorrowedCallbackContext{&fixture}, &Open});
        }();
        CHECK(creation.HasError());

        std::size_t requests{};
        {
            auto service = Service(jobs, fixture, {1, 1, 1'088});
            const auto request = Request();
            AudioStreamHandle handle;
            {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                const auto measured = service->Admit(request);
                requests = measurement.Snapshot().requests;
                REQUIRE(measured.HasValue());
                handle = measured.Value();
            }
            REQUIRE(service->Retire(handle).HasValue());
        }
        REQUIRE(requests > 0);

        bool admitted{};
        std::size_t failures{};
        for (std::size_t successfulAllocations{}; successfulAllocations <= requests && !admitted; ++successfulAllocations) {
            // One stream and exactly one request's byte charge expose any leaked slot or reservation.
            auto service = Service(jobs, fixture, {1, 1, 1'088});
            auto request = Request();
            const auto original = request;
            const auto result = [&] {
                Tests::AllocationProbe::ScopedFailure failure{successfulAllocations};
                return service->Admit(std::move(request));
            }();
            CHECK(request.asset == original.asset);
            CHECK(request.decoder.codec == original.decoder.codec);
            CHECK(request.decoder.outputFormat == original.decoder.outputFormat);
            CHECK(request.decoder.frameCount == original.decoder.frameCount);
            CHECK(request.decoder.maximumFramesPerDecode == original.decoder.maximumFramesPerDecode);
            CHECK(request.decoder.requiredWorkingBytes == original.decoder.requiredWorkingBytes);
            CHECK(request.decoder.seekable == original.decoder.seekable);
            CHECK(request.ringFrames == original.ringFrames);
            CHECK(request.lookaheadFrames == original.lookaheadFrames);
            CHECK(request.maximumPackageBytes == original.maximumPackageBytes);
            CHECK(request.priority == original.priority);
            CHECK(request.underrunPolicy == original.underrunPolicy);
            if (result.HasError()) {
                CHECK(result.ErrorValue().code.Value() == AudioErrors::StreamCapacityExceeded.code.Value());
                ++failures;
                const auto retry = service->Admit(request);
                REQUIRE(retry.HasValue());
                CHECK(retry.Value().slot == 1);
                CHECK(retry.Value().generation == 1);
                REQUIRE(service->Retire(retry.Value()).HasValue());
            } else {
                admitted = true;
                CHECK(result.Value().slot == 1);
                CHECK(result.Value().generation == 1);
                REQUIRE(service->Retire(result.Value()).HasValue());
            }
            CHECK(fixture.opens.load() == 0);
        }
        CHECK(failures == requests);
        CHECK(admitted);
    }

    TEST_CASE("Streaming descriptor and storage allocation failures stay inside typed admission", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        auto service = Service(jobs, fixture);
        const auto request = Request();
        for (std::size_t allowed = 0; allowed < 7; ++allowed) {
            const auto admitted = [&] {
                Tests::AllocationProbe::ScopedFailure failure{allowed};
                return service->Admit(request);
            }();
            if (admitted.HasValue())
                REQUIRE(service->Retire(admitted.Value()).HasValue());
            else
                CHECK(admitted.ErrorValue().code.Value() == AudioErrors::StreamCapacityExceeded.code.Value());
            CHECK(service->Snapshot().activeStreams == 0);
            CHECK(service->Snapshot().reservedBytes == 0);
            CHECK(request.decoder.outputFormat.layout.orderedChannels.size() == 2);
        }
        CHECK(fixture.opens.load() == 0);
    }

    TEST_CASE("Streaming source transfer retains its lease through work and retirement", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        AudioStreamPackageSource source{BorrowedCallbackContext{&fixture}, &Open, std::make_shared<int>(1)};
        const std::weak_ptr<const void> lease = source.ownerLease;
        auto created = AudioStreamingService::Create(jobs, std::move(source));
        REQUIRE(created.HasValue());
        CHECK_FALSE(source.ownerLease);
        auto service = std::move(created).Value();
        const auto handle = service->Admit(Request()).Value();
        REQUIRE(PumpUntil(*service, handle, 4));
        CHECK_FALSE(lease.expired());
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.releases.load() == 1);
        CHECK_FALSE(lease.expired());
        service.reset();
        CHECK(lease.expired());
    }

    TEST_CASE("Streaming provider failures preserve typed errors and worker causes without callback work", "[unit][audio][streaming]") {
        JobSystem jobs({.workerCount = 1});
        PackageFixture fixture;
        SECTION("Standard provider exception") {
            fixture.openFailure = OpenFailure::StandardException;
        }
        SECTION("Unknown provider exception") {
            fixture.openFailure = OpenFailure::UnknownException;
        }
        SECTION("Returned typed provider error") {
            fixture.openFailure = OpenFailure::TypedFailure;
        }
        auto service = Service(jobs, fixture);
        const auto handle = service->Admit(Request()).Value();
        auto port = std::move(service->RenderPort(handle)).Value();
        REQUIRE(PumpUntilFailure(*service, handle));
        const auto failure = service->Snapshot(handle).Value().failure;
        REQUIRE(failure.has_value());
        if (fixture.openFailure == OpenFailure::TypedFailure) {
            CHECK(failure->domain.Value() == AudioErrors::CookPayloadInvalid.domain.Value());
            CHECK(failure->code.Value() == AudioErrors::CookPayloadInvalid.code.Value());
            CHECK(failure->cause.Get() == nullptr);
        } else {
            CHECK(failure->domain.Value() == AudioErrors::StreamReadFailed.domain.Value());
            CHECK(failure->code.Value() == AudioErrors::StreamReadFailed.code.Value());
            const auto *cause = failure->cause.Get();
            REQUIRE(cause != nullptr);
            CHECK(cause->message == (fixture.openFailure == OpenFailure::StandardException ? "Injected package open failure"
                                                                                           : "Job callback threw an unknown exception."));
        }
        CallbackBlock output;
        const auto before = Tests::AllocationProbe::Count();
        const auto rendered = output.Render(port, 4);
        CHECK(Tests::AllocationProbe::Count() == before);
        CHECK(rendered.silentFrames == 4);
        CHECK(output.left[0] == 0.0F);
        CHECK(output.right[3] == 0.0F);
        REQUIRE(service->Retire(handle).HasValue());
        CHECK(fixture.opens.load() == 0);
        CHECK(fixture.releases.load() == 0);
    }
}  // namespace Horo::Audio::StreamingTests
