#include "SceneDocumentPersistenceTestSupport.h"
#include "editor/document/SceneDocumentPersistence.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <span>
#include <string>
#include <variant>
#include <vector>

using namespace Horo;
using namespace Horo::Editor;
using namespace Horo::Editor::PersistenceTestSupport;

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
