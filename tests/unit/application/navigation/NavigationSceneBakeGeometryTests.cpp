#include "Horo/Application/NavigationSceneBakeGeometry.h"
#include "navigation/NavigationSceneSourceFixture.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <numbers>

using namespace Horo;
using namespace Horo::Application;

namespace {
    namespace Sources = Tests::NavigationSources;

    struct Fixture final {
        NavigationSceneBakeSource scene{.asset = Sources::Asset(2), .path = ProjectPath::Parse("Assets/scene.horo").Value()};
        NavigationDefinitionBakeSource definition{.asset = Sources::Asset(1),
                                                  .path = ProjectPath::Parse("Assets/navigation.horoasset").Value()};

        explicit Fixture(float cellSize = 0.3F) {
            Navigation::NavigationDefinitionInput input;
            input.profiles.push_back({Sources::Id<Navigation::NavigationAgentProfileId>(1), "Walker", {.cellSizeMeters = cellSize}});
            const Navigation::NavigationDescriptorSource owner{Navigation::NavigationDescriptorSourceKind::Project,
                                                               Sources::Id<Navigation::NavigationDescriptorSourceId>(1)};
            input.areas.push_back({Sources::Id<Navigation::NavigationAreaId>(3), owner, 1, {1}});
            input.filters.push_back({Sources::Id<Navigation::NavigationFilterId>(4), owner, {1}, {}, {}});
            auto created = Navigation::NavigationDefinition::Create(std::move(input));
            REQUIRE(created.HasValue());
            definition.definition = std::make_shared<Navigation::NavigationDefinition>(std::move(created).Value());
            scene.document.objects.push_back(Sources::Object());
        }

        auto Capture(const NavigationSceneGeometryPolicy &policy = {}, CancellationToken cancellation = {}) const {
            return CaptureNavigationSceneColliderGeometry(scene, definition, policy, cancellation);
        }

        auto &Object() {
            return scene.document.objects.front();
        }
    };
}  // namespace

TEST_CASE("Scene collider capture preserves exact authored ownership and canonical source semantics", "[navigation][geometry][scene]") {
    Fixture fixture;
    const auto result = fixture.Capture();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().size() == 1);
    const auto &captured = result.Value().front();
    CHECK(captured.scene == fixture.scene.asset);
    CHECK(captured.object == fixture.Object().id);
    CHECK(captured.component == fixture.Object().components.colliders.front().id);
    CHECK(captured.collider == fixture.Object().components.colliders.front().collider);
    CHECK(captured.body == fixture.Object().components.colliders.front().body);
    CHECK(captured.source == fixture.Object().components.navigationSources.front());
    REQUIRE(captured.geometry.vertices.size() == 8);
    CHECK(captured.geometry.triangles.size() == 12);
    for (const auto &triangle : captured.geometry.triangles) {
        CHECK(triangle.area == captured.source.area);
        CHECK(triangle.materialSlot.value == 1);
    }
}

TEST_CASE("Collider local placement follows the referenced body through the full hierarchy", "[navigation][geometry][scene]") {
    Fixture fixture;
    auto &object = fixture.Object();
    auto body = SceneSource::SceneObjectSnapshot{.id = {10}, .parent = SceneSource::SceneObjectId{20}, .name = "Body"};
    body.localTransform.translation = {2, 0, 0};
    body.components.rigidBody = object.components.rigidBody;
    object.components.rigidBody.reset();
    object.components.colliders.front().body.object = {10};
    object.components.colliders.front().localPose.translation = {0, 3, 0};
    object.localTransform.translation = {100, 100, 100};
    SceneSource::SceneObjectSnapshot parent{.id = {20}, .name = "Parent"};
    parent.localTransform.translation = {10, 0, 0};
    parent.localTransform.rotation = Math::Quaternion::FromAxisAngle({0, 0, 1}, std::numbers::pi_v<float> / 2);
    fixture.scene.document.objects.push_back(body);
    fixture.scene.document.objects.push_back(parent);
    const auto result = fixture.Capture();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().size() == 1);
    const auto first = result.Value().front().geometry.vertices.front();
    CHECK(first.x == Catch::Approx(7.5F));
    CHECK(first.y == Catch::Approx(1.5F));
    CHECK(first.z == Catch::Approx(-0.5F));
}

