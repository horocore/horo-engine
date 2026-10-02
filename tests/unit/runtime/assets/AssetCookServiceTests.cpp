#include "HeadlessMeshCooker.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookService.h"
#include "Horo/Assets/CookCatalog.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Sha256.h"
#include "assets/AssetCookPublicationFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    AssetId Id(const std::string_view value) {
        auto parsed = AssetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetTypeId Type(const std::string_view value) {
        auto parsed = AssetTypeId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetCookTargetId Target(const std::string_view value) {
        auto parsed = AssetCookTargetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetRecord TestMeshRecord() {
        const auto sourcePath = ProjectPath::Parse("assets/test_mesh.fbx");
        const auto metadataPath = ProjectPath::Parse("assets/test_mesh.fbx.horo");
        REQUIRE(sourcePath.HasValue());
        REQUIRE(metadataPath.HasValue());
        return AssetRecord{.id = Id("00000000-0000-0000-0000-0000000000a1"),
                           .type = Type("core.mesh"),
                           .sourcePath = sourcePath.Value(),
                           .metadataPath = metadataPath.Value()};
    }

    struct TempDir {
        std::filesystem::path path;

        TempDir() {
            auto tmp = std::filesystem::temp_directory_path() / "horo_service_test";
            std::filesystem::create_directories(tmp);
            auto unique = tmp / ("test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(unique);
            path = unique;
        }

        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    /** @brief Creates a minimal file with given content. */
    void WriteFile(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    /** @brief Creates a fake .horo sidecar so the registry picks up the file. */
    std::string SidecarJson(std::string_view assetId, std::string_view assetType) {
        return std::string("{\"schemaVersion\":1,\"assetId\":\"") + std::string(assetId) + "\",\"assetType\":\"" + std::string(assetType) +
               "\"}";
    }

    /**
     * @brief Sets up a minimal project directory structure with one source asset and sidecar.
     */
    struct TestProject {
        TempDir dir;
        std::filesystem::path assetsDir;
        std::filesystem::path sourceFile;

        TestProject() {
            assetsDir = dir.path / "assets";
            std::filesystem::create_directories(assetsDir);

            // Create a minimal source file
            sourceFile = assetsDir / "test_mesh.fbx";
            std::vector<std::uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
            WriteFile(sourceFile, data);

            // Create sidecar
            auto sidecarJson = SidecarJson("00000000-0000-0000-0000-0000000000a1", "core.mesh");
            auto sidecarBytes =
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(sidecarJson.data()), sidecarJson.size());
            WriteFile(std::string(sourceFile.string()) + ".horo", sidecarBytes);
        }
    };

    /** @brief Adds a second distinct source asset for concurrent cancellation checks. */
    AssetRecord AddSecondMesh(TestProject &project) {
        const std::filesystem::path sourceFile = project.assetsDir / "second_mesh.fbx";
        std::filesystem::copy_file(project.sourceFile, sourceFile);
        const std::string sidecarJson = SidecarJson("00000000-0000-0000-0000-0000000000a2", "core.mesh");
        const auto sidecarBytes =
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(sidecarJson.data()), sidecarJson.size());
        WriteFile(std::string(sourceFile.string()) + ".horo", sidecarBytes);
        AssetRecord record = TestMeshRecord();
        record.id = Id("00000000-0000-0000-0000-0000000000a2");
        record.sourcePath = ProjectPath::Parse("assets/second_mesh.fbx").Value();
        record.metadataPath = ProjectPath::Parse("assets/second_mesh.fbx.horo").Value();
        return record;
    }

    /** @brief Requests cancellation while returning either an acknowledged cook cancellation or a real failure. */
    class CancellingCooker final : public ICookerStrategy {
    public:
        CancellingCooker(CancellationSource &source, const bool fail) : source_(source), fail_(fail) {}

        [[nodiscard]] Result<CookOutputSink> Cook(const CookSourceView &, const CancellationToken &) const override {
            source_.RequestCancellation();
            return Result<CookOutputSink>::Failure(Error{ErrorCode{fail_ ? "test.cook.failed" : "asset.cook.cancelled"}});
        }

    private:
        CancellationSource &source_;
        bool fail_;
    };

    /** @brief Test strategy whose effective settings must participate in AST cache admission. */
    class SettingsCooker final : public ICookerStrategy {
    public:
        explicit SettingsCooker(const std::uint8_t setting) : setting_(setting) {}

        [[nodiscard]] CookerCacheIdentity CacheIdentity() const noexcept override {
            const std::array<std::byte, 1> bytes{static_cast<std::byte>(setting_)};
            return {.version = "test.1", .settingsDigest = ComputeSha256(bytes), .settingsSchemaVersion = 1};
        }

        [[nodiscard]] Result<CookOutputSink> Cook(const CookSourceView &, const CancellationToken &) const override {
            return Result<CookOutputSink>::Success(CookOutputSink{.payload = {setting_}});
        }

        [[nodiscard]] Result<void> ValidateCookedPayload(const CookSourceView &,
                                                         const std::span<const std::uint8_t> payload) const override {
            return payload.size() == 1 && payload.front() == setting_
                       ? Result<void>::Success()
                       : Result<void>::Failure(Error{ErrorCode{"test.cook.payload_mismatch"}});
        }

    private:
        std::uint8_t setting_{};
    };

    /** @brief Verifies that the initial cook and later cache hit retain source attribution. */
    void AssertCachedCookOutput(const BuildOutputSnapshot &first, const BuildOutputSnapshot &second,
                                const std::filesystem::path &sourcePath) {
        const auto cached = std::ranges::find_if(second.records, [](const BuildOutputRecord &record) {
            return record.code.Value() == "asset.cook.cache_hit";
        });
        REQUIRE((cached != second.records.end()));
        REQUIRE((cached->result == BuildOutputResult::Cached));
        REQUIRE(cached->source.has_value());
        REQUIRE(cached->source->absolutePath == sourcePath.string());
        REQUIRE(cached->sessionId.has_value());
        REQUIRE((cached->sessionId == second.records.back().sessionId));
        REQUIRE((second.records.back().result == BuildOutputResult::Succeeded));

        const auto cooked = std::ranges::find_if(first.records, [](const BuildOutputRecord &record) {
            return record.code.Value() == "asset.cook.asset_cooked";
        });
        REQUIRE(cooked != first.records.end());
        REQUIRE(cooked->result == BuildOutputResult::Succeeded);
        REQUIRE(cooked->source.has_value());
        REQUIRE(cooked->source->absolutePath == sourcePath.string());
    }

    /** @brief Checks that both a failed or cancelled cook and its cancelled sibling retain source attribution. */
    void AssertCancelledSiblingOutput(const BuildOutputSnapshot &snapshot, const OperationRecord &operation, const TestProject &project,
                                      const bool fail) {
        REQUIRE(snapshot.records.back().result == (fail ? BuildOutputResult::Failed : BuildOutputResult::Cancelled));
        const auto assetFailure = std::ranges::find_if(snapshot.records, [](const BuildOutputRecord &record) {
            return record.source.has_value();
        });
        REQUIRE(assetFailure != snapshot.records.end());
        REQUIRE(assetFailure->result == (fail ? BuildOutputResult::Failed : BuildOutputResult::Cancelled));
        REQUIRE(assetFailure->source->absolutePath == project.sourceFile.string());
        REQUIRE(assetFailure->operationId == operation.id);
        REQUIRE(assetFailure->sessionId == snapshot.records.back().sessionId);
        const std::filesystem::path secondSource = project.assetsDir / "second_mesh.fbx";
        REQUIRE(std::ranges::count_if(snapshot.records, [&](const BuildOutputRecord &record) {
            const bool expectedTerminalResult =
                record.result == BuildOutputResult::Cancelled || (fail && record.result == BuildOutputResult::Failed);
            return record.source.has_value() && record.source->absolutePath == secondSource.string() && expectedTerminalResult;
        }) == 1);
    }

    /** @brief Owns a real native publication callback so cancellation can be injected on either side of replacement. */
    class CookPublicationFiles final : public DurableFileSystem {
    public:
        NativeDurableFileSystem native;
        CancellationSource *cancellation{};
        bool cancelAfterCommit{};
        bool failCommittedSync{};
        bool committed{};

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, const std::string_view owner) override {
            return native.TryAcquireExclusive(path, owner);
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native.AvailableBytes(path);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            auto written = native.WriteDurable(path, bytes);
            if (written.HasValue() && cancellation && !cancelAfterCommit && path.filename() == "current.json")
                cancellation->RequestCancellation();
            return written;
        }

        Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native.CopyDurable(source, destination);
        }

        Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            return native.AtomicReplace(prepared, destination);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (receipt.WasCommitted() && destination.filename() == "current.json" && prepared.filename() != "unpublished.current.json") {
                committed = true;
                if (cancellation && cancelAfterCommit)
                    cancellation->RequestCancellation();
                if (failCommittedSync)
                    return Result<void>::Failure(Error{ErrorCode{"test.cook.publication_sync_failed"}});
            }
            return replaced;
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native.RemoveDurable(path);
        }

        Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native.SyncDirectory(path);
        }
    };

    /** @brief Creates an empty full-cook host with genuine publication files and entropy. */
    struct EmptyCookPublicationFixture {
        TempDir source;
        TempDir cache;
        TempDir cooked;
        JobSystem jobs;
        AssetCookService service{jobs, Catalog()};
        AssetCookRequest request{.sourceRoot = source.path,
                                 .cacheRoot = cache.path,
                                 .cookedRoot = cooked.path,
                                 .registry = AssetRegistry{}.Snapshot(),
                                 .target = Target("headless-null")};

        EmptyCookPublicationFixture() {
            Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
        }

    private:
        [[nodiscard]] static std::shared_ptr<const CookerCatalogSnapshot> Catalog() {
            CookerCatalog catalog;
            REQUIRE(RegisterHeadlessMeshCooker(catalog).HasValue());
            auto snapshot = catalog.Publish();
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }
    };

    /** @brief Exercises both standard and foreign exceptions from optional operation-history delivery. */
    class ThrowingCookHistorySink final : public IOperationHistorySink {
    public:
        bool nonstandard{};
        std::size_t attempted{};

        void AppendTerminal(const OperationRecord &) override {
            ++attempted;
            if (nonstandard)
                throw 73;
            throw std::runtime_error("Optional cook history notification failed");
        }
    };

}  // namespace

