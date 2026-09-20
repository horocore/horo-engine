#include "editor/document/SceneDocumentComparison.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/document/SceneFileWatchService.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    const ErrorCodeDescriptor InjectedReplaceFailure{
        .domain = ErrorDomainId{"test.scene_persistence"},
        .code = ErrorCode{"replace_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Injected scene replacement failure.",
    };

    class TemporaryProject final {
    public:
        TemporaryProject() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path() / ("horo-scene-persistence-" + std::to_string(stamp));
            std::filesystem::create_directories(root_ / ".horo");
            std::filesystem::create_directories(root_ / "assets/scenes");
        }

        ~TemporaryProject() {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }

        [[nodiscard]] const std::filesystem::path &Root() const noexcept {
            return root_;
        }

        [[nodiscard]] std::filesystem::path ScenePath() const {
            return root_ / "assets/scenes/main.horo";
        }

        [[nodiscard]] std::filesystem::path RecoveryPath() const {
            return root_ / ".horo/local/recovery/default-scene.hororecovery";
        }

        void WriteMetadata(const std::string &defaultScene = "assets/scenes/main.horo") const {
            std::ofstream output(root_ / ".horo/project.json", std::ios::binary);
            output << R"({"settings":{"defaultScene":")" << defaultScene << R"("}})";
        }

        void WriteScene(std::string contents) const {
            std::ofstream output(ScenePath(), std::ios::binary);
            output << contents;
        }

    private:
        std::filesystem::path root_;
    };

    class ReplaceFailingFileSystem final : public DurableFileSystem {
    public:
        [[nodiscard]] Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                    const std::string_view ownerMetadata) override {
            return native_.TryAcquireExclusive(path, ownerMetadata);
        }

        [[nodiscard]] Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native_.AvailableBytes(path);
        }

        [[nodiscard]] Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            return native_.WriteDurable(path, bytes);
        }

        [[nodiscard]] Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native_.CopyDurable(source, destination);
        }

        [[nodiscard]] Result<void> AtomicReplace(const std::filesystem::path &, const std::filesystem::path &) override {
            return Result<void>::Failure(MakeError(InjectedReplaceFailure));
        }

        [[nodiscard]] Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native_.RemoveDurable(path);
        }

        [[nodiscard]] Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native_.SyncDirectory(path);
        }

    private:
        NativeDurableFileSystem native_;
    };

    class InterferingFileSystem final : public DurableFileSystem {
    public:
        explicit InterferingFileSystem(std::filesystem::path canonicalPath) : canonicalPath_(std::move(canonicalPath)) {}

        [[nodiscard]] Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                    const std::string_view ownerMetadata) override {
            return native_.TryAcquireExclusive(path, ownerMetadata);
        }

        [[nodiscard]] Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native_.AvailableBytes(path);
        }

        [[nodiscard]] Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            Result<void> written = native_.WriteDurable(path, bytes);
            if (written.HasValue() && path.extension() == ".tmp") {
                std::ofstream external(canonicalPath_, std::ios::binary | std::ios::trunc);
                external << "{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n";
            }
            return written;
        }

        [[nodiscard]] Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native_.CopyDurable(source, destination);
        }

        [[nodiscard]] Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            return native_.AtomicReplace(prepared, destination);
        }

        [[nodiscard]] Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native_.RemoveDurable(path);
        }

        [[nodiscard]] Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native_.SyncDirectory(path);
        }

    private:
        std::filesystem::path canonicalPath_;
        NativeDurableFileSystem native_;
    };

    [[nodiscard]] SceneDocumentSnapshot AuthoredScene() {
        SceneDocument document;
        EditorHistory history;
        SceneDocumentCommandExecutor commands(document, history);
        const Math::Transform transform{
            .translation = {2.0F, 3.0F, -4.0F},
            .rotation = Math::Quaternion::FromEulerRadians({0.1F, 0.2F, 0.3F}),
            .scale = {1.5F, 2.0F, 0.5F},
        };
        const auto created =
            commands.Execute(
                CreateSceneObjectCommand{
                    .name = "Persisted Box",
                    .localTransform = transform,
                    .primitiveMesh = PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box),
                    .components =
                        SceneObjectComponentSet{
                            .camera = Runtime::CameraComponent{.nearPlane = 0.25F, .farPlane = 500.0F, .enabled = false},
                            .light = Runtime::LightComponent{.kind = Runtime::LightKind::Point, .intensity = 3.0F},
                            .triggerVolume = Runtime::TriggerVolumeComponent{Runtime::ColliderShapeType::Sphere},
                            .audioSource = Runtime::AudioSourceComponent{.playback = {.gain = 0.75F, .spatial = false}},
                            .navigationSurface =
                                Runtime::NavigationSurfaceComponent{
                                    .id = Navigation::SurfaceId::Create(19).Value(),
                                    .definition = Assets::AssetId::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee").Value(),
                                    .schemaVersion = 1,
                                    .generation = 4,
                                    .bakeScope = Runtime::NavigationBakeScope::LocalBounds,
                                    .localBounds =
                                        Runtime::NavigationLocalBounds{.center = {1.0F, 0.0F, -2.0F}, .halfExtents = {8.0F, 2.0F, 5.0F}},
                                    .profiles = {Navigation::NavigationAgentProfileId::Create(6).Value()},
                                },
                            .navigationRegion =
                                Runtime::NavigationRegionComponent{
                                    .id = Navigation::NavigationRegionId::Create(27).Value(),
                                    .surface = Navigation::SurfaceId::Create(19).Value(),
                                    .generation = 3,
                                    .localBounds = {.center = {-1.0F, 0.5F, 2.0F}, .halfExtents = {3.0F, 1.0F, 4.0F}},
                                    .sourceSelection = Runtime::NavigationRegionSourceSelection::StaticCollisionInBounds,
                                    .mode = Runtime::NavigationRegionMode::Exclude,
                                },
                            .navigationModifier =
                                Runtime::NavigationModifierComponent{
                                    .id = Navigation::NavigationModifierId::Create(31).Value(),
                                    .surface = Navigation::SurfaceId::Create(19).Value(),
                                    .generation = 2,
                                    .volume = Runtime::NavigationCylinderVolume{.center = {0.5F, 1.0F, -0.5F},
                                                                                .radius = 2.5F,
                                                                                .halfHeight = 1.25F},
                                    .operation = Runtime::NavigationModifierOperation::OverrideAreaAndCost,
                                    .area = Navigation::NavigationAreaId::Create(12).Value(),
                                    .traversalCost = 1.75F,
                                },
                            .navigationLink =
                                Runtime::NavigationLinkComponent{
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
                                },
                            .rigidBody = Runtime::RigidBodyComponent{.id = {41}, .body = {42}},
                            .colliders = {Runtime::ColliderComponent{
                                .id = {43},
                                .collider = {44},
                                .body = {.object = {1}, .body = {42}},
                                .source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{.radiusMeters = 1.25F}},
                                .localPose = {.translation = {0.0F, 0.5F, 0.0F}},
                                .collisionProfile = Physics::CollisionProfileId::Parse("11111111-2222-4333-8444-555555555555").Value(),
                                .materials = {{.slot = Physics::PhysicsMaterialSlotId::FromValue(1),
                                               .material = Assets::AssetId::Parse("99999999-aaaa-4bbb-8ccc-dddddddddddd").Value()}},
                            }},
                            .physicsConstraints = {Runtime::PhysicsConstraintComponent{
                                .id = {45},
                                .constraint = {46},
                                .first = {.body = {.object = {1}, .body = {42}}},
                                .second = Runtime::PhysicsConstraintWorldEndpoint{.frame = {.translation = {0.0F, 4.0F, 0.0F}}},
                                .parameters = Runtime::PhysicsDistanceConstraint{.minimumMeters = 0.5F, .maximumMeters = 4.0F},
                            }},
                            .behaviors =
                                {
                                    Gameplay::BehaviorComponent{
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
                                    },
                                },
                        },
                });
        REQUIRE((created.HasValue()));
        const auto profile = Physics::CollisionProfileId::Parse("11111111-2222-4333-8444-555555555555").Value();
        const auto material = Assets::AssetId::Parse("99999999-aaaa-4bbb-8ccc-dddddddddddd").Value();
        const auto materialBinding =
            Runtime::PhysicsColliderMaterialBinding{.slot = Physics::PhysicsMaterialSlotId::FromValue(1), .material = material};
        REQUIRE(
            commands
                .Execute(CreateSceneObjectCommand{
                    .name = "Dynamic Physics Variants",
                    .components =
                        SceneObjectComponentSet{.rigidBody =
                                                    Runtime::RigidBodyComponent{.id = {51},
                                                                                .body = {52},
                                                                                .motion = Runtime::AuthoredPhysicsMotionType::Dynamic,
                                                                                .mass = Runtime::AuthoredPhysicsMass{.kilograms = 12.0F}},
                                                .colliders =
                                                    {Runtime::ColliderComponent{.id = {53},
                                                                                .collider = {54},
                                                                                .body = {.object = {2}, .body = {52}},
                                                                                .source =
                                                                                    Runtime::PhysicsAnalyticCollider{
                                                                                        Runtime::PhysicsBoxCollider{}},
                                                                                .collisionProfile = profile,
                                                                                .materials = {materialBinding}},
                                                     Runtime::ColliderComponent{.id = {55},
                                                                                .collider = {56},
                                                                                .body = {.object = {2}, .body = {52}},
                                                                                .source =
                                                                                    Runtime::PhysicsAnalyticCollider{
                                                                                        Runtime::PhysicsCapsuleCollider{}},
                                                                                .collisionProfile = profile,
                                                                                .materials = {materialBinding}},
                                                     Runtime::ColliderComponent{.id = {57},
                                                                                .collider = {58},
                                                                                .body = {.object = {2}, .body = {52}},
                                                                                .source = Runtime::
                                                                                    PhysicsShapeAssetReference{.asset =
                                                                                                                   Assets::AssetId::Parse(
                                                                                                                       "aaaaaaaa-1111-4222-"
                                                                                                                       "8333-bbbbbbbbbbbb")
                                                                                                                       .Value(),
                                                                                                               .subresource = {3}},
                                                                                .collisionProfile = profile,
                                                                                .materials = {materialBinding}}},
                                                .physicsConstraints =
                                                    {Runtime::PhysicsConstraintComponent{.id = {59},
                                                                                         .constraint = {60},
                                                                                         .first = {.body = {.object = {2}, .body = {52}}},
                                                                                         .second =
                                                                                             Runtime::PhysicsConstraintBodyEndpoint{
                                                                                                 .body = {.object = {1}, .body = {42}}},
                                                                                         .parameters =
                                                                                             Runtime::PhysicsFixedConstraint{}}}}})
                .HasValue());
        REQUIRE(
            commands
                .Execute(
                    CreateSceneObjectCommand{.name = "Density Physics Variant",
                                             .components =
                                                 SceneObjectComponentSet{.rigidBody = Runtime::
                                                                             RigidBodyComponent{.id = {61},
                                                                                                .body = {62},
                                                                                                .motion = Runtime::
                                                                                                    AuthoredPhysicsMotionType::Dynamic,
                                                                                                .mass = Runtime::AuthoredPhysicsDensity{}},
                                                                         .colliders =
                                                                             {Runtime::ColliderComponent{.id = {63},
                                                                                                         .collider = {64},
                                                                                                         .body = {.object = {3},
                                                                                                                  .body = {62}},
                                                                                                         .collisionProfile = profile,
                                                                                                         .materials = {materialBinding}}}}})
                .HasValue());
        REQUIRE(
            commands
                .Execute(CreateSceneObjectCommand{
                    .name = "Static Plane Physics Variant",
                    .components =
                        SceneObjectComponentSet{.rigidBody =
                                                    Runtime::RigidBodyComponent{.id = {71},
                                                                                .body = {72},
                                                                                .motion = Runtime::AuthoredPhysicsMotionType::Kinematic},
                                                .colliders = {Runtime::ColliderComponent{.id = {73},
                                                                                         .collider = {74},
                                                                                         .body = {.object = {4}, .body = {72}},
                                                                                         .source =
                                                                                             Runtime::PhysicsAnalyticCollider{
                                                                                                 Runtime::PhysicsStaticPlaneCollider{}},
                                                                                         .collisionProfile = profile,
                                                                                         .materials = {materialBinding}}}}})
                .HasValue());
        const auto prefabAsset = Assets::AssetId::Parse("11112222-3333-4444-8888-9999aaaabbbb");
        REQUIRE(prefabAsset.HasValue());
        const auto sourcePrefab = Prefab::PrefabAssetReference::Create(prefabAsset.Value());
        REQUIRE(sourcePrefab.HasValue());
        REQUIRE(commands
                    .Execute(CreateScenePrefabInstanceCommand{
                        sourcePrefab.Value(),
                        created.Value().object,
                        Math::Transform{.translation = {-3.0F, 2.0F, 7.0F}, .scale = {0.5F, 0.5F, 0.5F}},
                    })
                    .HasValue());
        return document.Snapshot();
    }

    void RequireSameSceneObject(const SceneObjectSnapshot &actual, const SceneObjectSnapshot &expected) {
        REQUIRE((actual.id == expected.id));
        REQUIRE((actual.name == expected.name));
        REQUIRE((actual.parent == expected.parent));
        REQUIRE((actual.localTransform == expected.localTransform));
        REQUIRE((actual.primitiveMesh == expected.primitiveMesh));
        REQUIRE((actual.components == expected.components));
    }
}  // namespace