TEST_CASE("Complete Scene graph admission rejects malformed disconnected branches and cached-depth overflow",
          "[navigation][geometry][scene]") {
    Fixture fixture;
    NavigationSceneGeometryPolicy policy;
    SECTION("duplicate object") {
        fixture.scene.document.objects.push_back(fixture.Object());
    }
    SECTION("absent parent") {
        fixture.Object().parent = SceneSource::SceneObjectId{99};
    }
    SECTION("self cycle") {
        fixture.Object().parent = fixture.Object().id;
    }
    SECTION("disconnected cycle") {
        fixture.scene.document.objects.push_back({.id = {2}, .parent = SceneSource::SceneObjectId{3}});
        fixture.scene.document.objects.push_back({.id = {3}, .parent = SceneSource::SceneObjectId{2}});
    }
    SECTION("cached parent depth") {
        policy.maximumHierarchyDepth = 2;
        fixture.scene.document.objects.push_back({.id = {2}, .parent = SceneSource::SceneObjectId{1}});
        fixture.scene.document.objects.push_back({.id = {3}, .parent = SceneSource::SceneObjectId{2}});
    }
    SECTION("nonfinite unrelated transform") {
        fixture.scene.document.objects.push_back({.id = {2}});
        fixture.scene.document.objects.back().localTransform.translation.x = std::numeric_limits<float>::infinity();
    }
    CHECK(fixture.Capture(policy).HasError());
}

TEST_CASE("Navigation geometry refuses invalid exact body and collider references", "[navigation][geometry][scene]") {
    Fixture fixture;
    auto &object = fixture.Object();
    auto &collider = object.components.colliders.front();
    SECTION("dynamic body") {
        object.components.rigidBody->motion = Runtime::AuthoredPhysicsMotionType::Dynamic;
        object.components.rigidBody->mass = Runtime::AuthoredPhysicsMass{1};
    }
    SECTION("disabled body") {
        object.components.rigidBody->enabled = false;
    }
    SECTION("disabled collider") {
        collider.enabled = false;
    }
    SECTION("sensor") {
        collider.sensor = true;
    }
    SECTION("wrong body slot") {
        collider.body.body = {99};
    }
    SECTION("body missing") {
        object.components.rigidBody.reset();
    }
    SECTION("unknown local collider") {
        object.components.navigationSources.front().collider = {99};
    }
    SECTION("scaled body") {
        object.localTransform.scale = {2, 2, 2};
    }
    CHECK(fixture.Capture().HasError());
}

TEST_CASE("Navigation geometry refuses invalid exact definition material and qualified-source references",
          "[navigation][geometry][scene]") {
    Fixture fixture;
    auto &object = fixture.Object();
    auto &collider = object.components.colliders.front();
    SECTION("unknown area") {
        object.components.navigationSources.front().area = Sources::Id<Navigation::NavigationAreaId>(99);
    }
    SECTION("unknown filter") {
        object.components.navigationSources.front().filter = Sources::Id<Navigation::NavigationFilterId>(99);
    }
    SECTION("unknown profile") {
        object.components.navigationSurface->profiles = {Sources::Id<Navigation::NavigationAgentProfileId>(99)};
    }
    SECTION("wide material slot") {
        collider.materials.front().slot = Physics::PhysicsMaterialSlotId::FromValue(1ULL << 40U);
    }
    SECTION("ambiguous analytic materials") {
        collider.materials.emplace_back(Physics::PhysicsMaterialSlotId::FromValue(2), Sources::Asset(5));
    }
    SECTION("unqualified cooked geometry") {
        collider.source = Runtime::PhysicsShapeAssetReference{Sources::Asset(7), {1}};
    }
    SECTION("unknown components") {
        fixture.scene.document.unsupportedComponentOwners.push_back(object.id);
    }
    CHECK(fixture.Capture().HasError());
}

