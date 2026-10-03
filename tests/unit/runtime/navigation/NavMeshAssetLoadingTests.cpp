#include "Horo/Application/NavigationBakeService.h"
#include "Horo/Assets/AssetArchive.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationAssetSceneActivation.h"
#include "Horo/Navigation/NavigationRuntimeQueues.h"
#include "navigation/IncrementalBakeFixture.h"
#include "navigation/NavMeshAssetTestFixtures.h"
#include "navigation/NavigationRuntimeTestFixtures.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace Horo::Navigation {
    using namespace AssetTestSupport;

    namespace {
        /** @brief Execute the authoritative native producer and read its durable published generation. */
        [[nodiscard]] std::vector<std::uint8_t> BakeCanonicalContent(const std::filesystem::path &projectRoot) {
            TestSupport::IncrementalBakeFixture input;
            OperationStore operations{8, 16};
            JobSystem bakeJobs{{.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 32}};
            Application::NavigationBakeServiceConfig config{.definition = Asset(),
                                                            .artifactType = Type(),
                                                            .target = Target(),
                                                            .cacheRoot = projectRoot / "tile-cache",
                                                            .targetRoot = projectRoot / "cook-output",
                                                            .builder = CreateRecastDetourNavigationMeshBuilder().Value(),
                                                            .files = std::make_shared<NativeDurableFileSystem>(),
                                                            .budget = {1, 1024ULL * 1024ULL * 1024ULL, 128U * 1024U * 1024U, 8,
                                                                       1024ULL * 1024ULL * 1024ULL, Duration::FromMilliseconds(2000)}};
            auto bake = Application::NavigationBakeService::Create(config, operations, bakeJobs).Value();
            REQUIRE(bake->Submit({.input = input.Input(),
                                  .compatibility = input.compatibility,
                                  .tiles = input.Tiles(),
                                  .sources = input.Observations()})
                        .HasValue());
            for (std::size_t iteration = 0; iteration < 5000 && !bake->Published(); ++iteration) {
                bake->Pump();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE(bake->Published());
            const auto current = Assets::ResolveCurrentCookGeneration(config.targetRoot);
            REQUIRE(current.HasValue());
            REQUIRE(current.Value().manifestDigest == bake->Published()->generation.manifestDigest);
            const auto contents = Assets::ReadCookGenerationContents(current.Value(), config.maximumCandidateBytes);
            REQUIRE(contents.HasValue());
            REQUIRE(contents.Value().artifacts.size() == 1);
            return contents.Value().artifacts.front();
        }
    }  // namespace

    TEST_CASE("Canonical NavMesh registry and provider loading deduplicate exact immutable tile allocations",
              "[unit][navigation][navmesh_asset]") {
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({Record(Asset()), Record(Asset(true))}).status == Assets::AssetRegistryBuildStatus::Complete);
        Assets::MemoryAssetProvider provider;
        const auto cooked = Cooked();
        provider.Insert(Asset(), cooked);
        auto cache = std::move(Assets::AssetPayloadCache::Create(8, 4096)).Value();
        CancellationSource cancellation;
        auto first = LoadNavMeshAsset(registry.Snapshot(), provider, Asset(), Target(), *cache, cancellation.Token());
        REQUIRE(first.HasValue());
        const auto envelope = Assets::DecodeCookedArtifact(cooked);
        provider.Insert(Asset(true), Envelope(envelope.Value().payload, Asset(true)));
        auto second = LoadNavMeshAsset(registry.Snapshot(), provider, Asset(true), Target(), *cache, cancellation.Token());
        REQUIRE(second.HasValue());
        REQUIRE(first.Value().tileBytes.front().SharesAllocationWith(second.Value().tileBytes.front()));
        REQUIRE(cache->Snapshot().residentEntries == 1);
        REQUIRE(first.Value().cookedContentDigest == ComputeSha256(std::as_bytes(std::span{cooked})));
        REQUIRE(first.Value().registryRevision == registry.Snapshot().Revision());
        REQUIRE(first.Value().sourceDigest == envelope.Value().sourceDigest);
        REQUIRE(first.Value().cacheKeyDigest == envelope.Value().cacheKeyDigest);
        REQUIRE(first.Value().partitions.front().surface == Id<SurfaceId>(101));
        cancellation.RequestCancellation();
        REQUIRE(LoadNavMeshAsset(registry.Snapshot(), provider, Asset(), Target(), *cache, cancellation.Token()).HasError());
    }

    TEST_CASE("Scene activation pins real Detour queries through eviction replacement and shutdown",
              "[unit][navigation][navmesh_asset][lifecycle]") {
        Harness harness;
        REQUIRE(!harness.Activate(Definition()).has_value());
        auto first = std::move(harness.participant->Acquire()).Value();
        const auto path = first.Backend().FindPath(PathRequest(first), first.Cancellation());
        REQUIRE(path.HasValue());
        REQUIRE(!path.Value().points.empty());
        REQUIRE(harness.participant->ActiveAssetProvenance().front().id == Asset());
        REQUIRE(harness.participant->ActiveAssetProvenance().front().registryRevision == harness.registry.Snapshot().Revision());
        const auto charged = harness.cache->Snapshot().retainedPayloadBytes;
        const auto digest = std::move(LoadNavMeshAsset(harness.registry.Snapshot(), harness.provider, Asset(), Target(), *harness.cache,
                                                       harness.cancellation.Token()))
                                .Value()
                                .tileBytes.front()
                                .Digest();
        harness.cache->Evict(digest);
        REQUIRE(harness.cache->Snapshot().residentPayloadBytes == 0);
        REQUIRE(harness.cache->Snapshot().retainedPayloadBytes == charged);
        REQUIRE(first.Backend().FindPath(PathRequest(first), first.Cancellation()).HasValue());
        harness.Update(Cooked(2));
        REQUIRE(!harness.Activate(Definition(2, 2)).has_value());
        REQUIRE(first.IsRevoked());
        REQUIRE(first.Cancellation().IsCancellationRequested());
        REQUIRE(first.Backend().FindPath(PathRequest(first), first.Cancellation()).HasError());
        auto second = std::move(harness.participant->Acquire()).Value();
        REQUIRE(second.Descriptor().world != first.Descriptor().world);
        REQUIRE(second.Backend().FindPath(PathRequest(second), second.Cancellation()).HasValue());
        REQUIRE(harness.participant->Snapshot().liveWorlds == 2);
        REQUIRE(harness.participant->Snapshot().reservedProviderBytes == 2 * ProviderReservation);
        harness.cache->Evict(digest);
        harness.service.Shutdown();
        REQUIRE(second.IsRevoked());
        REQUIRE(harness.participant->Acquire().HasError());
        REQUIRE(harness.participant->ActiveAssetProvenance().empty());
        REQUIRE(harness.cache->Snapshot().retainedPayloadBytes == charged);
        first = {};
        second = {};
        REQUIRE(harness.participant->Snapshot().liveWorlds == 0);
        REQUIRE(harness.participant->Snapshot().reservedProviderBytes == 0);
        REQUIRE(harness.cache->Snapshot().retainedPayloadBytes == 0);
    }

    TEST_CASE("Pinned asset backend preserves every existing spatial query seam and stable surface provenance",
              "[unit][navigation][navmesh_asset][provider]") {
        Harness harness;
        REQUIRE(!harness.Activate(Definition()).has_value());
        const auto lease = std::move(harness.participant->Acquire()).Value();
        const auto world = lease.Descriptor().world;
        const auto topology = lease.Descriptor().topology;
        const auto requirement = [](const NavigationQueryKind kind) {
            return NavigationQueryRequirement{.query = kind,
                                              .quality = NavigationQualityLevel::Balanced,
                                              .limits = {.maximumNodeExpansions = 32,
                                                         .maximumResultPoints = 8,
                                                         .maximumSearchDistanceMeters = 100}};
        };
        const auto projected =
            lease.Backend().ProjectPoint({world, topology, {1, 1, 1}, {2, 2, 2}, requirement(NavigationQueryKind::NearestPoint)},
                                         lease.Cancellation());
        REQUIRE(projected.HasValue());
        const auto sampled =
            lease.Backend().SamplePosition({world, topology, {1, 0, 1}, 3, requirement(NavigationQueryKind::SamplePosition)},
                                           lease.Cancellation());
        REQUIRE(sampled.HasValue());
        REQUIRE(!sampled.Value().samples.empty());
        REQUIRE(sampled.Value().samples.front().surface == Id<SurfaceId>(101));
        const auto ray = lease.Backend().Raycast({world, topology, {1, 0, 1}, {2, 0, 2}, requirement(NavigationQueryKind::Raycast)},
                                                 lease.Cancellation());
        REQUIRE(ray.HasValue());
        const auto polygons =
            lease.Backend().QueryPolygons({world, topology, {2, 0, 2}, {4, 1, 4}, requirement(NavigationQueryKind::PolygonQuery)},
                                          lease.Cancellation());
        REQUIRE(polygons.HasValue());
    }

    TEST_CASE("Missing corrupt stale or failed provider candidates preserve prior Scene and navigation world",
              "[unit][navigation][navmesh_asset][rollback]") {
        Harness harness;
        REQUIRE(!harness.Activate(Definition()).has_value());
        const auto first = std::move(harness.participant->Acquire()).Value();
        const auto active = harness.service.ActiveScene()->RuntimeId();
        const ErrorCodeDescriptor *expectedError{};
        SECTION("missing canonical provider artifact") {
            harness.Update(Cooked());
            harness.provider.Remove(Asset());
        }
        SECTION("corrupt verified envelope") {
            auto bytes = Cooked();
            bytes.back() ^= 1;
            harness.Update(std::move(bytes));
        }
        SECTION("missing required surface partition") {
            const auto payload = EncodeNavigationCookedTileSet(TileSet(Id<SurfaceId>(999)), 4096).Value();
            harness.Update(Envelope(payload));
            expectedError = &NavigationErrors::StaleSnapshot;
        }
        SECTION("unsupported bundle with recomputed canonical envelope checksum") {
            const std::array<std::uint8_t, 1> garbage{0};
            harness.Update(Envelope(garbage));
        }
        const auto failure = harness.Activate(Definition(2, 2));
        REQUIRE(failure.has_value());
        if (expectedError)
            REQUIRE(failure->code.Value() == expectedError->code.Value());
        REQUIRE(harness.service.ActiveScene()->RuntimeId() == active);
        REQUIRE(harness.participant->Acquire().Value().Descriptor() == first.Descriptor());
        REQUIRE(!first.IsRevoked());
        REQUIRE(first.Backend().FindPath(PathRequest(first), first.Cancellation()).HasValue());
    }

    TEST_CASE("Provider reservation remains charged while a retired query pins its world", "[unit][navigation][navmesh_asset][budget]") {
        NavigationAssetSceneLimits limits;
        limits.maximumLiveWorlds = 2;
        limits.maximumReservedProviderBytes = 2 * ProviderReservation;
        Harness harness{limits};
        REQUIRE(!harness.Activate(Definition()).has_value());
        auto first = std::move(harness.participant->Acquire()).Value();
        harness.Update(Cooked(2));
        REQUIRE(!harness.Activate(Definition(2, 2)).has_value());
        harness.Update(Cooked(3));
        REQUIRE(harness.Activate(Definition(3, 3)).has_value());
        REQUIRE(harness.participant->Acquire().Value().Descriptor().world == Id<NavigationWorldId>(2));
        first = {};
        REQUIRE(!harness.Activate(Definition(4, 3)).has_value());
        REQUIRE(harness.participant->Snapshot().liveWorlds == 1);
    }

    TEST_CASE("Later aggregate rejection and native preparation failure preserve the published world",
              "[unit][navigation][navmesh_asset][rollback]") {
        bool failFactory = false;
        bool failPublication = false;
        const NavigationAssetBackendFactory factory = [&](const auto &descriptor, const auto surfaces, const auto budget) {
            return failFactory ? Result<NavigationPreparedAssetBackend>::Failure(MakeError(NavigationErrors::ProviderFailed))
                               : NativeFactory(descriptor, surfaces, budget);
        };
        Harness harness{{}, factory, &failPublication};
        REQUIRE(!harness.Activate(Definition()).has_value());
        const auto lease = std::move(harness.participant->Acquire()).Value();
        SECTION("later aggregate participant rejects publication") {
            failPublication = true;
        }
        SECTION("host selected native factory fails preparation") {
            failFactory = true;
        }
        harness.Update(Cooked(2));
        REQUIRE(harness.Activate(Definition(2, 2)).has_value());
        REQUIRE(harness.participant->Acquire().Value().Descriptor() == lease.Descriptor());
        REQUIRE(!lease.IsRevoked());
        REQUIRE(harness.participant->Snapshot().liveWorlds == 1);
        REQUIRE(lease.Backend().FindPath(PathRequest(lease), lease.Cancellation()).HasValue());
    }

    TEST_CASE("Existing runtime query transport pins asset allocations until the accepted record drains",
              "[unit][navigation][navmesh_asset][queue]") {
        Harness harness;
        REQUIRE(!harness.Activate(Definition()).has_value());
        auto lease = std::move(harness.participant->Acquire()).Value();
        auto queues = std::move(NavigationRuntimeQueues::Create(TestSupport::QueueDescriptor(2))).Value();
        NavigationQueuedQuery query{.acceptedSequence = 1,
                                    .handle = TestSupport::RequestHandle(lease.Descriptor().world),
                                    .request = PathRequest(lease),
                                    .worldLease = lease};
        REQUIRE(queues.TryEnqueueQuery(query) == NavigationQueueEnqueueResult::Enqueued);
        lease = {};
        harness.cache->Shutdown();
        REQUIRE(harness.cache->Snapshot().residentPayloadBytes == 0);
        REQUIRE(harness.cache->Snapshot().retainedPayloadBytes > 0);
        auto accepted = queues.TryDequeueQuery();
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->worldLease.Backend().FindPath(accepted->request, accepted->worldLease.Cancellation()).HasValue());
        harness.service.Shutdown();
        REQUIRE(accepted->worldLease.IsRevoked());
        REQUIRE(harness.participant->Snapshot().liveWorlds == 1);
        accepted.reset();
        REQUIRE(harness.participant->Snapshot().liveWorlds == 0);
        REQUIRE(harness.cache->Snapshot().retainedPayloadBytes == 0);
    }

    TEST_CASE("Editor filesystem and packaged archive resolve identical canonical NavMesh content",
              "[unit][navigation][navmesh_asset][package]") {
        // Execute the authoritative producer before either transport sees a cooked asset.
        Assets::AssetRegistry registry;
        const FilesystemProject staging{Cooked(), registry};
        const auto bytes = BakeCanonicalContent(staging.directory);
        const std::array chunks{
            Assets::AssetChunkDefinition{Assets::AssetChunkId::Parse("base").Value(), Assets::AssetChunkKind::Base, {Asset()}}};
        const auto plan = Assets::AssetChunkPlan::Create(chunks);
        REQUIRE(plan.HasValue());
        const std::array artifacts{Assets::AssetArchiveInput{Asset(), bytes}};
        const auto archiveBytes = Assets::BuildAssetArchive(plan.Value(), Target(), artifacts);
        REQUIRE(archiveBytes.HasValue());
        auto archive = std::move(Assets::AssetArchiveProvider::Open(archiveBytes.Value(), Target())).Value();
        const FilesystemProject project{bytes, registry};
        Assets::FilesystemAssetProvider filesystem{project.directory};
        auto cache = std::move(Assets::AssetPayloadCache::Create(8, 4096)).Value();
        CancellationSource cancellation;
        auto editor = LoadNavMeshAsset(registry.Snapshot(), filesystem, Asset(), Target(), *cache, cancellation.Token());
        auto packaged = LoadNavMeshAsset(registry.Snapshot(), archive, Asset(), Target(), *cache, cancellation.Token());
        REQUIRE(editor.HasValue());
        REQUIRE(packaged.HasValue());
        REQUIRE(editor.Value().id == packaged.Value().id);
        REQUIRE(editor.Value().cookedContentDigest == packaged.Value().cookedContentDigest);
        REQUIRE(editor.Value().tileBytes.front().SharesAllocationWith(packaged.Value().tileBytes.front()));
        REQUIRE(NavMeshProfileDescriptor{.buildGeometry = editor.Value().partitions.front().descriptor.geometry} ==
                NavMeshProfileDescriptor{.buildGeometry = packaged.Value().partitions.front().descriptor.geometry});
        const std::array<Assets::IAssetProvider *, 2> transports{&filesystem, &archive};
        for (auto *transport : transports) {
            JobSystem jobs{JobSystemConfig{1, 8}};
            Assets::AssetLoadService loads{jobs, *transport};
            Runtime::RuntimeSceneService service{registry, loads};
            auto participant = std::make_unique<NavigationAssetSceneActivationParticipant>(*cache, Target(), NativeFactory);
            auto *registered = participant.get();
            REQUIRE(service.AddActivationParticipant(std::move(participant)).HasValue());
            REQUIRE(service.Startup(cancellation.Token()).HasValue());
            REQUIRE(!ActivateService(service, cancellation.Token(),
                                     Definition(1, 17, {}, true, Id<SurfaceId>(1), Id<NavigationAgentProfileId>(1)))
                         .has_value());
            auto lease = std::move(registered->Acquire()).Value();
            REQUIRE(lease.Backend().FindPath(PathRequest(lease), lease.Cancellation()).HasValue());
            REQUIRE(registered->ActiveAssetProvenance().front().cookedContentDigest == editor.Value().cookedContentDigest);
            REQUIRE(registered->ActiveAssetProvenance().front().id == Asset());
            service.Shutdown();
            REQUIRE(lease.IsRevoked());
            REQUIRE(registered->Snapshot().liveWorlds == 1);
            lease = {};
            REQUIRE(registered->Snapshot().liveWorlds == 0);
        }
    }

    TEST_CASE("Disabled navigation surfaces do not demand absent cooked assets", "[unit][navigation][navmesh_asset][scene]") {
        Harness harness;
        harness.provider.Remove(Asset());
        const auto definition = Definition(1, 1, {}, false);
        REQUIRE(definition.AssetDependencies().empty());
        REQUIRE(!harness.Activate(definition).has_value());
        REQUIRE(harness.participant->Acquire().HasError());
        REQUIRE(harness.participant->Snapshot().liveWorlds == 0);
    }

    TEST_CASE("Empty Scene publication revokes the old world and canonical dependency projection rejects conflicts",
              "[unit][navigation][navmesh_asset][scene]") {
        Harness harness;
        REQUIRE(Definition().AssetDependencies().size() == 1);
        REQUIRE(Definition().AssetDependencies().front().expectedType == Type());
        REQUIRE(!harness.Activate(Definition()).has_value());
        auto lease = std::move(harness.participant->Acquire()).Value();
        Runtime::SceneDefinitionBuilder empty{{4}, {2}};
        REQUIRE(!harness.Activate(std::move(std::move(empty).Build()).Value()).has_value());
        REQUIRE(lease.IsRevoked());
        REQUIRE(harness.participant->Acquire().HasError());
        Runtime::SceneDefinitionBuilder conflicting{{4}, {3}};
        auto entity = Definition().Entities().front();
        conflicting.Add(entity);
        REQUIRE(conflicting.RequireAsset({Asset(), Assets::AssetTypeId::Parse("core.mesh").Value()}).HasValue());
        REQUIRE(std::move(conflicting).Build().HasError());
    }

    TEST_CASE("NavMesh producer bundle rejects duplicate partitions and corrupt metadata before admission",
              "[unit][navigation][navmesh_asset][hostile]") {
        auto set = TileSet();
        set.tiles.push_back(set.tiles.front());
        REQUIRE(EncodeNavigationCookedTileSet(set, 4096).HasError());
        auto cache = std::move(Assets::AssetPayloadCache::Create(8, 4096)).Value();
        const auto payload = EncodeNavigationCookedTileSet(TileSet(), 4096);
        REQUIRE(payload.HasValue());
        for (std::size_t length = 0; length < payload.Value().size(); ++length) {
            const auto encoded = Envelope(std::span<const std::uint8_t>{payload.Value()}.first(length));
            REQUIRE(LoadNavMeshAsset({Asset(), Type()}, {1}, encoded, Target(), *cache).HasError());
        }
        REQUIRE(cache->Snapshot().retainedPayloadBytes == 0);
        REQUIRE(LoadNavMeshAsset({Asset(), Type()}, {0}, Cooked(), Target(), *cache).HasError());
        REQUIRE(LoadNavMeshAsset({Asset(true), Type()}, {1}, Cooked(), Target(), *cache).HasError());
        REQUIRE(LoadNavMeshAsset({Asset(), Assets::AssetTypeId::Parse("core.mesh").Value()}, {1}, Cooked(), Target(), *cache).HasError());
        REQUIRE(LoadNavMeshAsset({Asset(), Type()}, {1}, Cooked(), AssetCookTargetId::Parse("windows-x64").Value(), *cache).HasError());
    }

    TEST_CASE("Canonical empty tiles preserve complete closure and exact resolved runtime geometry",
              "[unit][navigation][navmesh_asset][canonical]") {
        auto set = TileSet();
        const auto &built = *set.tiles.front();
        const auto descriptor = ProjectNavigationCookedTileDescriptor(built).Value();
        NavigationPreparedTile input{.tile = {.key = {built.Key().profile, built.Key().surface, {.x = 1}},
                                              .bounds = {{32, -1, 0}, {64, 2, 32}},
                                              .tileSizeMeters = 32},
                                     .geometry = descriptor.geometry,
                                     .borderSizeCells = descriptor.borderSizeCells,
                                     .dependencyKey = Digest(4)};
        input.geometry.heightMeters = 2.3F;
        NavigationTileBuildResult empty{.state = NavigationTileBuildState::Empty, .key = input.tile.key.tile, .bounds = input.tile.bounds};
        auto tile = NavigationCookedTile::Create(input, empty);
        REQUIRE(tile.HasValue());
        auto roundtrip = NavigationCookedTile::Decode(tile.Value()->Bytes());
        REQUIRE(roundtrip.HasValue());
        REQUIRE(ProjectNavigationCookedTileDescriptor(*roundtrip.Value()).Value().geometry.heightMeters == 2.3F);
        REQUIRE(ProjectNavigationCookedTileDescriptor(*roundtrip.Value()).Value().tileSizeMeters == 32);
        REQUIRE(ProjectNavigationCookedTileDescriptor(*roundtrip.Value()).Value().borderSizeCells == 4);
        set.tiles.push_back(tile.Value());
        auto cache = Assets::AssetPayloadCache::Create(8, 4096).Value();
        const auto mismatch = EncodeNavigationCookedTileSet(set, 4096).Value();
        REQUIRE(LoadNavMeshAsset({Asset(), Type()}, {1}, Envelope(mismatch), Target(), *cache).HasError());
        REQUIRE(cache->Snapshot().retainedPayloadBytes == 0);
        input.geometry = descriptor.geometry;
        set.tiles.back() = NavigationCookedTile::Create(input, std::move(empty)).Value();
        const auto payload = EncodeNavigationCookedTileSet(set, 4096).Value();
        auto loaded = LoadNavMeshAsset({Asset(), Type()}, {1}, Envelope(payload), Target(), *cache);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().partitions.size() == 1);
        REQUIRE(loaded.Value().partitions.front().tiles.size() == 2);
        REQUIRE(loaded.Value().partitions.front().tiles.back()->Topology().IsEmpty());
        REQUIRE(loaded.Value().tileBytes.size() == 2);
        REQUIRE(loaded.Value().partitions.front().tiles.front()->ContentIdentity() == built.ContentIdentity());
    }

    TEST_CASE("Canonical source fingerprints and owner bounds close admission before world mutation",
              "[unit][navigation][navmesh_asset][canonical][budget]") {
        auto cache = Assets::AssetPayloadCache::Create(8, 4096).Value();
        auto envelope = Assets::DecodeCookedArtifact(Cooked()).Value();
        envelope.sourceDigest = Digest(99);
        const auto mismatch = Assets::EncodeCookedArtifact(envelope).Value();
        REQUIRE(LoadNavMeshAsset({Asset(), Type()}, {1}, mismatch, Target(), *cache).HasError());
        REQUIRE(cache->Snapshot().retainedPayloadBytes == 0);
        NavMeshAssetLimits limits;
        limits.maximumDecodedBytes = 1;
        REQUIRE(LoadNavMeshAsset({Asset(), Type()}, {1}, Cooked(), Target(), *cache, limits).HasError());
        limits.maximumDecodedBytes = 0;
        REQUIRE(LoadNavMeshAsset({Asset(), Type()}, {1}, Cooked(), Target(), *cache, limits).HasError());
        limits = {};
        limits.maximumPartitions = 0;
        REQUIRE(LoadNavMeshAsset({Asset(), Type()}, {1}, Cooked(), Target(), *cache, limits).HasError());
        auto full = Assets::AssetPayloadCache::Create(1, 1).Value();
        REQUIRE(LoadNavMeshAsset({Asset(), Type()}, {1}, Cooked(), Target(), *full).HasError());
        REQUIRE(full->Snapshot().retainedPayloadBytes == 0);
    }

    TEST_CASE("Runtime Scene generation fences remain independent of reusable baked content",
              "[unit][navigation][navmesh_asset][canonical][lifecycle]") {
        Harness harness;
        REQUIRE(!harness.Activate(Definition()).has_value());
        auto first = harness.participant->Acquire().Value();
        REQUIRE(!harness.Activate(Definition(2, 73)).has_value());
        REQUIRE(first.IsRevoked());
        const auto current = harness.participant->Acquire().Value();
        REQUIRE(current.Backend().FindPath(PathRequest(current), current.Cancellation()).HasValue());
        // A structural invalidation is tested while the owning Scene remains alive.
        auto local = Runtime::RuntimeScene::Create(Definition(8, 73, {}, false), Runtime::SceneRuntimeId{81}).Value();
        const auto old = local->View();
        Runtime::SceneCommandBuffer commands;
        commands.Destroy(*old.Find(Runtime::SceneObjectId{101}));
        REQUIRE(local->Commit(commands).HasValue());
        const auto stale = harness.participant->Prepare(Definition(3, 73), old);
        REQUIRE(stale.HasError());
        REQUIRE(stale.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
        REQUIRE(harness.participant->Acquire().Value().Descriptor() == current.Descriptor());
    }
}  // namespace Horo::Navigation
