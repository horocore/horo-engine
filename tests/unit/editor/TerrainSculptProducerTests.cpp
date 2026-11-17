#include "terrain/TerrainSculptProducer.h"
#include "terrain/TerrainViewportPicking.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    Terrain::TerrainCanonicalSource Source(std::uint32_t width = 9, std::uint32_t height = 9) {
        Terrain::TerrainCanonicalSource source;
        Terrain::SerializedTerrainIdentity bytes{};
        bytes.back() = 1;
        source.dataset = Terrain::TerrainDatasetId::Create(bytes).Value();
        source.sourceAsset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value();
        source.revision = Terrain::TerrainSourceRevision::Create(1).Value();
        source.capability = Terrain::TerrainCapabilityRevision::Create(1).Value();
        source.width = width;
        source.height = height;
        source.heightsMeters.assign(std::size_t{width} * height, 0.0F);
        return source;
    }

    TerrainAuthoringDocument Document(Terrain::TerrainCanonicalSource source) {
        return std::move(TerrainAuthoringDocument::Open(TerrainDocumentSessionId::Create(1).Value(), std::move(source), 4,
                                                        TerrainAuthoringCapability::Edit))
            .Value();
    }

    Math::Ray Down(float x, float z) {
        return {{x, 20.0F, z}, {0.0F, -1.0F, 0.0F}, 0.0F, 100.0F};
    }
}  // namespace

TEST_CASE("All terrain sculpt modes produce real exact patch history", "[terrain][sculpt]") {
    TerrainSculptBrush brush;
    brush.radiusMeters = 2;
    brush.amountMeters = 2;
    brush.blend = 0.5F;
    brush.flattenMeters = 6;
    float expected{};
    auto source = Source();
    SECTION("Raise") {
        brush.mode = TerrainSculptMode::Raise;
        expected = 2;
    }
    SECTION("Lower") {
        brush.mode = TerrainSculptMode::Lower;
        expected = -2;
    }
    SECTION("Flatten") {
        brush.mode = TerrainSculptMode::Flatten;
        expected = 3;
    }
    SECTION("Smooth") {
        brush.mode = TerrainSculptMode::Smooth;
        source.heightsMeters[40] = 9;
        expected = 5;
    }
    SECTION("Noise") {
        brush.mode = TerrainSculptMode::Noise;
    }
    auto document = Document(source);
    const std::array stamps{TerrainSculptStamp{4, 4}};
    auto operation = ProduceTerrainSculpt(document.Source(), document.Fence(), TerrainEditOperationId::Create(1).Value(), brush, stamps);
    REQUIRE(operation.HasValue());
    REQUIRE(operation.Value().patches.size() == 1);
    REQUIRE(operation.Value().patches[0].heightsMeters.size() < source.heightsMeters.size());
    REQUIRE(document.Source().heightsMeters == source.heightsMeters);
    if (brush.mode == TerrainSculptMode::Noise) {
        const auto repeated =
            ProduceTerrainSculpt(document.Source(), document.Fence(), TerrainEditOperationId::Create(2).Value(), brush, stamps);
        REQUIRE(repeated.HasValue());
        REQUIRE(repeated.Value().patches[0].heightsMeters == operation.Value().patches[0].heightsMeters);
    }
    REQUIRE(document.Execute(operation.Value()).HasValue());
    REQUIRE(document.IsDirty());
    if (brush.mode != TerrainSculptMode::Noise)
        REQUIRE(document.Source().heightsMeters[40] == Catch::Approx(expected));
    const auto changed = document.Source().heightsMeters;
    REQUIRE(document.Source().heightsMeters.front() == source.heightsMeters.front());
    REQUIRE(document.UndoCount() == 1);
    REQUIRE(document.Undo(document.Fence()).HasValue());
    REQUIRE(document.Source().heightsMeters == source.heightsMeters);
    REQUIRE_FALSE(document.IsDirty());
    REQUIRE(document.Redo(document.Fence()).HasValue());
    REQUIRE(document.Source().heightsMeters == changed);
    REQUIRE(document.Execute(operation.Value()).HasError());
}

TEST_CASE("Terrain sculpt preflight rejects invalid and overbudget commands without mutation", "[terrain][sculpt]") {
    auto document = Document(Source());
    auto fence = document.Fence();
    TerrainSculptBrush brush;
    TerrainSculptLimits limits;
    std::array stamps{TerrainSculptStamp{4, 4}};
    SECTION("Stale source") {
        fence.expectedRevision = Terrain::TerrainSourceRevision::Create(2).Value();
    }
    SECTION("Invalid radius") {
        brush.radiusMeters = 0;
    }
    SECTION("Nonfinite stamp") {
        stamps[0].sampleX = std::numeric_limits<double>::infinity();
    }
    SECTION("Patch budget") {
        limits.maximumPatchSamples = 1;
    }
    SECTION("Work budget") {
        limits.maximumSampleVisits = 1;
    }
    REQUIRE(ProduceTerrainSculpt(document.Source(), fence, TerrainEditOperationId::Create(1).Value(), brush, stamps, limits).HasError());
    REQUIRE(document.Fence().expectedRevision.Value() == 1);
    REQUIRE(document.UndoCount() == 0);
    REQUIRE_FALSE(document.IsDirty());
}

