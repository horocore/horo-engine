#include "Horo/Packages/PackageRestore.h"
#include "Horo/Packages/PackageRestoreErrors.h"
#include "PackageArchiveTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string_view>
#include <thread>
#include <utility>

namespace {
    [[nodiscard]] Horo::Sha256Digest Digest(const std::string_view value) {
        return Horo::ComputeSha256(std::as_bytes(std::span{value}));
    }

    using Horo::CancellationToken;
    using Horo::MakeError;
    using namespace Horo::Packages;
    using Horo::Result;
    using Horo::Tests::Packages::TemporaryDirectory;

    [[nodiscard]] Horo::Packages::ValidatedPackageArchive VerifiedArchive(const std::vector<std::byte> &bytes) {
        auto result = ValidatedPackageArchive::Verify(bytes);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    [[nodiscard]] HoroPackageId PackageId(const std::string_view value) {
        auto result = HoroPackageId::Parse(value);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    [[nodiscard]] HoroPackageSourceId SourceId(const std::string_view value) {
        auto result = HoroPackageSourceId::Parse(value);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    [[nodiscard]] PackageVersion Version(const std::string_view value) {
        auto result = PackageVersion::Parse(value);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    struct RestoreFixture final {
        std::vector<std::byte> bytes{Horo::Tests::Packages::ValidPackageArchiveBytes()};
        ValidatedPackageArchive archive{VerifiedArchive(bytes)};
        HoroPackageId package{PackageId("com.horo.restore-fixture")};
        HoroPackageSourceId source{SourceId("fixture.source")};
        PackageVersion version{Version("1.0.0")};
        Horo::Sha256Digest requestHash{Digest("restore-request")};
        std::string lockfile;

        RestoreFixture() {
            PackageResolutionPlan plan{.packages = {{package, version, source, archive.Digest(), {}}}};
            PackageLockArtifact artifact{.package = package,
                                         .version = version,
                                         .source = source,
                                         .artifactDigest = archive.Digest(),
                                         .manifestDigest = archive.PackageManifestDigest(),
                                         .fileManifestDigest = archive.Manifest().Digest(),
                                         .packageFormatVersion = 1U,
                                         .platforms = {},
                                         .contributions = {"assets"}};
            auto generated = ValidatedPackageLockfileV1::Generate(plan, std::span{&package, 1U}, requestHash, std::span{&artifact, 1U});
            REQUIRE(generated.HasValue());
            lockfile = generated.Value().SerializeCanonical();
        }

        [[nodiscard]] PackageRestoreRequest Request(const PackageRestoreMode mode = PackageRestoreMode::Online) const {
            return {.lockfileJson = lockfile, .expectedRequestHash = requestHash, .platform = {"linux", "x64", "horo-sdk-2"}, .mode = mode};
        }
    };

    class FixtureSource final : public IPackageRestoreSource {
    public:
        explicit FixtureSource(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

        [[nodiscard]] Result<PackageRestoreArtifact> Fetch(const PackageRestoreArtifactRequest &,
                                                           const CancellationToken &cancellation) override {
            ++calls;
            if (cancellation.IsCancellationRequested())
                return Result<PackageRestoreArtifact>::Failure(MakeError(PackageRestoreErrors::Cancelled));
            return Result<PackageRestoreArtifact>::Success(PackageRestoreArtifact{bytes_, std::nullopt});
        }

        std::atomic_uint64_t calls{0};

    private:
        std::vector<std::byte> bytes_;
    };

    PackageCacheStore CreateCache(Horo::DurableFileSystem &files, const std::filesystem::path &path) {
        auto result = PackageCacheStore::Create(files, path);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    struct ServiceFixture final {
        explicit ServiceFixture(std::vector<std::byte> sourceBytes)
            : cache(CreateCache(files, temporary.Path() / "cache")), jobs({.workerCount = 2, .maxQueuedJobs = 8}),
              source(std::move(sourceBytes)) {
            auto created = PackageRestoreService::Create(jobs, files, cache, &source);
            REQUIRE(created.HasValue());
            service.emplace(std::move(created).Value());
        }

        TemporaryDirectory temporary;
        Horo::NativeDurableFileSystem files;
        PackageCacheStore cache;
        Horo::JobSystem jobs;
        FixtureSource source;
        std::optional<PackageRestoreService> service;
    };

    PackageRestoreProgressSnapshot WaitForTerminal(PackageRestoreService &service, const PackageRestoreOperationId operation) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            service.Pump();
            const auto snapshot = service.Query(operation);
            REQUIRE(snapshot.has_value());
            if (snapshot->outcome != PackageRestoreOutcome::Running)
                return *snapshot;
            std::this_thread::yield();
        }
        FAIL("Package restore did not reach a terminal state.");
        return {};
    }

    PackageRestoreProgressSnapshot StartAndWait(ServiceFixture &fixture, PackageRestoreRequest request) {
        auto started = fixture.service->Start(std::move(request));
        REQUIRE(started.HasValue());
        return WaitForTerminal(*fixture.service, started.Value().Id());
    }

    PackageRestoreProgressSnapshot RestoreReady(ServiceFixture &fixture, PackageRestoreRequest request) {
        auto snapshot = StartAndWait(fixture, std::move(request));
        REQUIRE(snapshot.outcome == PackageRestoreOutcome::Ready);
        return snapshot;
    }

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