TEST_CASE("Authored subtree selection is checked independently of collider body placement", "[navigation][geometry][scene]") {
    Fixture fixture;
    auto surface = SceneSource::SceneObjectSnapshot{.id = {2}};
    surface.components.navigationSurface = fixture.Object().components.navigationSurface;
    fixture.Object().components.navigationSurface.reset();
    fixture.scene.document.objects.push_back(surface);
    CHECK(fixture.Capture().HasError());
    fixture.Object().parent = SceneSource::SceneObjectId{2};
    CHECK(fixture.Capture().HasValue());
}

TEST_CASE("LocalBounds clips actual triangles in the rotated surface frame and preserves valid empty closure",
          "[navigation][geometry][scene]") {
    Fixture fixture;
    auto surface = SceneSource::SceneObjectSnapshot{.id = {2}};
    surface.components.navigationSurface = fixture.Object().components.navigationSurface;
    fixture.Object().components.navigationSurface.reset();
    surface.components.navigationSurface->bakeScope = Runtime::NavigationBakeScope::LocalBounds;
    surface.components.navigationSurface->localBounds = Runtime::NavigationLocalBounds{{}, {0.25F, 2, 0.25F}};
    surface.localTransform.rotation = Math::Quaternion::FromAxisAngle({0, 1, 0}, std::numbers::pi_v<float> / 4);
    fixture.scene.document.objects.push_back(surface);
    auto result = fixture.Capture();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().size() == 1);
    REQUIRE_FALSE(result.Value().front().geometry.triangles.empty());
    const auto inverse = Math::TryInverseAffine(surface.localTransform.ToMatrix());
    REQUIRE(inverse.HasValue());
    for (const auto &vertex : result.Value().front().geometry.vertices) {
        const auto local = Math::TransformPoint(inverse.Value(), vertex);
        CHECK(std::abs(local.x) <= 0.250001F);
        CHECK(std::abs(local.z) <= 0.250001F);
    }
    fixture.scene.document.objects.back().localTransform.translation = {10, 0, 0};
    result = fixture.Capture();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().size() == 1);
    CHECK(result.Value().front().geometry.vertices.empty());
    CHECK(result.Value().front().geometry.triangles.empty());
}

TEST_CASE("Source container order and names do not change geometry or stable ownership", "[navigation][geometry][scene]") {
    Fixture fixture;
    auto second = fixture.Object().components.colliders.front();
    second.id = {4};
    second.collider = {5};
    second.localPose.translation = {5, 0, 0};
    fixture.Object().components.colliders.push_back(second);
    auto binding = fixture.Object().components.navigationSources.front();
    binding.collider = {5};
    fixture.Object().components.navigationSources.insert(fixture.Object().components.navigationSources.begin(), binding);
    const auto first = fixture.Capture();
    REQUIRE(first.HasValue());
    REQUIRE(first.Value().size() == 2);
    CHECK(first.Value()[0].collider.value == 3);
    CHECK(first.Value()[1].collider.value == 5);
    std::ranges::reverse(fixture.Object().components.colliders);
    std::ranges::reverse(fixture.Object().components.navigationSources);
    fixture.Object().name = "Renamed source";
    fixture.scene.path = ProjectPath::Parse("Assets/moved source.horo").Value();
    const auto reordered = fixture.Capture();
    REQUIRE(reordered.HasValue());
    REQUIRE(reordered.Value().size() == 2);
    for (std::size_t index = 0; index < 2; ++index) {
        CHECK(first.Value()[index].object == reordered.Value()[index].object);
        CHECK(first.Value()[index].component == reordered.Value()[index].component);
        CHECK(first.Value()[index].geometry.digest == reordered.Value()[index].geometry.digest);
    }
    NavigationSceneGeometryPolicy policy;
    policy.geometry.storage.maxVertices = 15;
    CHECK(fixture.Capture(policy).HasError());
    policy.geometry.storage.maxVertices = 100;
    policy.geometry.storage.maxTriangles = 23;
    CHECK(fixture.Capture(policy).HasError());
    policy.geometry.storage.maxTriangles = 100;
    policy.geometry.storage.maxContributions = 1;
    CHECK(fixture.Capture(policy).HasError());
}

