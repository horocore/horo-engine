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
}  // namespace

TEST_CASE("Physics Inspector staging never publishes an unresolved body collider set", "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    const SceneObject object{.id = {7}, .name = "Physical object"};
    CHECK(session.Begin(object, Context(), 1));
    session.AddBody();
    REQUIRE(session.body.has_value());
    REQUIRE(session.colliders.size() == 1);
    CHECK(session.colliders.front().value.body.object.value == object.id.value);
    CHECK_FALSE(session.colliders.front().value.collisionProfile.IsValid());
    CHECK_FALSE(session.colliders.front().materials.front().value.material.IsValid());
    CHECK(session.Request().HasError());
    session.Cancel();
    CHECK_FALSE(session.body.has_value());
    CHECK(session.colliders.empty());
    CHECK_FALSE(session.dirty);
}

TEST_CASE("Physics Inspector invalid identity text and exact selection revisions are retained", "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    const SceneObject object{.id = {7}, .name = "Physical object"};
    REQUIRE(session.Begin(object, Context(), 1));
    session.AddBody();
    session.colliders.front().profile = PhysicsInspectorSession::Identity("invalid");
    CHECK_FALSE(session.Begin(object, Context(), 1));
    CHECK_FALSE(session.colliders.front().profile.valid);
    REQUIRE(session.Begin(object, Context(), 3));
    CHECK_FALSE(session.body.has_value());
    CHECK(session.colliders.empty());
    session.AddConstraint();
    session.Clear();
    CHECK(session.constraints.empty());
    CHECK_FALSE(session.dirty);
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
    CHECK_FALSE(session.body.has_value());
    REQUIRE(session.colliders.size() == 1);
    CHECK(session.colliders.front().value == collider);
    CHECK_FALSE(session.dirty);
}

TEST_CASE("Physics Inspector authority revocation discards an unpublished creation draft", "[unit][editor][physics][inspector]") {
    PhysicsInspectorSession session;
    const SceneObject object{.id = {7}, .name = "Object"};
    auto context = Context();
    REQUIRE(session.Begin(object, context, 1));
    session.AddBody();
    REQUIRE(session.body);
    context.canEdit = false;
    CHECK(session.Begin(object, context, 1));
    CHECK_FALSE(session.body);
    CHECK(session.colliders.empty());
    CHECK_FALSE(session.dirty);
    context.canEdit = true;
    CHECK(session.Begin(object, context, 1));
    CHECK_FALSE(session.body);
}
