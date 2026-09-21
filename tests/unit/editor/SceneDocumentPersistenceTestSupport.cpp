#include "SceneDocumentPersistenceTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <thread>
#include <utility>

namespace Horo::Editor::PersistenceTestSupport {
    const ErrorCodeDescriptor InjectedReplaceFailure{
        .domain = ErrorDomainId{"test.scene_persistence"},
        .code = ErrorCode{"replace_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Injected scene replacement failure.",
    };

    TemporaryProject::TemporaryProject() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() / ("horo-scene-persistence-" + std::to_string(stamp));
        std::filesystem::create_directories(root_ / ".horo");
        std::filesystem::create_directories(root_ / "assets/scenes");
    }

    TemporaryProject::~TemporaryProject() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    const std::filesystem::path &TemporaryProject::Root() const noexcept {
        return root_;
    }

    std::filesystem::path TemporaryProject::ScenePath() const {
        return root_ / "assets/scenes/main.horo";
    }

    std::filesystem::path TemporaryProject::RecoveryPath() const {
        return root_ / ".horo/local/recovery/default-scene.hororecovery";
    }

    void TemporaryProject::PrepareEmptyScene(const std::string_view contents) const {
        WriteMetadata();
        WriteScene(std::string{contents});
    }

    void TemporaryProject::WriteMetadata(const std::string &defaultScene) const {
        std::ofstream output(root_ / ".horo/project.json", std::ios::binary);
        output << R"({"settings":{"defaultScene":")" << defaultScene << R"("}})";
    }

    void TemporaryProject::WriteScene(const std::string &contents) const {
        std::ofstream output(ScenePath(), std::ios::binary);
        output << contents;
    }

    Result<ExclusiveFileLock> NativeBackedFileSystem::TryAcquireExclusive(const std::filesystem::path &path,
                                                                          const std::string_view ownerMetadata) {
        return native_.TryAcquireExclusive(path, ownerMetadata);
    }

    Result<std::uint64_t> NativeBackedFileSystem::AvailableBytes(const std::filesystem::path &path) const {
        return native_.AvailableBytes(path);
    }

