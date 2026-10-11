#include "TerrainRenderTestSupport.h"

#include <thread>

using namespace Horo;
using Horo::Tests::IdentityValue;
using namespace Horo::Tests::TerrainRendering;

TEST_CASE("Terrain renderer publishes only complete frontend buffers and meshes", "[terrain][render][resource]") {
    ResourceFixture f;
    SourceFixture source;
    const auto header = source.Request().header;
    REQUIRE(f.owner->Stage(source.Capture(), header, 0).HasValue());
    auto pending = f.owner->Advance(header);
    REQUIRE(pending.HasValue());
    CHECK_FALSE(pending.Value());
    CHECK_FALSE(f.owner->Snapshot().Value());
    REQUIRE(f.frontend->ProcessResourceRequests().Value() == 2);
    REQUIRE_FALSE(f.owner->Advance(header).Value());
    REQUIRE(f.frontend->ProcessResourceRequests().Value() == 1);
    REQUIRE(f.owner->Advance(header).Value());
    auto reader = f.owner->Snapshot().Value();
    REQUIRE(reader);
    CHECK(reader->publication == 1);
    REQUIRE(reader->tiles.size() == 1);
    CHECK(reader->tiles[0].tile == source.selections[0].tile);
    CHECK(f.frontend->ResourceState(reader->tiles[0].mesh.handle).Value() == Render::RenderResourceState::Ready);
}

TEST_CASE("Terrain replacement retains old CPU readers and exact resource generation until readers release",
          "[terrain][render][resource]") {
    ResourceFixture f;
    SourceFixture source;
    f.Publish(source);
    auto old = f.owner->Snapshot().Value();
    const auto oldMesh = old->tiles[0].mesh.handle;
    f.Publish(source, 2, 1);
    auto current = f.owner->Snapshot().Value();
    CHECK(current->publication == 2);
    CHECK(current->tiles[0].mesh.handle != oldMesh);
    CHECK(f.owner->DrainRetired().Value() == 0);
    CHECK(f.frontend->ResourceState(oldMesh).Value() == Render::RenderResourceState::Ready);
    old.reset();
    CHECK(f.owner->DrainRetired().Value() == 1);
    CHECK(f.frontend->ResourceState(oldMesh).HasError());
    CHECK(f.frontend->ResourceState(current->tiles[0].mesh.handle).Value() == Render::RenderResourceState::Ready);
}

TEST_CASE("Terrain stale or cancelled candidates preserve active generation and retire partial queues", "[terrain][render][resource]") {
    ResourceFixture f;
    SourceFixture source;
    f.Publish(source);
    const auto oldMesh = f.owner->Snapshot().Value()->tiles[0].mesh.handle;
    auto header = source.Request(2).header;
    REQUIRE(f.owner->Stage(source.Capture(2), header, 1).HasValue());
    SECTION("changed owner evidence") {
        header.revisions.residency = IdentityValue<TerrainResidencyRevision>(2);
        CHECK(f.owner->Advance(header).HasError());
    }
    SECTION("origin rebased while upload pending") {
        header.origin.generation = IdentityValue<WorldStreaming::OriginGeneration>(2);
        header.datasetToWorld.translation.x = -100;
        CHECK(f.owner->Advance(header).HasError());
    }
    SECTION("cancelled before upload") {
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(f.owner->Advance(header, cancelled.Token()).HasError());
    }
    SECTION("cancelled after mesh staging") {
        REQUIRE(f.frontend->ProcessResourceRequests().HasValue());
        REQUIRE_FALSE(f.owner->Advance(header).Value());
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(f.owner->Advance(header, cancelled.Token()).HasError());
    }
    CHECK(f.owner->Snapshot().Value()->tiles[0].mesh.handle == oldMesh);
    CHECK(f.owner->DrainRetired().Value() == 1);
    CHECK(f.frontend->UploadSnapshot().pendingRequests == 0);
}

