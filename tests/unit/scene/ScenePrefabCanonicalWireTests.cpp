#include "ScenePrefabExpansionTestSupport.h"
#include "SceneSourceInternal.h"

#include <limits>
#include <nlohmann/json.hpp>

using namespace Horo;
using namespace Horo::Prefab;
using namespace Horo::SceneSource;
using namespace Horo::SceneSource::ExpansionTestSupport;

namespace {
    using OldWire = nlohmann::ordered_json;

    const std::string WireText = "héllo \"quote\"\\\n\t\x01";

    /** @brief Golden scalar/vector expectations use the original pinned codec, never the new writer. */
    OldWire OldTransform() {
        return {{"translation", OldWire::array({0.0F, 1.0e-20F, 1.0e20F})},
                {"rotation", OldWire::array({0.0F, 0.0F, 0.0F, 1.0F})},
                {"scale", OldWire::array({1.0F, 1.0F, 1.0F})}};
    }

    /** @brief Full typed behavior-kind matrix with canonical field order and normalized signed zero. */
    OldWire OldFields() {
        return OldWire::array(
            {{{"name", "a_null"}, {"value", {{"type", "null"}, {"value", nullptr}}}},
             {{"name", "b_bool"}, {"value", {{"type", "bool"}, {"value", false}}}},
             {{"name", "c_int"}, {"value", {{"type", "int"}, {"value", std::numeric_limits<std::int64_t>::min()}}}},
             {{"name", "d_number"}, {"value", {{"type", "number"}, {"value", 1.0e-100}}}},
             {{"name", "e_text"}, {"value", {{"type", "string"}, {"value", WireText}}}},
             {{"name", "f_vec2"}, {"value", {{"type", "vec2"}, {"value", OldWire::array({0.0F, 2.0F})}}}},
             {{"name", "g_vec3"}, {"value", {{"type", "vec3"}, {"value", OldWire::array({1.0F, 0.0F, 3.0F})}}}},
             {{"name", "h_quaternion"}, {"value", {{"type", "quaternion"}, {"value", OldWire::array({0.0F, 0.0F, 0.0F, 1.0F})}}}},
             {{"name", "i_zero"}, {"value", {{"type", "number"}, {"value", 0.0}}}}});
    }

    /** @brief Independent ordered golden hierarchy, including opaque unsigned payload bytes and empty children. */
    OldWire OldObject(const bool rich) {
        auto components = OldWire::array();
        auto behaviors = OldWire::array();
        if (rich) {
            components.push_back({{"instanceId", std::numeric_limits<std::uint64_t>::max()},
                                  {"typeId", "game.tests.wire"},
                                  {"schemaVersion", 3},
                                  {"encoding", "canonicalJson"},
                                  {"payload", {{"bytes", OldWire::array({0U, 127U, 255U})}}}});
            behaviors.push_back(
                {{"instanceId", 9U}, {"typeId", "game.tests.wire"}, {"schemaVersion", 2U}, {"enabled", true}, {"fields", OldFields()}});
        }
        return {{"localId", 0U},
                {"parentLocalId", nullptr},
                {"name", WireText},
                {"localTransform", OldTransform()},
                {"components", std::move(components)},
                {"behaviors", std::move(behaviors)}};
    }

    /** @brief Typed source fixture corresponding to the independent old-codec golden hierarchy. */
    PrefabDocumentData WireData(const ExpansionFixture &fixture, const bool rich) {
        PrefabDocumentData data{.projectVersion = fixture.version,
                                .assetId = fixture.asset,
                                .objects = {
                                    {.localId = {0}, .name = WireText, .localTransform = {.translation = {-0.0F, 1.0e-20F, 1.0e20F}}}}};
        if (rich) {
            data.objects.front().components = {
                {.instance = PrefabComponentInstanceId::Create(std::numeric_limits<std::uint64_t>::max()).Value(),
                 .component = {.typeId = Gameplay::ComponentTypeId::Parse("game.tests.wire").Value(),
                               .schemaVersion = 3,
                               .payload = {std::byte{0}, std::byte{127}, std::byte{255}}}}};
            data.objects.front().behaviors = {{.instanceId = {9},
                                               .typeId = Gameplay::BehaviorTypeId::Parse("game.tests.wire").Value(),
                                               .schemaVersion = 2,
                                               .enabled = true,
                                               .fields = {{"a_null", std::monostate{}},
                                                          {"b_bool", false},
                                                          {"c_int", std::numeric_limits<std::int64_t>::min()},
                                                          {"d_number", 1.0e-100},
                                                          {"e_text", WireText},
                                                          {"f_vec2", Math::Vec2{-0.0F, 2}},
                                                          {"g_vec3", Math::Vec3{1, -0.0F, 3}},
                                                          {"h_quaternion", Math::Quaternion{-0.0F, 0, 0, 1}},
                                                          {"i_zero", -0.0}}}};
        }
        return data;
    }