TEST_CASE("Project Scene Save Reopens The Same Authored State", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    const SceneDocumentSnapshot authored = AuthoredScene();
    auto expected = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE((expected.HasValue()));
    auto saved = SaveProjectScene(project.Root(), project.ScenePath(), authored, expected.Value(), false, mutations, files);
    REQUIRE((saved.HasValue()));
    REQUIRE((saved.Value().status == ProjectSceneSaveStatus::Saved));

    auto loaded = LoadProjectDefaultScene(project.Root());
    const std::string loadError = loaded.HasError() ? loaded.ErrorValue().code.Value() + ": " + loaded.ErrorValue().message : std::string{};
    INFO(loadError);
    REQUIRE((loaded.HasValue()));
    REQUIRE((loaded.Value().has_value()));
    REQUIRE((loaded.Value()->absolutePath.is_absolute()));
    REQUIRE((loaded.Value()->objects.size() == authored.objects.size()));
    REQUIRE((loaded.Value()->prefabInstances.size() == 1));

    SceneDocument reopened;
    REQUIRE((reopened.LoadSaved(std::move(loaded.Value()->objects), std::move(loaded.Value()->prefabInstances)).HasValue()));
    REQUIRE((!reopened.IsDirty()));
    REQUIRE((reopened.Objects().size() == authored.objects.size()));
    for (std::size_t index = 0; index < authored.objects.size(); ++index)
        RequireSameSceneObject(reopened.Objects()[index], authored.objects[index]);
    REQUIRE((reopened.PrefabInstances().size() == 1));
    REQUIRE((reopened.PrefabInstances().front() == authored.prefabInstances.front()));
}

