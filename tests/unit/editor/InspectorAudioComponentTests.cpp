#include "editor/screens/workspace/panels/inspector/InspectorEditSession.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    SceneObject SourceAndListener() {
        SceneObject object{.id = {11}, .name = "Audio"};
        object.components.audioSource = Runtime::AudioSourceComponent{};
        object.components.audioListener = Runtime::AudioListenerComponent{};
        return object;
    }

    TEST_CASE("Inspector full audio draft emits one typed source command without mutating authoring", "[editor][audio][inspector]") {
        const auto object = SourceAndListener();
        InspectorEditSession session;
        static_cast<void>(session.BeginObject(object, {1}));
        auto &source = *session.Draft().audioSource;
        source.sound = Audio::AudioSoundReference::ForClip(
                           Audio::AudioClipId::Create(Assets::AssetId::Parse("11111111-1111-4111-8111-111111111111").Value()).Value())
                           .Value();
        source.playback = {.gain = 0.25F,
                           .pitch = 1.5F,
                           .bus = Audio::AudioBusId::Create(3).Value(),
                           .loop = true,
                           .spatialMode = Audio::AudioSpatialMode::TwoD,
                           .enableDoppler = true,
                           .playOnStart = false,
                           .priority = 255,
                           .concurrency = {Audio::AudioConcurrencyGroupId::Create(17).Value(), 2, Audio::AudioConcurrencyMode::Replace}};
        source.sceneLifecycle = Audio::AudioSceneLifecyclePolicy::KeepAliveInHostContext;
        const auto command = session.ApplyAudioSourceEdit({.committed = true}, object, true);
        REQUIRE(command.command == EditorWorkspaceViewCommand::UpdateAudioSourceComponent);
        CHECK(command.audioSourcePayload == source);
        CHECK(object.components.audioSource == Runtime::AudioSourceComponent{});
        CHECK(session.ApplyAudioSourceEdit({.committed = true}, object, false).command == EditorWorkspaceViewCommand::None);
        static_cast<void>(session.ApplyAudioSourceEdit({.cancelRequested = true}, object, true));
        CHECK(session.Draft().audioSource == object.components.audioSource);
    }

    TEST_CASE("Inspector listener draft validates cancellation and emits only typed listener commands", "[editor][audio][inspector]") {
        const auto object = SourceAndListener();
        InspectorEditSession session;
        static_cast<void>(session.BeginObject(object, {1}));
        session.Draft().audioListener = Runtime::AudioListenerComponent{16, -50, 0.125F, false};
        auto command = session.ApplyAudioListenerEdit({.committed = true}, object, true);
        REQUIRE(command.command == EditorWorkspaceViewCommand::UpdateAudioListenerComponent);
        CHECK(command.audioListenerPayload == session.Draft().audioListener);
        session.Draft().audioListener->weight = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(session.IsAudioListenerValid());
        CHECK(session.ApplyAudioListenerEdit({.committed = true}, object, true).command == EditorWorkspaceViewCommand::None);
        static_cast<void>(session.ApplyAudioListenerEdit({.cancelRequested = true}, object, true));
        CHECK(session.Draft().audioListener == object.components.audioListener);
    }
}  // namespace
