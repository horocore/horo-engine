#include "EditorPlaySaveHostCapture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Application::Internal;

    /** @brief Keeps the original production error visible in actual Scene/codec fixture setup. */
    template <class T> T Value(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Actual operation accepted."));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Creates a real source-free Scene with camera/light data and a parent relationship. */
    RuntimeSceneDefinition Definition() {
        SceneDefinitionBuilder builder({41}, {3});
        RuntimeEntityDefinition camera{.object = {1}};
        camera.components.camera = CameraComponent{};
        builder.Add(std::move(camera));
        RuntimeEntityDefinition light{.object = {2}, .parent = SceneObjectId{1}};
        light.components.light = LightComponent{};
        builder.Add(std::move(light));
        return Value(std::move(builder).Build());
    }

    /** @brief Supplies authored durable identities independently from runtime slot allocation. */
    std::vector<PersistentEntityRecord> Identities() {
        return {{Value(PersistentEntityId::Parse("5dba4f6c-c5c1-4f03-a758-030b87859810")),
                 {1},
                 AuthoredPersistentEntityProvenance{{41}, {1}},
                 PersistentEntityDisposition::Live},
                {Value(PersistentEntityId::Parse("5dba4f6c-c5c1-4f03-a758-030b87859820")),
                 {1},
                 AuthoredPersistentEntityProvenance{{41}, {2}},
                 PersistentEntityDisposition::Live}};
    }

    /** @brief Uses the same actual runtime definition and command owner as isolated Play, without an authoring serializer. */
    struct Fixture final {
        RuntimeSceneDefinition baseline{Definition()};
        std::vector<PersistentEntityRecord> identities{Identities()};
        std::unique_ptr<RuntimeScene> scene{Value(RuntimeScene::Create(baseline, {501}))};
    };
}  // namespace

TEST_CASE("Play save captures actual transforms components hierarchy and explicit deleted authored identities", "[editor][play][save]") {
    Fixture f;
    const auto original = Value(SceneCook::EncodeCookedSceneDefinition(f.baseline));
    SceneCommandBuffer mutation;
    Math::Transform moved;
    moved.translation = {7, 8, 9};
    mutation.SetLocalTransform(*f.scene->View().Find({1}), moved);
    mutation.Destroy(*f.scene->View().Find({2}));
    REQUIRE(f.scene->Commit(mutation).HasValue());
    const auto captured = Value(CapturePlaySaveScene(f.scene->View(), f.baseline, f.identities));
    REQUIRE(captured.definition.Entities().size() == 1);
    CHECK(captured.definition.Entities()[0].localTransform == moved);
    CHECK(captured.definition.Entities()[0].components.camera == f.baseline.Entities()[0].components.camera);
    REQUIRE(captured.identities.size() == 2);
    CHECK(captured.identities[0].disposition == PersistentEntityDisposition::Live);
    CHECK(captured.identities[1].disposition == PersistentEntityDisposition::Tombstone);
    const auto bytes = Value(EncodePlaySaveScene(captured));
    const auto decoded = Value(DecodePlaySaveScene(bytes.Bytes(), f.baseline, f.identities));
    REQUIRE(decoded.definition.Entities().size() == 1);
    CHECK(decoded.definition.Entities()[0].localTransform == moved);
    CHECK(decoded.identities[1].disposition == PersistentEntityDisposition::Tombstone);
    CHECK(Value(SceneCook::EncodeCookedSceneDefinition(f.baseline)) == original);
    CHECK(f.baseline.Revision() == SceneDefinitionRevision{3});
}

TEST_CASE("Play save canonical record rejects foreign identities truncation trailing data and baseline changes", "[editor][play][save]") {
    Fixture f;
    const auto captured = Value(CapturePlaySaveScene(f.scene->View(), f.baseline, f.identities));
    const auto bytes = Value(EncodePlaySaveScene(captured));
    SECTION("foreign durable inventory") {
        f.identities[0].identity = Value(PersistentEntityId::Parse("a6baf2ab-65cd-4a29-99b1-31b557463e20"));
        CHECK(DecodePlaySaveScene(bytes.Bytes(), f.baseline, f.identities).HasError());
    }
    SECTION("truncated canonical value") {
        CHECK(DecodePlaySaveScene(bytes.Bytes().first(bytes.Bytes().size() - 1), f.baseline, f.identities).HasError());
    }
    SECTION("trailing bytes") {
        std::vector<std::byte> damaged(bytes.Bytes().begin(), bytes.Bytes().end());
        damaged.push_back(std::byte{0});
        CHECK(DecodePlaySaveScene(damaged, f.baseline, f.identities).HasError());
    }
    SECTION("foreign baseline") {
        SceneDefinitionBuilder builder({42}, {3});
        const auto other = Value(std::move(builder).Build());
        CHECK(DecodePlaySaveScene(bytes.Bytes(), other, f.identities).HasError());
    }
}

TEST_CASE("Play save capture refuses stale views incomplete identity coverage and unregistered runtime owners", "[editor][play][save]") {
    Fixture f;
    SECTION("stale view") {
        auto stale = f.scene->View();
        SceneCommandBuffer mutation;
        mutation.Destroy(*stale.Find({2}));
        REQUIRE(f.scene->Commit(mutation).HasValue());
        CHECK(CapturePlaySaveScene(stale, f.baseline, f.identities).HasError());
    }
    SECTION("missing record") {
        f.identities.pop_back();
        CHECK(CapturePlaySaveScene(f.scene->View(), f.baseline, f.identities).HasError());
    }
    SECTION("unknown authored origin") {
        f.identities[0].provenance = AuthoredPersistentEntityProvenance{{41}, {99}};
        CHECK(CapturePlaySaveScene(f.scene->View(), f.baseline, f.identities).HasError());
    }
    SECTION("native runtime data has no adapter") {
        SceneDefinitionBuilder builder({41}, {3});
        RuntimeEntityDefinition entity{.object = {1}};
        entity.components.audioListener = AudioListenerComponent{};
        builder.Add(std::move(entity));
        const auto baseline = Value(std::move(builder).Build());
        auto scene = Value(RuntimeScene::Create(baseline, {502}));
        CHECK(CapturePlaySaveScene(scene->View(), baseline, std::span{f.identities}.first(1)).HasError());
    }
}
