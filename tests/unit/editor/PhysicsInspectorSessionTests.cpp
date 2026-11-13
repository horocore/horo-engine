#include "editor/screens/workspace/panels/inspector/PhysicsInspectorSession.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief An owner-issued fence, independent of editor object addresses. */
    SceneTransactionContext Context() {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 1;
        return {{Assets::AssetId::FromBytes(bytes), 1}, 1, 1, true, {}};
    }

    /** @brief Supply complete canonical bindings so each invalid text case changes exactly one admitted input. */
    SceneObject BoundObject() {
        SceneObject object{.id = {7}, .name = "Bound physics"};
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 1;
        const auto asset = Assets::AssetId::FromBytes(bytes);
        object.components.rigidBody = Runtime::RigidBodyComponent{.id = {1}, .body = {10}};
        object.components.colliders.push_back({.id = {2},
                                               .collider = {20},
                                               .body = {{7}, {10}},
                                               .collisionProfile = Physics::CollisionProfileId::FromBytes(bytes),
                                               .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), asset}}});
        object.components.physicsConstraints.push_back({.id = {3},
                                                        .constraint = {30},
                                                        .first = {.body = {{7}, {10}}},
                                                        .second = Runtime::PhysicsConstraintBodyEndpoint{.body = {{8}, {11}}}});
        return object;
    }
}  // namespace

TEST_CASE("Physics Inspector staging never publishes an unresolved body collider set", "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    const SceneObject object{.id = {7}, .name = "Physical object"};
    CHECK(session.Begin(object, Context(), 1));
    session.AddBody();
    REQUIRE(session.Draft().body.has_value());
    REQUIRE(session.Draft().colliders.size() == 1);
    CHECK(session.Draft().colliders.front().value.body.object.value == object.id.value);
    CHECK_FALSE(session.Draft().colliders.front().value.collisionProfile.IsValid());
    CHECK_FALSE(session.Draft().colliders.front().materials.front().value.material.IsValid());
    CHECK(session.Request().HasError());
    session.Cancel();
    CHECK_FALSE(session.Draft().body.has_value());
    CHECK(session.Draft().colliders.empty());
    CHECK_FALSE(session.Draft().dirty);
}

TEST_CASE("Physics Inspector invalid identity text and exact selection revisions are retained", "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    const SceneObject object{.id = {7}, .name = "Physical object"};
    REQUIRE(session.Begin(object, Context(), 1));
    session.AddBody();
    session.Draft().colliders.front().profile = PhysicsInspectorSession::Identity("invalid");
    CHECK_FALSE(session.Begin(object, Context(), 1));
    CHECK_FALSE(session.Draft().colliders.front().profile.valid);
    REQUIRE(session.Begin(object, Context(), 3));
    CHECK_FALSE(session.Draft().body.has_value());
    CHECK(session.Draft().colliders.empty());
    session.AddConstraint();
    session.Clear();
    CHECK(session.Draft().constraints.empty());
    CHECK_FALSE(session.Draft().dirty);
}

TEST_CASE("Physics Inspector decimal identities preserve the complete uint64 range", "[unit][editor][physics][inspector]") {
    const auto maximum = PhysicsInspectorSession::Decimal(std::numeric_limits<std::uint64_t>::max());
    CHECK(maximum.valid);
    CHECK(std::string_view(maximum.bytes.data()) == "18446744073709551615");
    CHECK_FALSE(PhysicsInspectorSession::Decimal(0).valid);
}

TEST_CASE("Physics Inspector exhausted local identities cannot leave a partial staged body", "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    SceneObject object{.id = {7}, .name = "Collider contributor"};
    Runtime::ColliderComponent collider{.id = {1}, .collider = {1}, .body = {{8}, {10}}};
    SECTION("no room for both new component identities") {
        collider.id.value = std::numeric_limits<std::uint64_t>::max() - 1;
    }
    SECTION("no free collider slot") {
        collider.collider.value = std::numeric_limits<std::uint64_t>::max();
    }
    object.components.colliders.push_back(collider);
    REQUIRE(session.Begin(object, Context(), 1));
    session.AddBody();
    CHECK_FALSE(session.Draft().body.has_value());
    REQUIRE(session.Draft().colliders.size() == 1);
    CHECK(session.Draft().colliders.front().value == collider);
    CHECK_FALSE(session.Draft().dirty);
}

