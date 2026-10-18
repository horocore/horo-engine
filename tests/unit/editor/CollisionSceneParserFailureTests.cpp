#include "AllocationProbe.h"
#include "CollisionSettingsTestSupport.h"
#include "SceneAllocationQualification.h"

#include <cstdio>

namespace Horo::Tests::CollisionSettings {
    TEST_CASE("Canonical Scene decoder enforces duplicate depth and malformed unknown-field admission",
              "[collision][scene][serialization]") {
        const auto nested = [](const std::size_t arrays) {
            return std::string{R"({"schemaVersion":1,"objects":[],"unknown":)"} + std::string(arrays, '[') + "0" +
                   std::string(arrays, ']') + "}";
        };
        CHECK(SceneSource::DecodeSceneSource(nested(62)).HasValue());
        CHECK(SceneSource::DecodeSceneSource(nested(63)).HasError());
        CHECK(SceneSource::DecodeSceneSource(nested(4096)).HasError());
        CHECK(SceneSource::DecodeSceneSource(R"({"schemaVersion":1,"schemaVersion":1,"objects":[]})").HasError());
        CHECK(SceneSource::DecodeSceneSource(R"({"schemaVersion":1,"objects":[],"unknown":{"key":1,"key":2}})").HasError());
        CHECK(SceneSource::DecodeSceneSource(R"({"schemaVersion":1,"objects":[],"unknown":[{"broken":})").HasError());
    }

    TEST_CASE("Canonical Scene decoder unwinds every single allocation failure without terminating completed DOM cleanup",
              "[collision][scene][allocation]") {
        RequireSupportedSceneAllocationInjection();
        const SceneSource::SceneObjectSnapshot object{.id = {1}, .name = "Allocation owner"};
        const std::array objects{object};
        const auto source = SceneSource::EncodeSceneSource({objects, {}});
        std::size_t requests{};
        {
            AllocationProbe::ScopedMeasurement measure;
            const auto decoded = SceneSource::DecodeSceneSource(source);
            requests = measure.Snapshot().requests;
            REQUIRE(decoded.HasValue());
        }
        REQUIRE(requests > 0);
        std::size_t failures{};
        for (std::size_t allocation = 0; allocation < requests; ++allocation) {
            INFO(allocation);
            try {
                AllocationProbe::ScopedFailure fault{allocation};
                const auto decoded = SceneSource::DecodeSceneSource(source);
                REQUIRE(decoded.HasError());
                ++failures;
            } catch (const std::bad_alloc &) {
                ++failures;
            }
        }
        std::fprintf(stderr, "[collision-allocation] scene measured=%zu completed=%zu\n", requests, failures);
        REQUIRE(failures == requests);
        CHECK(SceneSource::DecodeSceneSource(source).HasValue());
    }
}  // namespace Horo::Tests::CollisionSettings
