#include "Horo/Editor/PhysicsInspector.h"
#include "Horo/Scene/SceneSource.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Supply explicit test-only project identities; production never creates these bindings. */
    Assets::AssetId Asset(const std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    /** @brief Complete portable body/collider baseline with unrelated authored content. */
    SceneSource::SceneObjectSnapshot Object(const std::uint64_t id) {
        SceneSource::SceneObjectSnapshot object{.id = {id}, .name = "Physical object"};
        object.components.camera = Runtime::CameraComponent{};
        const auto type = Gameplay::ComponentTypeId::Parse("game.unavailable.future");
        const std::string opaque = "{\"future\":\"opaque\"}";
        Gameplay::SerializedComponent envelope{.typeId = type.Value()};
        for (const char byte : opaque)
            envelope.payload.push_back(static_cast<std::byte>(byte));
        object.components.gameplayComponents.push_back(std::move(envelope));
        object.components.rigidBody = Runtime::RigidBodyComponent{.id = {1}, .body = {10}};
        object.components.colliders.push_back({.id = {2},
                                               .collider = {20},
                                               .body = {{id}, {10}},
                                               .collisionProfile = Physics::CollisionProfileId::FromBytes(Asset(2).Bytes()),
                                               .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), Asset(3)}}});
        return object;
    }

    /** @brief Capture the exact owner-issued transaction fence. */
    SceneTransactionContext Fence(const SceneTransactionSnapshot &snapshot) {
        return {snapshot.session, snapshot.revision, snapshot.state, true, {}};
    }

    /** @brief Extract only the portable Physics fields being edited. */
    PhysicsInspectorValues Values(const SceneSource::SceneObjectSnapshot &object) {
        return {object.components.rigidBody, object.components.colliders, object.components.physicsConstraints};
    }

    /** @brief Canonical source bytes prove undo and rejected edits preserve all unrelated content. */
    std::string Bytes(const SceneTransactionSnapshot &snapshot) {
        return SceneSource::EncodeSceneSource({snapshot.objects, snapshot.prefabInstances});
    }

    /** @brief Exercise one real canonical transaction rather than an independent inspector history. */
    Result<SceneTransactionResult> Apply(SceneDocumentTransactions &owner, const SceneTransactionSnapshot &snapshot,
                                         const PhysicsInspectorValues &values) {
        auto prepared = PreparePhysicsInspectorEdit(snapshot, snapshot.objects.front().id, values);
        if (prepared.HasError())
            return Result<SceneTransactionResult>::Failure(prepared.ErrorValue());
        const std::array changes{std::move(prepared).Value()};
        return owner.Execute(Fence(snapshot), changes);
    }
}  // namespace

TEST_CASE("Physics Inspector sensor edits share exact Scene history and persistence", "[unit][editor][physics][inspector]") {
    auto created = SceneDocumentTransactions::Create({Asset(1), 1}, {Object(1)});
    REQUIRE(created.HasValue());
    auto owner = std::move(created).Value();
    const auto before = owner.Snapshot().Value();
    auto values = Values(before.objects.front());
    values.colliders.front().sensor = true;
    REQUIRE(Apply(owner, before, values).HasValue());
    const auto changed = owner.Snapshot().Value();
    CHECK(changed.dirty);
    CHECK(changed.objects.front().components.colliders.front().sensor);
    CHECK(changed.objects.front().components.colliders.front().generation == 2);
    CHECK(changed.objects.front().components.rigidBody->generation == 1);
    CHECK(changed.objects.front().components.camera == before.objects.front().components.camera);
    CHECK(changed.objects.front().components.gameplayComponents == before.objects.front().components.gameplayComponents);
    const auto decoded = SceneSource::DecodeSceneSource(Bytes(changed));
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().objects.front().components == changed.objects.front().components);
    REQUIRE(owner.Replay(Fence(changed), false).HasValue());
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
    REQUIRE(owner.Replay(Fence(owner.Snapshot().Value()), true).HasValue());
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(changed));
}

TEST_CASE("Physics Inspector rejects invalid complete graphs before any publication", "[unit][editor][physics][inspector]") {
    auto created = SceneDocumentTransactions::Create({Asset(1), 1}, {Object(1), Object(2)});
    REQUIRE(created.HasValue());
    auto owner = std::move(created).Value();
    const auto before = owner.Snapshot().Value();
    auto values = Values(before.objects.front());
    SECTION("dangling collider body") {
        values.body.reset();
    }
    SECTION("body requires a collider") {
        values.colliders.clear();
    }
    SECTION("project profile is explicit") {
        values.colliders.front().collisionProfile = {};
    }
    SECTION("material is explicit") {
        values.colliders.front().materials.front().material = {};
    }
    SECTION("duplicate material slots") {
        values.colliders.front().materials.push_back(values.colliders.front().materials.front());
    }
    SECTION("finite scale") {
        values.colliders.front().scale.x = std::numeric_limits<float>::infinity();
    }
    SECTION("positive dimensions") {
        std::get<Runtime::PhysicsBoxCollider>(std::get<Runtime::PhysicsAnalyticCollider>(values.colliders.front().source))
            .halfExtentsMeters.x = -1;
    }
    SECTION("unsupported schema") {
        values.body->schemaVersion = 2;
    }
    SECTION("nonwrapping generation") {
        values.body->generation = std::numeric_limits<std::uint64_t>::max();
    }
    SECTION("missing constraint endpoint") {
        values.constraints.push_back({.id = {3}, .constraint = {30}, .first = {.body = {{99}, {10}}}});
    }
    SECTION("distance interval") {
        values.constraints.push_back(
            {.id = {3}, .constraint = {30}, .first = {.body = {{1}, {10}}}, .parameters = Runtime::PhysicsDistanceConstraint{2, 1}});
    }
    REQUIRE(Apply(owner, before, values).HasError());
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
    CHECK(owner.Snapshot().Value().revision == before.revision);
    CHECK(owner.Snapshot().Value().dirty == before.dirty);
}

