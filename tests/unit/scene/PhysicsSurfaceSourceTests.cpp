#include "Horo/Scene/SceneSource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace Horo::SceneSource {
    namespace {
        template <class T> T SurfaceWireId(std::uint8_t value) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return T::FromBytes(bytes);
        }

        std::string SurfaceSceneWire() {
            Runtime::ColliderComponent collider{.id = {11},
                                                .collider = {200},
                                                .body = {.object = {1}, .body = {100}},
                                                .collisionProfile = SurfaceWireId<Physics::CollisionProfileId>(3),
                                                .materials = {
                                                    {Physics::PhysicsMaterialSlotId::FromValue(1), SurfaceWireId<Assets::AssetId>(91)}}};
            collider.surfaceBindings = {{Physics::PhysicsMaterialSlotId::FromValue(1), SurfaceWireId<Physics::SurfaceMaterialId>(1)}};
            const std::array objects{SceneObjectSnapshot{.id = {1},
                                                         .name = "Surface",
                                                         .components = {.rigidBody = Runtime::RigidBodyComponent{.id = {10}, .body = {100}},
                                                                        .colliders = {collider}}}};
            return EncodeSceneSource({objects, {}});
        }
    }  // namespace

    TEST_CASE("Collider surface schema2 roundtrip preserves semantic bindings and legacy1 remains absent",
              "[scene][physics-surface][serialization]") {
        const auto encoded = SurfaceSceneWire();
        auto decoded = DecodeSceneSource(encoded);
        REQUIRE(decoded.HasValue());
        const auto &collider = decoded.Value().objects[0].components.colliders[0];
        CHECK(collider.schemaVersion == 2);
        REQUIRE(collider.surfaceBindings.size() == 1);
        CHECK(collider.surfaceBindings[0].surface == SurfaceWireId<Physics::SurfaceMaterialId>(1));
        CHECK(EncodeSceneSource({decoded.Value().objects, decoded.Value().prefabInstances}) == encoded);
        auto legacy = nlohmann::json::parse(encoded);
        auto &wire = legacy["objects"][0]["components"]["colliders"][0];
        wire["schemaVersion"] = 1;
        wire.erase("surfaceBindings");
        auto old = DecodeSceneSource(legacy.dump());
        REQUIRE(old.HasValue());
        CHECK(old.Value().objects[0].components.colliders[0].surfaceBindings.empty());
    }

    TEST_CASE("Collider semantic schema admits exactly64 rows and rejects one extra without lowering legacy physical bounds",
              "[scene][physics-surface][capacity]") {
        auto source = nlohmann::json::parse(SurfaceSceneWire());
        auto &collider = source["objects"][0]["components"]["colliders"][0];
        const auto physical = collider["materials"][0];
        const auto semantic = collider["surfaceBindings"][0];
        collider["materials"] = nlohmann::json::array();
        collider["surfaceBindings"] = nlohmann::json::array();
        for (std::uint32_t slot = 1; slot <= Runtime::MaximumPhysicsColliderSurfaceBindings; ++slot) {
            auto material = physical;
            material["slot"] = slot;
            collider["materials"].push_back(material);
            auto surface = semantic;
            surface["slot"] = slot;
            collider["surfaceBindings"].push_back(surface);
        }
        CHECK(DecodeSceneSource(source.dump()).HasValue());
        auto material = physical;
        material["slot"] = 65;
        collider["materials"].push_back(material);
        CHECK(DecodeSceneSource(source.dump()).HasValue());  // Physical capacity remains its existing4096.
        auto surface = semantic;
        surface["slot"] = 65;
        collider["surfaceBindings"].push_back(surface);
        CHECK(DecodeSceneSource(source.dump()).HasError());
    }

    TEST_CASE("Collider semantic rows reject duplicate unknown malformed and overflowing physical slots before conversion",
              "[scene][physics-surface][boundary]") {
        auto source = nlohmann::json::parse(SurfaceSceneWire());
        SECTION("duplicate slot") {
            auto &rows = source["objects"][0]["components"]["colliders"][0]["surfaceBindings"];
            rows.push_back(rows[0]);
        }
        SECTION("unknown physical slot") {
            source["objects"][0]["components"]["colliders"][0]["surfaceBindings"][0]["slot"] = 2;
        }
        SECTION("overflow semantic slot") {
            source["objects"][0]["components"]["colliders"][0]["surfaceBindings"][0]["slot"] = 4294967297ULL;
        }
        SECTION("overflow physical slot") {
            source["objects"][0]["components"]["colliders"][0]["materials"][0]["slot"] = 4294967297ULL;
        }
        SECTION("zero surface") {
            source["objects"][0]["components"]["colliders"][0]["surfaceBindings"][0]["surface"] = "00000000-0000-0000-0000-000000000000";
        }
        SECTION("unknown semantic field") {
            source["objects"][0]["components"]["colliders"][0]["surfaceBindings"][0]["guessed"] = 1;
        }
        SECTION("old schema cannot encode semantics") {
            source["objects"][0]["components"]["colliders"][0]["schemaVersion"] = 1;
        }
        SECTION("future schema") {
            source["objects"][0]["components"]["colliders"][0]["schemaVersion"] = 3;
        }
        CHECK(DecodeSceneSource(source.dump()).HasError());
    }
}  // namespace Horo::SceneSource
