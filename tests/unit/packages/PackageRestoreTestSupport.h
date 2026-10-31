#pragma once
/** @file PackageRestoreTestSupport.h @brief Shared existing restore fixtures, retaining the actual cache, jobs and verifier authorities. */
#include "Horo/Packages/PackageRestore.h"
#include "Horo/Packages/PackageRestoreErrors.h"
#include "PackageArchiveTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string_view>
#include <thread>
#include <utility>

namespace Horo::Tests::PackageRestore {
    [[nodiscard]] inline Horo::Sha256Digest Digest(const std::string_view value) {
        return Horo::ComputeSha256(std::as_bytes(std::span{value}));
    }

    using Horo::CancellationToken;
    using Horo::MakeError;
    using namespace Horo::Packages;
    using Horo::Result;
    using Horo::Tests::Packages::TemporaryDirectory;

    [[nodiscard]] inline Horo::Packages::ValidatedPackageArchive VerifiedArchive(const std::vector<std::byte> &bytes) {
        auto result = ValidatedPackageArchive::Verify(bytes);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    [[nodiscard]] inline HoroPackageId PackageId(const std::string_view value) {
        auto result = HoroPackageId::Parse(value);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    [[nodiscard]] inline HoroPackageSourceId SourceId(const std::string_view value) {
        auto result = HoroPackageSourceId::Parse(value);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    [[nodiscard]] inline PackageVersion Version(const std::string_view value) {
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
            return Result<PackageRestoreArtifact>::Success(PackageRestoreArtifact{bytes_, signature});
        }

        std::atomic_uint64_t calls{0};
        std::optional<Horo::Security::DetachedSignatureEnvelope> signature;

    private:
        std::vector<std::byte> bytes_;
    };

    inline PackageCacheStore CreateCache(Horo::DurableFileSystem &files, const std::filesystem::path &path) {
        auto result = PackageCacheStore::Create(files, path);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    struct ServiceFixture final {
        explicit ServiceFixture(std::vector<std::byte> sourceBytes, PackagePublisherVerificationService *publisherVerification = nullptr)
            : cache(CreateCache(files, temporary.Path() / "cache")), jobs({.workerCount = 2, .maxQueuedJobs = 8}),
              source(std::move(sourceBytes)) {
            auto created = PackageRestoreService::Create(jobs, files, cache, &source, publisherVerification);
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

    inline PackageRestoreProgressSnapshot WaitForTerminal(PackageRestoreService &service, const PackageRestoreOperationId operation) {
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

    inline PackageRestoreProgressSnapshot StartAndWait(ServiceFixture &fixture, PackageRestoreRequest request) {
        auto started = fixture.service->Start(std::move(request));
        REQUIRE(started.HasValue());
        return WaitForTerminal(*fixture.service, started.Value().Id());
    }

    inline PackageRestoreProgressSnapshot RestoreReady(ServiceFixture &fixture, PackageRestoreRequest request) {
        auto snapshot = StartAndWait(fixture, std::move(request));
        REQUIRE(snapshot.outcome == PackageRestoreOutcome::Ready);
        return snapshot;
    }

}  // namespace Horo::Tests::PackageRestore