    /** @brief Requires exact bytes, including whitespace/escaping/number spelling and the final newline. */
    void RequireGoldenWire(const PrefabDocumentData &data, const OldWire &expected, const PrefabLimitProfile &limits) {
        const auto document = PrefabDocument::Create(data, limits);
        REQUIRE(document.HasValue());
        const auto actual = document.Value().SerializeCanonical();
        REQUIRE(actual.HasValue());
        CHECK(actual.Value() == expected.dump(2) + '\n');
        const auto parsed = PrefabDocument::Parse(actual.Value(), limits);
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().SerializeCanonical().Value() == actual.Value());
    }

    /** @brief Independent old sorted-map wire for non-default persisted camera and dynamic-body authoring. */
    nlohmann::json OldSceneComponents() {
        return {{"camera",
                 {{"projection", "orthographic"},
                  {"verticalFieldOfViewRadians", 0.75F},
                  {"orthographicHeight", 12.0F},
                  {"nearPlane", 0.25F},
                  {"farPlane", 300.0F},
                  {"enabled", false}}},
                {"rigidBody",
                 {{"id", 21U},
                  {"body", 22U},
                  {"schemaVersion", 1U},
                  {"generation", 3U},
                  {"motion", "dynamic"},
                  {"mass", {{"kind", "density"}, {"kilogramsPerCubicMeter", 17.0F}}},
                  {"initialLinearVelocity", {1.0F, 2.0F, 3.0F}},
                  {"initialAngularVelocity", {4.0F, 5.0F, 6.0F}},
                  {"linearDampingPerSecond", 0.125F},
                  {"angularDampingPerSecond", 0.25F},
                  {"maximumLinearSpeed", 50.0F},
                  {"maximumAngularSpeed", 20.0F},
                  {"enabled", false}}}};
    }

    /** @brief Rich Scene fixture exercises the changed component, physics, primitive and transform encoders. */
    void PopulateRichScene(SceneSourceDocument &document, const Assets::AssetId mesh) {
        auto &object = document.objects.front();
        object.name = WireText;
        object.localTransform = {.translation = {-0.0F, 1.0e-20F, 1.0e20F}};
        object.editorState = {.visible = false, .locked = true};
        object.meshAsset = mesh;
        object.primitiveMesh = Runtime::PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box);
        object.primitiveMesh->parameters = Runtime::BoxMeshParameters{{2, 3, 4}};
        object.components.camera = Runtime::CameraComponent{.projection = Runtime::CameraProjection::Orthographic,
                                                            .verticalFieldOfViewRadians = 0.75F,
                                                            .orthographicHeight = 12,
                                                            .nearPlane = 0.25F,
                                                            .farPlane = 300,
                                                            .enabled = false};
        object.components.rigidBody = Runtime::RigidBodyComponent{.id = {21},
                                                                  .body = {22},
                                                                  .generation = 3,
                                                                  .motion = Runtime::AuthoredPhysicsMotionType::Dynamic,
                                                                  .mass = Runtime::AuthoredPhysicsDensity{17},
                                                                  .initialLinearVelocity = {1, 2, 3},
                                                                  .initialAngularVelocity = {4, 5, 6},
                                                                  .linearDampingPerSecond = 0.125F,
                                                                  .angularDampingPerSecond = 0.25F,
                                                                  .maximumLinearSpeed = 50,
                                                                  .maximumAngularSpeed = 20,
                                                                  .enabled = false};
    }

    /** @brief Checks both the safe wire and the explicit DOM bridge used by editor/cook against an independent oracle. */
    void RequireSceneGolden(const SceneSourceDocument &document, const nlohmann::json &expected) {
        const SceneSourceView view{document.objects, document.prefabInstances};
        const std::string oldBytes = expected.dump(2) + '\n';
        CHECK(EncodeSceneSource(view) == oldBytes);
        CHECK(Horo::SceneSource::Detail::SceneJson(view).ToJson().dump(2) + '\n' == oldBytes);
        const auto parsed = DecodeSceneSource(oldBytes);
        REQUIRE(parsed.HasValue());
        CHECK(EncodeSceneSource({parsed.Value().objects, parsed.Value().prefabInstances}) == oldBytes);
    }
}  // namespace