TEST_CASE("Physics Inspector authority revocation discards an unpublished creation draft", "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    const SceneObject object{.id = {7}, .name = "Object"};
    auto context = Context();
    REQUIRE(session.Begin(object, context, 1));
    session.AddBody();
    REQUIRE(session.Draft().body);
    context.canEdit = false;
    CHECK(session.Begin(object, context, 1));
    CHECK_FALSE(session.Draft().body);
    CHECK(session.Draft().colliders.empty());
    CHECK_FALSE(session.Draft().dirty);
    context.canEdit = true;
    CHECK(session.Begin(object, context, 1));
    CHECK_FALSE(session.Draft().body);
}

TEST_CASE("Physics Inspector preserves complete existing bindings and both constraint endpoints", "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    const auto object = BoundObject();
    REQUIRE(session.Begin(object, Context(), 1));
    const auto request = session.Request();
    REQUIRE(request.HasValue());
    CHECK(request.Value().values.body == object.components.rigidBody);
    CHECK(request.Value().values.colliders == object.components.colliders);
    CHECK(request.Value().values.constraints == object.components.physicsConstraints);
    CHECK(request.Value().selectionRevision == 1);
    const auto &captured = static_cast<const PhysicsInspectorSession &>(session).Draft();
    CHECK_FALSE(captured.dirty);
}

TEST_CASE("Physics Inspector refuses invalid material and endpoint text without losing captured values",
          "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    const auto object = BoundObject();
    REQUIRE(session.Begin(object, Context(), 1));
    SECTION("first endpoint") {
        session.Draft().constraints.front().firstObject.valid = false;
    }
    SECTION("second endpoint") {
        session.Draft().constraints.front().secondBody.valid = false;
    }
    SECTION("material slot") {
        session.Draft().colliders.front().materials.front().slot.valid = false;
    }
    SECTION("material asset") {
        session.Draft().colliders.front().materials.front().asset.valid = false;
    }
    CHECK(session.Request().HasError());
    session.Cancel();
    const auto restored = session.Request();
    REQUIRE(restored.HasValue());
    CHECK(restored.Value().values.constraints == object.components.physicsConstraints);
    CHECK(restored.Value().values.colliders == object.components.colliders);
    CHECK_FALSE(session.Draft().dirty);
}

TEST_CASE("Physics Inspector slot exhaustion preserves every captured contributor", "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    SceneObject object{.id = {7}, .name = "Exhausted draft"};
    object.components.colliders.push_back({.id = {2}, .collider = {std::numeric_limits<std::uint64_t>::max()}, .body = {{7}, {10}}});
    object.components.physicsConstraints.push_back(
        {.id = {3}, .constraint = {std::numeric_limits<std::uint64_t>::max()}, .first = {.body = {{7}, {10}}}});
    REQUIRE(session.Begin(object, Context(), 1));
    session.AddCollider();
    session.AddConstraint();
    REQUIRE(session.Draft().colliders.size() == 1);
    REQUIRE(session.Draft().constraints.size() == 1);
    CHECK(session.Draft().colliders.front().value == object.components.colliders.front());
    CHECK(session.Draft().constraints.front().value == object.components.physicsConstraints.front());
    CHECK_FALSE(session.Draft().dirty);
}

TEST_CASE("Adding a collider or trigger supplies its local static body without inventing project references",
          "[editor][physics][inspector]") {
    for (const bool trigger : {false, true}) {
        PhysicsInspectorSession session;
        const SceneObject object{.id = {7}, .name = "Box"};
        REQUIRE(session.Begin(object, Context(), 1));
        session.AddCollider(trigger);
        REQUIRE(session.Draft().body.has_value());
        REQUIRE(session.Draft().colliders.size() == 1);
        const auto &body = *session.Draft().body;
        const auto &collider = session.Draft().colliders.front();
        CHECK(body.motion == Runtime::AuthoredPhysicsMotionType::Static);
        CHECK(collider.value.sensor == trigger);
        CHECK(collider.value.body.object.value == object.id.value);
        CHECK(collider.value.body.body == body.body);
        CHECK(collider.value.id != body.id);
        CHECK(collider.bodyObject.valid);
        CHECK(collider.bodySlot.valid);
        CHECK_FALSE(collider.profile.valid);
        CHECK(session.Request().HasError());
        session.Cancel();
        CHECK_FALSE(session.Draft().body.has_value());
        CHECK(session.Draft().colliders.empty());
    }
}
