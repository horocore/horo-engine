#include "CollisionSettingsTestSupport.h"

#include <nlohmann/json.hpp>

namespace Horo::Tests::CollisionSettings {
    namespace {
        std::string SceneWithUnknown(const bool opaque) {
            const SceneSource::SceneObjectSnapshot object{.id = {7}, .name = "Owner"};
            const std::array objects{object};
            auto source = nlohmann::json::parse(SceneSource::EncodeSceneSource({objects, {}}));
            if (opaque)
                source["objects"][0]["components"]["futurePhysicsOwner"] = {{"profile", "opaque"}};
            return source.dump();
        }
    }  // namespace

    TEST_CASE("Collision closed Scene typed collider owner blocks profile deletion until its reference is migrated",
              "[collision][scene][references]") {
        Project project;
        const Runtime::ColliderComponent collider{.id = {43},
                                                  .collider = {44},
                                                  .body = {.object = {7}, .body = {42}},
                                                  .source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{.radiusMeters =
                                                                                                                                1.25F}},
                                                  .collisionProfile = IdOf<Physics::CollisionProfileId>(3),
                                                  .materials = {{.slot = Physics::PhysicsMaterialSlotId::FromValue(1),
                                                                 .material = IdOf<Assets::AssetId>(9)}}};
        const SceneSource::SceneObjectSnapshot object{.id = {7}, .name = "Collider", .components = {.colliders = {collider}}};
        const std::array objects{object};
        project.Scene(SceneSource::EncodeSceneSource({objects, {}}));
        auto session = project.Open();
        auto review = session->Preview(RemoveSpare(session->Snapshot().Value()->Input()));
        REQUIRE(review.HasValue());
        CHECK_FALSE(review.Value().CanCommit());
        CHECK(std::ranges::any_of(review.Value().Impacts(), [](const auto &v) {
            return v.profile == IdOf<Physics::CollisionProfileId>(3) && v.object == SceneSource::SceneObjectId{7} && v.blocksPublication;
        }));
        auto migrated = object;
        migrated.components.colliders.front().collisionProfile = IdOf<Physics::CollisionProfileId>(2);
        const std::array repaired{migrated};
        Write(project.root / "Assets/Scene.horoasset", SceneSource::EncodeSceneSource({repaired, {}}));
        Commit(*session, RemoveSpare(session->Snapshot().Value()->Input()));
        CHECK(session->IsDirty());
    }

    TEST_CASE("Collision deletion of disabled persistent IDs still requires complete opaque-owner impact admission",
              "[collision][references]") {
        auto input = Input();
        const auto disabled = IdOf<Physics::CollisionLayerId>(10);
        input.schema.layers.push_back({.id = disabled, .enabled = false});
        input.layers.push_back({disabled, "Retired authored layer", {}});
        Project project{std::move(input)};
        project.Scene(SceneWithUnknown(true));
        auto session = project.Open();
        auto candidate = session->Snapshot().Value()->Input();
        std::erase_if(candidate.schema.layers, [disabled](const auto &layer) {
            return layer.id == disabled;
        });
        std::erase_if(candidate.layers, [disabled](const auto &layer) {
            return layer.id == disabled;
        });
        auto review = session->Preview(std::move(candidate));
        REQUIRE(review.HasValue());
        CHECK_FALSE(review.Value().CanCommit());
        CHECK(std::ranges::any_of(review.Value().Impacts(), [](const auto &impact) {
            return impact.unsupported && impact.blocksPublication;
        }));
        CHECK_FALSE(session->IsDirty());
    }

    TEST_CASE("Collision decoder owns complete component recognition and reports sorted unique unsupported owners",
              "[collision][scene][inventory]") {
        auto known = SceneSource::DecodeSceneSource(SceneWithUnknown(false));
        REQUIRE(known.HasValue());
        CHECK(known.Value().unsupportedComponentOwners.empty());
        auto source = nlohmann::json::parse(SceneWithUnknown(true));
        source["objects"][0]["components"]["anotherUnknown"] = true;
        auto decoded = SceneSource::DecodeSceneSource(source.dump());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().unsupportedComponentOwners.size() == 1);
        CHECK(decoded.Value().unsupportedComponentOwners.front() == SceneSource::SceneObjectId{7});
        CHECK(SceneSource::EncodeSceneSource({decoded.Value().objects, decoded.Value().prefabInstances}) ==
              SceneSource::EncodeSceneSource({known.Value().objects, known.Value().prefabInstances}));
    }

    TEST_CASE("Collision opaque physical source evidence survives canonical active snapshot override", "[collision][scene][inventory]") {
        Project project;
        project.Scene(SceneWithUnknown(true));
        auto created = Editor::SceneDocumentTransactions::Create({IdOf<Assets::AssetId>(20), 1});
        REQUIRE(created.HasValue());
        auto scene = std::move(created).Value();
        auto session = project.Open(&scene);
        auto candidate = RemoveSpare(session->Snapshot().Value()->Input());
        auto review = session->Preview(candidate);
        REQUIRE(review.HasValue());
        CHECK_FALSE(review.Value().CanCommit());
        CHECK(std::ranges::any_of(review.Value().Impacts(), [](const auto &v) {
            return v.unsupported && v.blocksPublication && v.object == SceneSource::SceneObjectId{7};
        }));
        CHECK(session->Commit(review.Value()).HasError());
        CHECK_FALSE(session->IsDirty());
        session->Close();
        scene.Shutdown();
    }

    TEST_CASE("Collision canonical Scene changes after review reject without replacing either owner", "[collision][scene][fences]") {
        Project project;
        project.Scene(SceneWithUnknown(false));
        auto created = Editor::SceneDocumentTransactions::Create({IdOf<Assets::AssetId>(20), 1});
        REQUIRE(created.HasValue());
        auto scene = std::move(created).Value();
        auto session = project.Open(&scene);
        const auto before = session->Snapshot().Value();
        auto input = before->Input();
        input.layers.front().name = "Display";
        auto review = session->Preview(input);
        REQUIRE(review.HasValue());
        REQUIRE(review.Value().CanCommit());
        const auto snapshot = scene.Snapshot().Value();
        Editor::SceneObjectReplacement add{.object = {8},
                                           .desired = SceneSource::SceneObjectSnapshot{.id = {8}, .name = "New"},
                                           .resources = {4096, 4096, 4096}};
        REQUIRE(scene.Execute({snapshot.session, snapshot.revision, snapshot.state, true, {}}, std::span{&add, 1}).HasValue());
        CHECK(session->Commit(review.Value()).HasError());
        CHECK(session->Snapshot().Value() == before);
        CHECK(scene.Snapshot().Value().objects.size() == 1);
        session->Close();
        scene.Shutdown();
    }

    TEST_CASE("Collision closed Scene source deletion and malformed future policy never imply empty references",
              "[collision][scene][inventory]") {
        Project project;
        project.Scene(SceneWithUnknown(false));
        auto session = project.Open();
        auto candidate = RemoveSpare(session->Snapshot().Value()->Input());
        SECTION("duplicate root") {
            Write(project.root / "Assets/Scene.horoasset", R"({"schemaVersion":1,"schemaVersion":1,"objects":[]})");
        }
        SECTION("future") {
            Write(project.root / "Assets/Scene.horoasset", R"({"schemaVersion":999,"objects":[]})");
        }
        SECTION("deleted source") {
            std::filesystem::remove(project.root / "Assets/Scene.horoasset");
        }
        SECTION("malformed") {
            Write(project.root / "Assets/Scene.horoasset", "{");
        }
        CHECK(session->Preview(candidate).HasError());
        CHECK_FALSE(session->IsDirty());
    }

    TEST_CASE("Collision prefab and unknown registered owners block deletion but retain stable rename support", "[collision][inventory]") {
        Project project;
        Write(project.root / "Assets/Prefab.prefab", "opaque owner");
        Write(project.root / "Assets/Prefab.prefab.horo",
              R"({"schemaVersion":1,"assetId":"30213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.prefab"})");
        project.Rebuild();
        auto session = project.Open();
        auto candidate = RemoveSpare(session->Snapshot().Value()->Input());
        auto review = session->Preview(candidate);
        REQUIRE(review.HasValue());
        CHECK_FALSE(review.Value().CanCommit());
        CHECK(std::ranges::any_of(review.Value().Impacts(), [](const auto &v) {
            return v.unsupported && v.blocksPublication;
        }));
        auto rename = session->Snapshot().Value()->Input();
        rename.layers.front().name = "Stable display rename";
        Commit(*session, rename);
        CHECK(session->IsDirty());
    }
}  // namespace Horo::Tests::CollisionSettings
