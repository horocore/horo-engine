#pragma once

/** @file NavigationBakeServiceFixture.h
 * @brief Shared native bake owners, publication faults and real topology query fixtures.
 */

#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Assets/AssetCookTransaction.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "NativePublicationFiles.h"
#include "navigation/IncrementalBakeFixture.h"
#include "navigation/NavigationContentPolicyFixture.h"
#include "navigation/NavigationPublicationEntropy.h"

#include <algorithm>
#include <atomic>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <format>
#include <fstream>
#include <limits>
#include <string_view>
#include <thread>

namespace Horo::Application::BakeTestSupport {
    using namespace Horo::Navigation;
    using namespace Horo::Navigation::TestSupport;

    /** @brief Owns one atomically created private test directory with spaces and Unicode in its path. */
    struct TempDirectory {
        std::filesystem::path path = std::filesystem::current_path() /
                                     std::format("horo incremental bake ü {}", std::chrono::steady_clock::now().time_since_epoch().count());

        TempDirectory() {
            REQUIRE(std::filesystem::create_directory(path));
        }

        TempDirectory(const TempDirectory &) = delete;
        TempDirectory &operator=(const TempDirectory &) = delete;
        TempDirectory(TempDirectory &&) = delete;
        TempDirectory &operator=(TempDirectory &&) = delete;

        ~TempDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    /** @brief Real provider wrapper with deterministic cancellation coordination before the first native build. */
    class ControlledBuilder final : public INavigationMeshBuilder {
    public:
        std::shared_ptr<const INavigationMeshBuilder> native{CreateRecastDetourNavigationMeshBuilder().Value()};
        mutable std::atomic<std::size_t> builds{};
        mutable std::atomic<bool> entered{};
        std::atomic<bool> pause{};

        Result<NavigationTileBuildResult> BuildTile(const NavigationTileBuildRequest &request,
                                                    const CancellationToken &cancel) const override {
            ++builds;
            entered.store(true);
            while (pause.load() && !cancel.IsCancellationRequested())
                std::this_thread::yield();
            return native->BuildTile(request, cancel);
        }
    };

