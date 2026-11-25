#include "DecisionGraphCompilerTestSupport.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/ProjectAssetRegistrySource.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Packages/PackageRequest.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Scene/SceneSource.h"
#include "PackageApplicationServiceTestSupport.h"
#include "PackageBehaviorReferences.h"
#include "PackageProjectHost.h"
#include "PackageRestoreTestSupport.h"

#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <functional>
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

    /** @brief Runs an observation at the start of an otherwise native asset capture. */
    class ObservedAssetSource final : public Assets::IAssetRegistrySource {
    public:
        ObservedAssetSource(const Assets::IAssetRegistrySource &source, std::function<void()> observe)
            : source_(source), observe_(std::move(observe)) {}

        Result<Assets::AssetRegistrySourceManifest> Capture() const override {
            observe_();
            return source_.Capture();
        }

        Result<std::string> ReadSidecar(std::string_view path, std::chrono::steady_clock::time_point deadline) const override {
            return source_.ReadSidecar(path, deadline);
        }

    private:
        const Assets::IAssetRegistrySource &source_;
        std::function<void()> observe_;
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

        void BlockRecoveryRequestWrite(bool blocked) {
            blockRecoveryRequest_ = blocked;
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
            if (lock && write_ == LockWrite::FailBeforeReplace) {
                failedLockWrite_ = true;
                return Result<void>::Failure(Failure());
            }
            if (destination.filename() == "packages.json" && failedLockWrite_ && blockRecoveryRequest_)
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
        bool blockRecoveryRequest_{};
        bool failedLockWrite_{};
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
        std::shared_ptr<const Platform::IProjectReadFiles> projectReadFiles;
        std::optional<Packages::PackageInstallService> removalInstall;
        std::unique_ptr<Packages::PackageLifecycleService> removalLifecycle;
        std::shared_ptr<const Packages::PackageRemovalLease> removalLease;

        explicit HostFixture(bool runtime = false, NativeArchiveContributions contributions = NativeArchiveContributions::WithImporter)
            : graph(runtime, runtime, contributions) {
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
                                                   .contributions = {runtime ? "native" : "assets"}};
            auto lock = Packages::ValidatedPackageLockfileV1::Generate(plan, std::span{&graph.package, 1U}, request.Value().Digest(),
                                                                       std::span{&artifact, 1U});
            REQUIRE(lock.HasValue());
            Write("packages.json", intent.dump());
            Write("packages.lock", lock.Value().SerializeCanonical());
            auto created = PackageProjectHost::Create({jobs, files, clock, wall, trust,
                                                       {Platform::CreateNativeProjectReadFiles, CreateProjectAssetRegistrySource}},
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

        std::shared_ptr<const Assets::IAssetRegistrySource> AssetSource(std::vector<Packages::PackageRestorePackage> installed = {}) {
            const auto root = std::filesystem::canonical(project.Path());
            const auto lock = Packages::ValidatedPackageLockfileV1::Parse(Read("packages.lock"));
            REQUIRE(lock.HasValue());
            auto install = Packages::PackageInstallService::Create(files, root);
            REQUIRE(install.HasValue());
            const auto own = std::ranges::find(lock.Value().Packages(), graph.package, &Packages::LockedPackage::package);
            REQUIRE(own != lock.Value().Packages().end());
            installed.emplace_back(*own, std::make_shared<const Packages::ValidatedPackageArchive>(graph.archive));
            const auto verified = std::make_shared<const Packages::PackageRestoreGraph>(
                Packages::PackageRestoreGraph{.requestHash = lock.Value().RequestHash(),
                                              .platform = {"linux", "x64", "horo-sdk-2"},
                                              .packages = std::move(installed)});
            removalInstall.emplace(std::move(install).Value());
            REQUIRE(removalInstall->Install(verified, {}).HasValue());
            auto lifecycle = Packages::PackageLifecycleService::Create(
                *removalInstall, trust, {.temporaryRoot = std::filesystem::canonical(content.Path()),
                                        .artifactGate = Horo::Tests::CreateAcceptingArtifactGate()});
            REQUIRE(lifecycle.HasValue());
            removalLifecycle = std::move(lifecycle).Value();
            const auto request = Packages::ValidatedPackageRequest::Parse(Read("packages.json"));
            REQUIRE(request.HasValue());
            const auto plan = Packages::PackageRemovalPlan::Prepare(request.Value(), lock.Value(), graph.package);
            REQUIRE(plan.HasValue());
            auto reserved = removalLifecycle->ReserveRemoval(plan.Value().graph.removed);
            REQUIRE(reserved.HasValue());
            removalLease = std::move(reserved).Value();
            auto native = Platform::CreateNativeProjectReadFiles(root, "package-removal-test", 1);
            REQUIRE(native.HasValue());
            projectReadFiles = std::move(native).Value();
            auto registry = CreateProjectAssetRegistrySource(projectReadFiles, ReadContext());
            REQUIRE(registry.HasValue());
            return std::move(registry).Value();
        }

        static Platform::ProjectReadContext ReadContext() {
            return {.projectIdentity = "package-removal-test",
                    .projectGeneration = 1,
                    .deadline = std::chrono::steady_clock::time_point::max()};
        }

        PackageRemovalSources RemovalSources(const Assets::IAssetRegistrySource &assets) const {
            return {assets, *projectReadFiles, ReadContext(), {}, removalLease};
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

        PackageOperationSnapshot Wait(PackageApplication &application, const PackageOperationId operation) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (std::chrono::steady_clock::now() < deadline) {
                REQUIRE(host->Pump(Packages::PackageActivationBoundary::Quiescent).HasValue());
                auto current = application.Query(operation);
                REQUIRE(current.HasValue());
                if (current.Value().outcome != PackageOperationOutcome::Running)
                    return std::move(current).Value();
                std::this_thread::yield();
            }
            FAIL("Package operation did not finish");
            return {};
        }
    };

    TEST_CASE("Editor package removal rejects stale plans and saved importer references, then reloads the new lock",
              "[packages][application][host][removal]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        const auto beforeInstall = application->Plan().Value();
        const auto install = application->Install();
        REQUIRE(install.HasValue());
        REQUIRE(fixture.Wait(*application, install.Value()).outcome == PackageOperationOutcome::Succeeded);
        const auto preview = application->InspectRemoval(fixture.graph.package);
        REQUIRE(preview.HasValue());
        REQUIRE(preview.Value().supportsApply);
        CHECK(application->Remove(fixture.graph.package, beforeInstall).HasError());

        const auto plan = application->Plan().Value();
        const auto asset = fixture.project.Path() / "Assets" / "model.horoasset";
        auto sidecar = asset;
        sidecar += ".horo";
        std::ofstream{asset} << "{}";
        std::ofstream{sidecar} << nlohmann::json{{"schemaVersion", 1},
                                                 {"assetId", "174ee050-f0a1-4351-89c4-04a5895bff81"},
                                                 {"assetType", "core.scene"},
                                                 {"importerPackageId", fixture.graph.package.Value()}};
        const auto oldRequest = fixture.Read("packages.json");
        const auto oldLock = fixture.Read("packages.lock");
        const auto blocked = application->Remove(fixture.graph.package, plan);
        REQUIRE(blocked.HasValue());
        const auto rejection = fixture.Wait(*application, blocked.Value());
        REQUIRE(rejection.outcome == PackageOperationOutcome::Failed);
        REQUIRE(rejection.diagnostic);
        CHECK(rejection.diagnostic->code.Value() == "packages.host.importer_in_use");
        CHECK(fixture.Read("packages.lock") == oldLock);
        CHECK(fixture.Read("packages.json") == oldRequest);
        REQUIRE(std::filesystem::remove(sidecar));
        REQUIRE(std::filesystem::remove(asset));

        const auto generation = fixture.host->Generation();
        const auto removed = application->Remove(fixture.graph.package, plan);
        REQUIRE(removed.HasValue());
        REQUIRE(fixture.Wait(*application, removed.Value()).outcome == PackageOperationOutcome::Succeeded);
        CHECK(application->Install().HasError());
        CHECK(fixture.host->Generation() != generation);
        const auto locked = Packages::ValidatedPackageLockfileV1::Parse(fixture.Read("packages.lock"));
        REQUIRE(locked.HasValue());
        CHECK(locked.Value().Packages().empty());
        const auto refreshed = fixture.Acquire();
        CHECK(refreshed != application);
        CHECK(refreshed->Plan().Value()->packages.empty());
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto rollback = ReadPackageProjectRollback(policy, fixture.files, {});
        REQUIRE(rollback.HasValue());
        CHECK(rollback.Value().request == oldRequest);
        CHECK(rollback.Value().lock == oldLock);
    }

    TEST_CASE("Live removal requires the right project's reservation and revokes activation after commit", "[packages][application][host]") {
        HostFixture fixture;
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto expected = PreparePackageProject(policy, fixture.files, {});
        REQUIRE(expected.HasValue());
        const auto assets = fixture.AssetSource();
        auto sources = fixture.RemovalSources(*assets);
        const auto publish = [&] {
            return PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, sources);
        };
        HostFixture other;
        const auto otherAssets = other.AssetSource();
        const auto oldLock = fixture.Read("packages.lock");
        sources.live = other.RemovalSources(*otherAssets).live;
        const auto foreign = publish();
        REQUIRE(foreign.HasError());
        CHECK(foreign.ErrorValue().code.Value() == "packages.host.removal_reservation_mismatch");
        CHECK(fixture.Read("packages.lock") == oldLock);

        sources.live = fixture.removalLease;
        REQUIRE(publish().HasValue());
        CHECK(fixture.Read("packages.lock") != oldLock);
        sources.live.reset();
        fixture.removalLease.reset();
        const auto activated = fixture.removalLifecycle->Activate({}, Packages::PackageActivationBoundary::Quiescent, {});
        REQUIRE(activated.HasError());
        CHECK(activated.ErrorValue().code.Value() == "packages.activation.removal_reserved");
    }

    TEST_CASE("Editor package removal leaves executable contributions and their lock intact", "[packages][application][host][removal]") {
        HostFixture fixture{true};
        const auto application = fixture.Acquire();
        const auto install = application->Install();
        REQUIRE(install.HasValue());
        REQUIRE(fixture.Wait(*application, install.Value()).outcome == PackageOperationOutcome::Succeeded);
        const auto request = fixture.Read("packages.json");
        const auto lock = fixture.Read("packages.lock");
        const auto remove = application->Remove(fixture.graph.package, application->Plan().Value());
        REQUIRE(remove.HasValue());
        const auto rejected = fixture.Wait(*application, remove.Value());
        REQUIRE(rejected.outcome == PackageOperationOutcome::Failed);
        REQUIRE(rejected.diagnostic);
        CHECK(rejected.diagnostic->code.Value() == "packages.host.removal_requires_lifecycle_owner");
        CHECK(fixture.Read("packages.json") == request);
        CHECK(fixture.Read("packages.lock") == lock);
        CHECK(application->Plan().HasValue());
    }

    TEST_CASE("Package metadata removal recovers both sides of the lock commit point", "[packages][application][host][removal-rollback]") {
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

        bool expectRecovery = false;
        SECTION("Committed lock with an uncertain flush keeps the new pair") {
            files.SetLockWrite(MetadataFiles::LockWrite::FailAfterReplace);
            expectRecovery = true;
        }

        SECTION("Failed replacement leaves the prior pair intact and no rollback point") {
            files.SetLockWrite(MetadataFiles::LockWrite::FailBeforeReplace);
            CHECK(PublishPackageProjectRemoval(policy, initial.Value(), fixture.graph.package, files, fixture.RemovalSources(*assets))
                      .HasError());
            CHECK_FALSE(std::filesystem::exists(journal));
            CHECK(fixture.Read("packages.json") == oldRequest);
            CHECK(fixture.Read("packages.lock") == oldLock);
            CHECK(ReadPackageProjectRollback(policy, files, {}).HasError());
            files.SetLockWrite(MetadataFiles::LockWrite::Normal);
        }

        if (auto committed =
                PublishPackageProjectRemoval(policy, initial.Value(), fixture.graph.package, files, fixture.RemovalSources(*assets));
            committed.HasValue()) {
            CHECK(committed.Value().recoveryDiagnostic.has_value() == expectRecovery);
            CHECK(std::filesystem::exists(journal) == expectRecovery);
        } else {
            FAIL("Package removal did not commit");
        }

        const auto current = PreparePackageProject(policy, files, {});
        REQUIRE(current.HasValue());
        CHECK(current.Value().application.portableRequest->Dependencies().empty());
        CHECK(current.Value().application.lockedPlan->Packages().empty());
        CHECK_FALSE(std::filesystem::exists(journal));
        const auto previous = ReadPackageProjectRollback(policy, files, {});
        REQUIRE(previous.HasValue());
        CHECK(previous.Value().request == oldRequest);
        CHECK(previous.Value().lock == oldLock);
    }

    TEST_CASE("Interrupted package removal recovery keeps the journal for the next preparation", "[packages][application][host]") {
        HostFixture fixture;
        MetadataFiles files;
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto journal = policy.projectRoot / ".horo/local/package-metadata.pending";
        const auto oldRequest = fixture.Read("packages.json");
        const auto oldLock = fixture.Read("packages.lock");
        const auto initial = PreparePackageProject(policy, files, {});
        REQUIRE(initial.HasValue());
        const auto assets = fixture.AssetSource();

        files.SetLockWrite(MetadataFiles::LockWrite::FailBeforeReplace);
        files.BlockRecoveryRequestWrite(true);
        CHECK(PublishPackageProjectRemoval(policy, initial.Value(), fixture.graph.package, files, fixture.RemovalSources(*assets))
                  .HasError());
        REQUIRE(std::filesystem::exists(journal));
        CHECK(fixture.Read("packages.json") != oldRequest);
        {
            const Editor::ProjectMutationCoordinator mutations{files};
            const auto sceneSave = mutations.TryAcquire({policy.projectRoot, Editor::ProjectMutationOwner::Save, "scene-save"});
            REQUIRE(sceneSave.HasValue());
            const auto blocked = PreparePackageProject(policy, files, {});
            REQUIRE(blocked.HasError());
            CHECK(blocked.ErrorValue().code.Value() == "project.migration.locked");
            CHECK(std::filesystem::exists(journal));
            CHECK(fixture.Read("packages.json") != oldRequest);
        }
        files.BlockRecoveryRequestWrite(false);
        REQUIRE(PreparePackageProject(policy, files, {}).HasValue());
        CHECK(fixture.Read("packages.json") == oldRequest);
        CHECK(fixture.Read("packages.lock") == oldLock);
        CHECK_FALSE(std::filesystem::exists(journal));
    }

    TEST_CASE("Package update publishes only the reviewed staged candidate and recovers a failed commit",
              "[packages][application][host][update]") {
        HostFixture fixture;
        MetadataFiles files;
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto journal = policy.projectRoot / ".horo/local/package-metadata.pending";
        const auto base = PreparePackageProject(policy, files, {});
        REQUIRE(base.HasValue());
        const auto originalRequest = fixture.Read("packages.json");
        const auto originalLock = fixture.Read("packages.lock");
        auto intent = nlohmann::json::parse(originalRequest);
        intent["dependencies"][fixture.graph.package.Value()]["version"] = "^1.0.0";
        const auto requestJson = intent.dump();
        const auto request = Packages::ValidatedPackageRequest::Parse(requestJson);
        REQUIRE(request.HasValue());
        const auto &locked = base.Value().application.lockedPlan->Packages().front();
        const Packages::PackageResolutionPlan resolution{
            .packages = {{locked.package, locked.version, locked.source, locked.artifactDigest, {}}}};
        const Packages::PackageLockArtifact artifact{.package = locked.package,
                                                     .version = locked.version,
                                                     .source = locked.source,
                                                     .artifactDigest = locked.artifactDigest,
                                                     .manifestDigest = locked.manifestDigest,
                                                     .fileManifestDigest = locked.fileManifestDigest,
                                                     .contributions = locked.contributions};
        const auto lock = Packages::ValidatedPackageLockfileV1::Generate(resolution, std::span{&locked.package, 1},
                                                                         request.Value().Digest(), std::span{&artifact, 1});
        REQUIRE(lock.HasValue());
        const auto lockJson = lock.Value().SerializeCanonical();

        namespace RestoreTests = Horo::Tests::PackageRestore;
        RestoreTests::ServiceFixture restore(fixture.graph.bytes);
        std::filesystem::create_directories(journal.parent_path());
        const auto operation = restore.service->StartStaged({.projectRoot = policy.projectRoot,
                                                             .lockfileJson = lockJson,
                                                             .expectedRequestHash = request.Value().Digest(),
                                                             .platform = policy.platform});
        REQUIRE(operation.HasValue());
        const auto id = operation.Value().Id();
        REQUIRE(RestoreTests::WaitForTerminal(*restore.service, id).outcome == Packages::PackageRestoreOutcome::Ready);
        const auto staged = restore.service->Staged(id);
        REQUIRE(staged);

        ApprovedPackageProjectUpdate update{.base = base.Value(),
                                            .staged = *staged,
                                            .approval = {base.Value().requestDocumentDigest, base.Value().lockDigest,
                                                         RestoreTests::Digest(requestJson), RestoreTests::Digest(lockJson)},
                                            .candidateRequest = requestJson,
                                            .candidateLock = lockJson};
        const auto context = HostFixture::ReadContext();
        SECTION("a committed update retains the exact previous pair until the project changes") {
            REQUIRE(PublishPackageProjectUpdate(policy, update, files, context).HasValue());
            const auto previous = ReadPackageProjectRollback(policy, files, {});
            REQUIRE(previous.HasValue());
            CHECK(previous.Value().request == originalRequest);
            CHECK(previous.Value().lock == originalLock);
            fixture.Write("packages.json", requestJson + '\n');
            CHECK(ReadPackageProjectRollback(policy, files, {}).HasError());
            CHECK(fixture.Read("packages.json") == requestJson + '\n');
            return;
        }
        SECTION("a different candidate lock is rejected even with a matching approval digest") {
            const auto edited = lockJson + '\n';
            update.candidateLock = edited;
            update.approval.candidateLock = RestoreTests::Digest(edited);
            REQUIRE(PublishPackageProjectUpdate(policy, update, files, context).HasError());
            CHECK(fixture.Read("packages.json") == originalRequest);
        }
        SECTION("an externally edited base is preserved") {
            const auto edited = originalRequest + '\n';
            fixture.Write("packages.json", edited);
            REQUIRE(PublishPackageProjectUpdate(policy, update, files, context).HasError());
            CHECK(fixture.Read("packages.json") == edited);
        }
        SECTION("failure before the lock replacement restores the original pair before returning") {
            files.SetLockWrite(MetadataFiles::LockWrite::FailBeforeReplace);
            REQUIRE(PublishPackageProjectUpdate(policy, update, files, context).HasError());
            CHECK(fixture.Read("packages.json") == originalRequest);
        }
        SECTION("retry can keep the exact published pair after uncertain lock durability") {
            files.SetLockWrite(MetadataFiles::LockWrite::FailAfterReplace);
            const auto published = PublishPackageProjectUpdate(policy, update, files, context);
            REQUIRE(published.HasValue());
            CHECK(published.Value().recoveryDiagnostic.has_value());
            REQUIRE(PreparePackageProject(policy, files, {}).HasValue());
            CHECK(fixture.Read("packages.json") == requestJson);
            CHECK(fixture.Read("packages.lock") == lockJson);
            CHECK_FALSE(std::filesystem::exists(journal));
            const auto previous = ReadPackageProjectRollback(policy, files, {});
            REQUIRE(previous.HasValue());
            CHECK(previous.Value().lock == originalLock);
            CHECK(previous.Value().request == originalRequest);
            return;
        }
        CHECK(fixture.Read("packages.lock") == originalLock);
        CHECK_FALSE(std::filesystem::exists(journal));
        CHECK(ReadPackageProjectRollback(policy, files, {}).HasError());
    }

    TEST_CASE("Package removal fences authored writes and detects external edits during inventory", "[packages][application][host]") {
        HostFixture fixture;
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto expected = PreparePackageProject(policy, fixture.files, {});
        REQUIRE(expected.HasValue());
        const auto assets = fixture.AssetSource();
        const Editor::ProjectMutationCoordinator mutations{fixture.files};
        const Editor::ProjectMutationRequest save{policy.projectRoot, Editor::ProjectMutationOwner::Save, "scene-save"};
        const auto oldLock = fixture.Read("packages.lock");
        const auto edited = fixture.Read("packages.json") + "\n";
        const ObservedAssetSource observed{*assets, [&mutations, &save, &fixture, &edited] {
            CHECK(mutations.TryAcquire(save).HasError());
            fixture.Write("packages.json", edited);
        }};
        {
            const auto sceneSave = mutations.TryAcquire(save);
            REQUIRE(sceneSave.HasValue());
            const auto blocked = PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files,
                                                              fixture.RemovalSources(observed));
            REQUIRE(blocked.HasError());
            CHECK(blocked.ErrorValue().code.Value() == "project.migration.locked");
            CHECK(fixture.Read("packages.json") != edited);
        }
        const auto conflict =
            PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, fixture.RemovalSources(observed));
        REQUIRE(conflict.HasError());
        CHECK(conflict.ErrorValue().code.Value() == "packages.host.metadata_conflict");
        CHECK(fixture.Read("packages.json") == edited);
        CHECK(fixture.Read("packages.lock") == oldLock);
        CHECK_FALSE(std::filesystem::exists(policy.projectRoot / ".horo/local/package-metadata.pending"));
        CHECK(mutations.TryAcquire(save).HasValue());
    }

    TEST_CASE("Package removal cannot begin publication after project authority stops", "[packages][application][host]") {
        HostFixture fixture;
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto expected = PreparePackageProject(policy, fixture.files, {});
        REQUIRE(expected.HasValue());
        const auto oldRequest = fixture.Read("packages.json");
        const auto oldLock = fixture.Read("packages.lock");
        const auto assets = fixture.AssetSource();

        bool captured = false;
        const ObservedAssetSource observed{*assets, [&captured] {
            captured = true;
        }};
        auto sources = fixture.RemovalSources(observed);
        sources.context.authorityStopped = [&captured] {
            return captured;
        };
        const auto stopped = PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, sources);
        REQUIRE(captured);
        REQUIRE(stopped.HasError());
        CHECK(stopped.ErrorValue().code.Value() == "project_read.cancelled");
        CHECK(fixture.Read("packages.json") == oldRequest);
        CHECK(fixture.Read("packages.lock") == oldLock);
        CHECK_FALSE(std::filesystem::exists(policy.projectRoot / ".horo/local/package-metadata.pending"));

        const auto retry =
            PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, fixture.RemovalSources(*assets));
        REQUIRE(retry.HasValue());
        const auto current = PreparePackageProject(policy, fixture.files, {});
        REQUIRE(current.HasValue());
        CHECK(current.Value().application.portableRequest->Dependencies().empty());
        CHECK(current.Value().application.lockedPlan->Packages().empty());
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
        REQUIRE(
            PublishPackageProjectRemoval(policy, before.Value(), fixture.graph.package, files, fixture.RemovalSources(*assets)).HasError());
        CHECK(fixture.Read("packages.json") == oldRequest + "\n");
        const auto approved = PreparePackageProject(policy, files, {});
        REQUIRE(approved.HasValue());
        files.SetLockWrite(MetadataFiles::LockWrite::FailBeforeReplace);
        files.BlockRecoveryRequestWrite(true);
        REQUIRE(PublishPackageProjectRemoval(policy, approved.Value(), fixture.graph.package, files, fixture.RemovalSources(*assets))
                    .HasError());
        REQUIRE(std::filesystem::exists(policy.projectRoot / ".horo/local/package-metadata.pending"));
        files.BlockRecoveryRequestWrite(false);
        const auto edited = fixture.Read("packages.json") + "\n";
        fixture.Write("packages.json", edited);
        const auto conflicted = PreparePackageProject(policy, files, {});
        REQUIRE(conflicted.HasError());
        CHECK(conflicted.ErrorValue().code.Value() == "packages.host.metadata_conflict");
        CHECK(fixture.Read("packages.json") == edited);
        CHECK(fixture.Read("packages.lock") == oldLock);
        CHECK(std::filesystem::exists(policy.projectRoot / ".horo/local/package-metadata.pending"));
    }

    /** @brief Builds a registered graph with one package node in the chosen source family. */
    AI::DecisionGraphSource MakeReferencedDecisionGraph(const AI::DecisionPlanKind kind, const AI::DecisionNodeTypeId type) {
        using namespace AI::DecisionGraphTestSupport;
        if (kind == AI::DecisionPlanKind::BehaviorTree)
            return TaskTree(1, type.Value());
        if (kind == AI::DecisionPlanKind::StateMachine) {
            auto graph = TerminalMachine(1);
            auto &state = std::get<AI::StateMachineAssetDescriptor>(graph.body).states.front();
            state.entry = MakeIdentity<AI::TaskId>(8);
            state.entryProvider = AI::StateMachineProviderAction{{.id = Node(2), .type = type, .source = Location()}, {}};
            return graph;
        }
        const auto key = MakeIdentity<AI::BlackboardKeyId>(2);
        AI::UtilityDecisionSource utility{.root = Node(1), .requirements = {{.key = key, .kind = AI::BlackboardValueKind::Scalar}},
                                          .options = {{{Node(2), MakeIdentity<AI::TaskId>(9), 1, 0, {{.key = key}}},
                                                       {.id = Node(2), .type = type, .source = Location()}}}};
        return {Graph(1), 1, SchemaId(), {1, 1}, Location(), {}, std::move(utility)};
    }

    TEST_CASE("Package removal protects saved graphs using a departing Decision node", "[packages][application][host][removal]") {
        HostFixture fixture{true};
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto expected = PreparePackageProject(policy, fixture.files, {});
        REQUIRE(expected.HasValue());
        const auto assets = fixture.AssetSource();
        const auto &locked = expected.Value().application.lockedPlan->Packages().front();
        const auto type = AI::DecisionNodeTypeId::Create(201).Value();
        std::array owners{Packages::GameplayPackageModuleHost::RegistrationInventory{
            .package = locked.package,
            .version = locked.version,
            .artifact = locked.artifactDigest,
            .activationGeneration = 1,
            .moduleId = "com.horo.application-fixture.native",
            .descriptorPath = "extensions/native/extension.json",
            .decisionNodes = {type}}};
        using enum AI::DecisionPlanKind;
        const auto kind = GENERATE(BehaviorTree, StateMachine, Utility);
        const auto graph = MakeReferencedDecisionGraph(kind, type);
        const auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{&graph, 1}, graph.asset);
        REQUIRE(encoded.HasValue());
        const auto relative = std::string{"Assets/Agent"} + (kind == BehaviorTree ? ".horo_bt" : kind == StateMachine ? ".horo_sm" : ".horo_utility");
        const auto file = policy.projectRoot / relative;
        auto sidecar = file;
        sidecar += ".horo";
        std::ofstream{file, std::ios::binary}.write(reinterpret_cast<const char *>(encoded.Value().data()),
                                                     static_cast<std::streamsize>(encoded.Value().size()));
        std::ofstream{sidecar} << nlohmann::json{{"schemaVersion", 1},
                                                 {"assetId", "174ee050-f0a1-4351-89c4-04a5895bff81"},
                                                 {"assetType", AI::DecisionAssetType(kind)}};
        auto sources = fixture.RemovalSources(*assets);
        sources.registrations = owners;
        const auto oldLock = fixture.Read("packages.lock");
        const auto publish = [&] { return PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources); };
        const auto blocked = publish();
        REQUIRE(blocked.HasError());
        CHECK(blocked.ErrorValue().code.Value() == "packages.host.decision_node_in_use");
        REQUIRE(blocked.ErrorValue().diagnostics.size() == 1);
        CHECK(blocked.ErrorValue().diagnostics.front().location.source == relative);
        CHECK(blocked.ErrorValue().diagnostics.front().path == (kind == StateMachine ? "1/entry" : "2"));

        const auto broken = file.parent_path() / ("Broken" + file.extension().string());
        auto brokenSidecar = broken;
        brokenSidecar += ".horo";
        std::ofstream{broken, std::ios::binary} << "broken";
        std::ofstream{brokenSidecar} << nlohmann::json{{"schemaVersion", 1},
                                                       {"assetId", "174ee050-f0a1-4351-89c4-04a5895bff80"},
                                                       {"assetType", AI::DecisionAssetType(kind)}};
        const auto unreadable = publish();
        REQUIRE(unreadable.HasError());
        const auto &failure = unreadable.ErrorValue();
        CHECK(failure.code.Value() == "packages.host.decision_reference_unreadable");
        REQUIRE(failure.diagnostics.size() == 2);
        CHECK(failure.diagnostics[0].code.Value() == "packages.host.saved_reference_source_unreadable");
        CHECK(failure.diagnostics[0].location.source == "Assets/" + broken.filename().string());
        CHECK(failure.diagnostics[1].code.Value() == "packages.host.decision_node_in_use");
        CHECK(failure.diagnostics[1].location.source == relative);
        CHECK(fixture.Read("packages.lock") == oldLock);
    }

    TEST_CASE("Package removal verifies saved Scene and Prefab dependencies", "[packages][application][host]") {
        HostFixture fixture{true};
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto expected = PreparePackageProject(policy, fixture.files, {});
        REQUIRE(expected.HasValue());
        const auto assets = fixture.AssetSource();
        const auto type = Gameplay::BehaviorTypeId::Parse("game.tests.package_behavior").Value();
        const Gameplay::BehaviorComponent behavior{.instanceId = {3}, .typeId = type, .enabled = false};
        const auto &locked = expected.Value().application.lockedPlan->Packages().front();
        std::array owners{Packages::GameplayPackageModuleHost::RegistrationInventory{locked.package,
                                                                                     locked.version,
                                                                                     locked.artifactDigest,
                                                                                     1,
                                                                                     "com.horo.application-fixture.native",
                                                                                     {type},
                                                                                     {},
                                                                                     "extensions/native/extension.json"}};
        owners.front().hasCustomSenses = true;
        auto sources = fixture.RemovalSources(*assets);
        const auto unobserved = PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources);
        REQUIRE(unobserved.HasError());
        CHECK(unobserved.ErrorValue().code.Value() == "packages.host.runtime_registration_unobserved");
        REQUIRE(unobserved.ErrorValue().diagnostics.size() == 1);
        CHECK(unobserved.ErrorValue().diagnostics.front().path == "com.horo.application-fixture.native");
        sources.registrations = owners;
        const Error observationError{ErrorCode{"fixture.registry.capture_failed"}, ErrorDomainId{"fixture.registry"}};
        sources.registrationFailure = &observationError;
        const auto incomplete = PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources);
        REQUIRE(incomplete.HasError());
        CHECK(incomplete.ErrorValue().code.Value() == "packages.host.runtime_registration_unavailable");
        REQUIRE(incomplete.ErrorValue().cause.Get());
        CHECK(incomplete.ErrorValue().cause.Get()->code.Value() == observationError.code.Value());
        sources.registrationFailure = nullptr;

        const auto writeAsset = [&](const char *filename, const Assets::AssetId &asset, const char *kind, std::string_view bytes,
                                    std::string_view importer = {}) {
            auto path = policy.projectRoot / "Assets" / filename;
            {
                std::ofstream output{path, std::ios::binary};
                output << bytes;
                REQUIRE(output.good());
            }
            path += ".horo";
            std::ofstream metadata{path};
            nlohmann::json sidecar{{"schemaVersion", 1}, {"assetId", asset.ToString()}, {"assetType", kind}};
            if (!importer.empty())
                sidecar["importerPackageId"] = importer;
            metadata << sidecar;
            REQUIRE(metadata.good());
        };
        const auto sceneAsset = Assets::AssetId::Parse("10213243-5465-7687-98a9-bacbdcedfe0e").Value();
        SceneSource::SceneObjectSnapshot object{.id = {7}, .name = "Scene Object"};
        object.components.behaviors = {behavior};
        const auto scene = SceneSource::EncodeSceneSource({.objects = std::span{&object, 1}});
        writeAsset("Scene.horoasset", sceneAsset, "core.scene", scene);
        const auto prefabAsset = Assets::AssetId::Parse("10213243-5465-7687-98a9-bacbdcedfe0f").Value();
        Prefab::PrefabObjectNode root{.name = "Prefab Root", .behaviors = {behavior}};
        const auto prefab =
            Prefab::PrefabDocument::Create({.projectVersion = ParseHoroVersion("1.2.3").Value(), .assetId = prefabAsset, .objects = {root}},
                                           Prefab::PrefabLimitProfile::Create({}).Value());
        REQUIRE(prefab.HasValue());
        const auto prefabBytes = prefab.Value().SerializeCanonical();
        REQUIRE(prefabBytes.HasValue());
        writeAsset("Example.prefab", prefabAsset, "core.prefab", prefabBytes.Value());

        const auto newType = Gameplay::BehaviorTypeId::Parse("game.tests.new_behavior").Value();
        const std::array claims{PackageBehaviorMigration{locked.package, type, newType}};
        const auto impact = InspectPackageBehaviorMigrations(*assets, sources.files, sources.context, claims, 2);
        REQUIRE(impact.HasValue());
        REQUIRE(impact.Value().size() == 2);
        CHECK(impact.Value().front().path == "Assets/Scene.horoasset");
        CHECK(impact.Value().back().path == "Assets/Example.prefab");

        const auto oldRequest = fixture.Read("packages.json");
        const auto oldLock = fixture.Read("packages.lock");
        const auto blocked = PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources);
        REQUIRE(blocked.HasError());
        CHECK(blocked.ErrorValue().code.Value() == "packages.host.behavior_in_use");
        const auto &details = blocked.ErrorValue().diagnostics;
        REQUIRE(details.size() == 4);
        CHECK(details[0].location.source == "Assets/Scene.horoasset");
        CHECK(details[0].path == "7/3");
        CHECK(details[1].location.source == "Assets/Example.prefab");
        CHECK(details[1].path == "0/3");
        CHECK(details[2].code.Value() == "packages.host.perception_reference_review");
        CHECK(details[2].location.source == "Assets/Scene.horoasset");
        CHECK(details[3].code.Value() == "packages.host.perception_reference_review");
        CHECK(details[3].location.source == "Assets/Example.prefab");
        CHECK(fixture.Read("packages.json") == oldRequest);
        CHECK(fixture.Read("packages.lock") == oldLock);
        CHECK_FALSE(std::filesystem::exists(policy.projectRoot / ".horo/local/package-metadata.pending"));

        owners.front().artifact = {};
        const auto stale = PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources);
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == "packages.host.runtime_registration_unobserved");
        owners.front().artifact = locked.artifactDigest;
        writeAsset("Scene.horoasset", sceneAsset, "core.scene", scene, locked.package.Value());
        sources.registrations = {};
        const auto missing = PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources);
        REQUIRE(missing.HasError());
        CHECK(missing.ErrorValue().code.Value() == "packages.host.runtime_registration_unobserved");
        REQUIRE(missing.ErrorValue().diagnostics.size() == 2);
        CHECK(missing.ErrorValue().diagnostics[1].code.Value() == "packages.host.importer_in_use");
        CHECK(missing.ErrorValue().diagnostics[1].path == "Assets/Scene.horoasset");

        sources.registrations = owners;
        writeAsset("Scene.horoasset", sceneAsset, "core.scene", "{");
        const auto unreadable = PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources);
        REQUIRE(unreadable.HasError());
        CHECK(unreadable.ErrorValue().code.Value() == "packages.host.scene_reference_unreadable");
        const auto &unreadableDetails = unreadable.ErrorValue().diagnostics;
        REQUIRE(unreadableDetails.size() == 3);
        CHECK(unreadableDetails.front().code.Value() == "packages.host.saved_reference_source_unreadable");
        CHECK(unreadableDetails.front().location.source == "Assets/Scene.horoasset");
        CHECK(unreadableDetails[1].code.Value() == "packages.host.behavior_in_use");
        CHECK(unreadableDetails[1].location.source == "Assets/Example.prefab");
        CHECK(fixture.Read("packages.json") == oldRequest);
        CHECK(fixture.Read("packages.lock") == oldLock);

        root.behaviors.clear();
        const auto plainPrefab = Prefab::PrefabDocument::Create(
            {.projectVersion = ParseHoroVersion("1.2.3").Value(), .assetId = prefabAsset, .objects = {root}},
            Prefab::PrefabLimitProfile::Create({}).Value());
        REQUIRE(plainPrefab.HasValue());
        const auto plainPrefabBytes = plainPrefab.Value().SerializeCanonical();
        REQUIRE(plainPrefabBytes.HasValue());
        writeAsset("Example.prefab", prefabAsset, "core.prefab", plainPrefabBytes.Value());

        object.components.behaviors.clear();
        object.components.aiAgent = AI::AiAgentComponent{.agent = AI::AgentId::Create(9).Value(), .enabled = false};
        writeAsset("Scene.horoasset", sceneAsset, "core.scene", SceneSource::EncodeSceneSource({.objects = std::span{&object, 1}}));
        const auto requiresReview = PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources);
        REQUIRE(requiresReview.HasError());
        REQUIRE(requiresReview.ErrorValue().diagnostics.size() == 1);
        CHECK(requiresReview.ErrorValue().diagnostics.front().code.Value() == "packages.host.perception_reference_review");
        CHECK(requiresReview.ErrorValue().diagnostics.front().location.source == "Assets/Scene.horoasset");

        object.components.aiAgent.reset();
        writeAsset("Scene.horoasset", sceneAsset, "core.scene", SceneSource::EncodeSceneSource({.objects = std::span{&object, 1}}));
        REQUIRE(PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources).HasValue());
        CHECK(std::filesystem::is_regular_file(policy.projectRoot / "Assets/Scene.horoasset"));
        CHECK(std::filesystem::is_regular_file(policy.projectRoot / "Assets/Example.prefab"));
        const auto rollback = ReadPackageProjectRollback(policy, fixture.files, {});
        REQUIRE(rollback.HasValue());
        CHECK(rollback.Value().lock == oldLock);
        CHECK(rollback.Value().request == oldRequest);
    }

    /** @brief Writes direct native components and distinct active and orphaned topology edits. */
    void WriteSavedComponentReferences(const std::filesystem::path &folder, const Gameplay::ComponentTypeId &type) {
        const auto prefabId = Assets::AssetId::Parse("10213243-5465-7687-98a9-bacbdcedfe0f").Value();
        const auto version = ParseHoroVersion("1.2.3").Value();
        const Gameplay::SerializedComponent component{.typeId = type, .payload = {std::byte{'{'}, std::byte{'}'}}};
        const Prefab::PrefabObjectNode root{
            .name = "Prefab Root", .components = {{Prefab::PrefabComponentInstanceId::Create(2).Value(), component}}};
        const auto prefab = Prefab::PrefabDocument::Create({.projectVersion = version, .assetId = prefabId, .objects = {root}},
                                                            Prefab::PrefabLimitProfile::Create({}).Value());
        REQUIRE(prefab.HasValue());
        const auto prefabBytes = prefab.Value().SerializeCanonical();
        REQUIRE(prefabBytes.HasValue());
        const auto &bytes = prefabBytes.Value();
        const auto digest = ComputeSha256(std::as_bytes(std::span{bytes}));
        const Prefab::PrefabSourceRevision revision{version, digest};
        const auto add = [&](std::uint64_t instance) {
            return Prefab::PrefabAddComponentOverride{
                {}, {Prefab::PrefabComponentInstanceId::Create(instance).Value(), component}};
        };
        const Prefab::PrefabValueOverrideSet overrides{.sourcePrefab = prefabId, .authoredAgainst = revision, .schemaRevision = digest,
                                                       .componentRecords = {add(8)}, .componentOrphans = {{add(9), revision, digest}}};
        SceneSource::SceneObjectSnapshot object{.id = {7}, .name = "Scene Object"};
        object.components.gameplayComponents = {component};
        const SceneSource::ScenePrefabInstance instance{.instanceId = Prefab::PrefabInstanceId::Create(4).Value(),
                                                        .sourcePrefab = Prefab::PrefabAssetReference::Create(prefabId).Value(),
                                                        .valueOverrides = overrides};
        const auto scene = SceneSource::EncodeSceneSource({.objects = std::span{&object, 1}, .prefabInstances = std::span{&instance, 1}});
        std::ofstream{folder / "Scene.horoasset"} << scene;
        std::ofstream{folder / "Scene.horoasset.horo"}
            << R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0e","assetType":"core.scene"})";
        std::ofstream{folder / "Example.prefab"} << bytes;
        std::ofstream{folder / "Example.prefab.horo"}
            << R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.prefab"})";
    }

    TEST_CASE("Package removal identifies saved native components and retained prefab override intent", "[packages][application][host]") {
        HostFixture fixture{true};
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto expected = PreparePackageProject(policy, fixture.files, {});
        REQUIRE(expected.HasValue());
        const auto assets = fixture.AssetSource();
        const auto &locked = expected.Value().application.lockedPlan->Packages().front();
        const auto type = Gameplay::ComponentTypeId::Parse("game.tests.package_component").Value();
        std::array owners{Packages::GameplayPackageModuleHost::RegistrationInventory{
            .package = locked.package, .version = locked.version, .artifact = locked.artifactDigest, .activationGeneration = 1,
            .moduleId = "com.horo.application-fixture.native", .descriptorPath = "extensions/native/extension.json",
            .hasAdditionalReferenceClasses = true, .components = {type}}};
        auto sources = fixture.RemovalSources(*assets);
        sources.registrations = owners;
        sources.requireGameplayOwner = true;
        WriteSavedComponentReferences(policy.projectRoot / "Assets", type);
        const auto attempted = PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources);
        REQUIRE(attempted.HasError());
        CHECK(attempted.ErrorValue().code.Value() == "packages.host.removal_requires_lifecycle_owner");
        const auto &references = attempted.ErrorValue().diagnostics;
        REQUIRE(references.size() == 4);
        for (const auto &reference : references)
            CHECK(reference.code.Value() == "packages.host.component_in_use");
        CHECK(references[0].location.source == "Assets/Scene.horoasset");
        CHECK(references[0].path == "7");
        CHECK(references[1].path == "4/0/8");
        CHECK(references[2].path == "4/0/9");
        CHECK(references[3].location.source == "Assets/Example.prefab");
        CHECK(references[3].path == "0/2");

        sources.requireGameplayOwner = false;
        const auto ownerAdmitted = PublishPackageProjectRemoval(policy, expected.Value(), locked.package, fixture.files, sources);
        REQUIRE(ownerAdmitted.HasError());
        CHECK(ownerAdmitted.ErrorValue().code.Value() == "packages.host.component_in_use");
    }

    /** @brief Adds a second independently locked package with a declared runtime role. */
    Packages::PackageRestorePackage AddRetainedRuntime(HostFixture &fixture) {
        using namespace Packages;
        const auto id = HoroPackageId::Parse("com.horo.retained").Value();
        auto archive = std::make_shared<const ValidatedPackageArchive>(Verify(NativeAdmissionArchiveBytes(true, id.Value())));
        auto request = nlohmann::json::parse(fixture.Read("packages.json"));
        request["dependencies"][id.Value()] = {{"source", fixture.graph.source.Value()}, {"version", fixture.graph.version.ToString()}};
        const auto validated = ValidatedPackageRequest::Parse(request.dump());
        REQUIRE(validated.HasValue());
        const std::array roots{fixture.graph.package, id};
        PackageResolutionPlan plan;
        std::vector<PackageLockArtifact> artifacts;
        const auto add = [&](const auto &package, const auto &content) {
            plan.packages.emplace_back(package, fixture.graph.version, fixture.graph.source, content.Digest(),
                                       std::vector<HoroPackageId>{});
            artifacts.push_back({.package = package,
                                 .version = fixture.graph.version,
                                 .source = fixture.graph.source,
                                 .artifactDigest = content.Digest(),
                                 .manifestDigest = content.PackageManifestDigest(),
                                 .fileManifestDigest = content.Manifest().Digest(),
                                 .contributions = {"native"}});
        };
        add(fixture.graph.package, fixture.graph.archive);
        add(id, *archive);
        const auto lock = ValidatedPackageLockfileV1::Generate(plan, roots, validated.Value().Digest(), artifacts);
        REQUIRE(lock.HasValue());
        fixture.Write("packages.json", request.dump());
        fixture.Write("packages.lock", lock.Value().SerializeCanonical());
        const auto retained = std::ranges::find(lock.Value().Packages(), id, &LockedPackage::package);
        REQUIRE(retained != lock.Value().Packages().end());
        return {*retained, std::move(archive)};
    }

    /** @brief Captures the runtime role identity used by the independently locked fixture packages. */
    Packages::GameplayPackageModuleHost::RegistrationInventory ObserveFixtureRuntime(const Packages::LockedPackage &locked) {
        return {.package = locked.package,
                .version = locked.version,
                .artifact = locked.artifactDigest,
                .activationGeneration = 1,
                .moduleId = locked.package.Value() + ".native",
                .descriptorPath = "extensions/native/extension.json"};
    }

    TEST_CASE("Package removal needs coverage of retained users of a service or component",
              "[packages][application][host][removal-coverage]") {
        using namespace Packages;
        HostFixture fixture{true, NativeArchiveContributions::ModuleOnly};
        const auto retained = AddRetainedRuntime(fixture);
        const auto assets = fixture.AssetSource({retained});
        const PackageProjectPolicy policy{.projectRoot = std::filesystem::canonical(fixture.project.Path()),
                                          .platform = {"linux", "x64", "horo-sdk-2"}};
        const auto expected = PreparePackageProject(policy, fixture.files, {});
        REQUIRE(expected.HasValue());
        const auto service = Gameplay::GameplayServiceId::Parse("game.tests.required").Value();
        const auto component = Gameplay::ComponentTypeId::Parse("game.tests.required_component").Value();
        const bool providesService = GENERATE(true, false);
        const auto packages = expected.Value().application.lockedPlan->Packages();
        const auto provider = std::ranges::find(packages, fixture.graph.package, &LockedPackage::package);
        REQUIRE(provider != packages.end());
        std::array roles{ObserveFixtureRuntime(*provider), ObserveFixtureRuntime(retained.lock)};
        if (providesService)
            roles.front().services.push_back({.id = service});
        else
            roles.front().components.push_back(component);
        auto sources = fixture.RemovalSources(*assets);
        sources.requireGameplayOwner = true;
        sources.registrations = std::span{roles}.first(1);
        const auto oldRequest = fixture.Read("packages.json");
        const auto oldLock = fixture.Read("packages.lock");
        const auto blocked = PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, sources);
        REQUIRE(blocked.HasError());
        CHECK(blocked.ErrorValue().code.Value() == "packages.host.runtime_registration_unobserved");
        REQUIRE(blocked.ErrorValue().diagnostics.size() == 1);
        CHECK(blocked.ErrorValue().diagnostics.front().path == "com.horo.retained.native");
        CHECK(fixture.Read("packages.json") == oldRequest);
        CHECK(fixture.Read("packages.lock") == oldLock);

        sources.registrations = roles;
        REQUIRE(PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, sources).HasValue());
        const auto locked = ValidatedPackageLockfileV1::Parse(fixture.Read("packages.lock"));
        REQUIRE(locked.HasValue());
        REQUIRE(locked.Value().Packages().size() == 1);
        CHECK(locked.Value().Packages().front() == retained.lock);
    }

    /** @brief Verifies exact provider, retained descriptor and consumer identity for both dependency kinds. */
    void CheckRetainedNativeReferences(std::span<const Diagnostic> references, const std::string &provider) {
        REQUIRE(references.size() == 4);
        for (const auto &reference : references.first(2)) {
            CHECK(reference.code.Value() == "packages.host.service_in_use");
            CHECK(reference.message == provider + ": game.tests.required");
            CHECK(reference.location.source == "extensions/retained/extension.json");
        }
        CHECK(references[0].path == "com.horo.retained/game.tests.consumer");
        CHECK(references[1].path == "com.horo.retained/game.tests.tick");
        for (const auto &reference : references.last(2)) {
            CHECK(reference.code.Value() == "packages.host.component_access_in_use");
            CHECK(reference.message == provider + " / game.tests.provider_component");
            CHECK(reference.location.source == "extensions/retained/extension.json");
        }
        CHECK(references[2].path == "com.horo.retained/game.tests.retained_behavior");
        CHECK(references[3].path == "com.horo.retained/game.tests.tick");
    }

    TEST_CASE("Package removal reports retained native dependencies from the current lock", "[packages][application][host]") {
        using namespace Packages;
        HostFixture fixture;
        const auto assets = fixture.AssetSource();
        auto sources = fixture.RemovalSources(*assets);
        const auto retired = fixture.graph.package;
        const auto retained = HoroPackageId::Parse("com.horo.retained").Value();
        const auto version = fixture.graph.version;
        const auto source = fixture.graph.source;
        const auto retiredDigest = fixture.graph.archive.Digest();
        const auto retainedDigest = ComputeSha256(std::as_bytes(std::span{std::string_view{"retained"}}));
        const PackageResolutionPlan graph{{{retired, version, source, retiredDigest, {}}, {retained, version, source, retainedDigest, {}}}};
        const std::array roots{retired, retained};
        const std::array artifacts{PackageLockArtifact{retired, version, source, retiredDigest, retiredDigest, retiredDigest},
                                   PackageLockArtifact{retained, version, source, retainedDigest, retainedDigest, retainedDigest}};
        const auto lock = ValidatedPackageLockfileV1::Generate(graph, roots, retiredDigest, artifacts);
        REQUIRE(lock.HasValue());
        PackageRemovalGraphPreview removal{.directRoot = true, .removed = {{retired, version}}};
        const auto serviceId = Gameplay::GameplayServiceId::Parse("game.tests.required").Value();
        const auto consumerId = Gameplay::GameplayServiceId::Parse("game.tests.consumer").Value();
        const auto systemId = Gameplay::GameplaySystemId::Parse("game.tests.tick").Value();
        const auto component = Gameplay::ComponentTypeId::Parse("game.tests.provider_component").Value();
        const auto behaviorId = Gameplay::BehaviorTypeId::Parse("game.tests.retained_behavior").Value();
        using Inventory = GameplayPackageModuleHost::RegistrationInventory;
        std::array registrations{
            Inventory{.package = retired,
                      .version = version,
                      .artifact = retiredDigest,
                      .activationGeneration = 1,
                      .moduleId = "game.tests.provider",
                      .services = {{.id = serviceId}},
                      .components = {component}},
            Inventory{.package = retained,
                      .version = version,
                      .artifact = retainedDigest,
                      .activationGeneration = 2,
                      .moduleId = "game.tests.retained",
                      .services = {{.id = consumerId, .dependencies = {serviceId}}},
                      .descriptorPath = "extensions/retained/extension.json",
                      .systems = {{.id = systemId, .requiredServices = {serviceId}}},
                      .componentUses = {{component, behaviorId}, {component, systemId}}},
        };
        sources.registrations = registrations;

        const auto references = InspectPackageNativeReferences(sources, lock.Value(), removal);
        REQUIRE(references.HasValue());
        CheckRetainedNativeReferences(references.Value(), retired.Value());

        SECTION("stale retained artifact") {
            registrations[1].artifact = retiredDigest;
        }
        SECTION("both packages leave the graph") {
            removal.removed.emplace_back(retained, version);
        }
        const auto safe = InspectPackageNativeReferences(sources, lock.Value(), removal);
        REQUIRE(safe.HasValue());
        CHECK(safe.Value().empty());
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
        const auto blocked =
            PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, fixture.RemovalSources(*assets));
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
        const auto incomplete =
            PublishPackageProjectRemoval(policy, expected.Value(), fixture.graph.package, fixture.files, fixture.RemovalSources(*assets));
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

    TEST_CASE("Package project previews updates under the current host authority without writing",
              "[packages][application][host][update]") {
        HostFixture fixture;
        auto application = fixture.Acquire();
        const auto oldRequest = fixture.Read("packages.json");
        const auto oldLock = fixture.Read("packages.lock");
        auto intent = nlohmann::json::parse(oldRequest);
        intent["dependencies"][fixture.graph.package.Value()]["version"] = "2.0.0";
        const auto request = Packages::ValidatedPackageRequest::Parse(intent.dump());
        REQUIRE(request.HasValue());

        const auto version = Packages::PackageVersion::Parse("2.0.0").Value();
        const auto digest = fixture.graph.archive.Digest();
        const Packages::PackageResolutionPlan resolution{.packages = {{fixture.graph.package, version, fixture.graph.source, digest, {}}}};
        Packages::PackageLockArtifact artifact{.package = fixture.graph.package,
                                               .version = version,
                                               .source = fixture.graph.source,
                                               .artifactDigest = digest,
                                               .manifestDigest = fixture.graph.archive.PackageManifestDigest(),
                                               .fileManifestDigest = fixture.graph.archive.Manifest().Digest()};
        SECTION("an incompatible candidate reports the target platform error") {
            artifact.platforms = {{"windows", "x64", "horo-sdk-2"}};
            const auto candidate = Packages::ValidatedPackageLockfileV1::Generate(resolution, std::span{&fixture.graph.package, 1U},
                                                                                  request.Value().Digest(), std::span{&artifact, 1U});
            REQUIRE(candidate.HasValue());
            const auto rejected = application->PreviewUpdate(request.Value(), candidate.Value());
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == "packages.lockfile.unsupported_platform");
        }
        SECTION("an admitted candidate reports exact changes and the expired owner rejects it") {
            const auto candidate = Packages::ValidatedPackageLockfileV1::Generate(resolution, std::span{&fixture.graph.package, 1U},
                                                                                  request.Value().Digest(), std::span{&artifact, 1U});
            REQUIRE(candidate.HasValue());
            const auto preview = application->PreviewUpdate(request.Value(), candidate.Value());
            REQUIRE(preview.HasValue());
            CHECK(preview.Value().requestChanged);
            REQUIRE(preview.Value().changes.size() == 1);
            const auto &change = preview.Value().changes.front();
            REQUIRE(change.current.has_value());
            REQUIRE(change.candidate.has_value());
            CHECK(change.current->version == fixture.graph.version);
            CHECK(change.candidate->version == version);
            fixture.clock.now = Duration::FromMilliseconds(1001);
            CHECK(application->PreviewUpdate(request.Value(), candidate.Value()).HasError());
        }
        CHECK(fixture.Read("packages.json") == oldRequest);
        CHECK(fixture.Read("packages.lock") == oldLock);
        CHECK_FALSE(application->Current().Value());
        CHECK(fixture.source.calls.load() == 0);
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