TEST_CASE("AssetCookService empty registry publishes empty generation", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;

    JobSystem jobs;

    // Build an empty registry
    AssetRegistry registry;
    auto emptySnapshot = registry.Snapshot();

    // Build catalog with headless mesh cooker
    CookerCatalog catalog;
    REQUIRE((RegisterHeadlessMeshCooker(catalog).HasValue()));
    auto catSnapshot = catalog.Publish();
    REQUIRE((catSnapshot.HasValue()));

    AssetCookService service(jobs, catSnapshot.Value());
    BuildOutputStore buildOutput{8};
    OperationStore operations{4, 4};

    AssetCookRequest request{
        .sourceRoot = project.dir.path,
        .cacheRoot = cacheDir.path,
        .cookedRoot = cookedDir.path,
        .registry = emptySnapshot,
        .target = Target("headless-null"),
        .buildOutputStore = &buildOutput,
        .operationStore = &operations,
    };
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);

    CancellationToken cancellation;
    auto result = service.Cook(request, cancellation);
    REQUIRE((result.HasValue()));

    auto &report = result.Value();
    REQUIRE((report.totalAssets == 0));
    REQUIRE((report.cookedAssets == 0));
    REQUIRE((report.cacheHits == 0));
    const auto current = ResolveCurrentCookGeneration(request.cookedRoot, request.limits);
    REQUIRE(current.HasValue());
    CHECK(current.Value().manifestDigest == report.generation.manifestDigest);
    CHECK(current.Value().artifactCount == 0);
    const auto contents = ReadCookGenerationContents(current.Value(), request.limits.maximumArtifactBytes, request.limits);
    REQUIRE(contents.HasValue());
    CHECK(contents.Value().entries.empty());
    CHECK(contents.Value().artifacts.empty());
    const auto buildSnapshot = buildOutput.SnapshotIfChanged(0);
    REQUIRE(buildSnapshot.has_value());
    REQUIRE((buildSnapshot->records.size() == 2U));
    REQUIRE((buildSnapshot->records.back().result == BuildOutputResult::Succeeded));
    REQUIRE((buildSnapshot->records.back().code.Value() == "asset.cook.succeeded"));
    REQUIRE((std::ranges::count_if(buildSnapshot->records, [](const BuildOutputRecord &record) {
        return record.result != BuildOutputResult::None;
    }) == 1));
    const auto operationSnapshot = operations.SnapshotIfChanged(0);
    REQUIRE(operationSnapshot.has_value());
    REQUIRE((operationSnapshot->operations.size() == 1));
    REQUIRE((operationSnapshot->operations.front().state == OperationState::Succeeded));
    for (const BuildOutputRecord &record : buildSnapshot->records) {
        REQUIRE(record.sessionId.has_value());
        REQUIRE(record.sessionId->IsValid());
        REQUIRE((record.sessionId == buildSnapshot->records.front().sessionId));
        REQUIRE((record.operationId == operationSnapshot->operations.front().id));
    }
}

