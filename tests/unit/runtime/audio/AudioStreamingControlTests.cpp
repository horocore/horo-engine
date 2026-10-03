#include "AllocationProbe.h"
#include "AudioStreamingTestFixture.h"

namespace Horo::Audio::StreamingTests {
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