TEST_CASE("Terrain upload admission failure rolls back a queued prefix without publishing", "[terrain][render][resource]") {
    Render::RenderResourceUploadLimits limits;
    limits.maximumPendingRequests = limits.maximumRequestsPerDrain = 1;
    ResourceFixture f(limits);
    SourceFixture source;
    CHECK(f.owner->Stage(source.Capture(), source.Request().header, 0).HasError());
    CHECK_FALSE(f.owner->Snapshot().Value());
    CHECK(f.frontend->UploadSnapshot().pendingRequests == 1);
    CHECK(f.owner->DrainRetired().Value() == 1);
    CHECK(f.frontend->UploadSnapshot().pendingRequests == 0);
}

TEST_CASE("Terrain resources enforce publication lineage bounded retirement and render owner lane", "[terrain][render][resource]") {
    ResourceFixture f({}, 1);
    SourceFixture source;
    f.Publish(source);
    CHECK(f.owner->Stage(source.Capture(1), source.Request(1).header, 1).HasError());
    CHECK(f.owner->Stage(source.Capture(2), source.Request(2).header, 99).HasError());
    auto old = f.owner->Snapshot().Value();
    f.Publish(source, 2, 1);
    CHECK(f.owner->Stage(source.Capture(3), source.Request(3).header, 2).HasError());
    bool rejected = false;
    std::thread worker([&] {
        rejected = f.owner->BeginShutdown().HasError();
    });
    worker.join();
    CHECK(rejected);
    old.reset();
    REQUIRE(f.owner->DrainRetired().Value() == 1);
    f.Publish(source, 3, 2);
}

TEST_CASE("Terrain shutdown preserves issued readers and reports incomplete logical drain until release", "[terrain][render][resource]") {
    ResourceFixture f;
    SourceFixture source;
    f.Publish(source);
    auto reader = f.owner->Snapshot().Value();
    const auto mesh = reader->tiles[0].mesh.handle;
    REQUIRE(f.owner->BeginShutdown().HasValue());
    REQUIRE(f.owner->BeginShutdown().HasValue());
    CHECK(f.owner->Stage(source.Capture(2), source.Request(2).header, 1).HasError());
    CHECK(f.owner->Snapshot().HasError());
    CHECK_FALSE(f.owner->IsDrained());
    CHECK(f.owner->DrainRetired().Value() == 0);
    CHECK(f.frontend->ResourceState(mesh).Value() == Render::RenderResourceState::Ready);
    reader.reset();
    CHECK(f.owner->DrainRetired().Value() == 1);
    CHECK(f.owner->IsDrained());
}

TEST_CASE("Terrain fully holed generation publishes without fake GPU resources", "[terrain][render][resource]") {
    ResourceFixture f;
    SourceFixture source(true);
    f.Publish(source);
    auto reader = f.owner->Snapshot().Value();
    REQUIRE(reader->tiles.size() == 1);
    CHECK_FALSE(reader->tiles[0].mesh.handle.IsValid());
    CHECK(f.frontend->UploadSnapshot().pendingRequests == 0);
}

TEST_CASE("Terrain failed and skipped request generations do not strand a valid later replacement", "[terrain][render][resource]") {
    ResourceFixture f;
    SourceFixture source;
    f.Publish(source);
    const auto old = f.owner->Snapshot().Value()->tiles[0].mesh.handle;
    REQUIRE(f.owner->Stage(source.Capture(2), source.Request(2).header, 1).HasValue());
    CancellationSource cancelled;
    cancelled.RequestCancellation();
    REQUIRE(f.owner->Advance(source.Request(2).header, cancelled.Token()).HasError());
    REQUIRE(f.owner->DrainRetired().Value() == 1);
    CHECK(f.owner->Snapshot().Value()->tiles[0].mesh.handle == old);
    f.Publish(source, 3, 1);
    CHECK(f.owner->Snapshot().Value()->source.Header().generation == 3);
    REQUIRE(f.owner->DrainRetired().Value() == 1);
    f.Publish(source, 7, 2);
    CHECK(f.owner->Snapshot().Value()->source.Header().generation == 7);
}