TEST_CASE("AssetCookService honours cancellation before work", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;

    JobSystem jobs;

    AssetRegistry registry;
    auto snapshot = registry.Snapshot();

    CookerCatalog catalog;
    REQUIRE((RegisterHeadlessMeshCooker(catalog).HasValue()));
    auto catSnapshot = catalog.Publish();
    REQUIRE((catSnapshot.HasValue()));

    AssetCookService service(jobs, catSnapshot.Value());

    CancellationSource cancelSource;
    cancelSource.RequestCancellation();
    auto cancellation = cancelSource.Token();

    AssetCookRequest request{
        .sourceRoot = project.dir.path,
        .cacheRoot = cacheDir.path,
        .cookedRoot = cookedDir.path,
        .registry = snapshot,
        .target = Target("headless-null"),
    };
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);

    auto result = service.Cook(request, cancellation);
    REQUIRE((result.HasError()));
}

TEST_CASE("AssetCookService keeps cook cancellation separate from concurrent failure", "[native]") {
    for (const bool fail : {false, true}) {
        TestProject project;
        TempDir cacheDir;
        TempDir cookedDir;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        CancellationSource source;

        AssetRegistry registry;
        REQUIRE(registry.Publish({TestMeshRecord(), AddSecondMesh(project)}).status == AssetRegistryBuildStatus::Complete);

        CookerCatalog catalog;
        REQUIRE(catalog
                    .Register(CookerContribution{.contributionId = "test.cancelling",
                                                 .assetType = Type("core.mesh"),
                                                 .targets = {Target("headless-null")},
                                                 .strategy = std::make_shared<const CancellingCooker>(source, fail)})
                    .HasValue());
        const auto catalogSnapshot = catalog.Publish();
        REQUIRE(catalogSnapshot.HasValue());
        AssetCookService service(jobs, catalogSnapshot.Value());
        BuildOutputStore output{16};
        OperationStore operations{4, 4};
        AssetCookRequest request{.sourceRoot = project.dir.path,
                                 .cacheRoot = cacheDir.path,
                                 .cookedRoot = cookedDir.path,
                                 .registry = registry.Snapshot(),
                                 .target = Target("headless-null"),
                                 .buildOutputStore = &output,
                                 .operationStore = &operations};
        Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);

        const auto result = service.Cook(request, source.Token());
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == (fail ? "test.cook.failed" : "asset.cook.cancelled"));
        if (!fail)
            REQUIRE(ErrorChainContains(result.ErrorValue(), ErrorDomainId{"horo.foundation.jobs"}, ErrorCode{"job.cancelled"}));
        const auto operationSnapshot = operations.SnapshotIfChanged(0);
        REQUIRE(operationSnapshot.has_value());
        REQUIRE(operationSnapshot->operations.front().state == (fail ? OperationState::Failed : OperationState::Cancelled));
        const auto outputSnapshot = output.SnapshotIfChanged(0);
        REQUIRE(outputSnapshot.has_value());
        AssertCancelledSiblingOutput(*outputSnapshot, operationSnapshot->operations.front(), project, fail);
        jobs.Shutdown(ShutdownPolicy::Drain);
    }
}