TEST_CASE("Navigation link direction and modifier shape round trip explicitly", "[unit][editor][persistence][navigation]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");
    SceneDocumentSnapshot authored = AuthoredScene();
    authored.prefabInstances.clear();
    auto &components = authored.objects.front().components;
    components.navigationModifier->volume =
        Runtime::NavigationLocalBounds{.center = {-1.0F, 2.0F, 3.0F}, .halfExtents = {4.0F, 5.0F, 6.0F}};
    components.navigationModifier->operation = Runtime::NavigationModifierOperation::Exclude;
    components.navigationModifier->area.reset();
    components.navigationModifier->traversalCost.reset();
    components.navigationLink->kind = Runtime::NavigationLinkKind::Jump;
    components.navigationLink->direction = Runtime::NavigationLinkDirection::StartToEnd;

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    auto expected = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE(expected.HasValue());
    REQUIRE(SaveProjectScene(project.Root(), project.ScenePath(), authored, expected.Value(), false, mutations, files).HasValue());

    auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value().has_value());
    const auto &loadedComponents = loaded.Value()->objects.front().components;
    REQUIRE(loadedComponents.navigationModifier == components.navigationModifier);
    REQUIRE(loadedComponents.navigationLink == components.navigationLink);
    REQUIRE(std::holds_alternative<Runtime::NavigationLocalBounds>(loadedComponents.navigationModifier->volume));
    REQUIRE(loadedComponents.navigationLink->direction == Runtime::NavigationLinkDirection::StartToEnd);
}

