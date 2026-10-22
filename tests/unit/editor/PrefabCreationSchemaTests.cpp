#include "Horo/Audio/AudioSoundReference.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "NativePublicationFiles.h"
#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/prefab/PrefabCreation.h"

#include <catch2/generators/catch_generators.hpp>

namespace Horo::Editor {
    namespace {
        using namespace PrefabHierarchyTestSupport;

        struct CountingFiles final : TestSupport::NativePublicationFiles {
            std::size_t writes{};

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms mode) override {
                ++writes;
                return native.WritePrivateDurable(path, bytes, created, mode);
            }
        };

        /** @brief Capture only through the canonical admission owner; export receives no fabricated authoring state. */
        SceneTransactionSnapshot Capture(std::vector<SceneObjectSnapshot> objects) {
            auto admitted = SceneDocumentTransactions::Create({PrefabAsset(8).Asset(), 1}, std::move(objects));
            REQUIRE(admitted.HasValue());
            auto owner = std::move(admitted).Value();
            auto snapshot = owner.Snapshot();
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }

        Assets::AssetRecord Record(const Assets::AssetId asset, const std::string_view type, const std::string_view path) {
            const auto text = "{\"schemaVersion\":1,\"assetId\":\"" + asset.ToString() + "\",\"assetType\":\"" + std::string(type) + "\"}";
            auto record = Assets::DecodeAssetIdentitySidecar(path, text);
            REQUIRE(record.HasValue());
            return std::move(record).Value();
        }

        Result<Prefab::PrefabDocument> Export(const SceneTransactionSnapshot &snapshot, const Assets::AssetRegistry &registry,
                                              std::vector<PrefabCreationBoundary> &boundaries,
                                              const Gameplay::BehaviorRegistry *behaviors = nullptr) {
            return PreparePrefabSelectionSource(snapshot, {10}, PrefabAsset(9).Asset(), ScenePrefabVersion(), registry.Snapshot(),
                                                behaviors, boundaries);
        }

