#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "animation/SceneAnimationFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using Json = nlohmann::json;

    /** @brief Exercises the real canonical Scene encoder without an alternative component writer. */
    std::string Source(const Animation::AnimationAuthoringComponent &binding) {
        const std::array objects{SceneSource::SceneObjectSnapshot{.id = {1}, .name = "Animated", .components = {.animation = binding}}};
        return SceneSource::EncodeSceneSource({objects, {}});
    }

    /** @brief Converts the real decoded source through its canonical headless Scene boundary. */
    Runtime::RuntimeSceneDefinition Definition(const Animation::AnimationAuthoringComponent &binding) {
        const auto decoded = SceneSource::DecodeSceneSource(Source(binding));
        REQUIRE(decoded.HasValue());
        auto converted = SceneSource::ConvertSceneSourceToRuntime({decoded.Value().objects, decoded.Value().prefabInstances}, {11}, {13});
        REQUIRE(converted.HasValue());
        return std::move(converted).Value();
    }
}  // namespace

TEST_CASE("Animation Scene source retains all durable binding fields and requires schema4", "[scene][animation][serialization]") {
    for (const bool clip : {false, true}) {
        const auto binding = Tests::SceneAnimation::Binding(clip);
        const auto bytes = Source(binding);
        auto source = SceneSource::DecodeSceneSource(bytes);
        REQUIRE(source.HasValue());
        REQUIRE(source.Value().objects.size() == 1);
        CHECK(source.Value().objects.front().components.animation == binding);
        CHECK(SceneSource::EncodeSceneSource({source.Value().objects, source.Value().prefabInstances}) == bytes);
        CHECK(Json::parse(bytes).at("schemaVersion") == 4);
        for (const auto schema : {1, 2, 3, 5}) {
            auto downgraded = Json::parse(bytes);
            downgraded["schemaVersion"] = schema;
            CHECK(SceneSource::DecodeSceneSource(downgraded.dump()).HasError());
        }
    }
    const auto legacy = SceneSource::DecodeSceneSource(R"({"schemaVersion":1,"objects":[]})");
    REQUIRE(legacy.HasValue());
    CHECK(Json::parse(SceneSource::EncodeSceneSource({legacy.Value().objects, {}})).at("schemaVersion") == 1);
}

TEST_CASE("Animation source rejects malformed closed schemas and contradictory source unions", "[scene][animation][failure]") {
    const auto source = Json::parse(Source(Tests::SceneAnimation::Binding()));
    const std::array<std::pair<std::string_view, Json>, 15> invalid{{{"/component", 0},
                                                                     {"/component", -1},
                                                                     {"/component", "7"},
                                                                     {"/skeleton", "bad"},
                                                                     {"/skeleton", "00000000-0000-0000-0000-000000000000"},
                                                                     {"/source/kind", "future"},
                                                                     {"/source/asset", "00000000-0000-0000-0000-00000000000A"},
                                                                     {"/source/clip", "extra"},
                                                                     {"/rootMotion", "future"},
                                                                     {"/startsPlaying", 0},
                                                                     {"/retarget", false},
                                                                     {"/contractVersion/major", 2},
                                                                     {"/contractVersion/minor", 1},
                                                                     {"/contractVersion/patch", -1},
                                                                     {"/generation", 1}}};
    for (const auto &[pointer, value] : invalid) {
        INFO(pointer);
        auto malformed = source;
        malformed["objects"][0]["components"]["animation"][Json::json_pointer{std::string(pointer)}] = value;
        CHECK(SceneSource::DecodeSceneSource(malformed.dump()).HasError());
    }
    for (const auto field : {"skeleton", "source", "retarget", "contractVersion", "startsPlaying"}) {
        auto incomplete = source;
        incomplete["objects"][0]["components"]["animation"].erase(field);
        CHECK(SceneSource::DecodeSceneSource(incomplete.dump()).HasError());
    }
}

TEST_CASE("Animation conversion and cooked Scene retain binding and exact typed asset requirements", "[scene][animation][cook]") {
    for (const bool clip : {false, true}) {
        const auto binding = Tests::SceneAnimation::Binding(clip);
        const auto scene = Definition(binding);
        REQUIRE(scene.Entities().size() == 1);
        CHECK(scene.Entities().front().components.animation == binding);
        REQUIRE(scene.AssetDependencies().size() == 3);
        CHECK(scene.AssetDependencies()[0].expectedType.Value() == "core.animation.skeleton");
        CHECK(scene.AssetDependencies()[1].expectedType.Value() == (clip ? "core.animation.clip" : "core.animation.graph"));
        CHECK(scene.AssetDependencies()[2].expectedType.Value() == "core.animation.retarget_profile");
        const auto bytes = SceneCook::EncodeCookedSceneDefinition(scene);
        REQUIRE(bytes.HasValue());
        const auto document = Json::parse(bytes.Value());
        CHECK(document.at("format") == SceneCook::AnimationCookedSceneFormat);
        const auto decoded = SceneCook::DecodeCookedSceneDefinition(bytes.Value(), {11}, {13});
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().Entities().front().components.animation == binding);
        CHECK(SceneCook::EncodeCookedSceneDefinition(decoded.Value()).Value() == bytes.Value());
        auto wrongFormat = document;
        wrongFormat["format"] = SceneCook::CookedSceneFormat;
        const auto text = wrongFormat.dump() + '\n';
        const std::vector<std::uint8_t> downgraded{text.begin(), text.end()};
        CHECK(SceneCook::DecodeCookedSceneDefinition(downgraded, {11}, {13}).HasError());
    }
}

TEST_CASE("Animation dependency type conflicts reject Scene definition instead of replacing declared authority",
          "[scene][animation][failure]") {
    auto binding = Tests::SceneAnimation::Binding();
    Runtime::SceneDefinitionBuilder builder{{11}, {13}};
    builder.Add({.object = {1}, .components = {.animation = binding}});
    REQUIRE(builder.RequireAsset({binding.skeleton.Asset(), Assets::AssetTypeId::Parse("core.mesh").Value()}).HasValue());
    CHECK(std::move(builder).Build().HasError());
}

TEST_CASE("Canonical prefab animation projection preserves paused intent and rejects duplicate occurrence", "[prefab][animation][codec]") {
    const auto binding = Tests::SceneAnimation::Binding();
    const auto component = Json::parse(Source(binding)).at("objects")[0].at("components").at("animation").dump();
    Prefab::RawComponentPayload raw{.instance = Prefab::PrefabComponentInstanceId::Create(5).Value(),
                                    .component = {.typeId = Gameplay::ComponentTypeId::Parse("game.horo.animation").Value()}};
    for (const char byte : component)
        raw.component.payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
    Runtime::RuntimeComponentSet destination;
    std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
    const auto projected = SceneSource::ProjectPrefabBuiltinComponent(raw, destination, primitive);
    REQUIRE(projected.HasValue());
    REQUIRE(projected.Value());
    CHECK(destination.animation == binding);
    CHECK(Runtime::BuiltinSceneComponentCount(destination, Runtime::BuiltinSceneComponentKind::Animation) == 1);
    const auto retained = destination;
    CHECK(SceneSource::ProjectPrefabBuiltinComponent(raw, destination, primitive).HasError());
    CHECK(destination == retained);
}