TEST_CASE("Every authored light kind survives project scene save and reload", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");
    const SceneDocumentSnapshot snapshot{
        .revision = DocumentRevision{3},
        .state = DocumentStateId{3},
        .objects =
            {
                SceneObjectSnapshot{.id = SceneObjectId{1},
                                    .name = "Directional",
                                    .components =
                                        SceneObjectComponentSet{.light = Runtime::LightComponent{.kind = Runtime::LightKind::Directional}},
                                    .editorState = SceneObjectEditorState{.visible = false}},
                SceneObjectSnapshot{.id = SceneObjectId{2},
                                    .name = "Point",
                                    .components =
                                        SceneObjectComponentSet{.light = Runtime::LightComponent{.kind = Runtime::LightKind::Point}},
                                    .editorState = SceneObjectEditorState{.locked = true}},
                SceneObjectSnapshot{.id = SceneObjectId{3},
                                    .name = "Spot",
                                    .components =
                                        SceneObjectComponentSet{.light = Runtime::LightComponent{.kind = Runtime::LightKind::Spot}}},
            },
    };

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    auto expected = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE((expected.HasValue()));
    REQUIRE((SaveProjectScene(project.Root(), project.ScenePath(), snapshot, expected.Value(), false, mutations, files).HasValue()));

    auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE((loaded.HasValue() && loaded.Value().has_value()));
    REQUIRE((loaded.Value()->objects.size() == 3));
    REQUIRE((loaded.Value()->objects[0].components.light->kind == Runtime::LightKind::Directional));
    REQUIRE((loaded.Value()->objects[1].components.light->kind == Runtime::LightKind::Point));
    REQUIRE((loaded.Value()->objects[2].components.light->kind == Runtime::LightKind::Spot));
    REQUIRE_FALSE((loaded.Value()->objects[0].editorState.visible));
    REQUIRE((loaded.Value()->objects[1].editorState.locked));
    REQUIRE((loaded.Value()->objects[2].editorState == SceneObjectEditorState{}));
}

TEST_CASE("Imported mesh asset identity persists without a source path", "[unit][editor][persistence][asset]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");
    const auto asset = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff");
    REQUIRE((asset.HasValue()));
    SceneDocumentSnapshot snapshot{
        .revision = DocumentRevision{1},
        .state = DocumentStateId{1},
        .objects = {SceneObjectSnapshot{.id = SceneObjectId{1}, .name = "Chair", .meshAsset = asset.Value()}},
    };

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    auto fingerprint = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE((fingerprint.HasValue()));
    auto saved = SaveProjectScene(project.Root(), project.ScenePath(), snapshot, fingerprint.Value(), false, mutations, files);
    REQUIRE((saved.HasValue()));
    auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE((loaded.HasValue() && loaded.Value().has_value()));
    REQUIRE((loaded.Value()->objects.size() == 1));
    CHECK(loaded.Value()->objects.front().meshAsset == asset.Value());
    CHECK_FALSE(loaded.Value()->objects.front().primitiveMesh.has_value());
}

void RequireAudioSourceRoundTrip(const std::span<const Audio::AudioSoundReference> references) {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");

    std::vector<SceneObjectSnapshot> objects;
    objects.reserve(references.size());
    for (std::size_t index = 0; index < references.size(); ++index) {
        Runtime::AudioSourceComponent audioSource{.sound = references[index]};
        if (index == 0) {
            const auto bus = Audio::AudioBusId::Create(9);
            REQUIRE(bus.HasValue());
            audioSource.playback = {.gain = 0.75F,
                                    .pitch = 1.25F,
                                    .bus = bus.Value(),
                                    .loop = true,
                                    .spatial = false,
                                    .enableDoppler = true,
                                    .playOnStart = false};
        }
        objects.push_back(SceneObjectSnapshot{.id = SceneObjectId{static_cast<std::uint64_t>(index + 1)},
                                              .name = "AudioSource",
                                              .components = SceneObjectComponentSet{.audioSource = audioSource}});
    }
    const SceneDocumentSnapshot snapshot{.revision = DocumentRevision{1}, .state = DocumentStateId{1}, .objects = std::move(objects)};

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    const auto fingerprint = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE(fingerprint.HasValue());
    REQUIRE(SaveProjectScene(project.Root(), project.ScenePath(), snapshot, fingerprint.Value(), false, mutations, files).HasValue());
    const auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE((loaded.HasValue() && loaded.Value().has_value()));
    REQUIRE(loaded.Value()->objects.size() == snapshot.objects.size());
    for (std::size_t index = 0; index < snapshot.objects.size(); ++index) {
        REQUIRE(loaded.Value()->objects[index].components.audioSource.has_value());
        CHECK(loaded.Value()->objects[index].components.audioSource->sound == snapshot.objects[index].components.audioSource->sound);
        CHECK(loaded.Value()->objects[index].components.audioSource->playback == snapshot.objects[index].components.audioSource->playback);
    }
}

TEST_CASE("Audio source persists every built-in sound reference kind", "[unit][editor][persistence][audio]") {
    const auto clip = Audio::AudioClipId::Create(Assets::AssetId::Parse("11111111-1111-4111-8111-111111111111").Value());
    const auto variation = Audio::AudioSoundId::Create(Assets::AssetId::Parse("22222222-2222-4222-8222-222222222222").Value());
    const auto stream = Audio::AudioSoundId::Create(Assets::AssetId::Parse("33333333-3333-4333-8333-333333333333").Value());
    const auto music = Audio::AudioSoundId::Create(Assets::AssetId::Parse("44444444-4444-4444-8444-444444444444").Value());
    REQUIRE(clip.HasValue());
    REQUIRE(variation.HasValue());
    REQUIRE(stream.HasValue());
    REQUIRE(music.HasValue());
    const auto clipReference = Audio::AudioSoundReference::ForClip(clip.Value());
    const auto variationReference = Audio::AudioSoundReference::ForVariation(variation.Value());
    const auto streamReference = Audio::AudioSoundReference::ForStream(stream.Value());
    const auto musicReference = Audio::AudioSoundReference::ForMusic(music.Value());
    REQUIRE(clipReference.HasValue());
    REQUIRE(variationReference.HasValue());
    REQUIRE(streamReference.HasValue());
    REQUIRE(musicReference.HasValue());
    const std::array references{clipReference.Value(), variationReference.Value(), streamReference.Value(), musicReference.Value()};
    RequireAudioSourceRoundTrip(references);
}

