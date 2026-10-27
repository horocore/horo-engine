#include "PackageRestoreTestSupport.h"

namespace {
    using namespace Horo::Tests::PackageRestore;

    TEST_CASE("Package restore publishes one verified graph and reports cache progress", "[packages][restore]") {
        RestoreFixture fixture;
        ServiceFixture service(fixture.bytes);
        const auto snapshot = RestoreReady(service, fixture.Request());
        CHECK(snapshot.progress == 1.0F);
        CHECK(snapshot.totalPackages == 1U);
        CHECK(snapshot.completedPackages == 1U);
        CHECK(snapshot.downloadedPackages == 1U);
        CHECK(snapshot.cacheHits == 0U);
        CHECK(service.source.calls == 1U);

        const auto graph = service.service->ActiveGraph();
        REQUIRE(graph);
        REQUIRE(graph->packages.size() == 1U);
        CHECK(graph->requestHash == fixture.requestHash);
        CHECK(graph->packages.front().lock.package == fixture.package);
        CHECK(graph->packages.front().archive->Digest() == fixture.archive.Digest());
        CHECK_FALSE(graph->packages.front().cacheHit);
        CHECK_FALSE(graph->packages.front().publisher.has_value());
    }

    TEST_CASE("Package restore fails closed when release requires unavailable publisher evidence", "[packages][restore][release]") {
        RestoreFixture fixture;
        ServiceFixture service(fixture.bytes);
        auto request = fixture.Request();
        request.requirePublisherVerification = true;

        const auto snapshot = StartAndWait(service, std::move(request));
        REQUIRE(snapshot.outcome == PackageRestoreOutcome::Failed);
        REQUIRE(snapshot.diagnostic.has_value());
        CHECK(snapshot.diagnostic->code.Value() == PackageRestoreErrors::InvalidInput.code.Value());
        CHECK_FALSE(service.service->ActiveGraph());
    }

    TEST_CASE("Release restore rejects unsigned publisher policy even when normal restore permits it", "[packages][restore][release]") {
        RestoreFixture fixture;
        PackagePublisherVerificationPolicy policy;
        policy.allowUnsigned = true;
        auto created = PackagePublisherVerificationService::Create(std::move(policy), nullptr);
        REQUIRE(created.HasValue());
        auto verifier = std::move(created).Value();
        ServiceFixture service(fixture.bytes, &verifier);

        auto request = fixture.Request();
        request.requirePublisherVerification = true;
        const auto snapshot = StartAndWait(service, std::move(request));
        REQUIRE(snapshot.outcome == PackageRestoreOutcome::Failed);
        REQUIRE(snapshot.diagnostic.has_value());
        CHECK(snapshot.diagnostic->code.Value() == PackageRestoreErrors::PublisherRejected.code.Value());
        CHECK_FALSE(service.service->ActiveGraph());
    }

    TEST_CASE("Package restore retains publisher evidence in its immutable graph", "[packages][restore][publisher]") {
        RestoreFixture fixture;
        PackagePublisherVerificationPolicy policy;
        policy.allowUnsigned = true;
        auto created = PackagePublisherVerificationService::Create(std::move(policy), nullptr);
        REQUIRE(created.HasValue());
        auto verifier = std::move(created).Value();
        ServiceFixture service(fixture.bytes, &verifier);

        RestoreReady(service, fixture.Request());
        const auto graph = service.service->ActiveGraph();
        REQUIRE(graph);
        REQUIRE(graph->packages.size() == 1U);
        REQUIRE(graph->packages.front().publisher.has_value());
        CHECK(graph->packages.front().publisher->outcome == PackagePublisherVerificationOutcome::Unsigned);
        CHECK(graph->packages.front().publisher->artifactDigest == fixture.archive.Digest());

        auto releaseRequest = fixture.Request();
        releaseRequest.requirePublisherVerification = true;
        const auto release = StartAndWait(service, std::move(releaseRequest));
        REQUIRE(release.outcome == PackageRestoreOutcome::Failed);
        REQUIRE(release.diagnostic.has_value());
        CHECK(release.diagnostic->code.Value() == PackageRestoreErrors::PublisherRejected.code.Value());
        CHECK(service.source.calls == 2U);
    }

    TEST_CASE("Package restore is offline-aware and reuses only the verified cache", "[packages][restore][offline]") {
        RestoreFixture fixture;
        ServiceFixture service(fixture.bytes);
        RestoreReady(service, fixture.Request());

        const auto snapshot = RestoreReady(service, fixture.Request(PackageRestoreMode::Offline));
        CHECK(snapshot.cacheHits == 1U);
        CHECK(snapshot.downloadedPackages == 0U);
        CHECK(service.source.calls == 1U);
    }

    TEST_CASE("Package restore respects the cross-process project admission lock", "[packages][restore][concurrency]") {
        RestoreFixture fixture;
        ServiceFixture service(fixture.bytes);
        const auto projectRoot = service.temporary.Path() / "project";
        REQUIRE(std::filesystem::create_directories(projectRoot));
        auto held = service.files.TryAcquireExclusive(projectRoot / ".horo/local/package-restore.lock", "test-owner");
        REQUIRE(held.HasValue());

        auto request = fixture.Request();
        request.projectRoot = projectRoot;
        const auto snapshot = StartAndWait(service, std::move(request));
        REQUIRE(snapshot.outcome == PackageRestoreOutcome::Failed);
        CHECK(snapshot.diagnostic->code.Value() == PackageRestoreErrors::Busy.code.Value());
        CHECK(service.source.calls == 0U);
        CHECK_FALSE(service.service->ActiveGraph());
    }

