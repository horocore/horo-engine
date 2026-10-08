#include "Horo/Scene/CookedSceneDefinition.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::SceneCook;

    /** @brief Creates typed content whose disabled values must survive runtime wire encoding. */
    Runtime::RuntimeSceneDefinition Scene() {
        Runtime::SceneDefinitionBuilder builder{{1}, {2}};
        builder.Add({.object = {900}, .components = {.camera = Runtime::CameraComponent{.enabled = false}}});
        builder.Add({.object = {901}, .parent = Runtime::SceneObjectId{900}, .localTransform = {.translation = {0, 4, 0}}});
        auto scene = std::move(builder).Build();
        REQUIRE(scene.HasValue());
        return std::move(scene).Value();
    }
}  // namespace

TEST_CASE("Cooked scene payload is canonical source-free and round trips typed runtime content", "[native][prefab-cook]") {
    const auto scene = Scene();
    const auto encoded = EncodeCookedSceneDefinition(scene);
    REQUIRE(encoded.HasValue());
    const std::string text{encoded.Value().begin(), encoded.Value().end()};
    REQUIRE(text.find("prefabInstances") == std::string::npos);
    REQUIRE(text.find("sourceAsset") == std::string::npos);
    REQUIRE(text.find("editor") == std::string::npos);
    REQUIRE(text.find("name") == std::string::npos);
    const auto decoded = DecodeCookedSceneDefinition(encoded.Value(), {1}, {2});
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value().Entities().size() == 2);
    REQUIRE(decoded.Value().Entities()[0].components == scene.Entities()[0].components);
    REQUIRE(decoded.Value().Entities()[1].parent == scene.Entities()[1].parent);
    REQUIRE(decoded.Value().Entities()[1].localTransform == scene.Entities()[1].localTransform);
    const auto again = EncodeCookedSceneDefinition(decoded.Value());
    REQUIRE(again.HasValue());
    REQUIRE(again.Value() == encoded.Value());
}

TEST_CASE("Cooked scene payload rejects foreign malformed and noncanonical input", "[native][prefab-cook]") {
    const auto encoded = EncodeCookedSceneDefinition(Scene());
    REQUIRE(encoded.HasValue());
    REQUIRE(DecodeCookedSceneDefinition(encoded.Value(), {9}, {2}).HasError());
    REQUIRE(DecodeCookedSceneDefinition(encoded.Value(), {1}, {9}).HasError());
    auto bytes = encoded.Value();
    bytes.pop_back();
    REQUIRE(DecodeCookedSceneDefinition(bytes, {1}, {2}).HasError());
    bytes = encoded.Value();
    bytes.front() = '!';
    REQUIRE(DecodeCookedSceneDefinition(bytes, {1}, {2}).HasError());
    bytes = encoded.Value();
    const std::string raw = "\"prefabInstances\":[],";
    bytes.insert(bytes.begin() + 1, raw.begin(), raw.end());
    REQUIRE(DecodeCookedSceneDefinition(bytes, {1}, {2}).HasError());
}

TEST_CASE("Cooked scene payload honors the same exact bounds for encoding and decoding", "[native][prefab-cook]") {
    const auto scene = Scene();
    const auto encoded = EncodeCookedSceneDefinition(scene);
    REQUIRE(encoded.HasValue());
    CookedSceneLimits limits{.maximumEntities = 2, .maximumDependencies = 1, .maximumBytes = encoded.Value().size()};
    REQUIRE(EncodeCookedSceneDefinition(scene, limits).HasValue());
    REQUIRE(DecodeCookedSceneDefinition(encoded.Value(), {1}, {2}, limits).HasValue());
    SECTION("entity count") {
        limits.maximumEntities = 1;
    }
    SECTION("payload bytes") {
        --limits.maximumBytes;
    }
    REQUIRE(EncodeCookedSceneDefinition(scene, limits).HasError());
    REQUIRE(DecodeCookedSceneDefinition(encoded.Value(), {1}, {2}, limits).HasError());
}