        /** @brief Two fully admitted explicit bodies with one collider each and a genuine internal constraint. */
        std::vector<SceneObjectSnapshot> PhysicsObjects() {
            std::vector<SceneObjectSnapshot> objects{{.id = {10}, .name = "Root body"},
                                                     {.id = {20}, .parent = SceneObjectId{10}, .name = "Child body"}};
            for (auto &object : objects) {
                object.components.rigidBody = Runtime::RigidBodyComponent{.id = {1}, .body = {1}};
                object.components.colliders.push_back(
                    {.id = {2},
                     .collider = {1},
                     .body = {{object.id.value}, {1}},
                     .collisionProfile = Physics::CollisionProfileId::Parse("00000000-0000-0000-0000-000000000001").Value(),
                     .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), PrefabAsset(3).Asset()}}});
            }
            objects.front().components.physicsConstraints.push_back(
                {.id = {3},
                 .constraint = {1},
                 .first = {.body = {{10}, {1}}},
                 .second = Runtime::PhysicsConstraintBodyEndpoint{.body = {{20}, {1}}}});
            return objects;
        }

        /** @brief Crossing rejection preserves the admitted document and performs no source or sidecar publication. */
        void RequireCrossingRejectedWithoutPublication(const std::vector<SceneObjectSnapshot> &objects,
                                                       const Assets::AssetRegistry &registry) {
            auto admitted = SceneDocumentTransactions::Create({PrefabAsset(8).Asset(), 1}, objects);
            REQUIRE(admitted.HasValue());
            auto owner = std::move(admitted).Value();
            auto before = owner.Snapshot();
            REQUIRE(before.HasValue());
            ProjectDirectory directory;
            REQUIRE(std::filesystem::create_directory(directory.path / "assets"));
            CountingFiles files;
            ProjectMutationCoordinator mutations{files};
            const auto &state = before.Value();
            const auto outcome = CreatePrefabFromSelection(owner,
                                                           {{state.session, state.revision, state.state, true},
                                                            {10},
                                                            std::filesystem::canonical(directory.path),
                                                            std::filesystem::canonical(directory.path) / "assets/Crossing.prefab",
                                                            PrefabAsset(9).Asset(),
                                                            ScenePrefabVersion(),
                                                            registry.Snapshot()},
                                                           mutations, files);
            CHECK_FALSE(outcome.committed);
            REQUIRE(outcome.error);
            CHECK(files.writes == 0);
            CHECK_FALSE(std::filesystem::exists(directory.path / "assets/Crossing.prefab"));
            CHECK_FALSE(std::filesystem::exists(directory.path / "assets/Crossing.prefab.horo"));
            const auto after = owner.Snapshot();
            REQUIRE(after.HasValue());
            CHECK(after.Value().revision == state.revision);
            CHECK(after.Value().state == state.state);
            CHECK(after.Value().objects.size() == state.objects.size());
            CHECK(after.Value().prefabInstances.empty());
            CHECK(owner.Replay({state.session, state.revision, state.state, true}, false).HasError());
        }

        Gameplay::BehaviorTypeId BehaviorType() {
            return Gameplay::BehaviorTypeId::Parse("game.tests.portable_creation").Value();
        }

        Gameplay::IBehaviorInstance *CreateBehavior(void *) {
            return new Gameplay::IBehaviorInstance;
        }

        void DestroyBehavior(void *, Gameplay::IBehaviorInstance *instance) noexcept {
            delete instance;
        }

        Gameplay::BehaviorRegistry Behaviors(const bool frozen) {
            Gameplay::BehaviorRegistry registry;
            Gameplay::BehaviorDescriptor descriptor{.typeId = BehaviorType(),
                                                    .displayName = "Portable test",
                                                    .fields = {{"speed", 1.0}, {"label", std::string{"default"}}}};
            REQUIRE(registry.Register({std::move(descriptor), {nullptr, CreateBehavior, DestroyBehavior}}).HasValue());
            if (frozen)
                REQUIRE(registry.Freeze().HasValue());
            return registry;
        }
    }  // namespace

    TEST_CASE("Selection export preserves canonical builtins and disabled asset references without duplicate dependencies",
              "[prefab][creation][schema]") {
        const auto clip = PrefabAsset(2).Asset();
        const auto sound = Audio::AudioSoundReference::ForClip(Audio::AudioClipId::Create(clip).Value()).Value();
        SceneObjectSnapshot root{.id = {10}, .name = "Root"};
        root.components.camera = Runtime::CameraComponent{};
        root.components.light = Runtime::LightComponent{};
        root.components.audioSource = Runtime::AudioSourceComponent{.sound = sound, .enabled = false};
        SceneObjectSnapshot child{.id = {20}, .parent = SceneObjectId{10}, .name = "Child"};
        child.components.audioSource = Runtime::AudioSourceComponent{.sound = sound};
        const auto snapshot = Capture({root, child});
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({Record(clip, "core.audio_clip", "assets/clip.wav")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        std::vector<PrefabCreationBoundary> boundaries;
        const auto exported = Export(snapshot, registry, boundaries);
        REQUIRE(exported.HasValue());
        CHECK(boundaries.empty());
        CHECK(exported.Value().Data().referencedAssets == std::vector{clip});
        Runtime::RuntimeComponentSet projected;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        for (const auto &component : exported.Value().Data().objects.front().components)
            REQUIRE(SceneSource::ProjectPrefabBuiltinComponent(component, projected, primitive).HasValue());
        CHECK(projected.camera == root.components.camera);
        CHECK(projected.light == root.components.light);
        CHECK(projected.audioSource == root.components.audioSource);
        auto encoded = exported.Value().SerializeCanonical();
        REQUIRE(encoded.HasValue());
        REQUIRE(Prefab::PrefabDocument::Parse(encoded.Value(), Prefab::PrefabLimitProfile::Create({}).Value()).HasValue());
    }

    TEST_CASE("Internal Physics selection commits its native pair and linked instance in one undoable transaction",
              "[prefab][creation][native][physics]") {
        auto admitted = SceneDocumentTransactions::Create({PrefabAsset(8).Asset(), 1}, PhysicsObjects());
        REQUIRE(admitted.HasValue());
        auto owner = std::move(admitted).Value();
        const auto before = owner.Snapshot();
        REQUIRE(before.HasValue());
        ProjectDirectory directory;
        const auto root = std::filesystem::canonical(directory.path);
        REQUIRE(std::filesystem::create_directory(root / "assets"));
        CountingFiles files;
        ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({Record(PrefabAsset(3).Asset(), "core.physics_material", "assets/material.physicsmaterial")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        const auto &state = before.Value();
        const auto outcome = CreatePrefabFromSelection(owner,
                                                       {{state.session, state.revision, state.state, true},
                                                        {10},
                                                        root,
                                                        root / "assets/Physics.prefab",
                                                        PrefabAsset(9).Asset(),
                                                        ScenePrefabVersion(),
                                                        registry.Snapshot()},
                                                       mutations, files);
        REQUIRE(outcome.committed);
        CHECK_FALSE(outcome.error);
        CHECK(files.writes == 2);
        CHECK(std::filesystem::exists(outcome.files[0].path));
        CHECK(std::filesystem::exists(outcome.files[1].path));
        auto committed = owner.Snapshot();
        REQUIRE(committed.HasValue());
        CHECK(committed.Value().objects.empty());
        REQUIRE(committed.Value().prefabInstances.size() == 1);
        const auto context = [&](const SceneTransactionSnapshot &snapshot) {
            return SceneTransactionContext{snapshot.session, snapshot.revision, snapshot.state, true};
        };
        REQUIRE(owner.Replay(context(committed.Value()), false).HasValue());
        auto restored = owner.Snapshot();
        REQUIRE(restored.HasValue());
        CHECK(restored.Value().objects.size() == 2);
        CHECK(restored.Value().prefabInstances.empty());
        CHECK(owner.Replay(context(restored.Value()), false).HasError());
        REQUIRE(owner.Replay(context(restored.Value()), true).HasValue());
        CHECK(owner.Snapshot().Value().prefabInstances.size() == 1);
    }

    TEST_CASE("Selection collider assets retain exact shape material identities and reject absent or mistyped dependencies",
              "[prefab][creation][schema][physics]") {
        const auto mode = GENERATE(0, 1, 2, 3);
        auto objects = PhysicsObjects();
        const auto shape = PrefabAsset(4).Asset();
        objects.front().components.colliders.front().source = Runtime::PhysicsShapeAssetReference{shape, {1}};
        const auto snapshot = Capture(objects);
        Assets::AssetRegistry registry;
        std::vector<Assets::AssetRecord> records;
        if (mode != 1)
            records.push_back(Record(shape, mode == 2 ? "core.audio_clip" : "core.physics_shape", "assets/shape.physicsmesh"));
        records.push_back(
            Record(PrefabAsset(3).Asset(), mode == 3 ? "core.audio_clip" : "core.physics_material", "assets/material.physicsmaterial"));
        REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
        std::vector<PrefabCreationBoundary> boundaries;
        const auto exported = Export(snapshot, registry, boundaries);
        CHECK(boundaries.empty());
        if (mode != 0) {
            CHECK(exported.HasError());
            return;
        }
        REQUIRE(exported.HasValue());
        CHECK(exported.Value().Data().referencedAssets == std::vector{shape, PrefabAsset(3).Asset()});
        Runtime::RuntimeComponentSet projected;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        for (const auto &component : exported.Value().Data().objects.front().components)
            REQUIRE(SceneSource::ProjectPrefabBuiltinComponent(component, projected, primitive).HasValue());
        REQUIRE(projected.colliders.size() == 1);
        CHECK(projected.colliders.front().source == objects.front().components.colliders.front().source);
        CHECK(projected.colliders.front().materials == objects.front().components.colliders.front().materials);
    }

    TEST_CASE("Selection internal Physics endpoints preserve root zero and child identity through canonical runtime remapping",
              "[prefab][creation][schema][physics]") {
        const auto objects = PhysicsObjects();
        const auto snapshot = Capture(objects);
        Assets::AssetRegistry registry;
        const auto asset = PrefabAsset(9).Asset();
        REQUIRE(registry
                    .Publish({Record(PrefabAsset(3).Asset(), "core.physics_material", "assets/default.physicsmaterial"),
                              Record(asset, "core.prefab", "assets/created.prefab")})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        std::vector<PrefabCreationBoundary> boundaries;
        auto exported = Export(snapshot, registry, boundaries);
        REQUIRE(exported.HasValue());
        CHECK(boundaries.empty());
        Runtime::RuntimeComponentSet projected;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        for (const auto &component : exported.Value().Data().objects.front().components)
            REQUIRE(SceneSource::ProjectPrefabBuiltinComponent(component, projected, primitive).HasValue());
        REQUIRE(projected.colliders.size() == 1);
        CHECK(projected.colliders.front().body.object.value == 0);
        REQUIRE(projected.physicsConstraints.size() == 1);
        CHECK(projected.physicsConstraints.front().first.body.object.value == 0);
        CHECK(std::get<Runtime::PhysicsConstraintBodyEndpoint>(projected.physicsConstraints.front().second).body.object.value == 1);
        CHECK(Runtime::ValidateColliderComponent(projected.colliders.front()).HasError());
        CHECK(Runtime::ValidatePhysicsConstraintComponent(projected.physicsConstraints.front()).HasError());
        const auto encoded = exported.Value().SerializeCanonical();
        REQUIRE(encoded.HasValue());
        const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
        const auto digest = ComputeSha256(std::as_bytes(std::span{encoded.Value()}));
        auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(registry.Snapshot(),
                                                                  {{std::move(exported).Value(),
                                                                    {.projectVersion = ScenePrefabVersion(), .contentDigest = digest}}},
                                                                  limits);
        REQUIRE(resolver.HasValue());
        const std::vector<ScenePrefabInstance> placements{{.instanceId = Prefab::PrefabInstanceId::Create(1).Value(),
                                                           .sourcePrefab = Prefab::PrefabAssetReference::Create(asset).Value()}};
        const auto runtime = SceneSource::ConvertSceneSourceToRuntime({{}, placements}, {1}, {1}, resolver.Value(), limits);
        REQUIRE(runtime.HasValue());
        REQUIRE(runtime.Value().Entities().size() == 2);
        const auto entities = runtime.Value().Entities();
        CHECK(entities.front().components.colliders.front().body.object == entities.front().object);
        CHECK(entities.front().components.physicsConstraints.front().first.body.object == entities.front().object);
        CHECK(std::get<Runtime::PhysicsConstraintBodyEndpoint>(entities.front().components.physicsConstraints.front().second).body.object ==
              entities.back().object);
        CHECK(entities.back().components.colliders.front().body.object == entities.back().object);
        CHECK(Runtime::ValidateColliderComponent(entities.front().components.colliders.front()).HasValue());
        CHECK(Runtime::ValidatePhysicsConstraintComponent(entities.front().components.physicsConstraints.front()).HasValue());
    }

    TEST_CASE("Selection collider and constraint crossings retain exact incoming and outgoing source target diagnostics",
              "[prefab][creation][schema][physics]") {
        const bool incoming = GENERATE(false, true);
        auto objects = PhysicsObjects();
        objects.back().parent.reset();
        auto &source = incoming ? objects.back() : objects.front();
        const auto target = incoming ? objects.front().id : objects.back().id;
        auto crossing = source.components.colliders.front();
        crossing.id = {4};
        crossing.collider = {2};
        crossing.body.object = Runtime::SceneObjectId{target.value};
        crossing.enabled = false;
        objects.front().components.physicsConstraints.front().enabled = false;
        source.components.colliders.push_back(std::move(crossing));
        const auto snapshot = Capture(objects);
        Assets::AssetRegistry registry;
        std::vector<PrefabCreationBoundary> boundaries;
        REQUIRE(Export(snapshot, registry, boundaries).HasError());
        CHECK(std::ranges::any_of(boundaries, [&](const auto &site) {
            return site.source == source.id && site.target == target && site.property == "colliders.body";
        }));
        CHECK(std::ranges::any_of(boundaries, [](const auto &site) {
            return site.source == SceneObjectId{10} && site.target == SceneObjectId{20} &&
                   site.property == "physicsConstraints.second.body";
        }));
        RequireCrossingRejectedWithoutPublication(objects, registry);
    }

    TEST_CASE("Selection preserves an unreferenced navigation owner and unassigned disabled audio", "[prefab][creation][schema]") {
        SceneObjectSnapshot root{.id = {10}, .name = "Portable owners"};
        root.components.navigationSurface = NavigationSurface();
        root.components.navigationAgent = NavigationAgent();
        root.components.audioSource = Runtime::AudioSourceComponent{.enabled = false};
        Assets::AssetRegistry registry;
        const auto definition = root.components.navigationSurface->definition;
        REQUIRE(registry.Publish({Record(definition, "core.navmesh", "assets/world.navmesh")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        std::vector<PrefabCreationBoundary> boundaries;
        const auto exported = Export(Capture({root}), registry, boundaries);
        REQUIRE(exported.HasValue());
        CHECK(exported.Value().Data().referencedAssets == std::vector{definition});
        Runtime::RuntimeComponentSet projected;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        for (const auto &component : exported.Value().Data().objects.front().components)
            REQUIRE(SceneSource::ProjectPrefabBuiltinComponent(component, projected, primitive).HasValue());
        CHECK(projected.navigationSurface == root.components.navigationSurface);
        CHECK(projected.navigationAgent == root.components.navigationAgent);
        CHECK(projected.audioSource == root.components.audioSource);
        CHECK(boundaries.empty());
    }

    TEST_CASE("Selection asset admission rejects unresolved and mistyped navigation definitions before publication",
              "[prefab][creation][schema]") {
        const bool missing = GENERATE(false, true);
        SceneObjectSnapshot root{.id = {10}, .name = "Surface"};
        root.components.navigationSurface = NavigationSurface();
        const auto snapshot = Capture({root});
        Assets::AssetRegistry registry;
        if (!missing)
            REQUIRE(
                registry.Publish({Record(root.components.navigationSurface->definition, "core.audio_clip", "assets/wrong.wav")}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        std::vector<PrefabCreationBoundary> boundaries;
        CHECK(Export(snapshot, registry, boundaries).HasError());
        CHECK(boundaries.empty());  // Typed asset admission is distinct from object-reference diagnostics.
        CHECK(snapshot.objects.front().components.navigationSurface == root.components.navigationSurface);
    }

    TEST_CASE("Selection navigation boundary audit reports every incoming outgoing and internal surface owner",
              "[prefab][creation][schema]") {
        const bool incoming = GENERATE(false, true);
        const bool internal = GENERATE(false, true);
        SceneObjectSnapshot root{.id = {10}, .name = "Selected"};
        SceneObjectSnapshot other{.id = {20}, .name = "Other"};
        if (internal)
            other.parent = SceneObjectId{10};
        auto &surface = incoming ? root : other;
        auto &source = incoming ? other : root;
        surface.components.navigationSurface = NavigationSurface();
        source.components.navigationRegion = NavigationRegion();
        source.components.navigationModifier = NavigationModifier();
        source.components.navigationLink = NavigationLink();
        const auto snapshot = Capture({root, other});
        Assets::AssetRegistry registry;
        std::vector<PrefabCreationBoundary> boundaries;
        REQUIRE(Export(snapshot, registry, boundaries).HasError());
        for (const std::string_view property :
             {"navigationRegion.surface", "navigationModifier.surface", "navigationLink.start.surface", "navigationLink.end.surface"}) {
            CHECK(std::ranges::any_of(boundaries, [&](const auto &site) {
                return site.source == source.id && site.target == surface.id && site.property == property;
            }));
        }
    }

    TEST_CASE("Selection behavior export requires frozen exact schema authority and preserves supported scalar fields",
              "[prefab][creation][schema]") {
        const auto mode = GENERATE(0, 1, 2, 3, 4, 5);
        auto registry = Behaviors(mode != 1);
        SceneObjectSnapshot root{.id = {10}, .name = "Behavior owner"};
        Gameplay::BehaviorComponent behavior{.instanceId = {1},
                                             .typeId = BehaviorType(),
                                             .enabled = false,
                                             .fields = {{"speed", 2.5}, {"label", std::string{"preserved"}}}};
        if (mode == 2)
            behavior.schemaVersion = 2;
        if (mode == 3)
            behavior.fields.push_back({"unknown", true});
        if (mode == 4)
            behavior.fields.front().value = std::int64_t{2};
        root.components.behaviors.push_back(behavior);
        const auto snapshot = Capture({root});
        Assets::AssetRegistry assets;
        std::vector<PrefabCreationBoundary> boundaries;
        const auto exported = Export(snapshot, assets, boundaries, mode == 5 ? nullptr : &registry);
        if (mode == 0) {
            REQUIRE(exported.HasValue());
            CHECK(exported.Value().Data().objects.front().behaviors == root.components.behaviors);
            CHECK(boundaries.empty());
        } else {
            REQUIRE(exported.HasError());
            REQUIRE(boundaries.size() == 1);
            CHECK(boundaries.front().source == root.id);
            CHECK(boundaries.front().property == behavior.typeId.Value());
        }
    }
}  // namespace Horo::Editor