    TEST_CASE("Package restore exposes lifecycle state and closes admission", "[packages][restore][lifecycle]") {
        RestoreFixture fixture;
        ServiceFixture service(fixture.bytes);

        const auto initial = service.service->State();
        CHECK(initial.lifecycle == PackageRestoreLifecycleState::Ready);
        CHECK(initial.attempts == 0U);
        CHECK_FALSE(initial.activeOperation.has_value());
        CHECK_FALSE(initial.hasActiveGraph);

        const PackageRestoreOperationId unknown{9999U};
        CHECK_FALSE(service.service->Query(unknown).has_value());
        auto unknownCancel = service.service->RequestCancel(unknown);
        REQUIRE(unknownCancel.HasError());
        CHECK(unknownCancel.ErrorValue().code.Value() == PackageRestoreErrors::InvalidInput.code.Value());

        auto started = service.service->Start(fixture.Request());
        REQUIRE(started.HasValue());
        const auto running = service.service->State();
        CHECK(running.lifecycle == PackageRestoreLifecycleState::Running);
        REQUIRE(running.activeOperation.has_value());
        CHECK(running.activeOperation.value() == started.Value().Id());
        CHECK(service.service->RequestCancel(started.Value().Id()).HasValue());

        REQUIRE(service.service->Shutdown().HasValue());
        const auto closed = service.service->State();
        CHECK(closed.lifecycle == PackageRestoreLifecycleState::Closed);
        CHECK_FALSE(closed.activeOperation.has_value());
        auto closedCancel = service.service->RequestCancel(started.Value().Id());
        REQUIRE(closedCancel.HasError());
        CHECK(closedCancel.ErrorValue().code.Value() == PackageRestoreErrors::LifecycleClosed.code.Value());
        auto closedStart = service.service->Start(fixture.Request());
        REQUIRE(closedStart.HasError());
        CHECK(closedStart.ErrorValue().code.Value() == PackageRestoreErrors::LifecycleClosed.code.Value());
        CHECK(service.service->Shutdown().HasValue());
    }

    TEST_CASE("Package restore rejects invalid policy and request bounds", "[packages][restore][validation]") {
        RestoreFixture fixture;
        ServiceFixture service(fixture.bytes);

        auto invalidLimits = PackageRestoreLimits{};
        invalidLimits.validation.archiveBytes = 0U;
        auto invalidService =
            PackageRestoreService::Create(service.jobs, service.files, service.cache, &service.source, nullptr, invalidLimits);
        REQUIRE(invalidService.HasError());
        CHECK(invalidService.ErrorValue().code.Value() == PackageRestoreErrors::InvalidInput.code.Value());

        auto invalidRequest = fixture.Request();
        invalidRequest.platform.operatingSystem = "Linux";
        auto started = service.service->Start(invalidRequest);
        REQUIRE(started.HasError());
        CHECK(started.ErrorValue().code.Value() == PackageRestoreErrors::InvalidInput.code.Value());
    }

    TEST_CASE("Package restore leaves the previous graph untouched on stale or offline failure", "[packages][restore][atomic]") {
        RestoreFixture fixture;
        ServiceFixture service(fixture.bytes);
        RestoreReady(service, fixture.Request());
        const auto previous = service.service->ActiveGraph();
        REQUIRE(previous);

        auto staleRequest = fixture.Request(PackageRestoreMode::Offline);
        staleRequest.expectedRequestHash = Digest("changed-request");
        const auto staleSnapshot = StartAndWait(service, std::move(staleRequest));
        REQUIRE(staleSnapshot.outcome == PackageRestoreOutcome::Failed);
        CHECK(staleSnapshot.diagnostic->code.Value() == "packages.lockfile.stale");
        CHECK(service.service->ActiveGraph() == previous);

        REQUIRE(service.cache.Remove(fixture.archive.Digest()).HasValue());
        const auto offlineSnapshot = StartAndWait(service, fixture.Request(PackageRestoreMode::Offline));
        REQUIRE(offlineSnapshot.outcome == PackageRestoreOutcome::Failed);
        CHECK(offlineSnapshot.diagnostic->code.Value() == PackageRestoreErrors::OfflineArtifactUnavailable.code.Value());
        CHECK(service.service->ActiveGraph() == previous);
    }

    TEST_CASE("Package restore quarantines an artifact whose bytes do not match the lock", "[packages][restore][quarantine]") {
        RestoreFixture fixture;
        auto wrongBytes = fixture.bytes;
        wrongBytes.front() ^= std::byte{1};
        ServiceFixture service(std::move(wrongBytes));
        const auto snapshot = StartAndWait(service, fixture.Request());
        REQUIRE(snapshot.outcome == PackageRestoreOutcome::Failed);
        CHECK(snapshot.diagnostic->code.Value() == PackageRestoreErrors::ArtifactHashMismatch.code.Value());
        CHECK_FALSE(service.service->ActiveGraph());
        CHECK(std::filesystem::exists(service.temporary.Path() / "cache" / "quarantine" / "hash-mismatch"));
    }

    TEST_CASE("Package restore cancellation is terminal and retryable", "[packages][restore][cancellation]") {
        RestoreFixture fixture;
        ServiceFixture service(fixture.bytes);
        Horo::CancellationSource cancellation;
        cancellation.RequestCancellation();
        auto request = fixture.Request();
        request.cancellation = cancellation.Token();
        auto started = service.service->Start(std::move(request));
        REQUIRE(started.HasValue());
        const auto cancelled = WaitForTerminal(*service.service, started.Value().Id());
        REQUIRE(cancelled.outcome == PackageRestoreOutcome::Cancelled);
        CHECK(cancelled.diagnostic->code.Value() == PackageRestoreErrors::Cancelled.code.Value());
        CHECK_FALSE(service.service->ActiveGraph());

        CHECK(RestoreReady(service, fixture.Request()).outcome == PackageRestoreOutcome::Ready);
    }
}  // namespace
