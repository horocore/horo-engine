#include "FoliageAuthoringTestSupport.h"

#include <algorithm>
#include <limits>

namespace Horo::Terrain {
    using namespace FoliageAuthoringTest;

    TEST_CASE("Authored foliage source preserves accepted IDs and canonicalizes record order", "[foliage-authoring][source]") {
        auto source = Source();
        auto bytes = EncodeFoliageAuthoringSource(source, 4, 4);
        REQUIRE(bytes.HasValue());
        CHECK(bytes.Value().size() == 188);
        std::ranges::reverse(source.placements);
        auto reversed = EncodeFoliageAuthoringSource(source, 4, 4);
        REQUIRE(reversed.HasValue());
        CHECK(reversed.Value() == bytes.Value());
        auto decoded = DecodeFoliageAuthoringSource(bytes.Value(), 4, 4);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().placements.front().id == Id<FoliageInstanceId>(7));
        CHECK(decoded.Value().placements.front().offsetMeters == 0.25F);
        CHECK(decoded.Value().definitions == source.definitions);
    }

    TEST_CASE("Foliage source admits exact budgets and rejects malformed or unbound placement data", "[foliage-authoring][source]") {
        auto source = Source();
        auto measured = MeasureFoliageAuthoringSource(2, 1);
        REQUIRE(measured.HasValue());
        FoliageAuthoringSourceLimits limits;
        limits.maximumBytes = measured.Value().encodedBytes;
        limits.maximumWorkItems = measured.Value().workItems;
        auto bytes = EncodeFoliageAuthoringSource(source, 4, 4, limits);
        REQUIRE(bytes.HasValue());
        auto encodedMeasure = MeasureEncodedFoliageAuthoringSource(bytes.Value(), limits);
        REQUIRE(encodedMeasure.HasValue());
        CHECK(encodedMeasure.Value().encodedBytes == measured.Value().encodedBytes);
        CHECK(encodedMeasure.Value().workItems == measured.Value().workItems);
        --limits.maximumBytes;
        CHECK(EncodeFoliageAuthoringSource(source, 4, 4, limits).HasError());
        source.placements[1].id = source.placements[0].id;
        CHECK(EncodeFoliageAuthoringSource(source, 4, 4).HasError());
        source = Source();
        source.definitions.clear();
        CHECK(EncodeFoliageAuthoringSource(source, 4, 4).HasError());
        source = Source();
        source.placements[0].scale = std::numeric_limits<float>::infinity();
        CHECK(EncodeFoliageAuthoringSource(source, 4, 4).HasError());
        auto truncated = bytes.Value();
        truncated.pop_back();
        CHECK(DecodeFoliageAuthoringSource(truncated, 4, 4).HasError());
    }

    TEST_CASE("Foliage source cancellation and empty boundary do not publish partial data", "[foliage-authoring][source]") {
        auto empty = EncodeFoliageAuthoringSource({}, 2, 2);
        REQUIRE(empty.HasValue());
        CHECK(empty.Value().size() == 12);
        auto decoded = DecodeFoliageAuthoringSource(empty.Value(), 2, 2);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().placements.empty());
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CHECK(EncodeFoliageAuthoringSource({}, 2, 2, {}, cancellation.Token()).HasError());
        auto source = Source();
        const auto before = source;
        CHECK(EncodeFoliageAuthoringSource(source, 4, 4, {}, cancellation.Token()).HasError());
        CHECK(source == before);
    }

    TEST_CASE("Foliage type assets use real typed definition validation and exact asset binding", "[foliage-authoring][source]") {
        const auto source = Type();
        auto bytes = EncodeFoliageTypeSource(source, Configuration(), Capabilities());
        REQUIRE(bytes.HasValue());
        auto decoded = DecodeFoliageTypeSource(bytes.Value(), source.asset, Configuration(), Capabilities());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == source);
        CHECK(DecodeFoliageTypeSource(bytes.Value(), Asset(99), Configuration(), Capabilities()).HasError());
        auto malformed = bytes.Value();
        malformed[0] ^= std::byte{1};
        CHECK(DecodeFoliageTypeSource(malformed, source.asset, Configuration(), Capabilities()).HasError());
        auto bad = source;
        bad.definition.assets.meshLodCount = 0;
        CHECK(EncodeFoliageTypeSource(bad, Configuration(), Capabilities()).HasError());
    }
}  // namespace Horo::Terrain