TEST_CASE("Scene infinite planes receive extent only from explicit authored LocalBounds", "[navigation][geometry][scene]") {
    Fixture fixture;
    fixture.Object().components.colliders.front().source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsStaticPlaneCollider{}};
    NavigationSceneGeometryPolicy policy;
    policy.geometry.planeBounds = Math::Aabb{{-10, -10, -10}, {10, 10, 10}};
    CHECK(fixture.Capture(policy).HasError());
    fixture.Object().components.navigationSurface->bakeScope = Runtime::NavigationBakeScope::LocalBounds;
    fixture.Object().components.navigationSurface->localBounds = Runtime::NavigationLocalBounds{{}, {2, 1, 2}};
    const auto geometry = fixture.Capture(policy);
    REQUIRE(geometry.HasValue());
    REQUIRE(geometry.Value().size() == 1);
    CHECK(geometry.Value().front().geometry.triangles.size() == 2);
    for (const auto vertex : geometry.Value().front().geometry.vertices)
        CHECK(vertex.y == 0);
}

TEST_CASE("Selected profile resolution tightens faceting admission instead of using an implicit default", "[navigation][geometry][scene]") {
    Fixture coarse;
    coarse.Object().components.colliders.front().source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{}};
    NavigationSceneGeometryPolicy policy;
    policy.geometry.maximumSegments = 64;
    REQUIRE(coarse.Capture(policy).HasValue());
    Fixture fine{0.001F};
    fine.Object().components.colliders.front().source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{}};
    CHECK(fine.Capture(policy).HasError());
}

TEST_CASE("Disabled explicit source remains authored while capture admits no implicit collider", "[navigation][geometry][scene]") {
    Fixture fixture;
    fixture.Object().components.navigationSources.front().enabled = false;
    const auto result = fixture.Capture();
    REQUIRE(result.HasValue());
    CHECK(result.Value().empty());
    CHECK(fixture.Object().components.colliders.size() == 1);
    CHECK(fixture.Object().components.navigationSources.size() == 1);
}

TEST_CASE("Scene geometry bounds and cancellation reject the complete candidate", "[navigation][geometry][scene]") {
    Fixture fixture;
    NavigationSceneGeometryPolicy policy;
    CancellationSource cancellation;
    SECTION("cancelled") {
        cancellation.RequestCancellation();
    }
    SECTION("object bound") {
        policy.maximumObjects = 0;
    }
    SECTION("vertex aggregate") {
        policy.geometry.storage.maxVertices = 7;
    }
    SECTION("triangle aggregate") {
        policy.geometry.storage.maxTriangles = 11;
    }
    SECTION("owned byte budget") {
        policy.geometry.storage.maxOwnedBytes = sizeof(NavigationCapturedCollider) + 1;
    }
    CHECK(fixture.Capture(policy, cancellation.Token()).HasError());
}

TEST_CASE("Scene capture consumes inherited bake-job cancellation directly", "[navigation][geometry][scene][cancellation]") {
    Fixture fixture;
    CancellationSource parent;
    CancellationSource child{parent.Token()};
    parent.RequestCancellation();
    const auto result = fixture.Capture({}, child.Token());
    REQUIRE(result.HasError());
    CHECK(ErrorChainContains(result.ErrorValue(), Navigation::NavigationErrors::BakeInputCancelled.domain,
                             Navigation::NavigationErrors::BakeInputCancelled.code));
}
