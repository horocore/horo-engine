#include "TerrainRenderTestSupport.h"

#include <algorithm>
#include <limits>
#include <memory>

using namespace Horo;
using Horo::Tests::IdentityValue;
using namespace Horo::Tests::TerrainRendering;

TEST_CASE("Terrain visual capture preserves large coordinates semantic layers legal LODs and independent leases", "[terrain][render]") {
    auto f = std::make_unique<SourceFixture>(false, 9);
    const auto header = f->Request().header;
    const auto limits = f->Request().limits;
    std::reverse(f->selections.begin(), f->selections.end());
    auto a = CaptureTerrainRenderSnapshot(f->Request());
    REQUIRE(a.HasValue());
    REQUIRE(a.Value().Tiles().size() == 4);
    CHECK(a.Value().Tiles().front().tile == f->cooked.Tiles().tiles.front().id);
    CHECK(a.Value().Tiles().front().anchor[0] == 10'000'000'000.0);
    const auto &tile = a.Value().Tiles().front();
    CHECK(tile.mesh.vertices.front().position.x == 0);
    CHECK(tile.mesh.vertices.front().normal.y == 1);
    CHECK(tile.mesh.IsValid());
    CHECK(tile.layers == LayerData(1));
    CHECK_FALSE(tile.samplePayload.empty());
    auto secondView = a.Value();
    CHECK(secondView.Tiles().data() == a.Value().Tiles().data());
    const auto originalPayload = tile.samplePayload;
    f->source.heightsMeters[0] = 999;
    CHECK(f->source.heightsMeters[0] == 999);
    const auto replacementCook = Cook(f->source);
    CHECK(replacementCook.ManifestDigest() != f->cooked.ManifestDigest());
    f.reset();
    CHECK(secondView.Tiles().front().mesh.vertices.front().position.y == 0);
    CHECK(secondView.Tiles().front().samplePayload == originalPayload);
    CHECK(a.Value().OwnedBytes() <= limits.maximumOwnedBytes);
    CHECK(a.Value().ValidateCurrent(header).HasValue());
}

TEST_CASE("Terrain visual capture keeps fully holed tiles as explicit empty topology", "[terrain][render]") {
    SourceFixture f(true);
    auto s = f.Capture();
    REQUIRE(s.Tiles().size() == 1);
    CHECK(s.Tiles()[0].holes);
    CHECK(s.Tiles()[0].mesh.indices.empty());
    CHECK_FALSE(s.Tiles()[0].mesh.vertices.empty());
}

TEST_CASE("Terrain visual capture rejects exact publication and capability failures", "[terrain][render]") {
    SourceFixture f;
    auto r = f.Request();
    SECTION("wrong content") {
        r.header.revisions.content = IdentityValue<TerrainContentRevision>(2);
    }
    SECTION("wrong capability revision") {
        r.header.revisions.capability = IdentityValue<TerrainCapabilityRevision>(2);
    }
    SECTION("wrong manifest") {
        r.header.manifest = Digest(99);
    }
    SECTION("wrong target") {
        r.header.target = Digest(99);
    }
    SECTION("wrong configuration") {
        r.header.configuration = IdentityValue<TerrainConfigurationRevision>(2);
    }
    SECTION("missing capability") {
        r.header.capabilities = {};
    }
    SECTION("closed") {
        r.lifecycle = TerrainRuntimeLifecycle::Closed;
    }
    SECTION("missing world") {
        r.header.world = {};
    }
    SECTION("missing origin") {
        r.header.origin = {};
    }
    SECTION("cell from another world") {
        r.header.cell = WorldStreaming::StreamingFence{Stable<WorldStreaming::WorldPartitionId>(12),
                                                       IdentityValue<WorldStreaming::PartitionEpoch>(1),
                                                       {0, 0, 0, 0, WorldStreaming::StreamingLayerId::Create(0).Value()},
                                                       IdentityValue<WorldStreaming::StreamingGeneration>(1)};
    }
    SECTION("zero generation") {
        r.header.generation = 0;
    }
    CHECK(CaptureTerrainRenderSnapshot(r).HasError());
}

TEST_CASE("Terrain visual capture rejects membership layer and capacity failures", "[terrain][render]") {
    SourceFixture f;
    auto r = f.Request();
    SECTION("zero budget") {
        r.limits.maximumOwnedBytes = 0;
    }
    SECTION("byte ceiling") {
        r.limits.maximumOwnedBytes = 1;
    }
    SECTION("work ceiling") {
        r.limits.maximumWorkItems = 1;
    }
    SECTION("vertex ceiling") {
        r.limits.maximumVertices = 1;
    }
    SECTION("index ceiling") {
        r.limits.maximumIndices = 1;
    }
    SECTION("wrong visual digest") {
        f.selections[0].visual = Digest(99);
    }
    SECTION("foreign tile") {
        f.selections[0].tile.dataset = Stable<TerrainDatasetId>(99);
    }
    SECTION("wrong layers") {
        f.selections[0].layers = LayerData(2);
    }
    SECTION("duplicate") {
        f.selections.push_back(f.selections[0]);
        r.tiles = f.selections;
    }
    CHECK(CaptureTerrainRenderSnapshot(r).HasError());
}

TEST_CASE("Terrain visual capture rejects corrupt bytes nonfinite geometry and invalid winding", "[terrain][render]") {
    SourceFixture f;
    auto &a = const_cast<TerrainSourceArtifact &>(f.cooked.Artifacts().front());
    REQUIRE(a.role == TerrainSourceArtifactRole::Visual);
    SECTION("bytes") {
        a.payload.back() ^= 1;
    }
    SECTION("position") {
        a.vertices.front().x = std::numeric_limits<double>::infinity();
    }
    SECTION("indices") {
        a.triangles.front().indices[0] = std::numeric_limits<std::uint32_t>::max();
    }
    SECTION("winding") {
        std::swap(a.triangles.front().indices[1], a.triangles.front().indices[2]);
    }
    SECTION("coverage") {
        a.triangles.front().endX = a.triangles.front().beginX;
    }
    CHECK(CaptureTerrainRenderSnapshot(f.Request()).HasError());
}

TEST_CASE("Terrain visual snapshot memory stays valid while each currentness dimension fences new use", "[terrain][render]") {
    SourceFixture f;
    auto snapshot = f.Capture();
    auto current = snapshot.Header();
    SECTION("runtime") {
        ++current.terrain.slot.generation;
    }
    SECTION("content") {
        current.revisions.content = IdentityValue<TerrainContentRevision>(2);
    }
    SECTION("residency") {
        current.revisions.residency = IdentityValue<TerrainResidencyRevision>(2);
    }
    SECTION("mutation") {
        current.revisions.mutation = IdentityValue<TerrainMutationRevision>(2);
    }
    SECTION("capability") {
        current.revisions.capability = IdentityValue<TerrainCapabilityRevision>(2);
    }
    SECTION("request") {
        ++current.generation;
    }
    SECTION("world") {
        current.world = Stable<WorldStreaming::WorldPartitionId>(12);
    }
    SECTION("origin revision") {
        current.origin.revision = IdentityValue<WorldStreaming::OriginFrameRevision>(2);
    }
    SECTION("origin rebase") {
        current.origin.generation = IdentityValue<WorldStreaming::OriginGeneration>(2);
        current.datasetToWorld.translation.x = -100;
    }
    SECTION("cell replacement") {
        current.cell = WorldStreaming::StreamingFence{current.world,
                                                      IdentityValue<WorldStreaming::PartitionEpoch>(1),
                                                      {0, 0, 0, 0, WorldStreaming::StreamingLayerId::Create(0).Value()},
                                                      IdentityValue<WorldStreaming::StreamingGeneration>(2)};
    }
    SECTION("placement") {
        current.datasetToWorld.translation.x = 1;
    }
    CHECK(snapshot.ValidateCurrent(current).HasError());
    CHECK(snapshot.IsValid());
    CHECK(snapshot.Tiles()[0].mesh.IsValid());
}

TEST_CASE("Terrain visual capture cancellation and empty admitted selection return distinct outcomes", "[terrain][render]") {
    SourceFixture f;
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    CHECK(CaptureTerrainRenderSnapshot(f.Request(), cancellation.Token()).HasError());
    auto request = f.Request();
    request.tiles = {};
    auto empty = CaptureTerrainRenderSnapshot(request);
    REQUIRE(empty.HasValue());
    CHECK(empty.Value().IsValid());
    CHECK(empty.Value().Tiles().empty());
}

TEST_CASE("Terrain cell-scoped snapshot rejects replaced epoch and residency attempt", "[terrain][render]") {
    SourceFixture f;
    auto request = f.Request();
    request.header.cell = WorldStreaming::StreamingFence{request.header.world,
                                                         IdentityValue<WorldStreaming::PartitionEpoch>(1),
                                                         {0, 0, 0, 0, WorldStreaming::StreamingLayerId::Create(0).Value()},
                                                         IdentityValue<WorldStreaming::StreamingGeneration>(1)};
    auto snapshot = CaptureTerrainRenderSnapshot(request);
    REQUIRE(snapshot.HasValue());
    auto current = request.header;
    SECTION("epoch") {
        current.cell->epoch = IdentityValue<WorldStreaming::PartitionEpoch>(2);
    }
    SECTION("attempt") {
        current.cell->generation = IdentityValue<WorldStreaming::StreamingGeneration>(2);
    }
    CHECK(snapshot.Value().ValidateCurrent(current).HasError());
    CHECK(snapshot.Value().IsValid());
}
