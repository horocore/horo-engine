#include "Horo/Editor/PhysicsInspector.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <limits>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Use explicit fixture asset identities without production defaults. */
    Assets::AssetId Asset(const std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    /** @brief Build a real authored flat compound with a child contributed by another scene object. */
    std::vector<SceneSource::SceneObjectSnapshot> Root() {
        SceneSource::SceneObjectSnapshot body{.id = {1}, .name = "Body"};
        body.components.rigidBody = Runtime::RigidBodyComponent{.id = {1}, .body = {10}};
        Runtime::ColliderComponent collider{.id = {2},
                                            .collider = {20},
                                            .body = {{1}, {10}},
                                            .collisionProfile = Physics::CollisionProfileId::FromBytes(Asset(2).Bytes()),
                                            .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), Asset(3)}}};
        body.components.colliders.push_back(collider);
        SceneSource::SceneObjectSnapshot contributor{.id = {2}, .name = "External contributor"};
        contributor.localTransform.translation = {100, 200, 300};
        collider.id = {3};
        collider.collider = {30};
        collider.localPose.translation = {2, 0, 0};
        contributor.components.colliders.push_back(collider);
        return {body, contributor};
    }

    /** @brief Capture the actual canonical transaction fence, including replacement generation. */
    SceneTransactionContext Fence(const SceneTransactionSnapshot &snapshot) {
        return {snapshot.session, snapshot.revision, snapshot.state, true, {}};
    }

    /** @brief Encode canonical persistence bytes to prove rejected edits/undo preserve unrelated content. */
    std::string Bytes(const SceneTransactionSnapshot &snapshot) {
        return SceneSource::EncodeSceneSource({snapshot.objects, snapshot.prefabInstances});
    }

    /** @brief Build an explicit analytic fixture projection through the production source projector. */
    ColliderPresentation Project(const Runtime::PhysicsAnalyticCollider shape) {
        auto objects = Root();
        objects.front().components.colliders.front().source = shape;
        return ProjectPhysicsColliders(objects).Value().front();
    }
}  // namespace

TEST_CASE("Collider projection follows explicit body space and stable contributor identities", "[unit][editor][physics][collider]") {
    auto objects = Root();
    objects.front().localTransform.translation = {3, 4, 5};
    const auto projected = ProjectPhysicsColliders(objects);
    REQUIRE(projected.HasValue());
    REQUIRE(projected.Value().size() == 2);
    const auto &child = projected.Value().back();
    CHECK(child.target == ColliderEditTarget{{2}, {3}});
    CHECK(child.body == Runtime::PhysicsBodyReference{{1}, {10}});
    CHECK(Math::TransformAffinePoint(child.bodyToWorld, child.localTransform.translation) == Math::Vec3{5, 4, 5});
    CHECK(child.editable);
    CHECK(child.visible);
    std::ranges::reverse(objects);
    const auto reordered = ProjectPhysicsColliders(objects).Value();
    const auto same = std::ranges::find(reordered, child.target, &ColliderPresentation::target);
    REQUIRE(same != reordered.end());
    CHECK(same->bodyToWorld == child.bodyToWorld);
    CHECK(same->generation == child.generation);
}