TEST_CASE("Audio source persists extension sound references", "[unit][editor][persistence][audio]") {
    const auto contribution = Audio::AudioContributionId::Create(42);
    const auto definition = Audio::AudioSoundId::Create(Assets::AssetId::Parse("55555555-5555-4555-8555-555555555555").Value());
    REQUIRE(contribution.HasValue());
    REQUIRE(definition.HasValue());
    const auto extension = Audio::AudioSoundReference::ForExtension(contribution.Value(), definition.Value(), {3, 7});
    REQUIRE(extension.HasValue());
    const auto payload = std::get<Audio::AudioSoundExtensionReference>(extension.Value().target);
    CHECK(payload.contribution.Value() == 42);
    CHECK(payload.contractVersion == Audio::AudioSoundDefinitionSchemaVersion{3, 7});
    const std::array references{extension.Value()};
    RequireAudioSourceRoundTrip(references);
}

TEST_CASE("Audio source migration clears legacy native and middleware references", "[unit][editor][persistence][audio][migration]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene(R"({
        "schemaVersion": 1,
        "objects": [
            {"id": 1, "parent": null, "name": "Native Clip", "transform": {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}, "primitiveMesh": null, "components": {"audioSource": {"kind": "native_clip", "gain": 1.0, "spatial": true}}},
            {"id": 2, "parent": null, "name": "Middleware Event", "transform": {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}, "primitiveMesh": null, "components": {"audioSource": {"kind": "middleware_event", "gain": 1.0, "spatial": true}}}
        ]
    })");

    const auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE((loaded.HasValue() && loaded.Value().has_value()));
    REQUIRE(loaded.Value()->objects.size() == 2);
    for (const SceneObjectSnapshot &object : loaded.Value()->objects) {
        REQUIRE(object.components.audioSource.has_value());
        CHECK(object.components.audioSource->sound.kind == Audio::AudioSoundReferenceKind::Unassigned);
        CHECK(std::holds_alternative<std::monostate>(object.components.audioSource->sound.target));
    }
}