TEST_CASE("AssetCookService publishes cache hits as cached scoped results", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;

    AssetRegistry registry;
    const auto sourcePath = ProjectPath::Parse("assets/test_mesh.fbx");
    const auto metadataPath = ProjectPath::Parse("assets/test_mesh.fbx.horo");
    REQUIRE(sourcePath.HasValue());
    REQUIRE(metadataPath.HasValue());
    const AssetRegistryBuildReport registryBuild = registry.Publish({AssetRecord{.id = Id("00000000-0000-0000-0000-0000000000a1"),
                                                                                 .type = Type("core.mesh"),
                                                                                 .sourcePath = sourcePath.Value(),
                                                                                 .metadataPath = metadataPath.Value()}});
    REQUIRE((registryBuild.status == AssetRegistryBuildStatus::Complete));

    CookerCatalog catalog;
    REQUIRE((RegisterHeadlessMeshCooker(catalog).HasValue()));
    const auto catalogSnapshot = catalog.Publish();
    REQUIRE(catalogSnapshot.HasValue());
    AssetCookService service(jobs, catalogSnapshot.Value());
    BuildOutputStore buildOutput{16};
    AssetCookRequest request{
        .sourceRoot = project.dir.path,
        .cacheRoot = cacheDir.path,
        .cookedRoot = cookedDir.path,
        .registry = registry.Snapshot(),
        .target = Target("headless-null"),
        .buildOutputStore = &buildOutput,
    };
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
    CancellationToken cancellation;

    REQUIRE(service.Cook(request, cancellation).HasValue());
    const auto firstSnapshot = buildOutput.SnapshotIfChanged(0);
    REQUIRE(firstSnapshot.has_value());
    REQUIRE(service.Cook(request, cancellation).HasValue());
    const auto secondSnapshot = buildOutput.SnapshotIfChanged(firstSnapshot->revision);
    REQUIRE(secondSnapshot.has_value());
    AssertCachedCookOutput(*firstSnapshot, *secondSnapshot, project.sourceFile);
}

