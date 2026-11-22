#include "Horo/Application/ProjectAssetRegistrySource.h"
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "PackageApplicationServiceTestSupport.h"
#include "PackageProjectHost.h"

#include <fstream>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Application::Internal;
    using namespace Horo::Tests::PackageApplicationService;

    static_assert(!std::is_default_constructible_v<PackageApplication::ConstructionKey>);
    static_assert(!std::is_aggregate_v<PackageApplication::ConstructionKey>);
    static_assert(!std::is_default_constructible_v<PackageProjectHost::ConstructionKey>);
    static_assert(!std::is_aggregate_v<PackageProjectHost::ConstructionKey>);
    static_assert(!std::is_default_constructible_v<PackageProjectSession::ConstructionKey>);
    static_assert(!std::is_aggregate_v<PackageProjectSession::ConstructionKey>);
    static_assert(!std::is_constructible_v<PackageApplication, std::nullptr_t>);
    static_assert(!std::is_constructible_v<PackageProjectHost, std::nullptr_t>);
    static_assert(!std::is_constructible_v<PackageProjectSession, std::nullptr_t>);

    class ManualClock final : public Clock {
    public:
        Duration now;

        Duration MonotonicNow() const override {
            return now;
        }
    };

    class MetadataFiles final : public DurableFileSystem {
    public:
        enum class LockWrite {
            Normal,
            FailBeforeReplace,
            FailAfterReplace
        };

        void SetLockWrite(LockWrite write) {
            write_ = write;
        }

        Result<std::optional<FileEntryInfo>> InspectEntry(const std::filesystem::path &path) const override {
            return native_.InspectEntry(path);
        }

        Result<bool> CreateDirectoryPath(const std::filesystem::path &path, DirectoryCreationMode mode) override {
            return native_.CreateDirectoryPath(path, mode);
        }

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
            return native_.TryAcquireExclusive(path, owner);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native_.AvailableBytes(path);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms permissions) override {
            return native_.WritePrivateDurable(path, bytes, created, permissions);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            return native_.WriteDurable(path, bytes);
        }

        Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native_.CopyDurable(source, destination);
        }

        Result<void> AtomicReplace(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native_.AtomicReplace(source, destination);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &source, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            const bool lock = destination.filename() == "packages.lock";
            if (lock && write_ == LockWrite::FailBeforeReplace)
                return Result<void>::Failure(Failure());
            auto replaced = native_.AtomicReplaceTracked(source, destination, receipt);
            if (replaced.HasValue() && lock && write_ == LockWrite::FailAfterReplace)
                return Result<void>::Failure(Failure());
            return replaced;
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native_.RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native_.SyncDirectory(path);
        }

    private:
        static Error Failure() {
            return Error{ErrorCode{"test.package.metadata_write_failed"}, ErrorDomainId{"test"}};
        }

        NativeDurableFileSystem native_;
        LockWrite write_{LockWrite::Normal};
    };

    struct HostFixture final {
        Horo::Tests::Packages::TemporaryDirectory project{"package host ü"}, content{"package host content"};
        NativeDurableFileSystem files;
        ManualClock clock;
        SystemWallClock wall;
        NoExecutionTrust trust;
        AssetGraph graph;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8, .ioWorkerCount = 1}};
        ControlledSource source{graph.bytes};
        std::unique_ptr<PackageProjectHost> host;

        HostFixture() {
            const auto root = std::filesystem::canonical(project.Path());
            std::filesystem::create_directories(root / ".horo");
            std::filesystem::create_directories(root / "Assets");
            const nlohmann::json intent{{"sources", {{"fixture.source", {{"kind", "public-registry"}, {"registry", "official"}}}}},
                                        {"dependencies",
                                         {{"com.horo.application-fixture", {{"source", "fixture.source"}, {"version", "1.0.0"}}}}}};
            auto request = Packages::ValidatedPackageRequest::Parse(intent.dump());
            REQUIRE(request.HasValue());
            Packages::PackageResolutionPlan plan{.packages = {{graph.package, graph.version, graph.source, graph.archive.Digest(), {}}}};
            Packages::PackageLockArtifact artifact{.package = graph.package,
                                                   .version = graph.version,
                                                   .source = graph.source,
                                                   .artifactDigest = graph.archive.Digest(),
                                                   .manifestDigest = graph.archive.PackageManifestDigest(),
                                                   .fileManifestDigest = graph.archive.Manifest().Digest(),
                                                   .packageFormatVersion = 1,
                                                   .platforms = {},
                                                   .contributions = {"assets"}};
            auto lock = Packages::ValidatedPackageLockfileV1::Generate(plan, std::span{&graph.package, 1U}, request.Value().Digest(),
                                                                       std::span{&artifact, 1U});
            REQUIRE(lock.HasValue());
            Write("packages.json", intent.dump());
            Write("packages.lock", lock.Value().SerializeCanonical());
            auto created = PackageProjectHost::Create({jobs, files, clock, wall, trust},
                                                      {.cacheRoot = std::filesystem::canonical(content.Path()) / "cache",
                                                       .platform = {"linux", "x64", "horo-sdk-2"},
                                                       .lifecycle = {.temporaryRoot = std::filesystem::canonical(content.Path()),
                                                                     .artifactGate = Horo::Tests::CreateAcceptingArtifactGate()},
                                                       .mode = Packages::PackageRestoreMode::Online},
                                                      Duration::FromMilliseconds(1000), &source);
            REQUIRE(created.HasValue());
            host = std::move(created).Value();
        }

        ~HostFixture() {
            source.Unblock();
            if (host)
                static_cast<void>(host->Close());
            jobs.Shutdown(ShutdownPolicy::Cancel);
            if (host && host->Close().HasError())
                std::terminate();
        }

        void Write(const char *name, const std::string &bytes) {
            std::ofstream output{project.Path() / ".horo" / name, std::ios::binary | std::ios::trunc};
            output << bytes;
            REQUIRE(output.good());
        }

        std::string Read(const char *name) const {
            std::ifstream input{project.Path() / ".horo" / name, std::ios::binary};
            REQUIRE(input.good());
            return {std::istreambuf_iterator<char>{input}, {}};
        }

        std::shared_ptr<const Assets::IAssetRegistrySource> AssetSource() const {
            auto native = Platform::CreateNativeProjectReadFiles(std::filesystem::canonical(project.Path()), "package-removal-test", 1);
            REQUIRE(native.HasValue());
            auto registry =
                CreateProjectAssetRegistrySource(std::move(native).Value(), {.projectIdentity = "package-removal-test",
                                                                             .projectGeneration = 1,
                                                                             .deadline = std::chrono::steady_clock::time_point::max()});
            REQUIRE(registry.HasValue());
            return std::move(registry).Value();
        }

        std::shared_ptr<PackageApplication> Acquire() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                auto acquired = host->Acquire(std::filesystem::canonical(project.Path()));
                if (acquired.HasValue())
                    return std::move(acquired).Value();
                REQUIRE(acquired.ErrorValue().code.Value() == "packages.host.preparing");
                std::this_thread::yield();
            }
            FAIL("Metadata preparation did not complete");
            return {};
        }
    };

    TEST_CASE("Package metadata removal recovers both sides of the lock commit point", "[packages][application][host]") {
        HostFixture fixture;
        MetadataFiles files;
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto journal = policy.projectRoot / ".horo/local/package-metadata.pending";
        const auto oldRequest = fixture.Read("packages.json") + "\n";
        fixture.Write("packages.json", oldRequest);
        const auto initial = PreparePackageProject(policy, files, {});
        REQUIRE(initial.HasValue());
        CHECK_FALSE(std::filesystem::exists(journal.parent_path()));
        const auto oldLock = fixture.Read("packages.lock");
        const auto assets = fixture.AssetSource();

        SECTION("Failed replacement restores the exact previous request and allows retry") {
            files.SetLockWrite(MetadataFiles::LockWrite::FailBeforeReplace);
            CHECK(PublishPackageProjectRemoval(policy, initial.Value(), fixture.graph.package, files, *assets).HasError());
            REQUIRE(std::filesystem::exists(journal));
            CHECK(fixture.Read("packages.json") != oldRequest);
            REQUIRE(PreparePackageProject(policy, files, {}).HasValue());
            CHECK(fixture.Read("packages.json") == oldRequest);
            CHECK(fixture.Read("packages.lock") == oldLock);
            CHECK_FALSE(std::filesystem::exists(journal));
            files.SetLockWrite(MetadataFiles::LockWrite::Normal);
            REQUIRE(PublishPackageProjectRemoval(policy, initial.Value(), fixture.graph.package, files, *assets).HasValue());
        }

        SECTION("Committed lock with an uncertain flush keeps the new pair") {
            files.SetLockWrite(MetadataFiles::LockWrite::FailAfterReplace);
            const auto committed = PublishPackageProjectRemoval(policy, initial.Value(), fixture.graph.package, files, *assets);
            REQUIRE(committed.HasValue());
            CHECK(committed.Value().recoveryDiagnostic.has_value());
            CHECK(std::filesystem::exists(journal));
        }

        const auto current = PreparePackageProject(policy, files, {});
        REQUIRE(current.HasValue());
        CHECK(current.Value().application.portableRequest->Dependencies().empty());
        CHECK(current.Value().application.lockedPlan->Packages().empty());
        CHECK_FALSE(std::filesystem::exists(journal));
    }

    TEST_CASE("Package metadata recovery leaves an independently edited request intact", "[packages][application][host]") {
        HostFixture fixture;
        MetadataFiles files;
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto before = PreparePackageProject(policy, files, {});
        const auto assets = fixture.AssetSource();
        REQUIRE(before.HasValue());
        const auto oldLock = fixture.Read("packages.lock");
        const auto oldRequest = fixture.Read("packages.json");
        fixture.Write("packages.json", oldRequest + "\n");
        REQUIRE(PublishPackageProjectRemoval(policy, before.Value(), fixture.graph.package, files, *assets).HasError());
        CHECK(fixture.Read("packages.json") == oldRequest + "\n");
        const auto approved = PreparePackageProject(policy, files, {});
        REQUIRE(approved.HasValue());
        files.SetLockWrite(MetadataFiles::LockWrite::FailBeforeReplace);
        REQUIRE(PublishPackageProjectRemoval(policy, approved.Value(), fixture.graph.package, files, *assets).HasError());
        const auto edited = fixture.Read("packages.json") + "\n";
        fixture.Write("packages.json", edited);
        const auto conflicted = PreparePackageProject(policy, files, {});
        REQUIRE(conflicted.HasError());
        CHECK(conflicted.ErrorValue().code.Value() == "packages.host.metadata_conflict");
        CHECK(fixture.Read("packages.json") == edited);
        CHECK(fixture.Read("packages.lock") == oldLock);
        CHECK(std::filesystem::exists(policy.projectRoot / ".horo/local/package-metadata.pending"));
    }

    TEST_CASE("Package removal reports every affected native asset importer before changing metadata", "[packages][application][host]") {
        HostFixture fixture;
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto expected = PreparePackageProject(policy, fixture.files, {});
        REQUIRE(expected.HasValue());
        const auto assets = fixture.AssetSource();
        const auto writeAsset = [&](std::string_view filename, std::string_view id, const nlohmann::json &importer) {
            const auto path = policy.projectRoot / "Assets" / filename;
            std::ofstream source{path};
            source << "{}";
            REQUIRE(source.good());
            auto sidecar = path;
            sidecar += ".horo";
            std::ofstream metadata{sidecar};
            metadata << nlohmann::json{{"schemaVersion", 1}, {"assetId", id}, {"assetType", "core.scene"}, {"importerPackageId", importer}};
            REQUIRE(metadata.good());
        };
        const std::string_view package = fixture.graph.package.Value();
        writeAsset("Z.horoasset", "10213243-5465-7687-98a9-bacbdcedfe0f", package);
        writeAsset("A.horoasset", "10213243-5465-7687-98a9-bacbdcedfe0e", package);
        writeAsset("Other.horoasset", "10213243-5465-7687-98a9-bacbdcedfe0d", "com.horo.other");

        const auto oldRequest = fixture.Read("packages.json");
        const auto oldLock = fixture.Read("packages.lock");
        const auto blocked = PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, *assets);
        REQUIRE(blocked.HasError());
        CHECK(blocked.ErrorValue().code.Value() == "packages.host.importer_in_use");
        const auto &diagnostics = blocked.ErrorValue().diagnostics;
        REQUIRE(diagnostics.size() == 2);
        CHECK(diagnostics[0].path == "Assets/A.horoasset");
        CHECK(diagnostics[1].path == "Assets/Z.horoasset");
        CHECK(diagnostics[0].message == package);
        CHECK(fixture.Read("packages.json") == oldRequest);
        CHECK(fixture.Read("packages.lock") == oldLock);
        CHECK_FALSE(std::filesystem::exists(policy.projectRoot / ".horo/local/package-metadata.pending"));

        writeAsset("Invalid.horoasset", "10213243-5465-7687-98a9-bacbdcedfe0c", 23);
        const auto incomplete = PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, *assets);
        REQUIRE(incomplete.HasError());
        CHECK(incomplete.ErrorValue().code.Value() == "packages.host.asset_inventory_incomplete");
        CHECK(fixture.Read("packages.json") == oldRequest);
        CHECK(fixture.Read("packages.lock") == oldLock);
    }

    TEST_CASE("Package project root rejects stale screen boundaries and retains closed capabilities safely",
              "[packages][application][host]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        const auto generation = fixture.host->Generation();
        CHECK(generation > 0);
        CHECK(fixture.host->PumpProject(fixture.content.Path(), Packages::PackageActivationBoundary::Quiescent).HasError());
        REQUIRE(fixture.host->Retire(std::filesystem::canonical(fixture.project.Path())).HasValue());
        CHECK(application->Restore().HasError());
        REQUIRE(fixture.host->Close().HasValue());
        fixture.host.reset();
        // The alias keeps concrete closed authorities alive, and no callback can touch the destroyed host.
        CHECK(application->Install().HasError());
        REQUIRE(application->Close().HasValue());
    }

    TEST_CASE("Package project metadata changes revoke old admission before activation", "[packages][application][host]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        const auto generation = fixture.host->Generation();
        fixture.Write("packages.json", "{}");
        fixture.clock.now = Duration::FromMilliseconds(1001);
        CHECK(application->Restore().HasError());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (fixture.host->Generation() == generation && std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(fixture.host->Pump(Packages::PackageActivationBoundary::Quiescent));
            std::this_thread::yield();
        }
        CHECK(fixture.host->Generation() != generation);
        CHECK(application->Activate({}).HasError());
        CHECK(fixture.host->Acquire(std::filesystem::canonical(fixture.project.Path())).HasError());
        CHECK(fixture.source.calls == 0);
    }

    TEST_CASE("Package root shutdown retains blocked I/O until cancellation joins before Close", "[packages][application][host]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        fixture.source.block = true;
        REQUIRE(application->Restore().HasValue());
        REQUIRE(fixture.source.entered.try_acquire_for(std::chrono::seconds{2}));
        REQUIRE(fixture.host->Close().HasError());
        CHECK(fixture.host->Generation() == 0);
        fixture.source.Unblock();
        fixture.jobs.Shutdown(ShutdownPolicy::Cancel);
        REQUIRE(fixture.host->Close().HasValue());
        CHECK(application->Restore().HasError());
    }

    TEST_CASE("Observing package execution trust does not bind or advance a project", "[packages][application][host]") {
        HostFixture fixture;
        const auto root = std::filesystem::canonical(fixture.project.Path());
        CHECK_FALSE(fixture.host->ActivationTrustRequired(root));
        CHECK(fixture.host->Generation() == 0);
        CHECK(fixture.source.calls.load() == 0);
        const auto application = fixture.Acquire();
        REQUIRE(application);
        const auto generation = fixture.host->Generation();
        REQUIRE(generation != 0);
        CHECK_FALSE(fixture.host->ActivationTrustRequired(root));
        CHECK_FALSE(fixture.host->ActivationTrustRequired(root / "a-different-project"));
        CHECK(fixture.host->Generation() == generation);
        CHECK(fixture.source.calls.load() == 0);
        CHECK_FALSE(application->Current().Value());
    }

    TEST_CASE("Package installed snapshot fences exact fresh owner generation without restoring", "[packages][application][host]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto generation = fixture.host->Generation();
        const auto empty = fixture.host->InstalledRecord(root, generation);
        REQUIRE(empty.HasValue());
        CHECK_FALSE(empty.Value());
        const auto plan = application->Plan();
        REQUIRE(plan.HasValue());
        REQUIRE(plan.Value()->packages.size() == 1);
        const auto &locked = plan.Value()->packages.front().locked;
        const auto removal = application->PreviewRemoval(locked.package);
        REQUIRE(removal.HasValue());
        CHECK(removal.Value().removed == std::vector<Packages::LockedPackageReference>{{locked.package, locked.version}});
        CHECK(fixture.source.calls == 0);
        CHECK(fixture.host->InstalledRecord(root, 0).HasError());
        CHECK(fixture.host->InstalledRecord(root, generation + 1).HasError());
        CHECK(fixture.host->InstalledRecord(fixture.content.Path(), generation).HasError());
        bool foreignRejected{};
        std::thread foreign{[&] {
            foreignRejected = fixture.host->InstalledRecord(root, generation).HasError();
        }};
        foreign.join();
        CHECK(foreignRejected);
        fixture.clock.now = Duration::FromMilliseconds(1001);
        CHECK(fixture.host->InstalledRecord(root, generation).HasError());
        CHECK(application->PreviewRemoval(locked.package).HasError());
        CHECK(fixture.source.calls == 0);
    }

    TEST_CASE("Package installed snapshot retains sealed bytes after exact project retirement", "[packages][application][host]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        const auto operation = application->Install();
        REQUIRE(operation.HasValue());
        bool installed{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            REQUIRE(fixture.host->Pump(Packages::PackageActivationBoundary::Quiescent).HasValue());
            const auto current = application->Query(operation.Value());
            REQUIRE(current.HasValue());
            if (current.Value().outcome != PackageOperationOutcome::Running) {
                REQUIRE(current.Value().outcome == PackageOperationOutcome::Succeeded);
                installed = true;
                break;
            }
            std::this_thread::yield();
        }
        REQUIRE(installed);
        const auto root = std::filesystem::canonical(fixture.project.Path());
        const auto generation = fixture.host->Generation();
        auto snapshot = fixture.host->InstalledRecord(root, generation);
        REQUIRE(snapshot.HasValue());
        REQUIRE(snapshot.Value());
        REQUIRE(snapshot.Value()->Revision() != 0);
        REQUIRE(fixture.host->Retire(root).HasValue());
        CHECK(fixture.host->InstalledRecord(root, generation).HasError());
        REQUIRE(snapshot.Value()->Graph()->packages.size() == 1);
        CHECK(snapshot.Value()->Graph()->packages.front().archive->Digest() == fixture.graph.archive.Digest());
    }
}  // namespace