    Result<void> NativeBackedFileSystem::WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
        return native_.WriteDurable(path, bytes);
    }

    Result<void> NativeBackedFileSystem::CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) {
        return native_.CopyDurable(source, destination);
    }

    Result<void> NativeBackedFileSystem::AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) {
        return native_.AtomicReplace(prepared, destination);
    }

    Result<void> ReplaceFailingFileSystem::AtomicReplace(const std::filesystem::path &, const std::filesystem::path &) {
        return Result<void>::Failure(MakeError(InjectedReplaceFailure));
    }

    Result<void> NativeBackedFileSystem::RemoveDurable(const std::filesystem::path &path) {
        return native_.RemoveDurable(path);
    }

    Result<void> NativeBackedFileSystem::SyncDirectory(const std::filesystem::path &path) {
        return native_.SyncDirectory(path);
    }

    InterferingFileSystem::InterferingFileSystem(std::filesystem::path canonicalPath) : canonicalPath_(std::move(canonicalPath)) {}

    Result<void> InterferingFileSystem::WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
        Result<void> written = native_.WriteDurable(path, bytes);
        if (written.HasValue() && path.extension() == ".tmp") {
            std::ofstream external(canonicalPath_, std::ios::binary | std::ios::trunc);
            external << "{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n";
        }
        return written;
    }

    [[nodiscard]] Runtime::NavigationSurfaceComponent MakeNavigationSurface() {
        return {
            .id = Navigation::SurfaceId::Create(19).Value(),
            .definition = Assets::AssetId::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee").Value(),
            .schemaVersion = 1,
            .generation = 4,
            .bakeScope = Runtime::NavigationBakeScope::LocalBounds,
            .localBounds = Runtime::NavigationLocalBounds{.center = {1.0F, 0.0F, -2.0F}, .halfExtents = {8.0F, 2.0F, 5.0F}},
            .profiles = {Navigation::NavigationAgentProfileId::Create(6).Value()},
        };
    }

    [[nodiscard]] Runtime::NavigationRegionComponent MakeNavigationRegion() {
        return {
            .id = Navigation::NavigationRegionId::Create(27).Value(),
            .surface = Navigation::SurfaceId::Create(19).Value(),
            .generation = 3,
            .localBounds = {.center = {-1.0F, 0.5F, 2.0F}, .halfExtents = {3.0F, 1.0F, 4.0F}},
            .sourceSelection = Runtime::NavigationRegionSourceSelection::StaticCollisionInBounds,
            .mode = Runtime::NavigationRegionMode::Exclude,
        };
    }

    [[nodiscard]] Runtime::NavigationModifierComponent MakeNavigationModifier() {
        return {
            .id = Navigation::NavigationModifierId::Create(31).Value(),
            .surface = Navigation::SurfaceId::Create(19).Value(),
            .generation = 2,
            .volume = Runtime::NavigationCylinderVolume{.center = {0.5F, 1.0F, -0.5F}, .radius = 2.5F, .halfHeight = 1.25F},
            .operation = Runtime::NavigationModifierOperation::OverrideAreaAndCost,
            .area = Navigation::NavigationAreaId::Create(12).Value(),
            .traversalCost = 1.75F,
        };
    }

    [[nodiscard]] Runtime::NavigationLinkComponent MakeNavigationLink() {
        return {
            .id = Navigation::NavigationLinkId::Create(37).Value(),
            .generation = 5,
            .start = {.surface = Navigation::SurfaceId::Create(19).Value(),
                      .localPosition = {-2.0F, 0.0F, 0.0F},
                      .connectionRadiusMeters = 0.75F},
            .end = {.surface = Navigation::SurfaceId::Create(19).Value(),
                    .localPosition = {2.0F, 0.0F, 0.0F},
                    .connectionRadiusMeters = 1.0F},
            .kind = Runtime::NavigationLinkKind::Door,
            .direction = Runtime::NavigationLinkDirection::Bidirectional,
            .profiles = {Navigation::NavigationAgentProfileId::Create(6).Value()},
            .traversalCost = 2.0F,
        };
    }

    [[nodiscard]] Runtime::ColliderComponent MakeAuthoredSphereCollider() {
        return {
            .id = {43},
            .collider = {44},
            .body = {.object = {1}, .body = {42}},
            .source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{.radiusMeters = 1.25F}},
            .localPose = {.translation = {0.0F, 0.5F, 0.0F}},
            .collisionProfile = Physics::CollisionProfileId::Parse("11111111-2222-4333-8444-555555555555").Value(),
            .materials = {{.slot = Physics::PhysicsMaterialSlotId::FromValue(1),
                           .material = Assets::AssetId::Parse("99999999-aaaa-4bbb-8ccc-dddddddddddd").Value()}},
        };
    }

    [[nodiscard]] Runtime::PhysicsConstraintComponent MakeAuthoredWorldConstraint() {
        return {
            .id = {45},
            .constraint = {46},
            .first = {.body = {.object = {1}, .body = {42}}},
            .second = Runtime::PhysicsConstraintWorldEndpoint{.frame = {.translation = {0.0F, 4.0F, 0.0F}}},
            .parameters = Runtime::PhysicsDistanceConstraint{.minimumMeters = 0.5F, .maximumMeters = 4.0F},
        };
    }

    [[nodiscard]] Gameplay::BehaviorComponent MakeAuthoredBehavior() {
        return {
            .instanceId = Gameplay::BehaviorInstanceId{44},
            .typeId = Gameplay::BehaviorTypeId::Parse("game.tests.persisted_behavior").Value(),
            .schemaVersion = 3,
            .enabled = false,
            .fields =
                {
                    Gameplay::BehaviorField{"speed", 2.5},
                    Gameplay::BehaviorField{"label", std::string{"Unknown payload survives"}},
                    Gameplay::BehaviorField{"offset", Math::Vec3{1.0F, 2.0F, 3.0F}},
                },
        };
    }

    [[nodiscard]] SceneObjectComponentSet MakeAuthoredComponents() {
        SceneObjectComponentSet components{
            .camera = Runtime::CameraComponent{.nearPlane = 0.25F, .farPlane = 500.0F, .enabled = false},
            .light = Runtime::LightComponent{.kind = Runtime::LightKind::Point, .intensity = 3.0F},
            .triggerVolume = Runtime::TriggerVolumeComponent{Runtime::ColliderShapeType::Sphere},
            .audioSource = Runtime::AudioSourceComponent{.playback = {.gain = 0.75F, .spatialMode = Audio::AudioSpatialMode::TwoD}},
        };
        components.navigationSurface = MakeNavigationSurface();
        components.navigationRegion = MakeNavigationRegion();
        components.navigationModifier = MakeNavigationModifier();
        components.navigationLink = MakeNavigationLink();
        components.rigidBody = Runtime::RigidBodyComponent{.id = {41}, .body = {42}};
        components.colliders = {MakeAuthoredSphereCollider()};
        components.physicsConstraints = {MakeAuthoredWorldConstraint()};
        components.behaviors = {MakeAuthoredBehavior()};
        return components;
    }

    [[nodiscard]] CreateSceneObjectCommand MakePrimaryObjectCommand() {
        return {
            .name = "Persisted Box",
            .localTransform =
                Math::Transform{
                    .translation = {2.0F, 3.0F, -4.0F},
                    .rotation = Math::Quaternion::FromEulerRadians({0.1F, 0.2F, 0.3F}),
                    .scale = {1.5F, 2.0F, 0.5F},
                },
            .primitiveMesh = PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box),
            .components = MakeAuthoredComponents(),
        };
    }

    struct PhysicsFixture {
        Physics::CollisionProfileId profile;
        Runtime::PhysicsColliderMaterialBinding materialBinding;
    };

    [[nodiscard]] PhysicsFixture MakePhysicsFixture() {
        const auto profile = Physics::CollisionProfileId::Parse("11111111-2222-4333-8444-555555555555").Value();
        const auto material = Assets::AssetId::Parse("99999999-aaaa-4bbb-8ccc-dddddddddddd").Value();
        return {profile, {.slot = Physics::PhysicsMaterialSlotId::FromValue(1), .material = material}};
    }

    [[nodiscard]] CreateSceneObjectCommand MakeDynamicPhysicsCommand(const PhysicsFixture &fixture) {
        return {
            .name = "Dynamic Physics Variants",
            .components =
                SceneObjectComponentSet{
                    .rigidBody = Runtime::RigidBodyComponent{.id = {51},
                                                             .body = {52},
                                                             .motion = Runtime::AuthoredPhysicsMotionType::Dynamic,
                                                             .mass = Runtime::AuthoredPhysicsMass{.kilograms = 12.0F}},
                    .colliders =
                        {
                            Runtime::ColliderComponent{.id = {53},
                                                       .collider = {54},
                                                       .body = {.object = {2}, .body = {52}},
                                                       .source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsBoxCollider{}},
                                                       .collisionProfile = fixture.profile,
                                                       .materials = {fixture.materialBinding}},
                            Runtime::ColliderComponent{.id = {55},
                                                       .collider = {56},
                                                       .body = {.object = {2}, .body = {52}},
                                                       .source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsCapsuleCollider{}},
                                                       .collisionProfile = fixture.profile,
                                                       .materials = {fixture.materialBinding}},
                            Runtime::ColliderComponent{.id = {57},
                                                       .collider = {58},
                                                       .body = {.object = {2}, .body = {52}},
                                                       .source = Runtime::
                                                           PhysicsShapeAssetReference{.asset = Assets::AssetId::Parse(
                                                                                                   "aaaaaaaa-1111-4222-8333-bbbbbbbbbbbb")
                                                                                                   .Value(),
                                                                                      .subresource = {3}},
                                                       .collisionProfile = fixture.profile,
                                                       .materials = {fixture.materialBinding}},
                        },
                    .physicsConstraints = {Runtime::PhysicsConstraintComponent{
                        .id = {59},
                        .constraint = {60},
                        .first = {.body = {.object = {2}, .body = {52}}},
                        .second = Runtime::PhysicsConstraintBodyEndpoint{.body = {.object = {1}, .body = {42}}},
                        .parameters = Runtime::PhysicsFixedConstraint{},
                    }},
                },
        };
    }

    [[nodiscard]] CreateSceneObjectCommand MakeDensityPhysicsCommand(const PhysicsFixture &fixture) {
        return {
            .name = "Density Physics Variant",
            .components =
                SceneObjectComponentSet{
                    .rigidBody = Runtime::RigidBodyComponent{.id = {61},
                                                             .body = {62},
                                                             .motion = Runtime::AuthoredPhysicsMotionType::Dynamic,
                                                             .mass = Runtime::AuthoredPhysicsDensity{}},
                    .colliders = {Runtime::ColliderComponent{.id = {63},
                                                             .collider = {64},
                                                             .body = {.object = {3}, .body = {62}},
                                                             .collisionProfile = fixture.profile,
                                                             .materials = {fixture.materialBinding}}},
                },
        };
    }

    [[nodiscard]] CreateSceneObjectCommand MakeStaticPlanePhysicsCommand(const PhysicsFixture &fixture) {
        return {
            .name = "Static Plane Physics Variant",
            .components =
                SceneObjectComponentSet{
                    .rigidBody =
                        Runtime::RigidBodyComponent{.id = {71}, .body = {72}, .motion = Runtime::AuthoredPhysicsMotionType::Kinematic},
                    .colliders = {Runtime::ColliderComponent{.id = {73},
                                                             .collider = {74},
                                                             .body = {.object = {4}, .body = {72}},
                                                             .source =
                                                                 Runtime::PhysicsAnalyticCollider{Runtime::PhysicsStaticPlaneCollider{}},
                                                             .collisionProfile = fixture.profile,
                                                             .materials = {fixture.materialBinding}}},
                },
        };
    }

    void AppendPhysicsFixtures(SceneDocumentCommandExecutor &commands, const PhysicsFixture &fixture) {
        REQUIRE(commands.Execute(MakeDynamicPhysicsCommand(fixture)).HasValue());
        REQUIRE(commands.Execute(MakeDensityPhysicsCommand(fixture)).HasValue());
        REQUIRE(commands.Execute(MakeStaticPlanePhysicsCommand(fixture)).HasValue());
    }

    void AppendPrefabFixture(SceneDocumentCommandExecutor &commands, const SceneObjectId parent) {
        const auto prefabAsset = Assets::AssetId::Parse("11112222-3333-4444-8888-9999aaaabbbb");
        REQUIRE(prefabAsset.HasValue());
        const auto sourcePrefab = Prefab::PrefabAssetReference::Create(prefabAsset.Value());
        REQUIRE(sourcePrefab.HasValue());
        REQUIRE(commands
                    .Execute(CreateScenePrefabInstanceCommand{
                        sourcePrefab.Value(),
                        parent,
                        Math::Transform{.translation = {-3.0F, 2.0F, 7.0F}, .scale = {0.5F, 0.5F, 0.5F}},
                    })
                    .HasValue());
    }

    [[nodiscard]] SceneDocumentSnapshot AuthoredScene() {
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands(document, history);
        const auto created = commands.Execute(MakePrimaryObjectCommand());
        REQUIRE(created.HasValue());
        AppendPhysicsFixtures(commands, MakePhysicsFixture());
        AppendPrefabFixture(commands, created.Value().object);
        return document.Snapshot();
    }

    [[nodiscard]] SceneFileWatchUpdate WaitForWatchUpdate(SceneFileWatchService &watcher) {
        std::vector<SceneFileWatchUpdate> updates;
        for (std::size_t attempt = 0; attempt < 100'000 && updates.empty(); ++attempt) {
            updates = watcher.DrainUpdates();
            std::this_thread::yield();
        }
        REQUIRE((updates.size() == 1));
        return std::move(updates.front());
    }

    void RequireSameSceneObject(const SceneObjectSnapshot &actual, const SceneObjectSnapshot &expected) {
        REQUIRE((actual.id == expected.id));
        REQUIRE((actual.name == expected.name));
        REQUIRE((actual.parent == expected.parent));
        REQUIRE((actual.localTransform == expected.localTransform));
        REQUIRE((actual.primitiveMesh == expected.primitiveMesh));
        REQUIRE((actual.components == expected.components));
    }
}  // namespace Horo::Editor::PersistenceTestSupport