TEST_CASE("Scene Comparison Classifies Typed Added Removed And Modified Objects", "[unit][editor][persistence][compare]") {
    const auto instanceOne = Prefab::PrefabInstanceId::Create(1);
    const auto instanceTwo = Prefab::PrefabInstanceId::Create(2);
    const auto instanceThree = Prefab::PrefabInstanceId::Create(3);
    const auto documentSourceOne =
        Prefab::PrefabAssetReference::Create(Assets::AssetId::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa").Value());
    const auto documentSourceTwo =
        Prefab::PrefabAssetReference::Create(Assets::AssetId::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb").Value());
    const auto diskSourceOne = Prefab::PrefabAssetReference::Create(Assets::AssetId::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc").Value());
    const auto diskSourceThree =
        Prefab::PrefabAssetReference::Create(Assets::AssetId::Parse("dddddddd-dddd-4ddd-8ddd-dddddddddddd").Value());
    REQUIRE((instanceOne.HasValue()));
    REQUIRE((instanceTwo.HasValue()));
    REQUIRE((instanceThree.HasValue()));
    REQUIRE((documentSourceOne.HasValue()));
    REQUIRE((documentSourceTwo.HasValue()));
    REQUIRE((diskSourceOne.HasValue()));
    REQUIRE((diskSourceThree.HasValue()));

    SceneObjectSnapshot documentModified{
        .id = SceneObjectId{1},
        .name = "Document Name",
        .localTransform = Math::Transform{.translation = {1.0F, 0.0F, 0.0F}},
        .primitiveMesh = PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box),
    };
    SceneObjectSnapshot diskModified = documentModified;
    diskModified.name = "Disk Name";
    diskModified.localTransform.translation = {2.0F, 0.0F, 0.0F};
    diskModified.components.light = Runtime::LightComponent{.kind = Runtime::LightKind::Point};
    diskModified.editorState.locked = true;

    const SceneDocumentSnapshot document{
        .objects =
            {
                documentModified,
                SceneObjectSnapshot{
                    .id = SceneObjectId{2},
                    .name = "Removed",
                },
            },
        .prefabInstances =
            {
                ScenePrefabInstance{
                    .instanceId = instanceOne.Value(),
                    .sourcePrefab = documentSourceOne.Value(),
                },
                ScenePrefabInstance{
                    .instanceId = instanceTwo.Value(),
                    .sourcePrefab = documentSourceTwo.Value(),
                },
            },
    };
    const SceneDocumentSnapshot disk{
        .objects =
            {
                diskModified,
                SceneObjectSnapshot{
                    .id = SceneObjectId{3},
                    .name = "Added",
                },
            },
        .prefabInstances =
            {
                ScenePrefabInstance{
                    .instanceId = instanceOne.Value(),
                    .sourcePrefab = diskSourceOne.Value(),
                },
                ScenePrefabInstance{
                    .instanceId = instanceThree.Value(),
                    .sourcePrefab = diskSourceThree.Value(),
                },
            },
    };

    const SceneDocumentComparison comparison = CompareSceneDocuments(document, disk);
    REQUIRE((comparison.addedOnDisk == 1));
    REQUIRE((comparison.removedFromDisk == 1));
    REQUIRE((comparison.modified == 1));
    REQUIRE((comparison.prefabInstancesAddedOnDisk == 1));
    REQUIRE((comparison.prefabInstancesRemovedFromDisk == 1));
    REQUIRE((comparison.prefabInstancesModified == 1));
    REQUIRE((comparison.objects.size() == 3));
    REQUIRE((comparison.prefabInstances.size() == 3));

    const SceneObjectComparison &modified = comparison.objects[0];
    REQUIRE((modified.id == SceneObjectId{1}));
    REQUIRE((modified.kind == SceneObjectComparisonKind::Modified));
    REQUIRE((modified.documentName == "Document Name"));
    REQUIRE((modified.diskName == "Disk Name"));
    REQUIRE((modified.fields.name));
    REQUIRE((modified.fields.transform));
    REQUIRE((modified.fields.components));
    REQUIRE((modified.fields.editorState));
    REQUIRE((!modified.fields.parent));
    REQUIRE((!modified.fields.primitive));
    REQUIRE((comparison.objects[1].kind == SceneObjectComparisonKind::RemovedFromDisk));
    REQUIRE((comparison.objects[2].kind == SceneObjectComparisonKind::AddedOnDisk));
    REQUIRE((comparison.prefabInstances[0].id == instanceOne.Value()));
    REQUIRE((comparison.prefabInstances[0].kind == SceneObjectComparisonKind::Modified));
    REQUIRE((comparison.prefabInstances[0].fields.sourcePrefab));
    REQUIRE((!comparison.prefabInstances[0].fields.parent));
    REQUIRE((!comparison.prefabInstances[0].fields.rootTransform));
    REQUIRE((comparison.prefabInstances[1].kind == SceneObjectComparisonKind::RemovedFromDisk));
    REQUIRE((comparison.prefabInstances[2].kind == SceneObjectComparisonKind::AddedOnDisk));
}

TEST_CASE("Failed Atomic Scene Replace Preserves Canonical Bytes", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.WriteMetadata();
    const std::string original = "{\"schemaVersion\":1,\"objects\":[]}\n";
    project.WriteScene(original);

    ReplaceFailingFileSystem files;
    ProjectMutationCoordinator mutations(files);
    auto expected = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE((expected.HasValue()));
    REQUIRE((SaveProjectScene(project.Root(), project.ScenePath(), AuthoredScene(), expected.Value(), false, mutations, files).HasError()));

    std::ifstream input(project.ScenePath(), std::ios::binary);
    const std::string persisted{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    REQUIRE((persisted == original));
    REQUIRE((!std::filesystem::exists(project.ScenePath().string() + ".save.tmp")));
}

TEST_CASE("Project Scene Resolver Rejects Parent Traversal", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.WriteMetadata("../outside.horo");
    REQUIRE((LoadProjectDefaultScene(project.Root()).HasError()));
}

TEST_CASE("Project Scene Resolver Rejects A Missing Configured Default Scene", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.WriteMetadata();
    REQUIRE((LoadProjectDefaultScene(project.Root()).HasError()));
}

TEST_CASE("Project Scene Resolver Accepts An Empty Default Scene", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.WriteMetadata("");
    const auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE((loaded.HasValue()));
    REQUIRE_FALSE((loaded.Value().has_value()));
}

TEST_CASE("Project Scene Loader Rejects Unknown Schema Versions", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":2,\"objects\":[]}\n");
    REQUIRE((LoadProjectDefaultScene(project.Root()).HasError()));
}

TEST_CASE("Project Scene Loader Rejects Malformed Prefab Instance Records", "[unit][editor][persistence][prefab]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene(R"({"schemaVersion":1,"objects":[],"prefabInstances":{}})");
    REQUIRE((LoadProjectDefaultScene(project.Root()).HasError()));

    project.WriteScene(R"({"schemaVersion":1,"objects":[],"prefabInstances":[{"instanceId":1,"sourceAsset":"not-an-id"}]})");
    REQUIRE((LoadProjectDefaultScene(project.Root()).HasError()));
}

TEST_CASE("Scene Save Detects External Byte Changes Before Atomic Replacement", "[unit][editor][persistence][conflict]") {
    TemporaryProject project;
    project.WriteMetadata();
    const std::string original = "{\"schemaVersion\":1,\"objects\":[]}\n";
    project.WriteScene(original);

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    auto expected = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE((expected.HasValue()));

    const std::string external = "{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n";
    project.WriteScene(external);
    auto conflict = SaveProjectScene(project.Root(), project.ScenePath(), AuthoredScene(), expected.Value(), false, mutations, files);
    REQUIRE((conflict.HasValue()));
    REQUIRE((conflict.Value().status == ProjectSceneSaveStatus::Conflict));

    std::ifstream input(project.ScenePath(), std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    REQUIRE((bytes == external));
    REQUIRE((!std::filesystem::exists(project.ScenePath().string() + ".save.tmp")));
}

TEST_CASE("Explicit Scene Conflict Overwrite Returns The New Canonical Identity", "[unit][editor][persistence][conflict]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    auto expected = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE((expected.HasValue()));
    project.WriteScene("{\n\"schemaVersion\":1,\"objects\":[]}\n");

    auto saved = SaveProjectScene(project.Root(), project.ScenePath(), AuthoredScene(), expected.Value(), true, mutations, files);
    REQUIRE((saved.HasValue()));
    REQUIRE((saved.Value().status == ProjectSceneSaveStatus::Saved));
    auto current = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE((current.HasValue()));
    REQUIRE((current.Value() == saved.Value().fingerprint));
}

TEST_CASE("Scene Destination Save Requires Explicit Existing File Approval", "[unit][editor][persistence][save-as]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");
    const std::filesystem::path destination = project.Root() / "assets/scenes/existing.horo";
    const std::string original = "{\"schemaVersion\":1,\"objects\":[]}\n";
    {
        std::ofstream output(destination, std::ios::binary);
        output << original;
    }

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    auto rejected = SaveProjectSceneToPath(project.Root(), destination, AuthoredScene(), false, mutations, files);
    REQUIRE((rejected.HasValue()));
    REQUIRE((rejected.Value().status == ProjectSceneDestinationSaveStatus::DestinationExists));

    {
        std::ifstream input(destination, std::ios::binary);
        const std::string unchanged{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        REQUIRE((unchanged == original));
    }

    auto saved = SaveProjectSceneToPath(project.Root(), destination, AuthoredScene(), true, mutations, files);
    REQUIRE((saved.HasValue()));
    REQUIRE((saved.Value().status == ProjectSceneDestinationSaveStatus::Saved));
}

TEST_CASE("Scene Destination Save Rejects Invalid Paths And Detects Races", "[unit][editor][persistence][save-as][conflict]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");
    const std::filesystem::path destination = project.Root() / "assets/scenes/copy.horo";

    NativeDurableFileSystem nativeFiles;
    ProjectMutationCoordinator nativeMutations(nativeFiles);
    REQUIRE(
        (SaveProjectSceneToPath(project.Root(), std::filesystem::path{"relative.horo"}, AuthoredScene(), true, nativeMutations, nativeFiles)
             .HasError()));
    REQUIRE((SaveProjectSceneToPath(project.Root(), project.Root() / "assets/scenes/copy.txt", AuthoredScene(), true, nativeMutations,
                                    nativeFiles)
                 .HasError()));
    REQUIRE((SaveProjectSceneToPath(project.Root(), project.Root().parent_path() / "outside.horo", AuthoredScene(), true, nativeMutations,
                                    nativeFiles)
                 .HasError()));

    const std::filesystem::path outsideDirectory = project.Root().parent_path() / (project.Root().filename().string() + "-outside");
    std::filesystem::create_directories(outsideDirectory);
    std::error_code symlinkError;
    const std::filesystem::path linkedDirectory = project.Root() / "assets/scenes/linked";
    std::filesystem::create_directory_symlink(outsideDirectory, linkedDirectory, symlinkError);
    if (!symlinkError) {
        REQUIRE(
            (SaveProjectSceneToPath(project.Root(), linkedDirectory / "escaped.horo", AuthoredScene(), true, nativeMutations, nativeFiles)
                 .HasError()));
    }
    std::error_code outsideCleanupError;
    std::filesystem::remove_all(outsideDirectory, outsideCleanupError);

    InterferingFileSystem interferingFiles(destination);
    ProjectMutationCoordinator interferingMutations(interferingFiles);
    auto conflict = SaveProjectSceneToPath(project.Root(), destination, AuthoredScene(), true, interferingMutations, interferingFiles);
    REQUIRE((conflict.HasValue()));
    REQUIRE((conflict.Value().status == ProjectSceneDestinationSaveStatus::Conflict));
    REQUIRE((!std::filesystem::exists(destination.string() + ".save.tmp")));
}

TEST_CASE("Scene Save Rechecks External Identity Immediately Before Replacement", "[unit][editor][persistence][conflict]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");

    auto expected = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
    REQUIRE((expected.HasValue()));
    InterferingFileSystem files(project.ScenePath());
    ProjectMutationCoordinator mutations(files);

    auto conflict = SaveProjectScene(project.Root(), project.ScenePath(), AuthoredScene(), expected.Value(), false, mutations, files);
    REQUIRE((conflict.HasValue()));
    REQUIRE((conflict.Value().status == ProjectSceneSaveStatus::Conflict));
    REQUIRE((!std::filesystem::exists(project.ScenePath().string() + ".save.tmp")));

    auto external = LoadProjectDefaultScene(project.Root());
    REQUIRE((external.HasValue()));
    REQUIRE((external.Value().has_value()));
    REQUIRE((external.Value()->objects.empty()));
}

TEST_CASE("Scene Recovery Round Trips Without Mutating Canonical Scene", "[unit][editor][persistence][recovery]") {
    TemporaryProject project;
    project.WriteMetadata();
    const std::string canonical = "{\"schemaVersion\":1,\"objects\":[]}\n";
    project.WriteScene(canonical);

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    const SceneDocumentSnapshot authored = AuthoredScene();
    REQUIRE(
        (WriteProjectSceneRecovery(project.Root(), project.ScenePath(), authored, DocumentRevision{}, DocumentStateId{1}, mutations, files)
             .HasValue()));

    std::ifstream canonicalInput(project.ScenePath(), std::ios::binary);
    const std::string canonicalAfterAutosave{std::istreambuf_iterator<char>{canonicalInput}, std::istreambuf_iterator<char>{}};
    REQUIRE((canonicalAfterAutosave == canonical));
    REQUIRE((std::filesystem::exists(project.RecoveryPath())));

    auto inspected = InspectProjectSceneRecovery(project.Root(), project.ScenePath());
    REQUIRE((inspected.HasValue()));
    REQUIRE((inspected.Value().has_value()));
    REQUIRE((inspected.Value()->absoluteCanonicalPath == project.ScenePath()));
    REQUIRE((inspected.Value()->savedRevision == DocumentRevision{}));
    REQUIRE((inspected.Value()->savedState == DocumentStateId{1}));
    REQUIRE((inspected.Value()->recoveredRevision == authored.revision));
    REQUIRE((inspected.Value()->recoveredState == authored.state));
    REQUIRE((inspected.Value()->objects.size() == authored.objects.size()));
    RequireSameSceneObject(inspected.Value()->objects.front(), authored.objects.front());

    SceneDocument restored;
    REQUIRE((restored.LoadRecovered(std::move(inspected.Value()->objects), std::move(inspected.Value()->prefabInstances)).HasValue()));
    REQUIRE((restored.PrefabInstances().size() == authored.prefabInstances.size()));
    REQUIRE((restored.IsDirty()));
    REQUIRE((restored.Objects().size() == authored.objects.size()));
    RequireSameSceneObject(restored.Objects().front(), authored.objects.front());

    REQUIRE((DiscardProjectSceneRecovery(project.Root(), mutations, files).HasValue()));
    REQUIRE((!std::filesystem::exists(project.RecoveryPath())));
    auto absent = InspectProjectSceneRecovery(project.Root(), project.ScenePath());
    REQUIRE((absent.HasValue()));
    REQUIRE((!absent.Value().has_value()));
}

TEST_CASE("Scene Recovery Rejects Payload Whose Checksum No Longer Matches", "[unit][editor][persistence][recovery]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    const SceneDocumentSnapshot authored = AuthoredScene();
    REQUIRE(
        (WriteProjectSceneRecovery(project.Root(), project.ScenePath(), authored, DocumentRevision{}, DocumentStateId{1}, mutations, files)
             .HasValue()));

    std::ifstream input(project.RecoveryPath(), std::ios::binary);
    std::string corrupted{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const std::size_t name = corrupted.find("Persisted Box");
    REQUIRE((name != std::string::npos));
    corrupted.replace(name, std::string{"Persisted Box"}.size(), "Corrupted Box");
    {
        std::ofstream output(project.RecoveryPath(), std::ios::binary | std::ios::trunc);
        output << corrupted;
    }

    REQUIRE((InspectProjectSceneRecovery(project.Root(), project.ScenePath()).HasError()));
}

TEST_CASE("Failed Atomic Recovery Replace Leaves No Partial Recovery", "[unit][editor][persistence][recovery]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");

    ReplaceFailingFileSystem files;
    ProjectMutationCoordinator mutations(files);
    REQUIRE((WriteProjectSceneRecovery(project.Root(), project.ScenePath(), AuthoredScene(), DocumentRevision{}, DocumentStateId{1},
                                       mutations, files)
                 .HasError()));
    REQUIRE((!std::filesystem::exists(project.RecoveryPath())));
    REQUIRE((!std::filesystem::exists(project.RecoveryPath().string() + ".tmp")));
}

TEST_CASE("Scene File Watch Inspects Canonical Bytes Off The Owner Thread", "[unit][editor][persistence][watch]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");

    JobSystem jobs(JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8});
    {
        SceneFileWatchService watcher(jobs);
        auto requested = watcher.Request(project.Root(), project.ScenePath());
        REQUIRE((requested.HasValue()));

        std::vector<SceneFileWatchUpdate> updates;
        for (std::size_t attempt = 0; attempt < 100'000 && updates.empty(); ++attempt) {
            updates = watcher.DrainUpdates();
            std::this_thread::yield();
        }
        REQUIRE((updates.size() == 1));
        REQUIRE((updates.front().generation == requested.Value()));
        REQUIRE((updates.front().fingerprint.has_value()));
        REQUIRE((!updates.front().error.has_value()));

        const SceneFileFingerprint first = *updates.front().fingerprint;
        project.WriteScene("{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n");
        requested = watcher.Request(project.Root(), project.ScenePath());
        REQUIRE((requested.HasValue()));
        updates.clear();
        for (std::size_t attempt = 0; attempt < 100'000 && updates.empty(); ++attempt) {
            updates = watcher.DrainUpdates();
            std::this_thread::yield();
        }
        REQUIRE((updates.size() == 1));
        REQUIRE((updates.front().fingerprint.has_value()));
        REQUIRE((*updates.front().fingerprint != first));
    }
    jobs.Shutdown(ShutdownPolicy::Drain);
}

