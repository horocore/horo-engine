#include "terrain/TerrainViewportPicking.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Editor {
    namespace {
        Terrain::TerrainCanonicalSource HoledSource() {
            Terrain::TerrainCanonicalSource source;
            source.width=2; source.height=2; source.heightsMeters={1,1,1,1}; source.holes={0,0,0,1};
            return source;
        }
    }
    TEST_CASE("Hole restoration explicitly picks retained heightfield while ordinary tools see no surface", "[terrain][restoration]") {
        const auto source=HoledSource(); const Math::Ray ray{{.5F,5,.5F},{0,-1,0},0,10};
        auto visible=PickTerrainSurface(source,{},ray); REQUIRE(visible.HasValue()); CHECK_FALSE(visible.Value());
        auto restore=PickTerrainSurface(source,{},ray,4096,{},TerrainSurfacePickMode::UnderlyingHeightfield);
        REQUIRE(restore.HasValue()); REQUIRE(restore.Value());
        CHECK(restore.Value()->heightMeters==Catch::Approx(1)); CHECK(restore.Value()->distance==Catch::Approx(4));
        CHECK(source.holes==std::vector<std::uint8_t>{0,0,0,1});
    }
    TEST_CASE("Underlying heightfield query preserves typed cancellation and malformed-input admission", "[terrain][restoration]") {
        auto source=HoledSource(); const Math::Ray ray{{.5F,5,.5F},{0,-1,0},0,10};
        source.holes[3]=2;
        auto invalid=PickTerrainSurface(source,{},ray,4096,{},TerrainSurfacePickMode::UnderlyingHeightfield);
        REQUIRE(invalid.HasError()); CHECK(invalid.ErrorValue().code.Value()==TerrainEditErrors::Invalid.code.Value());
        source.holes[3]=1; CancellationSource cancellation; cancellation.RequestCancellation();
        auto cancelled=PickTerrainSurface(source,{},ray,4096,cancellation.Token(),TerrainSurfacePickMode::UnderlyingHeightfield);
        REQUIRE(cancelled.HasError()); CHECK(cancelled.ErrorValue().code.Value()==TerrainEditErrors::Cancelled.code.Value());
        CHECK(PickTerrainSurface(source,{},ray,0,{},TerrainSurfacePickMode::UnderlyingHeightfield).HasError());
    }
}