TEST_CASE("AssetCookService isolates cache entries by exact strategy settings", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;
    AssetRegistry registry;
    REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);

    const auto target = Target("headless-null");
    AssetCookRequest request{
        .sourceRoot = project.dir.path,
        .cacheRoot = cacheDir.path,
        .cookedRoot = cookedDir.path,
        .registry = registry.Snapshot(),
        .target = target,
    };
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
    const auto cookWithSetting = [&](const std::uint8_t setting) {
        CookerCatalog catalog;
        REQUIRE(catalog
                    .Register(CookerContribution{
                        .contributionId = "test.settings-cooker",
                        .assetType = Type("core.mesh"),
                        .targets = {target},
                        .strategy = std::make_shared<const SettingsCooker>(setting),
                    })
                    .HasValue());
        auto snapshot = catalog.Publish();
        REQUIRE(snapshot.HasValue());
        AssetCookService service(jobs, snapshot.Value());
        return service.Cook(request, CancellationToken{});
    };

    auto first = cookWithSetting(1);
    REQUIRE(first.HasValue());
    CHECK(first.Value().cookedAssets == 1);
    auto different = cookWithSetting(2);
    REQUIRE(different.HasValue());
    CHECK(different.Value().cookedAssets == 1);
    CHECK(different.Value().cacheHits == 0);
    auto sameAgain = cookWithSetting(2);
    REQUIRE(sameAgain.HasValue());
    CHECK(sameAgain.Value().cookedAssets == 0);
    CHECK(sameAgain.Value().cacheHits == 1);
    jobs.Shutdown(ShutdownPolicy::Drain);
}