TEST_CASE("Scene File Watch Reset Discards Completed Stale Generation", "[unit][editor][persistence][watch]") {
    TemporaryProject project;
    project.WriteMetadata();
    project.WriteScene("{\"schemaVersion\":1,\"objects\":[]}\n");

    JobSystem jobs(JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8});
    {
        SceneFileWatchService watcher(jobs);
        const auto staleGeneration = watcher.Request(project.Root(), project.ScenePath());
        REQUIRE((staleGeneration.HasValue()));
        for (std::size_t attempt = 0; attempt < 100'000 && watcher.HasPendingInspection(); ++attempt)
            std::this_thread::yield();
        REQUIRE((!watcher.HasPendingInspection()));

        watcher.Reset();
        REQUIRE((watcher.DrainUpdates().empty()));

        project.WriteScene("{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n");
        const auto currentGeneration = watcher.Request(project.Root(), project.ScenePath());
        REQUIRE((currentGeneration.HasValue()));
        REQUIRE((currentGeneration.Value() > staleGeneration.Value()));

        std::vector<SceneFileWatchUpdate> updates;
        for (std::size_t attempt = 0; attempt < 100'000 && updates.empty(); ++attempt) {
            updates = watcher.DrainUpdates();
            std::this_thread::yield();
        }
        REQUIRE((updates.size() == 1));
        REQUIRE((updates.front().generation == currentGeneration.Value()));
    }
    jobs.Shutdown(ShutdownPolicy::Drain);
}
