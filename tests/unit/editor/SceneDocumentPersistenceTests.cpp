#include "SceneDocumentPersistenceTestSupport.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/document/SceneFileWatchService.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace Horo;
using namespace Horo::Editor;
using namespace Horo::Editor::PersistenceTestSupport;

namespace {
    [[nodiscard]] Result<ProjectSceneSaveResult> SaveDefaultScene(const TemporaryProject &project, const SceneDocumentSnapshot &snapshot) {
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        const auto expected = InspectProjectSceneFingerprint(project.Root(), project.ScenePath());
        if (expected.HasError())
            return Result<ProjectSceneSaveResult>::Failure(expected.ErrorValue());
        return SaveProjectScene(project.Root(), project.ScenePath(), snapshot, expected.Value(), false, mutations, files);
    }

    [[nodiscard]] Result<void> WriteRecoveryForTest(const TemporaryProject &project, const SceneDocumentSnapshot &snapshot) {
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations(files);
        return WriteProjectSceneRecovery(project.Root(), project.ScenePath(), snapshot, DocumentRevision{}, DocumentStateId{1}, mutations,
                                         files);
    }
}  // namespace

TEST_CASE("Project Scene Save Reopens The Same Authored State", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.PrepareEmptyScene();

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

TEST_CASE("Project default scene mutation validates containment and replaces metadata durably", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.PrepareEmptyScene();

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    CHECK(SetProjectDefaultScenePath(project.Root() / "relative", project.ScenePath(), mutations, files).HasError());
    CHECK(SetProjectDefaultScenePath(project.Root(), project.Root().parent_path() / "outside.horo", mutations, files).HasError());
    CHECK(SetProjectDefaultScenePath(project.Root(), project.Root() / "assets/scenes/main.json", mutations, files).HasError());

    REQUIRE((SetProjectDefaultScenePath(project.Root(), project.ScenePath(), mutations, files).HasValue()));
    const auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE((loaded.HasValue() && loaded.Value().has_value()));
    CHECK(loaded.Value()->absolutePath == std::filesystem::weakly_canonical(project.ScenePath()));
}

TEST_CASE("Project default scene mutation removes its prepared file after replacement failure", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.PrepareEmptyScene();
    ReplaceFailingFileSystem files;
    ProjectMutationCoordinator mutations(files);

    const auto result = SetProjectDefaultScenePath(project.Root(), project.ScenePath(), mutations, files);
    REQUIRE(result.HasError());
    CHECK_FALSE(std::filesystem::exists(project.Root() / ".horo/project.json.save.tmp"));
}

TEST_CASE("Navigation link direction and modifier shape round trip explicitly", "[unit][editor][persistence][navigation]") {
    TemporaryProject project;
    project.PrepareEmptyScene();
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
    components.navigationAgent = Runtime::NavigationAgentComponent{
        .schemaVersion = 1,
        .profile = Navigation::NavigationAgentProfileId::Create(6).Value(),
        .filter = Navigation::NavigationFilterId::Create(8).Value(),
        .radiusOverride = 0.65F,
        .movementCapability = Navigation::NavigationAgentMovementCapability::Grounded,
    };

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
    REQUIRE(loadedComponents.navigationAgent == components.navigationAgent);
    REQUIRE(std::holds_alternative<Runtime::NavigationLocalBounds>(loadedComponents.navigationModifier->volume));
    REQUIRE(loadedComponents.navigationLink->direction == Runtime::NavigationLinkDirection::StartToEnd);
}

TEST_CASE("AI scene component bindings remain inspectable when activation descriptors are unavailable", "[unit][editor][persistence][ai]") {
    TemporaryProject project;
    project.PrepareEmptyScene();
    SceneDocumentSnapshot snapshot{
        .revision = DocumentRevision{1},
        .state = DocumentStateId{1},
        .objects = {SceneObjectSnapshot{
            .id = SceneObjectId{1},
            .name = "Unresolved AI Agent",
            .components =
                SceneObjectComponentSet{
                    .aiAgent = AI::AiAgentComponent{.agent = AI::AgentId::Create(41).Value(), .enabled = true},
                    .aiController = AI::AiControllerComponent{.controller = AI::ControllerTypeId::Create(51).Value(),
                                                              .decisionAsset = AI::DecisionGraphAssetId::Create(61).Value(),
                                                              .blackboardSchema = AI::BlackboardSchemaId::Create(71).Value(),
                                                              .requiredCapabilities = AI::AiCapabilitySet::Of(AI::AiCapability::Behavior),
                                                              .enabled = true},
                },
        }},
    };

    REQUIRE(SaveDefaultScene(project, snapshot).HasValue());
    const auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value().has_value());
    REQUIRE(loaded.Value()->objects.size() == 1);
    REQUIRE(loaded.Value()->objects.front().components.aiAgent.has_value());
    REQUIRE(loaded.Value()->objects.front().components.aiController.has_value());
    CHECK(loaded.Value()->objects.front().components.aiAgent->agent.Value() == 41);
    CHECK(loaded.Value()->objects.front().components.aiController->controller.Value() == 51);
    CHECK(loaded.Value()->objects.front().components.aiController->decisionAsset.Value() == 61);
    CHECK(loaded.Value()->objects.front().components.aiController->blackboardSchema.Value() == 71);
}