TEST_CASE("Collider projection diagnoses invalid unsupported and locked lifecycle input", "[unit][editor][physics][collider]") {
    auto objects = Root();
    SECTION("inherited body lock blocks contributors") {
        objects.front().editorState.locked = true;
        const auto projection = ProjectPhysicsColliders(objects).Value();
        CHECK_FALSE(projection.back().editable);
        return;
    }
    SECTION("hidden body suppresses every contributor") {
        objects.front().editorState.visible = false;
        CHECK_FALSE(ProjectPhysicsColliders(objects).Value().back().visible);
        return;
    }
    SECTION("disabled body suppresses contributors") {
        objects.front().components.rigidBody->enabled = false;
        CHECK_FALSE(ProjectPhysicsColliders(objects).Value().back().visible);
        return;
    }
    SECTION("cooked source is explicit unsupported") {
        objects.front().components.colliders.front().source = Runtime::PhysicsShapeAssetReference{Asset(9), {1}};
    }
    SECTION("scaled body cannot pretend its collider is accurate") {
        objects.front().localTransform.scale = {2, 1, 1};
    }
    SECTION("missing body is invalid") {
        objects.front().components.rigidBody.reset();
    }
    SECTION("malformed shape is invalid") {
        objects.front().components.colliders.front().scale.x = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("hierarchy cycle is bounded") {
        objects.front().parent = objects.back().id;
        objects.back().parent = objects.front().id;
        CHECK(ProjectPhysicsColliders(objects).HasError());
        return;
    }
    const auto projection = ProjectPhysicsColliders(objects);
    REQUIRE(projection.HasValue());
    CHECK(projection.Value().front().diagnostic.has_value());
    CHECK_FALSE(projection.Value().front().editable);
    CHECK(BuildColliderWireframe(projection.Value().front()).HasError());
}

TEST_CASE("Collider picking rejects conservative bounds and far-offset false hits", "[unit][editor][physics][collider]") {
    auto child = Project(Runtime::PhysicsBoxCollider{});
    Math::Ray ray{{0, 0, 5}, {0, 0, -1}, 0, 10};
    SECTION("rotated thin box rejects conservative world bounds") {
        child.shape = Runtime::PhysicsBoxCollider{{2, 0.1F, 0.1F}};
        child.localTransform.rotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, Math::DegreesToRadians(45));
        ray.origin = {1, -1, 5};
        CHECK_FALSE(PickPhysicsCollider(std::span{&child, 1}, ray).Value());
        return;
    }
    SECTION("sphere silhouette rejects bounding-box corners") {
        child.shape = Runtime::PhysicsSphereCollider{1};
        ray.origin = {0.9F, 0.9F, 5};
        CHECK_FALSE(PickPhysicsCollider(std::span{&child, 1}, ray).Value());
        return;
    }
    SECTION("capsule hemisphere rejects cylinder bounding-box corner") {
        child.shape = Runtime::PhysicsCapsuleCollider{1, 1};
        ray.origin = {0.9F, 1.9F, 5};
        CHECK_FALSE(PickPhysicsCollider(std::span{&child, 1}, ray).Value());
        return;
    }
    SECTION("small sphere at a large admitted ray offset avoids float discriminant cancellation") {
        child.shape = Runtime::PhysicsSphereCollider{0.1F};
        ray.origin = {0.2F, 0, 8000};
        ray.maximumDistance = 8192;
        CHECK_FALSE(PickPhysicsCollider(std::span{&child, 1}, ray).Value());
        return;
    }
}

TEST_CASE("Collider picking intersects exact oriented primitive surfaces and world distance", "[unit][editor][physics][collider]") {
    auto child = Project(Runtime::PhysicsBoxCollider{});
    Math::Ray ray{{0, 0, 5}, {0, 0, -1}, 0, 10};
    SECTION("capsule cap hit") {
        child.shape = Runtime::PhysicsCapsuleCollider{1, 1};
        ray = {{0, 5, 0}, {0, -1, 0}, 0, 10};
    }
    SECTION("plane equation and local pose") {
        child.shape = Runtime::PhysicsStaticPlaneCollider{{0, 1, 0}, 1};
        child.localTransform.translation = {0, 2, 0};
        ray = {{0, 5, 0}, {0, -1, 0}, 0, 10};
    }
    SECTION("nonuniform box preserves world ray segment") {
        child.localTransform.scale = {2, 3, 4};
        ray.maximumDistance = 3.1F;
    }
    SECTION("inside surface chooses the exit") {
        ray = {{0, 0, 0}, {0, 0, 1}, 0, 10};
    }
    SECTION("hidden is not picked") {
        child.visible = false;
        CHECK_FALSE(PickPhysicsCollider(std::span{&child, 1}, ray).Value());
        return;
    }
    SECTION("invalid pick identity fails") {
        child.target.component = {};
        CHECK(PickPhysicsCollider(std::span{&child, 1}, ray).HasError());
        return;
    }
    SECTION("invalid ray fails") {
        ray.direction = {};
        CHECK(PickPhysicsCollider(std::span{&child, 1}, ray).HasError());
        return;
    }
    const auto picked = PickPhysicsCollider(std::span{&child, 1}, ray);
    REQUIRE(picked.HasValue());
    CHECK(picked.Value() == child.target);
    const auto wire = BuildColliderWireframe(child);
    REQUIRE(wire.HasValue());
    CHECK(wire.Value().count > 0);
    CHECK(wire.Value().count <= wire.Value().segments.size());
}

