#include "TerrainProducerSnapshotTestSupport.h"

#include <limits>
#include <utility>

namespace Horo::Terrain {
    using namespace ProducerSnapshotTests;

    TEST_CASE("Producer projects exact manifest-owned collision and navigation surfaces", "[terrain][producer]") {
        Fixture fixture;
        for (const auto consumer : {TerrainProducerConsumer::Collision, TerrainProducerConsumer::Navigation}) {
            auto request = fixture.Request(consumer);
            const auto captured = CaptureTerrainProducerSnapshot(request);
            REQUIRE(captured.HasValue());
            const auto &snapshot = captured.Value();
            CHECK(snapshot.IsValid());
            CHECK(snapshot.Header() == request.header);
            REQUIRE(snapshot.Meshes().size() == 1);
            CHECK(snapshot.Foliage().empty());
            const auto &mesh = snapshot.Meshes().front();
            CHECK(mesh.sourceAsset == fixture.source.sourceAsset);
            CHECK(mesh.sourceRevision == fixture.source.revision);
            CHECK(mesh.cookFingerprint == fixture.terrain.Fingerprint());
            CHECK(mesh.vertices.front().x == -20);
            CHECK(mesh.vertices.front().y == 1);
            CHECK(mesh.triangles.size() == 32);
            CHECK(snapshot.OwnedBytes() <= request.limits.maximumOwnedBytes);
            CHECK(snapshot.ValidateCurrent(request.header).HasValue());
        }
        auto standalone = fixture.Request();
        standalone.header.cell.reset();
        CHECK(CaptureTerrainProducerSnapshot(standalone).HasValue());
    }

    TEST_CASE("Fully holed producer tiles are explicit empty surfaces without visual fallback", "[terrain][producer]") {
        Fixture fixture(true);
        const auto snapshot = CaptureTerrainProducerSnapshot(fixture.Request());
        REQUIRE(snapshot.HasValue());
        REQUIRE(snapshot.Value().Meshes().size() == 1);
        CHECK_FALSE(snapshot.Value().Meshes().front().vertices.empty());
        CHECK(snapshot.Value().Meshes().front().triangles.empty());
    }

