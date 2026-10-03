#include "Horo/Assets/AssetArchive.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationAssetSceneActivation.h"
#include "Horo/Navigation/NavigationRuntimeQueues.h"
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
        SECTION("stale generated surface generation") {
            harness.Update(Cooked(9));
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

    TEST_CASE("Canonical dependency closure is verified against exact prepared Scene content",
              "[unit][navigation][navmesh_asset][dependencies]") {
        Harness harness;
        const auto depType = Assets::AssetTypeId::Parse("core.mesh").Value();
        const std::array<std::uint8_t, 1> payload{42};
        const auto depBytes = Envelope(payload, Asset(true), depType);
        const std::array dependencies{NavMeshAssetDependency{{Asset(true), depType}, ComputeSha256(std::as_bytes(std::span{depBytes}))}};
        harness.provider.Insert(Asset(true), depBytes);
        harness.provider.Insert(Asset(), Cooked(1, dependencies));
        REQUIRE(harness.registry.Publish({Record(Asset()), Record(Asset(true), depType)}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        REQUIRE(!harness.Activate(Definition(1, 1, dependencies)).has_value());
        const auto first = std::move(harness.participant->Acquire()).Value();
        bool declareDependency = true;
        harness.provider.Insert(Asset(), Cooked(2, dependencies));
        SECTION("undeclared dependency is not resolved by hidden I/O") {
            declareDependency = false;
        }
        SECTION("changed dependency bytes reject the generated content evidence") {
            const std::array<std::uint8_t, 1> changed{43};
            harness.provider.Insert(Asset(true), Envelope(changed, Asset(true), depType));
        }
        REQUIRE(harness.registry.Publish({Record(Asset()), Record(Asset(true), depType)}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        const auto declared =
            declareDependency ? std::span<const NavMeshAssetDependency>{dependencies} : std::span<const NavMeshAssetDependency>{};
        REQUIRE(harness.Activate(Definition(2, 2, declared)).has_value());
        REQUIRE(harness.participant->Acquire().Value().Descriptor() == first.Descriptor());
        REQUIRE(!first.IsRevoked());
    }

    TEST_CASE("Editor filesystem and packaged archive resolve identical canonical NavMesh content",
              "[unit][navigation][navmesh_asset][package]") {
        const auto bytes = Cooked();
        Assets::AssetRegistry registry;
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
        REQUIRE(editor.Value().partitions.front().data.Header() == packaged.Value().partitions.front().data.Header());
        const std::array<Assets::IAssetProvider *, 2> transports{&filesystem, &archive};
        for (auto *transport : transports) {
            JobSystem jobs{JobSystemConfig{1, 8}};
            Assets::AssetLoadService loads{jobs, *transport};
            Runtime::RuntimeSceneService service{registry, loads};
            auto participant = std::make_unique<NavigationAssetSceneActivationParticipant>(*cache, Target(), NativeFactory);
            auto *registered = participant.get();
            REQUIRE(service.AddActivationParticipant(std::move(participant)).HasValue());
            REQUIRE(service.Startup(cancellation.Token()).HasValue());
            REQUIRE(!ActivateService(service, cancellation.Token(), Definition()).has_value());
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
        const auto fixture = GroundMesh();
        const NavMeshAssetPartitionInput input{Id<SurfaceId>(101), 1, fixture.View()};
        const std::array duplicates{input, input};
        REQUIRE(EncodeNavMeshAssetPayload(duplicates, {}).HasError());
        auto cache = std::move(Assets::AssetPayloadCache::Create(8, 4096)).Value();
        const std::array inputs{input};
        const auto payload = EncodeNavMeshAssetPayload(inputs, {});
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
}  // namespace Horo::Navigation
