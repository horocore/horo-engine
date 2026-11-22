#pragma once

#include "Horo/Application/PackageApplication.h"
#include "Horo/Packages/PackageLifecycleErrors.h"
#include "Horo/Packages/PackageRestoreErrors.h"
#include "PackageArchiveTestSupport.h"
#include "SecurityTestSupport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <semaphore>
#include <thread>

namespace Horo::Tests::PackageApplicationService {
    using namespace Horo::Application;
    using namespace Horo::Packages;

    template <typename T> T Parse(const std::string_view value) {
        auto parsed = T::Parse(value);
        REQUIRE(parsed.HasValue());
        return std::move(parsed).Value();
    }

    inline ValidatedPackageArchive Verify(const std::vector<std::byte> &bytes) {
        auto verified = ValidatedPackageArchive::Verify(bytes);
        REQUIRE(verified.HasValue());
        return std::move(verified).Value();
    }

    /** @brief Verified declared image bytes for admission tests; denying trust must prevent the loader from reading them. */
    inline std::vector<std::byte> NativeAdmissionArchiveBytes(bool runtime = false) {
        constexpr auto root = "extensions/native";
#if defined(_WIN32)
        constexpr auto image = "module.dll";
#elif defined(__APPLE__)
        constexpr auto image = "module.dylib";
#else
        constexpr auto image = "module.so";
#endif
        constexpr auto id = "com.horo.application-fixture";
        constexpr auto module = "com.horo.application-fixture.native";
        const nlohmann::json descriptor{{"id", id},
                                        {"version", "1.0.0"},
                                        {"modules",
                                         {{{"id", module},
                                           {"version", "1.0.0"},
                                           {"kind", "native"},
                                           {"roles", {runtime ? "runtime-participant" : "backend-capability"}},
                                           {"entry", image}}}},
                                        {"contributions", {{{"type", "asset.importer"}, {"id", "fixture.importer"}, {"module", module}}}}};
        const std::array files{std::pair{std::string{"horo-package.toml"}, std::string{"schemaVersion = 1\n"}},
                               std::pair{std::string{root} + "/extension.json", descriptor.dump()},
                               std::pair{std::string{root} + '/' + image, std::string{"unloadable fixture image"}}};
        mz_zip_archive zip{};
        REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
        auto inventory = nlohmann::json::array();
        for (const auto &[path, bytes] : files) {
            auto entry = Tests::Packages::FileInventoryEntry(path, bytes);
            if (path.starts_with(root))
                entry["contributionRoot"] = root;
            inventory.push_back(std::move(entry));
            REQUIRE(mz_zip_writer_add_mem(&zip, path.c_str(), bytes.data(), bytes.size(), MZ_BEST_COMPRESSION));
        }
        const auto manifest = nlohmann::json{{"schemaVersion", 1}, {"files", inventory}}.dump();
        REQUIRE(mz_zip_writer_add_mem(&zip, "files.manifest.json", manifest.data(), manifest.size(), MZ_BEST_COMPRESSION));
        return Tests::Packages::FinalizeArchive(zip);
    }

    struct AssetGraph final {
        std::vector<std::byte> bytes;
        ValidatedPackageArchive archive{Verify(bytes)};
        HoroPackageId package{Parse<HoroPackageId>("com.horo.application-fixture")};
        HoroPackageSourceId source{Parse<HoroPackageSourceId>("fixture.source")};
        PackageVersion version{Parse<PackageVersion>("1.0.0")};
        Sha256Digest requestHash{ComputeSha256(std::as_bytes(std::span{std::string_view{"application-request"}}))};
        std::optional<ValidatedPackageLockfileV1> lock;

        explicit AssetGraph(bool native = false, bool runtime = false)
            : bytes(native ? NativeAdmissionArchiveBytes(runtime) : Tests::Packages::ValidPackageArchiveBytes()) {
            PackageResolutionPlan plan{.packages = {{package, version, source, archive.Digest(), {}}}};
            PackageLockArtifact artifact{.package = package,
                                         .version = version,
                                         .source = source,
                                         .artifactDigest = archive.Digest(),
                                         .manifestDigest = archive.PackageManifestDigest(),
                                         .fileManifestDigest = archive.Manifest().Digest(),
                                         .packageFormatVersion = 1U,
                                         .platforms = {},
                                         .contributions = {native ? "native" : "assets"}};
            auto generated = ValidatedPackageLockfileV1::Generate(plan, std::span{&package, 1U}, requestHash, std::span{&artifact, 1U});
            REQUIRE(generated.HasValue());
            lock.emplace(std::move(generated).Value());
        }
    };

    class ControlledSource final : public IPackageRestoreSource {
    public:
        explicit ControlledSource(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}

        Result<PackageRestoreArtifact> Fetch(const PackageRestoreArtifactRequest &, const CancellationToken &cancellation) override {
            ++calls;
            if (block.load()) {
                entered.release();
                resume_.acquire();
            }
            if (cancellation.IsCancellationRequested())
                return Result<PackageRestoreArtifact>::Failure(MakeError(PackageRestoreErrors::Cancelled));
            if (failFetch)
                return Result<PackageRestoreArtifact>::Failure(
                    Error{ErrorCode{"fixture.source.unavailable"}, ErrorDomainId{"fixture.source"}});
            return Result<PackageRestoreArtifact>::Success({bytes_, std::nullopt});
        }