TEST_CASE("AssetCookService reports source admission failures with navigable diagnostics", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;
    AssetRegistry registry;
    REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);
    CookerCatalog catalog;
    const auto catalogSnapshot = catalog.Publish();
    REQUIRE(catalogSnapshot.HasValue());
    AssetCookService service(jobs, catalogSnapshot.Value());
    BuildOutputStore output{8};
    OperationStore operations{4, 4};
    AssetCookRequest request{.sourceRoot = project.dir.path,
                             .cacheRoot = cacheDir.path,
                             .cookedRoot = cookedDir.path,
                             .registry = registry.Snapshot(),
                             .target = Target("headless-null"),
                             .buildOutputStore = &output,
                             .operationStore = &operations};
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
    CancellationToken cancellation;

    const auto result = service.Cook(request, cancellation);
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "asset.cook.cooker_missing");
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->records.size() == 3);
    REQUIRE(snapshot->records[1].code.Value() == "asset.cook.cooker_missing");
    REQUIRE(snapshot->records[1].source.has_value());
    REQUIRE(snapshot->records[1].source->absolutePath == project.sourceFile.string());
    REQUIRE(snapshot->records[1].result == BuildOutputResult::Failed);
    REQUIRE(snapshot->records[2].result == BuildOutputResult::Failed);
}

TEST_CASE("AssetCookService reports unreadable source paths without publishing a generation", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;
    AssetRegistry registry;
    REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);
    CookerCatalog catalog;
    REQUIRE(RegisterHeadlessMeshCooker(catalog).HasValue());
    const auto catalogSnapshot = catalog.Publish();
    REQUIRE(catalogSnapshot.HasValue());
    AssetCookService service(jobs, catalogSnapshot.Value());
    BuildOutputStore output{8};
    AssetCookRequest request{.sourceRoot = project.dir.path,
                             .cacheRoot = cacheDir.path,
                             .cookedRoot = cookedDir.path,
                             .registry = registry.Snapshot(),
                             .target = Target("headless-null"),
                             .buildOutputStore = &output};
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
    REQUIRE(std::filesystem::remove(project.sourceFile));
    CancellationToken cancellation;

    const auto result = service.Cook(request, cancellation);
    REQUIRE(result.HasError());
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->records.size() == 3);
    REQUIRE(snapshot->records[1].code.Value() == result.ErrorValue().code.Value());
    REQUIRE(snapshot->records[1].source.has_value());
    REQUIRE(snapshot->records[1].source->absolutePath == project.sourceFile.string());
    REQUIRE_FALSE(std::filesystem::exists(cookedDir.path / "current.json"));
}

TEST_CASE("AssetCookService rejects cook when operation admission is full", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;
    AssetRegistry registry;
    CookerCatalog catalog;
    REQUIRE(RegisterHeadlessMeshCooker(catalog).HasValue());
    const auto catalogSnapshot = catalog.Publish();
    REQUIRE(catalogSnapshot.HasValue());
    AssetCookService service(jobs, catalogSnapshot.Value());
    BuildOutputStore output{8};
    OperationStore operations{1, 4};
    const auto occupied = operations.Begin(OperationDescriptor{.kind = OperationKind::Cook, .title = "Other cook"});
    REQUIRE(occupied.has_value());
    AssetCookRequest request{.sourceRoot = project.dir.path,
                             .cacheRoot = cacheDir.path,
                             .cookedRoot = cookedDir.path,
                             .registry = registry.Snapshot(),
                             .target = Target("headless-null"),
                             .buildOutputStore = &output,
                             .operationStore = &operations};
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
    CancellationToken cancellation;

    const auto result = service.Cook(request, cancellation);
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "asset.cook.operation_admission_failed");
    REQUIRE_FALSE(output.SnapshotIfChanged(0).has_value());
    REQUIRE_FALSE(std::filesystem::exists(cookedDir.path / "current.json"));
}