    TEST_CASE("Producer rejects foreign and mixed same-tile cook roots", "[terrain][producer]") {
        Fixture fixture;
        auto request = fixture.Request();
        auto foreignSource = fixture.source;
        foreignSource.dataset = Id<TerrainDatasetId>(2);
        auto foreign = Cook(foreignSource);
        request.terrain = &foreign;
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Stale);
        auto changedSource = fixture.source;
        changedSource.heightsMeters[0] = 20;
        auto changed = Cook(changedSource);
        request.terrain = &changed;
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Stale);
        request.terrain = &fixture.terrain;
        auto wrong = fixture.selections;
        wrong.front().digest = Digest(99);
        request.tiles = wrong;
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Invalid);
        wrong.front() = fixture.selections.front();
        wrong.front().tile.tile.x += 1;
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Invalid);
        wrong.assign(2, fixture.selections.front());
        request.tiles = wrong;
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Invalid);
    }

    TEST_CASE("Producer admits finite complete work and storage before returning output", "[terrain][producer]") {
        Fixture fixture;
        auto request = fixture.Request();
        SECTION("Work ceiling") {
            request.limits.maximumWorkItems = 1;
        }
        SECTION("Byte ceiling") {
            request.limits.maximumOwnedBytes = 1;
        }
        SECTION("Vertex ceiling") {
            request.limits.maximumVertices = 1;
        }
        SECTION("Triangle ceiling") {
            request.limits.maximumTriangles = 1;
        }
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Limit);
    }

    TEST_CASE("Producer rejects malformed identity lifecycle capability and geometry", "[terrain][producer]") {
        Fixture fixture;
        auto request = fixture.Request();
        SECTION("Missing manifest") {
            request.manifest = nullptr;
        }
        SECTION("Invalid request") {
            request.header.request = {};
        }
        SECTION("Invalid limits") {
            request.limits.maximumWorkItems = 0;
        }
        SECTION("Invalid transform") {
            request.header.datasetToWorld.scale.x = 0;
        }
        SECTION("Foreign cell") {
            request.header.cell->partition = Id<WorldStreaming::WorldPartitionId>(12);
        }
        SECTION("Nonfinite vertex") {
            auto &artifact = const_cast<TerrainSourceArtifact &>(fixture.terrain.Artifacts()[1]);
            REQUIRE(artifact.role == TerrainSourceArtifactRole::Collision);
            artifact.vertices.front().x = std::numeric_limits<double>::infinity();
        }
        SECTION("Invalid triangle") {
            auto &artifact = const_cast<TerrainSourceArtifact &>(fixture.terrain.Artifacts()[1]);
            artifact.triangles.front().indices[0] = std::numeric_limits<std::uint32_t>::max();
        }
        SECTION("Empty source coverage") {
            auto &artifact = const_cast<TerrainSourceArtifact &>(fixture.terrain.Artifacts()[1]);
            artifact.triangles.front().endX = artifact.triangles.front().beginX;
        }
        SECTION("Inverted winding") {
            auto &triangle = const_cast<TerrainSourceArtifact &>(fixture.terrain.Artifacts()[1]).triangles.front();
            std::swap(triangle.indices[1], triangle.indices[2]);
        }
        SECTION("Degenerate triangle") {
            auto &triangle = const_cast<TerrainSourceArtifact &>(fixture.terrain.Artifacts()[1]).triangles.front();
            triangle.indices[1] = triangle.indices[0];
        }
        SECTION("Corrupt payload") {
            auto &artifact = const_cast<TerrainSourceArtifact &>(fixture.terrain.Artifacts()[1]);
            REQUIRE_FALSE(artifact.payload.empty());
            artifact.payload.front() ^= 1;
        }
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Invalid);
    }

    TEST_CASE("Consumer capability and authoritative requirement have no silent fallback", "[terrain][producer]") {
        Fixture fixture;
        auto request = fixture.Request();
        request.header.capabilities = {};
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Unavailable);
        request = fixture.Request();
        TerrainPayloadManifestRequest manifestRequest;
        manifestRequest.terrain = &fixture.terrain;
        manifestRequest.content = Rev<TerrainContentRevision>();
        manifestRequest.terrainRequirements.visual = TerrainPayloadRequirement::Required;
        auto visualOnly = GenerateTerrainPayloadManifest(manifestRequest);
        REQUIRE(visualOnly.HasValue());
        request.manifest = &visualOnly.Value();
        request.header.manifestDigest = visualOnly.Value().Digest();
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Unavailable);
        request = fixture.Request();
        request.lifecycle = TerrainRuntimeLifecycle::Closed;
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Closed);
        request.lifecycle = TerrainRuntimeLifecycle::Cancelled;
        ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Cancelled);
        request.lifecycle = TerrainRuntimeLifecycle::Active;
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        ErrorIs(CaptureTerrainProducerSnapshot(request, cancelled.Token()), TerrainProducerErrors::Cancelled);
    }

    TEST_CASE("Producer leases retain memory without granting logical currentness or readiness", "[terrain][producer][lifecycle]") {
        Fixture fixture;
        const auto request = fixture.Request();
        auto first = CaptureTerrainProducerSnapshot(request);
        REQUIRE(first.HasValue());
        TerrainProducerSnapshotOwner owner;
        ErrorIs(owner.Snapshot(), TerrainProducerErrors::Unavailable);
        REQUIRE(owner.Publish(first.Value(), std::nullopt).HasValue());
        auto reader = owner.Snapshot();
        REQUIRE(reader.HasValue());
        auto nextRequest = request;
        nextRequest.header.request = Rev<TerrainProducerRequestGeneration>(2);
        nextRequest.header.revision.residency = Rev<TerrainResidencyRevision>(2);
        nextRequest.header.origin.generation = Rev<WorldStreaming::OriginGeneration>(2);
        nextRequest.header.origin.revision = Rev<WorldStreaming::OriginFrameRevision>(2);
        auto next = CaptureTerrainProducerSnapshot(nextRequest);
        REQUIRE(next.HasValue());
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        ErrorIs(owner.Publish(next.Value(), request.header, cancelled.Token()), TerrainProducerErrors::Cancelled);
        CHECK(owner.Snapshot().Value().Header() == request.header);
        ErrorIs(owner.Publish(next.Value(), std::nullopt), TerrainProducerErrors::Stale);
        REQUIRE(owner.Publish(next.Value(), request.header).HasValue());
        ErrorIs(reader.Value().ValidateCurrent(nextRequest.header), TerrainProducerErrors::Stale);
        CHECK(reader.Value().Meshes().front().vertices.front().y == 1);
        ErrorIs(owner.Publish(first.Value(), nextRequest.header), TerrainProducerErrors::Stale);
        owner.Shutdown();
        owner.Shutdown();
        ErrorIs(owner.Snapshot(), TerrainProducerErrors::Closed);
        ErrorIs(owner.Publish(next.Value(), std::nullopt), TerrainProducerErrors::Closed);
        CHECK(reader.Value().IsValid());
    }

    TEST_CASE("Moved-from producer leases are safe and cannot enter a cache", "[terrain][producer][lifecycle]") {
        Fixture fixture;
        auto captured = CaptureTerrainProducerSnapshot(fixture.Request());
        REQUIRE(captured.HasValue());
        auto snapshot = std::move(captured).Value();
        auto retained = std::move(snapshot);
        CHECK(retained.IsValid());
        CHECK_FALSE(snapshot.IsValid());
        CHECK(snapshot.Meshes().empty());
        CHECK(snapshot.Foliage().empty());
        CHECK(snapshot.OwnedBytes() == 0);
        CHECK_FALSE(snapshot.Header().terrain.IsValid());
        ErrorIs(snapshot.ValidateCurrent(snapshot.Header()), TerrainProducerErrors::Stale);
        TerrainProducerSnapshotOwner owner;
        ErrorIs(owner.Publish(std::move(snapshot), std::nullopt), TerrainProducerErrors::Invalid);
    }

    TEST_CASE("Producer currentness compares every independent host and source fence", "[terrain][producer][lifecycle]") {
        Fixture fixture;
        const auto request = fixture.Request();
        auto snapshot = CaptureTerrainProducerSnapshot(request);
        REQUIRE(snapshot.HasValue());
        auto current = request.header;
        SECTION("Content") {
            current.revision.content = Rev<TerrainContentRevision>(2);
        }
        SECTION("Residency") {
            current.revision.residency = Rev<TerrainResidencyRevision>(2);
        }
        SECTION("Mutation") {
            current.revision.mutation = Rev<TerrainMutationRevision>(2);
        }
        SECTION("Capability") {
            current.revision.capability = Rev<TerrainCapabilityRevision>(2);
        }
        SECTION("Request") {
            current.request = Rev<TerrainProducerRequestGeneration>(2);
        }
        SECTION("Runtime incarnation") {
            current.terrain.slot.generation = 2;
        }
        SECTION("World") {
            current.world = Id<WorldStreaming::WorldPartitionId>(12);
        }
        SECTION("Cell attempt") {
            current.cell->generation = Rev<WorldStreaming::StreamingGeneration>(2);
        }
        SECTION("Partition incarnation") {
            current.cell->epoch = Rev<WorldStreaming::PartitionEpoch>(2);
        }
        SECTION("Origin generation") {
            current.origin.generation = Rev<WorldStreaming::OriginGeneration>(2);
        }
        SECTION("Origin revision") {
            current.origin.revision = Rev<WorldStreaming::OriginFrameRevision>(2);
        }
        SECTION("Origin owner") {
            current.origin.identity = Rev<WorldStreaming::OriginFrameId>(2);
        }
        SECTION("Manifest") {
            current.manifestDigest = Digest(99);
        }
        SECTION("Placement") {
            current.datasetToWorld.translation.x = 10;
        }
        ErrorIs(snapshot.Value().ValidateCurrent(current), TerrainProducerErrors::Stale);
    }

    TEST_CASE("Producer cache rejects origin and cell resurrection within a host scope", "[terrain][producer][lifecycle]") {
        Fixture fixture;
        auto request = fixture.Request();
        request.header.origin.generation = Rev<WorldStreaming::OriginGeneration>(2);
        request.header.origin.revision = Rev<WorldStreaming::OriginFrameRevision>(2);
        request.header.cell->generation = Rev<WorldStreaming::StreamingGeneration>(2);
        const auto first = CaptureTerrainProducerSnapshot(request);
        REQUIRE(first.HasValue());
        TerrainProducerSnapshotOwner owner;
        REQUIRE(owner.Publish(first.Value(), std::nullopt).HasValue());
        auto next = request;
        next.header.request = Rev<TerrainProducerRequestGeneration>(2);
        SECTION("Origin generation regression") {
            next.header.origin.generation = Rev<WorldStreaming::OriginGeneration>();
        }
        SECTION("Origin revision regression") {
            next.header.origin.revision = Rev<WorldStreaming::OriginFrameRevision>();
        }
        SECTION("Cell regression") {
            next.header.cell->generation = Rev<WorldStreaming::StreamingGeneration>();
        }
        SECTION("Different cell") {
            next.header.cell->cell.x += 1;
        }
        SECTION("Different epoch") {
            next.header.cell->epoch = Rev<WorldStreaming::PartitionEpoch>(2);
        }
        SECTION("Skipped attempt") {
            next.header.request = Rev<TerrainProducerRequestGeneration>(3);
        }
        const auto candidate = CaptureTerrainProducerSnapshot(next);
        REQUIRE(candidate.HasValue());
        ErrorIs(owner.Publish(candidate.Value(), request.header), TerrainProducerErrors::Stale);
        CHECK(owner.Snapshot().Value().Header() == request.header);
    }

    TEST_CASE("Detached producer snapshots outlive source roots and integration owner", "[terrain][producer][lifecycle]") {
        const auto detached = [] {
            Fixture fixture;
            const auto request = fixture.Request();
            auto captured = CaptureTerrainProducerSnapshot(request);
            REQUIRE(captured.HasValue());
            TerrainProducerSnapshotOwner owner;
            REQUIRE(owner.Publish(captured.Value(), std::nullopt).HasValue());
            auto reader = owner.Snapshot();
            REQUIRE(reader.HasValue());
            return std::move(reader).Value();
        }();
        CHECK(detached.IsValid());
        REQUIRE(detached.Meshes().size() == 1);
        CHECK(detached.Meshes().front().vertices.front().y == 1);
        CHECK(detached.Meshes().front().triangles.size() == 32);
        CHECK(detached.Header().manifestDigest != Sha256Digest{});
    }

    TEST_CASE("Standalone and cell producer caches preserve inputs through failed replacement and shutdown",
              "[terrain][producer][lifecycle]") {
        Fixture fixture;
        auto request = fixture.Request();
        SECTION("Standalone scope") {
            request.header.cell.reset();
        }
        SECTION("Cell scope") {
            REQUIRE(request.header.cell.has_value());
        }
        const auto captured = CaptureTerrainProducerSnapshot(request);
        REQUIRE(captured.HasValue());
        TerrainProducerSnapshotOwner owner;
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        ErrorIs(owner.Publish(captured.Value(), std::nullopt, cancellation.Token()), TerrainProducerErrors::Cancelled);
        ErrorIs(owner.Snapshot(), TerrainProducerErrors::Unavailable);
        ErrorIs(owner.Publish(captured.Value(), request.header), TerrainProducerErrors::Stale);
        REQUIRE(owner.Publish(captured.Value(), std::nullopt).HasValue());
        auto reader = owner.Snapshot();
        REQUIRE(reader.HasValue());
        auto next = request;
        next.header.request = Rev<TerrainProducerRequestGeneration>(2);
        next.limits.maximumTriangles = 1;
        ErrorIs(CaptureTerrainProducerSnapshot(next), TerrainProducerErrors::Limit);
        CHECK(owner.Snapshot().Value().Header() == request.header);
        next.limits = request.limits;
        auto replacement = CaptureTerrainProducerSnapshot(next);
        REQUIRE(replacement.HasValue());
        auto wrongExpected = request.header;
        wrongExpected.revision.mutation = Rev<TerrainMutationRevision>(2);
        ErrorIs(owner.Publish(replacement.Value(), wrongExpected), TerrainProducerErrors::Stale);
        CHECK(owner.Snapshot().Value().Header() == request.header);
        REQUIRE(owner.Publish(replacement.Value(), request.header).HasValue());
        CHECK(owner.Snapshot().Value().Header() == next.header);
        owner.Shutdown();
        ErrorIs(owner.Publish(replacement.Value(), next.header), TerrainProducerErrors::Closed);
        ErrorIs(owner.Snapshot(), TerrainProducerErrors::Closed);
        CHECK(reader.Value().Meshes().front().triangles.size() == 32);
        ErrorIs(reader.Value().ValidateCurrent(next.header), TerrainProducerErrors::Stale);
    }
}  // namespace Horo::Terrain