        /** @brief Release test-owned I/O on every assertion/teardown path without over-releasing a binary semaphore. */
        void Unblock() noexcept {
            block.store(false);
            if (!released_.exchange(true))
                resume_.release();
        }

        std::atomic_uint64_t calls{};
        std::atomic_bool block{};
        std::binary_semaphore entered{0};
        bool failFetch{};

    private:
        std::vector<std::byte> bytes_;
        std::binary_semaphore resume_{0};
        std::atomic_bool released_{};
    };

    class NoExecutionTrust final : public IPackageExtensionTrust {
    public:
        Result<PackageExtensionTrustApproval> Approve(const PackageRestorePackage &, const Extensions::ExtensionManifest &) const override {
            ++calls;
            return Result<PackageExtensionTrustApproval>::Failure(MakeError(PackageLifecycleErrors::TrustRequired));
        }

        mutable unsigned calls{};
    };

    inline PackageCacheStore CreateCache(DurableFileSystem &files, const std::filesystem::path &root) {
        auto created = PackageCacheStore::Create(files, root);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    inline PackageInstallService CreateInstall(DurableFileSystem &files, const std::filesystem::path &root) {
        auto created = PackageInstallService::Create(files, root);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    struct Fixture final {
        Tests::Packages::TemporaryDirectory project{"horo application project ü"};
        Tests::Packages::TemporaryDirectory content{"horo application content ü"};
        NativeDurableFileSystem files;
        AssetGraph graph;
        PackageCacheStore cache;
        JobSystem jobs;
        ControlledSource source;
        NoExecutionTrust trust;
        PackageInstallService install;
        std::optional<PackageRestoreService> restore;
        std::unique_ptr<PackageLifecycleService> lifecycle;
        bool admissionAllowed{true};
        std::unique_ptr<PackageApplication> application;

        explicit Fixture(bool native = false, std::shared_ptr<IPackageModuleHost> moduleHost = {},
                         const IPackageExtensionTrust *executionTrust = nullptr)
            : graph(native), cache(CreateCache(files, content.Path() / "cache")),
              jobs({.workerCount = 1, .maxQueuedJobs = 8, .ioWorkerCount = 1}), source(graph.bytes),
              install(CreateInstall(files, std::filesystem::canonical(project.Path()))) {
            auto restored = PackageRestoreService::Create(jobs, files, cache, &source);
            REQUIRE(restored.HasValue());
            restore.emplace(std::move(restored).Value());
            PackageLifecycleConfiguration lifecyclePolicy{.temporaryRoot = std::filesystem::canonical(content.Path()),
                                                          .artifactGate = Tests::CreateAcceptingArtifactGate()};
            if (moduleHost) {
                lifecyclePolicy.moduleHosts.push_back(std::move(moduleHost));
                lifecyclePolicy.moduleRetirements = std::make_shared<PackageModuleRetirementQueue>(4);
            }
            auto activated = PackageLifecycleService::Create(install, executionTrust ? *executionTrust : trust, std::move(lifecyclePolicy));
            REQUIRE(activated.HasValue());
            lifecycle = std::move(activated).Value();
            auto created = PackageApplication::Create(jobs, *restore, install, *lifecycle, Configuration());
            REQUIRE(created.HasValue());
            application = std::move(created).Value();
        }

        Fixture(const Fixture &) = delete;
        Fixture &operator=(const Fixture &) = delete;

        ~Fixture() {
            source.Unblock();
            if (application) {
                if (application->Close().HasError()) {
                    jobs.Shutdown(ShutdownPolicy::Drain);
                    if (application->Close().HasError())
                        std::terminate();
                }
                application.reset();
            }
            if (restore)
                static_cast<void>(restore->Shutdown());
            static_cast<void>(lifecycle->Shutdown());
        }

        PackageApplicationConfiguration Configuration() {
            return {.restore = {.projectRoot = std::filesystem::canonical(project.Path()),
                                .lockfileJson = graph.lock->SerializeCanonical(),
                                .expectedRequestHash = graph.requestHash,
                                .platform = {"linux", "x64", "horo-sdk-2"}},
                    .lockedPlan = graph.lock,
                    .validateAdmission = [this] {
                if (!admissionAllowed)
                    return Result<void>::Failure(Error{ErrorCode{"fixture.metadata.stale"}, ErrorDomainId{"fixture.policy"}});
                return Result<void>::Success();
            }};
        }
    };

    inline PackageOperationSnapshot WaitForTerminal(Fixture &fixture, const PackageOperationId operation) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            REQUIRE(fixture.application->Pump(PackageActivationBoundary::Quiescent).HasValue());
            auto snapshot = fixture.application->Query(operation);
            REQUIRE(snapshot.HasValue());
            if (snapshot.Value().outcome != PackageOperationOutcome::Running)
                return std::move(snapshot).Value();
            std::this_thread::yield();
        }
        FAIL("The application package operation did not retire its owned work.");
        return {};
    }
}  // namespace Horo::Tests::PackageApplicationService