TEST_CASE("AssetCookService rejects missing host publication authority before writing", "[native]") {
    for (const bool missingFiles : {false, true}) {
        EmptyCookPublicationFixture fixture;
        if (missingFiles)
            fixture.request.publicationFiles.reset();
        else
            fixture.request.newPublicationOperationId = {};
        const auto result = fixture.service.Cook(fixture.request, {});
        REQUIRE(result.HasError());
        CHECK_FALSE(std::filesystem::exists(fixture.cooked.path / "current.json"));
        CHECK(std::filesystem::is_empty(fixture.cache.path));
    }
}

TEST_CASE("AssetCookService honors the same native writer lock as partial generation publishers", "[native]") {
    EmptyCookPublicationFixture fixture;
    {
        auto lock = fixture.request.publicationFiles->TryAcquireExclusive(fixture.cooked.path / ".cook-writer.lock", "another publisher");
        REQUIRE(lock.HasValue());
        const auto blocked = fixture.service.Cook(fixture.request, {});
        REQUIRE(blocked.HasError());
        CHECK_FALSE(std::filesystem::exists(fixture.cooked.path / "current.json"));
    }
    REQUIRE(fixture.service.Cook(fixture.request, {}).HasValue());
    REQUIRE(ResolveCurrentCookGeneration(fixture.cooked.path).HasValue());
}

TEST_CASE("AssetCookService publication cancellation preserves true commit outcome and durability diagnostic", "[native]") {
    for (const bool committed : {false, true}) {
        EmptyCookPublicationFixture fixture;
        CancellationSource cancellation;
        auto files = std::make_shared<CookPublicationFiles>();
        files->cancellation = &cancellation;
        files->cancelAfterCommit = committed;
        files->failCommittedSync = committed;
        fixture.request.publicationFiles = files;
        OperationStore operations{4, 4};
        fixture.request.operationStore = &operations;
        const auto result = fixture.service.Cook(fixture.request, cancellation.Token());
        CHECK(cancellation.IsCancellationRequested());
        CHECK(result.HasValue() == committed);
        const auto snapshot = operations.SnapshotIfChanged(0);
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->operations.front().state == (committed ? OperationState::Succeeded : OperationState::Cancelled));
        if (committed) {
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().generation.durabilityError.has_value());
            CHECK(result.Value().generation.durabilityError->code.Value() == "test.cook.publication_sync_failed");
            REQUIRE(ResolveCurrentCookGeneration(fixture.cooked.path).HasValue());
        } else {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == "asset.cook.cancelled");
            CHECK(ResolveCurrentCookGeneration(fixture.cooked.path).HasError());
        }
    }
}

TEST_CASE("AssetCookService rejects corrupt current authority instead of replacing it", "[native]") {
    EmptyCookPublicationFixture fixture;
    const std::array<std::uint8_t, 3> malformed{'b', 'a', 'd'};
    WriteFile(fixture.cooked.path / "current.json", malformed);
    const auto result = fixture.service.Cook(fixture.request, {});
    REQUIRE(result.HasError());
    CHECK(std::filesystem::file_size(fixture.cooked.path / "current.json") == malformed.size());
}

TEST_CASE("AssetCookService contains optional history exceptions before and after pointer commit", "[native]") {
    for (const bool committed : {false, true}) {
        for (const bool nonstandard : {false, true}) {
            EmptyCookPublicationFixture fixture;
            CancellationSource cancellation;
            auto files = std::make_shared<CookPublicationFiles>();
            files->cancellation = &cancellation;
            files->cancelAfterCommit = committed;
            fixture.request.publicationFiles = files;
            auto sink = std::make_shared<ThrowingCookHistorySink>();
            sink->nonstandard = nonstandard;
            OperationStore operations{4, 4, sink};
            fixture.request.operationStore = &operations;
            const auto result = fixture.service.Cook(fixture.request, cancellation.Token());
            CHECK(result.HasValue() == committed);
            CHECK(sink->attempted == 1);
            const auto snapshot = operations.SnapshotIfChanged(0);
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->operations.front().state == (committed ? OperationState::Succeeded : OperationState::Cancelled));
            CHECK(ResolveCurrentCookGeneration(fixture.cooked.path).HasValue() == committed);
        }
    }
}