TEST_CASE("Allocation-safe Prefab writer preserves original canonical bytes for every persisted envelope", "[native][prefab][wire]") {
    ExpansionFixture fixture;
    bool rich{};
    SECTION("empty payload arrays") {}
    SECTION("all behavior kinds opaque payload unsigned identities and escaped UTF-8") {
        rich = true;
    }
    auto data = WireData(fixture, rich);
    OldWire expected{{"projectVersion", "1.2.3"},
                     {"assetId", fixture.asset.ToString()},
                     {"objects", OldWire::array({OldObject(rich)})},
                     {"referencedAssets", OldWire::array()}};
    RequireGoldenWire(data, expected, fixture.limits);
}

TEST_CASE("Allocation-safe Prefab writer preserves nested and variant composition wire", "[native][prefab][wire]") {
    ExpansionFixture fixture;
    const auto dependency = Assets::AssetId::FromBytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 18});
    const std::string digestInput{"wire"};
    const PrefabSourceRevision revision{fixture.version, ComputeSha256(std::as_bytes(std::span{digestInput}))};
    auto data = WireData(fixture, false);
    data.referencedAssets = {dependency};
    OldWire expected{{"projectVersion", "1.2.3"}, {"assetId", fixture.asset.ToString()}, {"objects", OldWire::array({OldObject(false)})}};
    const OldWire oldRevision{{"projectVersion", "1.2.3"}, {"contentDigest", FormatSha256(revision.contentDigest)}};
    SECTION("nested root placement canonicalizes absent parent") {
        data.composition = PrefabComposition{.nestedPlacements = {{.placementLocalId = {1},
                                                                   .sourcePrefab = PrefabAssetReference::Create(dependency).Value(),
                                                                   .authoredAgainst = revision,
                                                                   .localRootTransform = data.objects.front().localTransform}}};
        expected["composition"] = {{"nestedPlacements", OldWire::array({{{"placementLocalId", 1U},
                                                                         {"parentLocalId", nullptr},
                                                                         {"sourceAsset", dependency.ToString()},
                                                                         {"authoredAgainst", oldRevision},
                                                                         {"localRootTransform", OldTransform()}}})}};
    }
    SECTION("variant includes both optional revision and source with empty object array") {
        data.objects.clear();
        data.composition =
            PrefabComposition{.variantParent = PrefabAssetReference::Create(dependency).Value(), .variantAuthoredAgainst = revision};
        expected["objects"] = OldWire::array();
        expected["composition"] = {{"variantParent", dependency.ToString()}, {"variantAuthoredAgainst", oldRevision}};
    }
    expected["referencedAssets"] = OldWire::array({dependency.ToString()});
    RequireGoldenWire(data, expected, fixture.limits);
}

TEST_CASE("Allocation-safe Scene writer preserves the original sorted map wire", "[native][scene][wire]") {
    ExpansionFixture fixture;
    auto request = fixture.Request();
    const nlohmann::json transform{{"translation", {0.0F, 0.0F, 0.0F}},
                                   {"rotation", {0.0F, 0.0F, 0.0F, 1.0F}},
                                   {"scale", {1.0F, 1.0F, 1.0F}}};
    auto placementTransform = transform;
    placementTransform["translation"][0] = 3.0F;
    nlohmann::json expected{{"schemaVersion", 1U},
                            {"objects",
                             {{{"id", 9U},
                               {"parent", nullptr},
                               {"name", "Owner"},
                               {"transform", transform},
                               {"components", nlohmann::json::object()},
                               {"editor", {{"visible", true}, {"locked", false}}},
                               {"primitiveMesh", nullptr},
                               {"meshAsset", nullptr}}}},
                            {"prefabInstances",
                             {{{"instanceId", 4U},
                               {"sourceAsset", fixture.asset.ToString()},
                               {"parent", 9U},
                               {"rootTransform", placementTransform}}}}};
    SECTION("default authored object and placement") {}
    SECTION("rich persisted components physics primitive mesh and escaped text") {
        PopulateRichScene(request.document, fixture.asset);
        auto &object = expected["objects"][0];
        object["name"] = WireText;
        // Scene preserves authored signed zero; Prefab canonical normalization is deliberately separate.
        object["transform"]["translation"] = {-0.0F, 1.0e-20F, 1.0e20F};
        object["editor"] = {{"visible", false}, {"locked", true}};
        object["meshAsset"] = fixture.asset.ToString();
        object["primitiveMesh"] = {{"id", "primitive.mesh.box"}, {"version", 1U}, {"parameters", {{"size", {2.0F, 3.0F, 4.0F}}}}};
        object["components"] = OldSceneComponents();
    }
    SECTION("empty document retains empty arrays and envelope") {
        request.document = {};
        expected["objects"] = nlohmann::json::array();
        expected["prefabInstances"] = nlohmann::json::array();
    }
    RequireSceneGolden(request.document, expected);
}
