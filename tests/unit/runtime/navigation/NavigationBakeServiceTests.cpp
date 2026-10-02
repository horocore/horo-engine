#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "navigation/IncrementalBakeFixture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

namespace Horo::Application {
    using namespace Horo::Navigation;
    using namespace Horo::Navigation::TestSupport;

    namespace {
        struct TempDirectory {
            std::filesystem::path path =
                std::filesystem::temp_directory_path() /
                ("horo incremental bake " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

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
        class ControlledFiles final : public DurableFileSystem {
        public:
            NativeDurableFileSystem native;
            std::atomic<bool> failReplacement{};
            std::atomic<bool> failAfterReplacement{};
            std::atomic<bool> holdCurrent{};
            std::atomic<bool> currentStaged{};

            Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
                return native.TryAcquireExclusive(path, owner);
            }

            Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
                return native.AvailableBytes(path);
            }

            Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
                auto written = native.WriteDurable(path, bytes);
                if (path.filename().string().starts_with("current.json.tmp.")) {
                    currentStaged.store(true);
                    while (holdCurrent.load())
                        std::this_thread::yield();
                }
                return written;
            }

            Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
                return native.CopyDurable(source, destination);
            }

            Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
                const bool current = destination.filename() == "current.json";
                if (current && failReplacement.load())
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputFailed));
                auto replaced = native.AtomicReplace(prepared, destination);
                if (current && replaced.HasValue() && failAfterReplacement.load())
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputFailed));
                return replaced;
            }

            Result<void> RemoveDurable(const std::filesystem::path &path) override {
                return native.RemoveDurable(path);
            }

            Result<void> SyncDirectory(const std::filesystem::path &path) override {
                return native.SyncDirectory(path);
            }
        };

        [[nodiscard]] NavigationBakeServiceConfig Config(const TempDirectory &directory,
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
                               .childDrainTimeout = Duration::FromMilliseconds(2000)}};
        }

        [[nodiscard]] OperationRecord Terminal(NavigationBakeService &service, OperationStore &operations, OperationId id) {
            for (std::size_t i = 0; i < 5000; ++i) {
                service.Pump();
                const auto snapshot = operations.SnapshotIfChanged(0);
                const auto found = std::ranges::find(snapshot->operations, id, &OperationRecord::id);
                if (found != snapshot->operations.end() && found->finishedAt)
                    return *found;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            FAIL("Navigation bake did not drain within the bounded test deadline");
            return {};
        }

        [[nodiscard]] OperationId Submit(NavigationBakeService &service, const IncrementalBakeFixture &fixture) {
            auto id = service.Submit({.input = fixture.Input(),
                                      .compatibility = fixture.compatibility,
                                      .tiles = fixture.Tiles(),
                                      .sources = fixture.Observations()});
            REQUIRE(id.HasValue());
            return id.Value();
        }

        /** @brief Sends actual cooked topology through the production Detour query API, welding exact shared portal vertices. */
        [[nodiscard]] Result<NavigationPath> Query(const NavigationCookedTileSet &tiles) {
            std::vector<Math::Vec3> vertices;
            std::vector<GroundedNavigationPolygon> polygons;
            for (const auto &tile : tiles.tiles) {
                const auto &topology = tile->Topology();
                for (const auto &polygon : topology.polygons) {
                    GroundedNavigationPolygon grounded{.vertexCount = static_cast<std::uint8_t>(polygon.vertexIndices.count),
                                                       .area = polygon.area,
                                                       .surface = tile->Key().surface};
                    for (std::uint32_t i = 0; i < polygon.vertexIndices.count; ++i) {
                        const auto vertex = topology.vertices[topology.polygonVertexIndices[polygon.vertexIndices.first + i]];
                        auto found = std::ranges::find(vertices, vertex);
                        if (found == vertices.end()) {
                            vertices.push_back(vertex);
                            found = vertices.end() - 1;
                        }
                        grounded.vertexIndices[i] = static_cast<std::uint32_t>(found - vertices.begin());
                    }
                    polygons.push_back(grounded);
                }
            }
            const std::array areas{NavigationAreaDescriptor{.id = Id<NavigationAreaId>(1),
                                                            .source = {.id = Id<NavigationDescriptorSourceId>(1)},
                                                            .flags = {.bits = 1}}};
            const std::array filters{NavigationQueryFilterDescriptor{.id = Id<NavigationFilterId>(1),
                                                                     .source = {.id = Id<NavigationDescriptorSourceId>(1)},
                                                                     .includedFlags = {.bits = 1}}};
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
    }  // namespace

    TEST_CASE("Incremental production cook rebuilds both sides of an edited border and reuses remote content identities") {
        TempDirectory directory;
        auto builder = std::make_shared<ControlledBuilder>();
        OperationStore operations(4, 16);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32});
        auto config = Config(directory, builder);
        auto service = NavigationBakeService::Create(config, operations, jobs).Value();
        IncrementalBakeFixture fixture;
        REQUIRE(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Succeeded);
        const auto before = service->Published();
        REQUIRE(before);
        REQUIRE(before->rebuiltTiles == 4);
        REQUIRE(Query(before->tiles).Value().status == NavigationPathStatus::Reachable);
        fixture.ExcludeBorder();
        REQUIRE(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Succeeded);
        const auto after = service->Published();
        REQUIRE(after);
        CHECK(after->rebuiltTiles == 2);
        CHECK(after->reusedTiles == 2);
        CHECK(builder->builds.load() == 6);
        CHECK(before->tiles.tiles[2] == after->tiles.tiles[2]);
        CHECK(before->tiles.tiles[3]->ContentIdentity() == after->tiles.tiles[3]->ContentIdentity());
        CHECK(Query(after->tiles).Value().status != NavigationPathStatus::Reachable);
        CHECK(Query(before->tiles).Value().status == NavigationPathStatus::Reachable);  // Retained generation remains queryable.
        auto current = Assets::ResolveCurrentCookGeneration(config.targetRoot);
        REQUIRE(current.HasValue());
        auto contents = Assets::ReadCookGenerationContents(current.Value(), config.maximumCandidateBytes);
        REQUIRE(contents.HasValue());
        auto envelope = Assets::DecodeCookedArtifact(contents.Value().artifacts.front());
        REQUIRE(envelope.HasValue());
        auto restored = DecodeNavigationCookedTileSet(envelope.Value().payload, config.maximumCandidateBytes);
        REQUIRE(restored.HasValue());
        CHECK(restored.Value().tiles[3]->ContentIdentity() == after->tiles.tiles[3]->ContentIdentity());
        CHECK(Query(restored.Value()).Value().status != NavigationPathStatus::Reachable);
        service->Close();
        service = NavigationBakeService::Create(config, operations, jobs).Value();
        REQUIRE(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Succeeded);
        CHECK(service->Published()->reusedTiles == 4);
        CHECK(builder->builds.load() == 6);
    }

    TEST_CASE("Latest request replaces pending work and shutdown cancels unadopted native baking") {
        TempDirectory directory;
        auto builder = std::make_shared<ControlledBuilder>();
        builder->pause.store(true);
        OperationStore operations(8, 16);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32});
        auto service = NavigationBakeService::Create(Config(directory, builder), operations, jobs).Value();
        IncrementalBakeFixture fixture;
        const auto first = Submit(*service, fixture);
        for (std::size_t i = 0; i < 2000 && !builder->entered.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(builder->entered.load());
        fixture.ExcludeBorder();
        const auto second = Submit(*service, fixture);
        fixture.ExcludeBorder(16);
        const auto third = Submit(*service, fixture);
        builder->pause.store(false);
        CHECK(Terminal(*service, operations, first).state == OperationState::Cancelled);
        CHECK(Terminal(*service, operations, second).state == OperationState::Cancelled);
        CHECK(Terminal(*service, operations, third).state == OperationState::Succeeded);
        const auto last = service->Published();
        builder->pause.store(true);
        builder->entered.store(false);
        fixture.compatibility.provider = Digest(80);
        const auto closing = Submit(*service, fixture);
        for (std::size_t i = 0; i < 2000 && !builder->entered.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(builder->entered.load());
        service->Close();
        CHECK(Terminal(*service, operations, closing).state == OperationState::Cancelled);
        CHECK(service->Published() == last);
        CHECK(service->Submit({.input = fixture.Input(), .compatibility = fixture.compatibility, .tiles = fixture.Tiles()}).HasError());
    }

    TEST_CASE("Malformed current authority fails incremental publication and preserves the last valid lease") {
        TempDirectory directory;
        auto builder = std::make_shared<ControlledBuilder>();
        OperationStore operations(4, 16);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32});
        auto config = Config(directory, builder);
        auto service = NavigationBakeService::Create(config, operations, jobs).Value();
        IncrementalBakeFixture fixture;
        REQUIRE(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Succeeded);
        const auto before = service->Published();
        {
            std::ofstream current(config.targetRoot / "current.json", std::ios::trunc);
            current << "invalid";
        }
        fixture.ExcludeBorder();
        CHECK(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Failed);
        CHECK(service->Published() == before);
        CHECK(Assets::ResolveCurrentCookGeneration(config.targetRoot).HasError());
    }

    TEST_CASE("Source invalidation and replacement failure preserve current while post-rename failure reports committed truth") {
        TempDirectory directory;
        auto builder = std::make_shared<ControlledBuilder>();
        auto files = std::make_shared<ControlledFiles>();
        OperationStore operations(4, 16);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32});
        auto config = Config(directory, builder);
        config.files = files;
        auto service = NavigationBakeService::Create(config, operations, jobs).Value();
        IncrementalBakeFixture fixture;
        REQUIRE(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Succeeded);
        const auto before = service->Published();
        fixture.ExcludeBorder();
        files->holdCurrent.store(true);
        files->currentStaged.store(false);
        const auto stale = Submit(*service, fixture);
        for (std::size_t i = 0; i < 2000 && !files->currentStaged.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool staged = files->currentStaged.load();
        service->Invalidate();
        files->holdCurrent.store(false);
        REQUIRE(staged);
        CHECK(Terminal(*service, operations, stale).state == OperationState::Cancelled);
        CHECK(service->Published() == before);
        CHECK(Assets::ResolveCurrentCookGeneration(config.targetRoot).Value().manifestDigest == before->generation.manifestDigest);
        files->failReplacement.store(true);
        CHECK(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Failed);
        CHECK(service->Published() == before);
        CHECK(Assets::ResolveCurrentCookGeneration(config.targetRoot).Value().manifestDigest == before->generation.manifestDigest);
        files->failReplacement.store(false);
        files->failAfterReplacement.store(true);
        REQUIRE(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Succeeded);
        REQUIRE(service->Published()->generation.durabilityError);
        CHECK(Assets::ResolveCurrentCookGeneration(config.targetRoot).Value().manifestDigest ==
              service->Published()->generation.manifestDigest);
    }

    TEST_CASE("Production cache reuse rejects valid foreign source envelopes and unchanged producer digests cannot hide edits") {
        TempDirectory directory;
        auto builder = std::make_shared<ControlledBuilder>();
        OperationStore operations(4, 16);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32});
        auto config = Config(directory, builder);
        auto service = NavigationBakeService::Create(config, operations, jobs).Value();
        IncrementalBakeFixture fixture;
        REQUIRE(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Succeeded);
        fixture.vertices.front().y = 0.2F;
        REQUIRE(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Succeeded);
        CHECK(service->Published()->rebuiltTiles == 2);
        CHECK(service->Published()->reusedTiles == 2);
        const auto before = service->Published();
        service->Close();
        for (const auto &entry : std::filesystem::recursive_directory_iterator(config.cacheRoot)) {
            if (entry.path().extension() != ".cooked")
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
            auto decoded = Assets::DecodeCookedArtifact(bytes);
            REQUIRE(decoded.HasValue());
            auto envelope = std::move(decoded).Value();
            envelope.sourceDigest = Digest(90);
            auto encoded = Assets::EncodeCookedArtifact(envelope).Value();
            file.close();
            std::ofstream output(entry.path(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        }
        service = NavigationBakeService::Create(config, operations, jobs).Value();
        CHECK(Terminal(*service, operations, Submit(*service, fixture)).state == OperationState::Failed);
        CHECK_FALSE(service->Published());
        CHECK(Assets::ResolveCurrentCookGeneration(config.targetRoot).Value().manifestDigest == before->generation.manifestDigest);
    }
}  // namespace Horo::Application