    /** @brief Native durable writer with fault injection at the current-pointer barrier. */
    class ControlledFiles final : public Horo::TestSupport::NativePublicationFiles {
    public:
        std::atomic<bool> failWrites{};
        std::atomic<bool> failReplacement{};
        std::atomic<bool> failAfterReplacement{};
        std::atomic<bool> holdCurrent{};
        std::atomic<bool> currentStaged{};

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            if (failWrites.load())
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputFailed));
            auto written = native.WriteDurable(path, bytes);
            if (const std::string_view content{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
                (path.filename() == "current.json" || path.filename().string().starts_with("current.json.tmp.")) &&
                content.find("generationPath") != std::string_view::npos) {
                currentStaged.store(true);
                while (holdCurrent.load())
                    std::this_thread::yield();
            }
            return written;
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                          AtomicFileReplacementReceipt &receipt) override {
            const bool current = destination.filename() == "current.json";
            if (current && failReplacement.load())
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputFailed));
            auto replaced = native.AtomicReplaceTracked(prepared, destination, receipt);
            if (current && replaced.HasValue() && failAfterReplacement.load())
                return Result<void>::Failure(MakeError(NavigationErrors::BakeInputFailed));
            return replaced;
        }
    };

    [[nodiscard]] inline NavigationBakeServiceConfig Config(const TempDirectory &directory,
                                                            std::shared_ptr<const INavigationMeshBuilder> builder) {
        return {.definition = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value(),
                .artifactType = Assets::AssetTypeId::Parse("core.navmesh").Value(),
                .target = AssetCookTargetId::Parse("headless-null").Value(),
                .cacheRoot = directory.path / "cache",
                .targetRoot = directory.path / "cooked",
                .builder = std::move(builder),
                .files = std::make_shared<NativeDurableFileSystem>(),
                .budget = {.maximumConcurrentJobs = 1,
                           .maximumResidentBytes = 1024ULL * 1024ULL * 1024ULL,
                           .maximumTemporaryBytes = 128U * 1024U * 1024U,
                           .maximumWorkItems = 8,
                           .maximumWorkUnits = 1024ULL * 1024ULL * 1024ULL,
                           .childDrainTimeout = Duration::FromMilliseconds(2000)},
                .sourceAuthority = std::make_shared<NavigationBakeSourceAuthority>(),
                .writerWaitTimeout = Duration::FromMilliseconds(30),
                .newOperationId = TestSupport::NewPublicationOperationId};
    }

    /** @brief Shared owner order keeps operation storage and job execution alive beyond the service facade. */
    struct BakeHarness {
        TempDirectory directory;
        std::shared_ptr<ControlledBuilder> builder{std::make_shared<ControlledBuilder>()};
        OperationStore operations{8, 16};
        JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32}};
        NavigationBakeServiceConfig config{Config(directory, builder)};
        std::unique_ptr<NavigationBakeService> service;
        IncrementalBakeFixture fixture;

        explicit BakeHarness(std::shared_ptr<DurableFileSystem> files = {}) {
            if (files)
                config.files = std::move(files);
            service = NavigationBakeService::Create(config, operations, jobs).Value();
        }
    };

    /** @brief Seeds a valid existing generation whose portable filename predates canonical asset-ID naming. */
    inline void PublishLegacyArtifact(const BakeHarness &harness, const Assets::AssetCookManifestEntry &entry,
                                      const std::vector<std::uint8_t> &artifact) {
        NativeDurableFileSystem files;
        const auto lock = files.TryAcquireExclusive(harness.config.targetRoot / ".cook-writer.lock", "legacy fixture");
        REQUIRE(lock.HasValue());
        const auto manifest = std::format(
            R"({{"schemaVersion":1,"target":"{}","artifacts":[{{"assetId":"{}","assetType":"{}","artifact":"{}","artifactHash":"{}"}}]}})",
            harness.config.target.Value(), entry.assetId.ToString(), entry.assetType.Value(), entry.artifactFile,
            FormatSha256(entry.artifactHash));
        const auto manifestBytes = std::as_bytes(std::span{manifest.data(), manifest.size()});
        const auto manifestHex = FormatSha256(ComputeSha256(manifestBytes)).substr(7);
        const auto generationPath = "generations/" + manifestHex;
        const auto generationRoot = harness.config.targetRoot / generationPath;
        REQUIRE(files.WriteDurable(generationRoot / entry.artifactFile, std::as_bytes(std::span{artifact})).HasValue());
        REQUIRE(files.WriteDurable(generationRoot / "manifest.json", manifestBytes).HasValue());
        const auto current =
            std::format(R"({{"schemaVersion":1,"target":"{}","manifestDigest":"{}","generationPath":"{}","artifactCount":"1"}})",
                        harness.config.target.Value(), manifestHex, generationPath);
        const auto prepared = harness.config.targetRoot / "legacy-current.json";
        REQUIRE(files.WriteDurable(prepared, std::as_bytes(std::span{current.data(), current.size()})).HasValue());
        REQUIRE(files.AtomicReplace(prepared, harness.config.targetRoot / "current.json").HasValue());
    }

    [[nodiscard]] inline OperationRecord Terminal(NavigationBakeService &service, const OperationStore &operations, OperationId id) {
        for (std::size_t i = 0; i < 5000; ++i) {
            service.Pump();
            const auto snapshot = operations.SnapshotIfChanged(0);
            if (const auto found = std::ranges::find(snapshot->operations, id, &OperationRecord::id);
                found != snapshot->operations.end() && found->finishedAt)
                return *found;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FAIL("Navigation bake did not drain within the bounded test deadline");
        return {};
    }

    [[nodiscard]] inline OperationId Submit(BakeHarness &harness) {
        auto &service = *harness.service;
        const auto &fixture = harness.fixture;
        REQUIRE(harness.config.sourceAuthority->UpdateCurrent(fixture.revisions, fixture.Observations()).HasValue());
        auto id = service.Submit({.input = fixture.Input(),
                                  .compatibility = fixture.compatibility,
                                  .tiles = fixture.Tiles(),
                                  .sources = fixture.Observations()});
        REQUIRE(id.HasValue());
        return id.Value();
    }

    /** @brief Reuses exact shared portal vertices when feeding cooked polygons to the production query provider. */
    [[nodiscard]] inline std::uint32_t WeldVertex(std::vector<Math::Vec3> &vertices, const Math::Vec3 vertex) {
        if (const auto found = std::ranges::find(vertices, vertex); found != vertices.end())
            return static_cast<std::uint32_t>(found - vertices.begin());
        vertices.push_back(vertex);
        return static_cast<std::uint32_t>(vertices.size() - 1);
    }

    /** @brief Translates one portable polygon without changing cooked vertex positions. */
    [[nodiscard]] inline GroundedNavigationPolygon QueryPolygon(const NavigationCookedTile &tile, const NavMeshPolygon &polygon,
                                                                std::vector<Math::Vec3> &vertices) {
        const auto &topology = tile.Topology();
        GroundedNavigationPolygon grounded{.vertexCount = static_cast<std::uint8_t>(polygon.vertexIndices.count),
                                           .area = polygon.area,
                                           .surface = tile.Key().surface};
        for (std::uint32_t i = 0; i < polygon.vertexIndices.count; ++i) {
            const auto vertex = topology.vertices[topology.polygonVertexIndices[polygon.vertexIndices.first + i]];
            grounded.vertexIndices[i] = WeldVertex(vertices, vertex);
        }
        return grounded;
    }

    /** @brief Sends actual cooked topology through the production Detour query API. */
    [[nodiscard]] inline Result<NavigationPath> Query(const NavigationCookedTileSet &tiles) {
        std::vector<Math::Vec3> vertices;
        std::vector<GroundedNavigationPolygon> polygons;
        for (const auto &tile : tiles.tiles)
            for (const auto &polygon : tile->Topology().polygons)
                polygons.push_back(QueryPolygon(*tile, polygon, vertices));
        const auto areas = IncrementalBakeFixture::Areas();
        const auto filters = IncrementalBakeFixture::Filters();
        const RecastDetourProviderCreateInfo info{.world = Id<NavigationWorldId>(1),
                                                  .topology = Id<NavigationGeneration>(1),
                                                  .vertices = vertices,
                                                  .polygons = polygons,
                                                  .cellSizeMeters = 0.5F,
                                                  .cellHeightMeters = 0.2F,
                                                  .maximumQueryNodes = 256,
                                                  .maximumResultPoints = 256,
                                                  .areas = areas,
                                                  .filters = filters};
        auto provider = CreateRecastDetourNavigationQueryBackend(info);
        REQUIRE(provider.HasValue());
        return provider.Value()->FindPath({.world = info.world,
                                           .topology = info.topology,
                                           .start = {4, 0, 4},
                                           .destination = {12, 0, 4},
                                           .filter = Id<NavigationFilterId>(1),
                                           .requirement = {.query = NavigationQueryKind::Path,
                                                           .quality = NavigationQualityLevel::Balanced,
                                                           .limits = {.maximumNodeExpansions = 256,
                                                                      .maximumResultPoints = 256,
                                                                      .maximumSearchDistanceMeters = 100}}},
                                          {});
    }
}  // namespace Horo::Application::BakeTestSupport
