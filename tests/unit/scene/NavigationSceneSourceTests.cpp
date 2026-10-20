#include "Horo/Scene/SceneSource.h"
#include "navigation/NavigationSceneSourceFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>

using namespace Horo;
using namespace Horo::SceneSource;
namespace SourceTest = Horo::Tests::NavigationSources;

TEST_CASE("Explicit navigation sources retain exact authored policy through Scene schema3", "[scene][navigation][source]") {
    const std::array objects{SourceTest::Object()};
    const auto encoded = EncodeSceneSource({objects, {}});
    const auto json = nlohmann::json::parse(encoded);
    CHECK(json["schemaVersion"] == 3);
    const auto decoded = DecodeSceneSource(encoded);
    INFO((decoded.HasError() ? decoded.ErrorValue().message : "decoded"));
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value().objects.size() == 1);
    CHECK(decoded.Value().objects.front().components == objects.front().components);
    CHECK(decoded.Value().unsupportedComponentOwners.empty());
    CHECK(EncodeSceneSource({decoded.Value().objects, {}}) == encoded);
    CHECK_FALSE(json["objects"][0]["components"]["navigationSources"][0].contains("vertices"));
    CHECK_FALSE(json["objects"][0]["components"]["navigationSources"][0].contains("provider"));
}

TEST_CASE("Source-capable Scenes reject legacy envelopes and preserve absent legacy source intent", "[scene][navigation][source]") {
    auto object = SourceTest::Object();
    const std::array objects{object};
    auto json = nlohmann::json::parse(EncodeSceneSource({objects, {}}));
    for (const unsigned schema : {1, 2, 4}) {
        json["schemaVersion"] = schema;
        CHECK(DecodeSceneSource(json.dump()).HasError());
    }
    object.components.navigationSources.clear();
    const std::array legacyObjects{object};
    const auto legacy = EncodeSceneSource({legacyObjects, {}});
    CHECK(nlohmann::json::parse(legacy)["schemaVersion"] == 1);
    const auto decoded = DecodeSceneSource(legacy);
    INFO((decoded.HasError() ? decoded.ErrorValue().message : "decoded"));
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().objects.front().components.navigationSources.empty());
    CHECK(DecodeSceneSource(R"({"schemaVersion":3,"objects":[]})").HasValue());
}

TEST_CASE("Scene schema3 composes explicit navigation sources with disabled Character authoring", "[scene][navigation][source]") {
    auto object = SourceTest::Object();
    object.components.characterController = Runtime::CharacterControllerComponent{.id = {8}};
    object.components.navigationSources.front().enabled = false;
    const std::array objects{object};
    const auto encoded = EncodeSceneSource({objects, {}});
    const auto decoded = DecodeSceneSource(encoded);
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().objects.front().components == object.components);
    CHECK(EncodeSceneSource({decoded.Value().objects, {}}) == encoded);
}

TEST_CASE("Scene navigation source parser rejects missing malformed oversized and duplicate authored policies",
          "[scene][navigation][source]") {
    using Json = nlohmann::json;
    const std::array objects{SourceTest::Object()};
    const auto original = Json::parse(EncodeSceneSource({objects, {}}));
    const std::array<std::pair<std::string_view, Json>, 11> invalid{{{"collider", 0},
                                                                     {"surface", 0},
                                                                     {"area", 0},
                                                                     {"filter", 0},
                                                                     {"generation", 0},
                                                                     {"schemaVersion", 2},
                                                                     {"schemaVersion", std::numeric_limits<std::uint64_t>::max()},
                                                                     {"collider", -1},
                                                                     {"area", "walkable"},
                                                                     {"filter", 1.5},
                                                                     {"enabled", 1}}};
    for (const auto &[field, replacement] : invalid) {
        CAPTURE(field);
        auto json = original;
        json["objects"][0]["components"]["navigationSources"][0][field] = replacement;
        CHECK(DecodeSceneSource(json.dump()).HasError());
    }
    for (const auto field : {"collider", "surface", "area", "filter", "schemaVersion", "generation", "enabled"}) {
        auto json = original;
        json["objects"][0]["components"]["navigationSources"][0].erase(field);
        CHECK(DecodeSceneSource(json.dump()).HasError());
    }
    auto json = original;
    auto &sources = json["objects"][0]["components"]["navigationSources"];
    sources[0]["vertices"] = Json::array();
    CHECK(DecodeSceneSource(json.dump()).HasError());
    sources[0].erase("vertices");
    sources.push_back(sources[0]);
    CHECK(DecodeSceneSource(json.dump()).HasError());
    sources = Json::array();
    for (std::size_t i = 0; i <= Runtime::MaximumNavigationSourcesPerObject; ++i)
        sources.push_back(original["objects"][0]["components"]["navigationSources"][0]);
    CHECK(DecodeSceneSource(json.dump()).HasError());
    sources = false;
    CHECK(DecodeSceneSource(json.dump()).HasError());
}