TEST_CASE("Every authored light kind survives project scene save and reload", "[unit][editor][persistence]") {
    TemporaryProject project;
    project.PrepareEmptyScene();
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

    const auto saved = SaveDefaultScene(project, snapshot);
    REQUIRE((saved.HasValue()));

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
    project.PrepareEmptyScene();
    const auto asset = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff");
    REQUIRE((asset.HasValue()));
    SceneDocumentSnapshot snapshot{
        .revision = DocumentRevision{1},
        .state = DocumentStateId{1},
        .objects = {SceneObjectSnapshot{.id = SceneObjectId{1}, .name = "Chair", .meshAsset = asset.Value()}},
    };

    const auto saved = SaveDefaultScene(project, snapshot);
    REQUIRE((saved.HasValue()));
    auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE((loaded.HasValue() && loaded.Value().has_value()));
    REQUIRE((loaded.Value()->objects.size() == 1));
    CHECK(loaded.Value()->objects.front().meshAsset == asset.Value());
    CHECK_FALSE(loaded.Value()->objects.front().primitiveMesh.has_value());
}

void RequireAudioSourceRoundTrip(const std::span<const Audio::AudioSoundReference> references) {
    TemporaryProject project;
    project.PrepareEmptyScene();

    std::vector<SceneObjectSnapshot> objects;
    objects.reserve(references.size());
    for (std::size_t index = 0; index < references.size(); ++index) {
        Runtime::AudioSourceComponent audioSource{.sound = references[index]};
        if (index == 0) {
            const auto bus = Audio::AudioBusId::Create(9);
            const auto concurrencyGroup = Audio::AudioConcurrencyGroupId::Create(17);
            REQUIRE(bus.HasValue());
            REQUIRE(concurrencyGroup.HasValue());
            audioSource.playback = {.gain = 0.75F,
                                    .pitch = 1.25F,
                                    .bus = bus.Value(),
                                    .loop = true,
                                    .spatialMode = Audio::AudioSpatialMode::TwoD,
                                    .enableDoppler = true,
                                    .playOnStart = false,
                                    .priority = 240,
                                    .concurrency = Audio::AudioConcurrencyPolicy{.group = concurrencyGroup.Value(),
                                                                                 .maxInstances = 3,
                                                                                 .mode = Audio::AudioConcurrencyMode::StealOldest}};
            audioSource.sceneLifecycle = Audio::AudioSceneLifecyclePolicy::KeepAliveInHostContext;
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
    project.PrepareEmptyScene(R"({
        "schemaVersion": 1,
        "objects": [
            {"id": 1, "parent": null, "name": "Native Clip", "transform": {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}, "primitiveMesh": null, "components": {"audioSource": {"kind": "native_clip", "sound": "11111111-1111-4111-8111-111111111111", "gain": 1.0, "spatial": true}}},
            {"id": 2, "parent": null, "name": "Middleware Event", "transform": {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}, "primitiveMesh": null, "components": {"audioSource": {"kind": "middleware_event", "gain": 1.0, "spatial": false}}}
        ]
    })");

    const auto loaded = LoadProjectDefaultScene(project.Root());
    REQUIRE((loaded.HasValue() && loaded.Value().has_value()));
    REQUIRE(loaded.Value()->objects.size() == 2);
    CHECK(loaded.Value()->objects[0].components.audioSource->playback.spatialMode == Audio::AudioSpatialMode::ThreeD);
    CHECK(loaded.Value()->objects[1].components.audioSource->playback.spatialMode == Audio::AudioSpatialMode::TwoD);
    for (const SceneObjectSnapshot &object : loaded.Value()->objects) {
        REQUIRE(object.components.audioSource.has_value());
        CHECK(object.components.audioSource->sound.kind == Audio::AudioSoundReferenceKind::Unassigned);
        CHECK(std::holds_alternative<std::monostate>(object.components.audioSource->sound.target));
    }
}

TEST_CASE("Audio source persistence rejects malformed admission policies", "[unit][editor][persistence][audio][validation]") {
    const std::array<std::string, 3> malformedPolicies{
        R"("priority":256)",
        R"("concurrency":{"mode":"unknown"})",
        R"("sceneLifecycle":"unknown")",
    };
    for (const std::string &policy : malformedPolicies) {
        TemporaryProject project;
        project.PrepareEmptyScene(
            std::string{
                R"({"schemaVersion":1,"objects":[{"id":1,"parent":null,"name":"Audio","transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"primitiveMesh":null,"components":{"audioSource":{"kind":"native_clip","gain":1.0,)"} +
            policy + R"(}}}]})");

        const auto loaded = LoadProjectDefaultScene(project.Root());
        CHECK(loaded.HasError());
    }
}

TEST_CASE("Failed Atomic Scene Replace Preserves Canonical Bytes", "[unit][editor][persistence]") {
    TemporaryProject project;
    const std::string original = "{\"schemaVersion\":1,\"objects\":[]}\n";
    project.PrepareEmptyScene(original);

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
    project.PrepareEmptyScene("{\"schemaVersion\":2,\"objects\":[]}\n");
    REQUIRE((LoadProjectDefaultScene(project.Root()).HasError()));
}

TEST_CASE("Project Scene Loader Rejects Malformed Prefab Instance Records", "[unit][editor][persistence][prefab]") {
    TemporaryProject project;
    project.PrepareEmptyScene(R"({"schemaVersion":1,"objects":[],"prefabInstances":{}})");
    REQUIRE((LoadProjectDefaultScene(project.Root()).HasError()));

    project.WriteScene(R"({"schemaVersion":1,"objects":[],"prefabInstances":[{"instanceId":1,"sourceAsset":"not-an-id"}]})");
    REQUIRE((LoadProjectDefaultScene(project.Root()).HasError()));
}

TEST_CASE("Scene Save Detects External Byte Changes Before Atomic Replacement", "[unit][editor][persistence][conflict]") {
    TemporaryProject project;
    const std::string original = "{\"schemaVersion\":1,\"objects\":[]}\n";
    project.PrepareEmptyScene(original);

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
    project.PrepareEmptyScene();

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
    project.PrepareEmptyScene();
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
    project.PrepareEmptyScene();
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
    project.PrepareEmptyScene();

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
    const std::string canonical = "{\"schemaVersion\":1,\"objects\":[]}\n";
    project.PrepareEmptyScene(canonical);

    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);

    const SceneDocumentSnapshot authored = AuthoredScene();
    REQUIRE((WriteRecoveryForTest(project, authored).HasValue()));

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
    project.PrepareEmptyScene();

    const SceneDocumentSnapshot authored = AuthoredScene();
    REQUIRE((WriteRecoveryForTest(project, authored).HasValue()));

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
    project.PrepareEmptyScene();

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
    project.PrepareEmptyScene();

    JobSystem jobs(JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8});
    {
        SceneFileWatchService watcher(jobs);
        auto requested = watcher.Request(project.Root(), project.ScenePath());
        REQUIRE((requested.HasValue()));

        const SceneFileWatchUpdate firstUpdate = WaitForWatchUpdate(watcher);
        REQUIRE((firstUpdate.generation == requested.Value()));
        REQUIRE((firstUpdate.fingerprint.has_value()));
        REQUIRE((!firstUpdate.error.has_value()));

        const SceneFileFingerprint first = *firstUpdate.fingerprint;
        project.WriteScene("{\n  \"schemaVersion\": 1,\n  \"objects\": []\n}\n");
        requested = watcher.Request(project.Root(), project.ScenePath());
        REQUIRE((requested.HasValue()));
        const SceneFileWatchUpdate secondUpdate = WaitForWatchUpdate(watcher);
        REQUIRE((secondUpdate.fingerprint.has_value()));
        REQUIRE((*secondUpdate.fingerprint != first));
    }
    jobs.Shutdown(ShutdownPolicy::Drain);
}