TEST_CASE("Physics Inspector preserves cross-object constraints and rolls back body removal", "[unit][editor][physics][inspector]") {
    auto first = Object(1);
    auto second = Object(2);
    second.components.physicsConstraints.push_back({.id = {3},
                                                    .constraint = {30},
                                                    .first = {.body = {{2}, {10}}},
                                                    .second = Runtime::PhysicsConstraintBodyEndpoint{.body = {{1}, {10}}}});
    auto created = SceneDocumentTransactions::Create({Asset(1), 1}, {first, second});
    REQUIRE(created.HasValue());
    auto owner = std::move(created).Value();
    const auto before = owner.Snapshot().Value();
    CHECK(Apply(owner, before, {}).HasError());
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
    auto values = Values(first);
    values.constraints.push_back(
        {.id = {4}, .constraint = {31}, .first = {.body = {{1}, {10}}}, .parameters = Runtime::PhysicsDistanceConstraint{1, 3}});
    REQUIRE(Apply(owner, before, values).HasValue());
    CHECK(owner.Snapshot().Value().objects.front().components.physicsConstraints.size() == 1);
}

TEST_CASE("Physics Inspector owner fences reject stale foreign denied and closed edits", "[unit][editor][physics][inspector]") {
    auto created = SceneDocumentTransactions::Create({Asset(1), 1}, {Object(1)});
    REQUIRE(created.HasValue());
    auto owner = std::move(created).Value();
    const auto before = owner.Snapshot().Value();
    auto values = Values(before.objects.front());
    values.colliders.front().sensor = true;
    auto prepared = PreparePhysicsInspectorEdit(before, {1}, values);
    REQUIRE(prepared.HasValue());
    const std::array changes{prepared.Value()};
    SECTION("permission") {
        auto context = Fence(before);
        context.canEdit = false;
        CHECK(owner.Execute(context, changes).HasError());
    }
    SECTION("foreign opening") {
        auto context = Fence(before);
        ++context.session.generation;
        CHECK(owner.Execute(context, changes).HasError());
    }
    SECTION("cancelled gesture") {
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        auto context = Fence(before);
        context.cancellation = cancellation.Token();
        CHECK(owner.Execute(context, changes).HasError());
    }
    SECTION("owner thread") {
        bool rejected = false;
        std::thread worker([&] {
            rejected = owner.Execute(Fence(before), changes).HasError();
        });
        worker.join();
        CHECK(rejected);
    }
    SECTION("shutdown") {
        owner.Shutdown();
        CHECK(owner.Execute(Fence(before), changes).HasError());
        return;
    }
    SECTION("stale revision") {
        REQUIRE(owner.Execute(Fence(before), changes).HasValue());
        const auto committed = owner.Snapshot().Value();
        CHECK(owner.Execute(Fence(before), changes).HasError());
        CHECK(Bytes(owner.Snapshot().Value()) == Bytes(committed));
        return;
    }
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
}

TEST_CASE("Physics Inspector cannot wrap a real baseline component generation", "[unit][editor][physics][inspector]") {
    auto object = Object(1);
    object.components.rigidBody->generation = std::numeric_limits<std::uint64_t>::max();
    auto created = SceneDocumentTransactions::Create({Asset(1), 1}, {object});
    REQUIRE(created.HasValue());
    auto owner = std::move(created).Value();
    const auto before = owner.Snapshot().Value();
    auto values = Values(object);
    values.body->enabled = false;
    const auto rejected = Apply(owner, before, values);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == SceneTransactionError(SceneTransactionFailure::Capacity).code.Value());
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
}

TEST_CASE("Physics Inspector refuses locked objects through canonical transaction admission", "[unit][editor][physics][inspector]") {
    auto object = Object(1);
    object.editorState.locked = true;
    auto created = SceneDocumentTransactions::Create({Asset(1), 1}, {object});
    REQUIRE(created.HasValue());
    auto owner = std::move(created).Value();
    const auto before = owner.Snapshot().Value();
    auto values = Values(object);
    values.colliders.front().sensor = true;
    CHECK(Apply(owner, before, values).HasError());
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
}

TEST_CASE("Physics Inspector authored constraint profile rejects hinge and slider wire values", "[unit][editor][physics][inspector]") {
    auto object = Object(1);
    object.components.physicsConstraints.push_back({.id = {3}, .constraint = {30}, .first = {.body = {{1}, {10}}}});
    const std::array objects{object};
    const auto encoded = SceneSource::EncodeSceneSource({objects, {}});
    REQUIRE(SceneSource::DecodeSceneSource(encoded).HasValue());
    for (const auto kind : {"hinge", "slider"}) {
        auto unsupported = encoded;
        const auto position = unsupported.find("\"fixed\"");
        REQUIRE(position != std::string::npos);
        unsupported.replace(position, 7, "\"" + std::string(kind) + "\"");
        CHECK(SceneSource::DecodeSceneSource(unsupported).HasError());
    }
}
