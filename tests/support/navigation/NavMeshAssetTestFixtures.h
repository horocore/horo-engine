#pragma once

#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationAssetSceneActivation.h"
#include "navigation/NavMeshArtifactFixture.h"
#include "scene/SceneActivationTestGate.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace Horo::Navigation::AssetTestSupport {

    using TestSupport::ArtifactFixture;
    using TestSupport::Digest;
    using TestSupport::Id;
    constexpr std::size_t ProviderReservation = 1024U * 1024U;

    [[nodiscard]] inline Assets::AssetId Asset(const bool second = false) {
        return Assets::AssetId::Parse(second ? "11112233-4455-6677-8899-aabbccddeeff" : "00112233-4455-6677-8899-aabbccddeeff").Value();
    }

    [[nodiscard]] inline Assets::AssetTypeId Type() {
        return Assets::AssetTypeId::Parse(Assets::NavMeshAssetTypeName).Value();
    }

    [[nodiscard]] inline AssetCookTargetId Target() {
        return AssetCookTargetId::Parse("linux-x64").Value();
    }

    [[nodiscard]] inline Assets::AssetRecord Record(const Assets::AssetId id, const Assets::AssetTypeId &type = Type()) {
        const auto name = id.ToString();
        return {id, type, ProjectPath::Parse("assets/" + name + ".horoasset").Value(),
                ProjectPath::Parse("assets/" + name + ".horoasset.horo").Value()};
    }

    [[nodiscard]] inline ArtifactFixture GroundMesh() {
        ArtifactFixture fixture;
        fixture.header.coordinateFrame.origin = {};
        fixture.tiles.front().key = {};
        fixture.tiles.front().bounds = {{0, -1, 0}, {32, 2, 32}};
        fixture.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 0, 10}};
        fixture.offMeshLinks.clear();
        fixture.header.offMeshLinkCount = 0;
        fixture.tiles.front().offMeshLinks = {};
        return fixture;
    }

    [[nodiscard]] inline std::vector<std::uint8_t> Envelope(const std::span<const std::uint8_t> payload, const Assets::AssetId id = Asset(),
                                                            const Assets::AssetTypeId &type = Type(),
                                                            const AssetCookTargetId &target = Target()) {
        Assets::AssetCookArtifact
            artifact{id, type, target, Digest(1), Digest(2), ComputeSha256(std::as_bytes(payload)), {payload.begin(), payload.end()}};
        return std::move(Assets::EncodeCookedArtifact(artifact)).Value();
    }

    /** @brief The same canonical HNT1/HNS1 format used by the real producer; generations do not change bytes. */
    [[nodiscard]] inline NavigationCookedTileSet TileSet(const SurfaceId surface = Id<SurfaceId>(101)) {
        const auto fixture = GroundMesh();
        NavigationPreparedTile input{.tile = {.key = {Id<NavigationAgentProfileId>(7), surface, {}},
                                              .bounds = fixture.tiles.front().bounds,
                                              .tileSizeMeters = 32},
                                     .geometry = fixture.header.profile.buildGeometry,
                                     .borderSizeCells = 4,
                                     .dependencyKey = Digest(3)};
        input.geometry.cellSizeMeters = 0.5F;
        input.geometry.radiusMeters = 0.5F;
        NavigationTileBuildResult topology{.state = NavigationTileBuildState::Built,
                                           .key = {},
                                           .bounds = input.tile.bounds,
                                           .vertices = fixture.vertices,
                                           .polygons = fixture.polygons,
                                           .polygonVertexIndices = fixture.polygonVertexIndices,
                                           .polygonAdjacencies = fixture.polygonAdjacencies,
                                           .provenance = fixture.provenance};
        auto tile = NavigationCookedTile::Create(input, std::move(topology));
        REQUIRE(tile.HasValue());
        return {Digest(2), {std::move(tile).Value()}};
    }

    [[nodiscard]] inline std::vector<std::uint8_t> Cooked(const std::uint64_t = 1) {
        const auto payload = EncodeNavigationCookedTileSet(TileSet(), 4096);
        REQUIRE(payload.HasValue());
        return Envelope(payload.Value());
    }

    [[nodiscard]] inline Runtime::RuntimeSceneDefinition Definition(
        const std::uint64_t revision = 1, const std::uint64_t generation = 1,
        const std::span<const Assets::AssetDependency> dependencies = {}, const bool enabled = true,
        const SurfaceId surface = Id<SurfaceId>(101), const NavigationAgentProfileId profile = Id<NavigationAgentProfileId>(7)) {
        Runtime::SceneDefinitionBuilder builder{{4}, {revision}};
        Runtime::RuntimeEntityDefinition entity;
        entity.object = {101};
        entity.components.navigationSurface = Runtime::NavigationSurfaceComponent{.id = surface,
                                                                                  .definition = Asset(),
                                                                                  .generation = generation,
                                                                                  .profiles = {profile},
                                                                                  .enabled = enabled};
        builder.Add(std::move(entity));
        for (const auto &dependency : dependencies)
            REQUIRE(builder.RequireAsset(dependency).HasValue());
        return std::move(std::move(builder).Build()).Value();
    }

    /** @brief Convert already validated neutral rows without truncating unsupported provider features. */
    [[nodiscard]] inline Result<void> ConvertGroundMesh(const LoadedNavMeshPartition &partition, std::vector<Math::Vec3> &vertices,
                                                        std::vector<GroundedNavigationPolygon> &polygons) {
        for (const auto &tile : partition.tiles) {
            const auto &topology = tile->Topology();
            if (!topology.offMeshLinks.empty())
                return Result<void>::Failure(MakeError(NavigationErrors::OperationUnsupported));
            const auto offset = static_cast<std::uint32_t>(vertices.size());
            vertices.insert(vertices.end(), topology.vertices.begin(), topology.vertices.end());
            for (const auto &row : topology.polygons) {
                if (row.vertexIndices.count > 6)
                    return Result<void>::Failure(MakeError(NavigationErrors::OperationUnsupported));
                GroundedNavigationPolygon polygon{.vertexCount = static_cast<std::uint8_t>(row.vertexIndices.count),
                                                  .area = row.area,
                                                  .surface = partition.surface};
                for (std::size_t index = 0; index < row.vertexIndices.count; ++index)
                    polygon.vertexIndices[index] = offset + topology.polygonVertexIndices[row.vertexIndices.first + index];
                polygons.push_back(polygon);
            }
        }
        return Result<void>::Success();
    }

    /** @brief Actual Detour qualification factory; rejects unsupported semantic features explicitly. */
    [[nodiscard]] inline Result<NavigationPreparedAssetBackend> NativeFactory(const NavigationWorldActivationDescriptor &descriptor,
                                                                              const std::span<const NavigationLoadedSurface> surfaces,
                                                                              const std::size_t budget) {
        if (surfaces.size() != 1 || budget < ProviderReservation) {
            return Result<NavigationPreparedAssetBackend>::Failure(MakeError(NavigationErrors::CapacityExceeded));
        }
        const auto &partition = *surfaces.front().partition;
        std::vector<Math::Vec3> vertices;
        std::vector<GroundedNavigationPolygon> polygons;
        const auto converted = ConvertGroundMesh(partition, vertices, polygons);
        if (converted.HasError())
            return Result<NavigationPreparedAssetBackend>::Failure(converted.ErrorValue());
        const std::array areas{NavigationAreaDescriptor{.id = Id<NavigationAreaId>(11),
                                                        .source = {.kind = NavigationDescriptorSourceKind::Project,
                                                                   .id = Id<NavigationDescriptorSourceId>(1)},
                                                        .traversalCost = 1,
                                                        .flags = {.bits = 1}},
                               NavigationAreaDescriptor{.id = Id<NavigationAreaId>(1),
                                                        .source = {.kind = NavigationDescriptorSourceKind::Project,
                                                                   .id = Id<NavigationDescriptorSourceId>(1)},
                                                        .traversalCost = 1,
                                                        .flags = {.bits = 1}}};
        const std::array filters{NavigationQueryFilterDescriptor{.id = Id<NavigationFilterId>(1),
                                                                 .source = {.kind = NavigationDescriptorSourceKind::Project,
                                                                            .id = Id<NavigationDescriptorSourceId>(1)}}};
        const auto &geometry = partition.descriptor.geometry;
        RecastDetourProviderCreateInfo info{.world = descriptor.world,
                                            .topology = descriptor.topology,
                                            .vertices = vertices,
                                            .polygons = polygons,
                                            .cellSizeMeters = geometry.cellSizeMeters,
                                            .cellHeightMeters = geometry.cellHeightMeters,
                                            .walkableHeightMeters = geometry.heightMeters,
                                            .walkableRadiusMeters = geometry.radiusMeters,
                                            .walkableClimbMeters = geometry.stepHeightMeters,
                                            .maximumQueryNodes = 64,
                                            .maximumResultPoints = 16,
                                            .maximumConcurrentQueries = 2,
                                            .maximumOwnedBytes = ProviderReservation,
                                            .areas = areas,
                                            .filters = filters};
        auto backend = CreateRecastDetourNavigationQueryBackend(info);
        if (backend.HasError())
            return Result<NavigationPreparedAssetBackend>::Failure(backend.ErrorValue());
        return Result<NavigationPreparedAssetBackend>::Success({std::move(backend).Value(), ProviderReservation});
    }

    [[nodiscard]] inline NavigationPathRequest PathRequest(const NavigationWorldReadLease &lease) {
        return {.world = lease.Descriptor().world,
                .topology = lease.Descriptor().topology,
                .start = {1, 0, 1},
                .destination = {2, 0, 2},
                .filter = Id<NavigationFilterId>(1),
                .coveragePolicy = NavigationPathCoveragePolicy::RequireComplete,
                .requirement = {.query = NavigationQueryKind::Path,
                                .quality = NavigationQualityLevel::Balanced,
                                .limits = {.maximumNodeExpansions = 32, .maximumResultPoints = 8, .maximumSearchDistanceMeters = 100}}};
    }

    /** @brief Exercise the actual asynchronous Scene provider service and aggregate safe point. */
    [[nodiscard]] inline std::optional<Error> ActivateService(Runtime::RuntimeSceneService &service, const CancellationToken &token,
                                                              Runtime::RuntimeSceneDefinition definition) {
        const auto revision = definition.Revision();
        const auto queued = service.QueuePreparation(std::move(definition));
        if (queued.HasError())
            return queued.ErrorValue();
        const Runtime::FrameContext context{1, {}, 0, 0, {}, false, token};
        for (std::size_t iteration = 0; iteration < 2000; ++iteration) {
            REQUIRE(service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, context).HasValue());
            if (auto error = service.TakeOperationError())
                return error;
            if (service.ActiveScene() && service.ActiveScene()->DefinitionRevision() == revision)
                return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        FAIL("Bounded Scene preparation did not complete");
        return std::nullopt;
    }

    /** @brief Actual committed sidecar and cooked filesystem fixture, with an isolated lifetime-owned directory. */
    struct FilesystemProject final {
        std::filesystem::path directory{
            std::filesystem::temp_directory_path() /
            ("horo-nav-asset-parity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};

        FilesystemProject(const std::span<const std::uint8_t> bytes, Assets::AssetRegistry &registry) {
            REQUIRE(std::filesystem::create_directory(directory));
            REQUIRE(std::filesystem::create_directory(directory / "assets"));
            std::ofstream file{directory / (Asset().ToString() + ".cooked"), std::ios::binary};
            file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.close();
            std::ofstream source{directory / "assets/navigation.horoasset"};
            source << "authored definition";
            source.close();
            std::ofstream sidecar{directory / "assets/navigation.horoasset.horo"};
            sidecar << "{\"schemaVersion\":1,\"assetId\":\"" << Asset().ToString() << "\",\"assetType\":\"core.navmesh\"}";
            sidecar.close();
            const auto rebuilt = Assets::RebuildAssetRegistry(registry, directory, Assets::AssetRegistryOpenMode::ReadOnly);
            REQUIRE(rebuilt.HasValue());
            REQUIRE(rebuilt.Value().status == Assets::AssetRegistryBuildStatus::Complete);
            REQUIRE(registry.Snapshot().Find(Asset())->type == Type());
            REQUIRE(registry.Snapshot().Find(Asset())->sourcePath.String() == "assets/navigation.horoasset");
        }

        ~FilesystemProject() {
            std::filesystem::remove_all(directory);
        }
    };

    struct Harness final {
        Assets::AssetRegistry registry;
        Assets::MemoryAssetProvider provider;
        JobSystem jobs{JobSystemConfig{2, 16}};
        Assets::AssetLoadService loads{jobs, provider};
        std::unique_ptr<Assets::AssetPayloadCache> cache{std::move(Assets::AssetPayloadCache::Create(16, 4096)).Value()};
        Runtime::RuntimeSceneService service{registry, loads};
        NavigationAssetSceneActivationParticipant *participant{};
        CancellationSource cancellation;

        explicit Harness(const NavigationAssetSceneLimits limits = {}, const NavigationAssetBackendFactory &factory = NativeFactory,
                         const bool *gate = nullptr) {
            auto owned = std::make_unique<NavigationAssetSceneActivationParticipant>(*cache, Target(), factory, limits);
            participant = owned.get();
            REQUIRE(service.AddActivationParticipant(std::move(owned)).HasValue());
            if (gate)
                REQUIRE(service
                            .AddActivationParticipant(
                                std::make_unique<Runtime::TestSupport::PublicationGateParticipant>(*gate, NavigationErrors::StaleSnapshot))
                            .HasValue());
            Update(Cooked());
            REQUIRE(service.Startup(cancellation.Token()).HasValue());
        }

        void Update(std::vector<std::uint8_t> bytes) {
            provider.Insert(Asset(), std::move(bytes));
            REQUIRE(registry.Publish({Record(Asset())}).status == Assets::AssetRegistryBuildStatus::Complete);
        }

        [[nodiscard]] inline std::optional<Error> Activate(Runtime::RuntimeSceneDefinition definition) {
            return ActivateService(service, cancellation.Token(), std::move(definition));
        }
    };

}  // namespace Horo::Navigation::AssetTestSupport