TEST_CASE("Terrain picking uses the cooked nonplanar triangle diagonal", "[terrain][picking]") {
    auto source = Source(2, 2);
    source.heightsMeters = {0, 0, 0, 8};
    auto first = PickTerrainSurface(source, {}, Down(0.25F, 0.25F));
    REQUIRE(first.HasValue());
    REQUIRE(first.Value().has_value());
    REQUIRE(first.Value()->heightMeters == Catch::Approx(0));
    auto second = PickTerrainSurface(source, {}, Down(0.75F, 0.75F));
    REQUIRE(second.HasValue());
    REQUIRE(second.Value().has_value());
    REQUIRE(second.Value()->heightMeters == Catch::Approx(4));
    REQUIRE(second.Value()->stamp.sampleX == Catch::Approx(0.75));
    REQUIRE(second.Value()->distance == Catch::Approx(16));
}

TEST_CASE("Terrain picking preserves first sample frame under nonuniform placement", "[terrain][picking]") {
    auto source = Source(2, 2);
    source.coordinates.originX = 4'000'000;
    source.coordinates.originZ = -3'000'000;
    source.coordinates.spacingX = 2;
    source.coordinates.spacingZ = 3;
    source.heightsMeters = {2, 2, 2, 2};
    Math::Transform transform;
    transform.translation = {10, 5, -7};
    transform.scale = {2, 3, 4};
    const auto hit = PickTerrainSurface(source, transform, {{12, 30, -1}, {0, -1, 0}, 0, 100});
    REQUIRE(hit.HasValue());
    REQUIRE(hit.Value().has_value());
    REQUIRE(hit.Value()->stamp.sampleX == Catch::Approx(0.5));
    REQUIRE(hit.Value()->stamp.sampleZ == Catch::Approx(0.5));
    REQUIRE(hit.Value()->viewportPoint.y == Catch::Approx(11));
    REQUIRE(hit.Value()->distance == Catch::Approx(19));
}

TEST_CASE("Terrain DDA traverses oblique rays and terminates at grid boundaries", "[terrain][picking]") {
    auto source = Source();
    const float inverse = 1.0F / std::sqrt(3.0F);
    Math::Ray ray{{-1, 6, -1}, {inverse, -inverse, inverse}, 0, 30};
    auto hit = PickTerrainSurface(source, {}, ray);
    REQUIRE(hit.HasValue());
    REQUIRE(hit.Value().has_value());
    REQUIRE(hit.Value()->stamp.sampleX == Catch::Approx(5));
    REQUIRE(hit.Value()->stamp.sampleZ == Catch::Approx(5));
    REQUIRE(PickTerrainSurface(source, {}, ray, 1).HasError());
    for (const float boundary : {0.0F, 4.0F, 8.0F}) {
        const auto corner = PickTerrainSurface(source, {}, Down(boundary, boundary));
        REQUIRE(corner.HasValue());
        REQUIRE(corner.Value().has_value());
        REQUIRE(corner.Value()->stamp.sampleX == Catch::Approx(boundary));
    }
    const auto miss = PickTerrainSurface(source, {}, Down(-1, 4));
    REQUIRE(miss.HasValue());
    REQUIRE_FALSE(miss.Value().has_value());
}

TEST_CASE("Terrain picking rejects holes and malformed sampled cells without a plane fallback", "[terrain][picking]") {
    auto source = Source(2, 2);
    SECTION("Canonical hole") {
        source.holes = {0, 0, 0, 1};
        const auto hit = PickTerrainSurface(source, {}, Down(0.5F, 0.5F));
        REQUIRE(hit.HasValue());
        REQUIRE_FALSE(hit.Value().has_value());
    }
    SECTION("Invalid hole") {
        source.holes = {0, 0, 0, 2};
        REQUIRE(PickTerrainSurface(source, {}, Down(0.5F, 0.5F)).HasError());
    }
    SECTION("Invalid height") {
        source.heightsMeters[3] = std::numeric_limits<float>::quiet_NaN();
        REQUIRE(PickTerrainSurface(source, {}, Down(0.5F, 0.5F)).HasError());
    }
}