TEST_CASE("Collider nearest hit ordering is stable across reorder and affine scale", "[unit][editor][physics][collider]") {
    auto first = Project(Runtime::PhysicsSphereCollider{1});
    auto second = first;
    second.target = {{2}, {3}};
    second.localTransform.scale = {2, 2, 2};
    std::array children{first, second};
    const Math::Ray ray{{0, 0, 5}, {0, 0, -1}, 0, 10};
    CHECK(PickPhysicsCollider(children, ray).Value() == second.target);
    std::ranges::reverse(children);
    CHECK(PickPhysicsCollider(children, ray).Value() == second.target);
    children.front().localTransform.scale = {1, 1, 1};
    CHECK(PickPhysicsCollider(children, ray).Value() == first.target);
}

TEST_CASE("Collider group gesture snaps from baseline and commits one canonical history item", "[unit][editor][physics][collider]") {
    auto created = SceneDocumentTransactions::Create({Asset(1), 1}, Root());
    REQUIRE(created.HasValue());
    auto owner = std::move(created).Value();
    const auto before = owner.Snapshot().Value();
    const auto children = ProjectPhysicsColliders(before.objects).Value();
    auto draft = children.front().localTransform;
    draft.translation = {0.26F, 0, 0};
    const auto edits = EvaluateColliderGroupTransform(children, draft, true);
    REQUIRE(edits.HasValue());
    REQUIRE(edits.Value().size() == 2);
    CHECK(Math::NearlyEqual(edits.Value()[0].transform.translation.x, 0.3F));
    CHECK(Math::NearlyEqual(edits.Value()[1].transform.translation.x, 2.3F));
    const auto prepared = PrepareColliderTransformEdits(before, edits.Value());
    REQUIRE(prepared.HasValue());
    REQUIRE(prepared.Value().size() == 2);
    REQUIRE(owner.Execute(Fence(before), prepared.Value()).HasValue());
    const auto after = owner.Snapshot().Value();
    CHECK(after.revision == before.revision + 1);
    CHECK(after.objects.back().localTransform == before.objects.back().localTransform);
    CHECK(after.objects.back().components.colliders.front().generation == 2);
    CHECK(after.objects.back().components.colliders.front().materials == before.objects.back().components.colliders.front().materials);
    CHECK(after.objects.back().components.colliders.front().body == before.objects.back().components.colliders.front().body);
    REQUIRE(owner.Replay(Fence(after), false).HasValue());
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
    REQUIRE(owner.Replay(Fence(owner.Snapshot().Value()), true).HasValue());
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(after));
    CHECK(owner.Execute(Fence(before), prepared.Value()).HasError());
}

TEST_CASE("Collider rotation size snapping and unsupported group transforms are explicit", "[unit][editor][physics][collider]") {
    auto children = ProjectPhysicsColliders(Root()).Value();
    auto draft = children.front().localTransform;
    SECTION("rotation snap rotates compound positions around primary pivot") {
        draft.rotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, Math::DegreesToRadians(43));
        const auto result = EvaluateColliderGroupTransform(children, draft, true);
        REQUIRE(result.HasValue());
        CHECK(Math::NearlyEqual(result.Value().back().transform.translation, {std::sqrt(2.0F), std::sqrt(2.0F), 0}));
    }
    SECTION("uniform size snap") {
        draft.scale = {1.26F, 1.26F, 1.26F};
        const auto result = EvaluateColliderGroupTransform(children, draft, true);
        REQUIRE(result.HasValue());
        CHECK(Math::NearlyEqual(result.Value().front().transform.scale, {1.3F, 1.3F, 1.3F}));
    }
    SECTION("sphere shape cannot silently become ellipsoid") {
        children.back().shape = Runtime::PhysicsSphereCollider{};
        draft.scale = {2, 1, 1};
        CHECK(EvaluateColliderGroupTransform(children, draft, false).HasError());
    }
    SECTION("negative size fails even with snapping") {
        draft.scale = {-1, 1, 1};
        CHECK(EvaluateColliderGroupTransform(children, draft, true).HasError());
    }
    SECTION("no motion preserves exact authored values") {
        const auto result = EvaluateColliderGroupTransform(children, draft, false);
        REQUIRE(result.HasValue());
        CHECK(result.Value().back().transform == children.back().localTransform);
    }
}

TEST_CASE("Collider atomic preparation rejects invalid stale cancelled reload and shutdown inputs", "[unit][editor][physics][collider]") {
    auto created = SceneDocumentTransactions::Create({Asset(1), 1}, Root());
    REQUIRE(created.HasValue());
    auto owner = std::move(created).Value();
    const auto before = owner.Snapshot().Value();
    const auto children = ProjectPhysicsColliders(before.objects).Value();
    auto draft = children.front().localTransform;
    draft.translation.x = 1;
    auto edits = EvaluateColliderGroupTransform(children, draft, false).Value();
    SECTION("duplicate identity") {
        edits.push_back(edits.front());
    }
    SECTION("missing child") {
        edits.back().target.component = {999};
    }
    SECTION("stale generation") {
        ++edits.back().generation;
    }
    SECTION("nonfinite pose") {
        edits.back().transform.translation.x = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("invalid dimensions") {
        edits.back().transform.scale.x = 0;
    }
    SECTION("bounded capacity") {
        auto limits = SceneTransactionLimits{};
        limits.maximumPreparedBytes = 1;
        CHECK(PrepareColliderTransformEdits(before, edits, limits).HasError());
        CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
        return;
    }
    CHECK(PrepareColliderTransformEdits(before, edits).HasError());
    CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
}

TEST_CASE("Collider transactions reject cancelled replaced foreign-thread and closed owners", "[unit][editor][physics][collider]") {
    auto created = SceneDocumentTransactions::Create({Asset(1), 1}, Root());
    REQUIRE(created.HasValue());
    auto owner = std::move(created).Value();
    const auto before = owner.Snapshot().Value();
    const auto children = ProjectPhysicsColliders(before.objects).Value();
    auto draft = children.front().localTransform;
    draft.translation.x = 1;
    auto edits = EvaluateColliderGroupTransform(children, draft, false).Value();
    SECTION("cancelled commit") {
        const auto changes = PrepareColliderTransformEdits(before, edits).Value();
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        auto fence = Fence(before);
        fence.cancellation = cancellation.Token();
        CHECK(owner.Execute(fence, changes).HasError());
        CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
        return;
    }
    SECTION("foreign replacement session") {
        const auto changes = PrepareColliderTransformEdits(before, edits).Value();
        auto fence = Fence(before);
        ++fence.session.generation;
        CHECK(owner.Execute(fence, changes).HasError());
        CHECK(Bytes(owner.Snapshot().Value()) == Bytes(before));
        return;
    }
    SECTION("wrong thread and shutdown") {
        const auto changes = PrepareColliderTransformEdits(before, edits).Value();
        bool rejected = false;
        std::thread other([&] {
            rejected = owner.Execute(Fence(before), changes).HasError();
        });
        other.join();
        CHECK(rejected);
        owner.Shutdown();
        CHECK(owner.Execute(Fence(before), changes).HasError());
        return;
    }
}

TEST_CASE("Collider translation snapping follows active axes through rotated body and child poses", "[unit][editor][physics][collider]") {
    auto objects = Root();
    const auto bodyRotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, Math::DegreesToRadians(45));
    const auto childRotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, Math::DegreesToRadians(45));
    objects.front().localTransform.rotation = bodyRotation;
    objects.front().components.colliders.front().localPose.rotation = childRotation;
    const auto children = ProjectPhysicsColliders(objects).Value();
    auto draft = children.front().localTransform;
    const bool localSpace = GENERATE(false, true);
    const Math::Quaternion snapAxes = localSpace ? bodyRotation * childRotation : Math::Quaternion{};
    const Math::Vec3 worldIntent = snapAxes.Rotate({0.26F, 0, 0});
    draft.translation = bodyRotation.Inverse().Rotate(worldIntent);
    const auto edits = EvaluateColliderGroupTransform(children, draft, true, snapAxes);
    REQUIRE(edits.HasValue());
    const auto actualWorldDelta = Math::TransformDirection(children.front().bodyToWorld, edits.Value().front().transform.translation);
    const auto expectedWorldDelta = snapAxes.Rotate({0.3F, 0, 0});
    CHECK(Math::NearlyEqual(actualWorldDelta, expectedWorldDelta));
}
