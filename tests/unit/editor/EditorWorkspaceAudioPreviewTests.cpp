#include "AudioPreviewHostTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::AudioPreviewTest;

    struct AudioWorkspace final {
        ClipProject project;
        std::shared_ptr<OutputEvidence> evidence{std::make_shared<OutputEvidence>()};
        EditorAudioPreviewHost host{ObservedOutput(evidence)};
        CancellationSource cancellation;
        Runtime::RuntimeSceneService runtime;
        std::unique_ptr<EditorWorkspaceController> controller;
        SceneObjectId object;

        AudioWorkspace() {
            REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
            controller = std::make_unique<EditorWorkspaceController>(project.root, runtime, project.registry.Snapshot(),
                                                                     EditorWorkspaceDependencies{.documentSessionOwner = project.workspace,
                                                                                                 .audioPreview = &host});
            Commit();
            REQUIRE_FALSE(controller->ViewModel().objects.empty());
            object = controller->ViewModel().objects.front().id;
            Process({.command = EditorWorkspaceViewCommand::SelectObject, .objectPayload = object});
            Process({.command = EditorWorkspaceViewCommand::AddComponentToObject,
                     .objectPayload = object,
                     .componentTypePayload = ComponentType::AudioSource});
            auto audio = *controller->ViewModel().objects.front().components.audioSource;
            audio.sound = project.Request().source.sound;
            audio.playback = project.Request().source.playback;
            Process(
                {.command = EditorWorkspaceViewCommand::UpdateAudioSourceComponent, .objectPayload = object, .audioSourcePayload = audio});
        }

        void Commit() {
            const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation.Token()};
            REQUIRE(runtime.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
            controller->SynchronizeRuntimeScenePreview();
        }

        void Process(const EditorWorkspaceViewCommandData &command) {
            controller->ProcessCommand(command);
            Commit();
        }

        void Start() {
            REQUIRE(controller->StartAudioPreview(object, controller->ViewModel().documentRevision).HasValue());
            Pump(host);
            controller->ReconcileAudioPreview();
            REQUIRE(host.Snapshot().output->phase == Audio::AudioFrontendPhase::Active);
            REQUIRE(evidence->peak > 0.01F);
        }
    };

    TEST_CASE("Workspace preview is source-inert and selection revision or close revokes real output",
              "[editor][audio][preview][headless]") {
        AudioWorkspace fixture;
        const auto revision = fixture.controller->ViewModel().documentRevision;
        const bool dirty = fixture.controller->ViewModel().isDirty;
        const bool undo = fixture.controller->ViewModel().canUndo;
        fixture.Start();
        CHECK(fixture.controller->ViewModel().documentRevision == revision);
        CHECK(fixture.controller->ViewModel().isDirty == dirty);
        CHECK(fixture.controller->ViewModel().canUndo == undo);
        SECTION("selection changes") {
            fixture.Process({.command = EditorWorkspaceViewCommand::SelectObject, .objectSelection = ObjectSelectionRequest{}});
        }
        SECTION("document revision changes") {
            auto audio = *fixture.controller->ViewModel().objects.front().components.audioSource;
            audio.playback.gain = 0.25F;
            fixture.Process({.command = EditorWorkspaceViewCommand::UpdateAudioSourceComponent,
                             .objectPayload = fixture.object,
                             .audioSourcePayload = audio});
        }
        SECTION("workspace closes while application remains") {
            fixture.controller.reset();
        }
        Pump(fixture.host);
        REQUIRE(fixture.host.Snapshot().output);
        CHECK(fixture.host.Snapshot().output->phase == Audio::AudioFrontendPhase::Closed);
        CHECK(fixture.evidence->destroyed);
        const auto blocks = fixture.evidence->renderingBlocks;
        Pump(fixture.host);
        CHECK(fixture.evidence->renderingBlocks == blocks);
    }

    TEST_CASE("Workspace audio command routes reject stale preview and stop without authoring changes",
              "[editor][audio][preview][headless]") {
        AudioWorkspace fixture;
        const auto revision = fixture.controller->ViewModel().documentRevision;
        const auto dirty = fixture.controller->ViewModel().isDirty;
        const auto canUndo = fixture.controller->ViewModel().canUndo;
        fixture.Process({.command = EditorWorkspaceViewCommand::StartAudioComponentPreview,
                         .objectPayload = fixture.object,
                         .audioPreviewRevision = DocumentRevision{revision.value + 1}});
        REQUIRE(fixture.controller->ViewModel().audioPreviewFailure);
        CHECK(fixture.controller->ViewModel().audioPreviewFailure->code.Value() == AudioComponentPreviewErrors::Stale.code.Value());
        CHECK_FALSE(fixture.host.Snapshot().output);
        fixture.Process({.command = EditorWorkspaceViewCommand::StartAudioComponentPreview,
                         .objectPayload = fixture.object,
                         .audioPreviewRevision = revision});
        Pump(fixture.host);
        fixture.controller->ReconcileAudioPreview();
        REQUIRE(fixture.host.Snapshot().output);
        CHECK(fixture.host.Snapshot().output->phase == Audio::AudioFrontendPhase::Active);
        CHECK_FALSE(fixture.controller->ViewModel().audioPreviewFailure);
        CHECK(fixture.evidence->peak > 0.01F);
        fixture.Process({.command = EditorWorkspaceViewCommand::StopAudioComponentPreview});
        Pump(fixture.host);
        REQUIRE(fixture.host.Snapshot().output);
        CHECK(fixture.host.Snapshot().output->phase == Audio::AudioFrontendPhase::Closed);
        CHECK(fixture.evidence->destroyed);
        CHECK(fixture.controller->ViewModel().documentRevision == revision);
        CHECK(fixture.controller->ViewModel().isDirty == dirty);
        CHECK(fixture.controller->ViewModel().canUndo == canUndo);
    }

    TEST_CASE("Workspace listener commands retain source and undo redo ownership", "[editor][audio][listener][headless]") {
        AudioWorkspace fixture;
        fixture.Process({.command = EditorWorkspaceViewCommand::AddComponentToObject,
                         .objectPayload = fixture.object,
                         .componentTypePayload = ComponentType::AudioListener});
        const auto original = fixture.controller->ViewModel().objects.front().components.audioListener;
        const auto source = fixture.controller->ViewModel().objects.front().components.audioSource;
        const Runtime::AudioListenerComponent authored{2, -7, 0.25F, false};
        fixture.Process({.command = EditorWorkspaceViewCommand::UpdateAudioListenerComponent,
                         .objectPayload = fixture.object,
                         .audioListenerPayload = authored});
        CHECK(fixture.controller->ViewModel().objects.front().components.audioListener == authored);
        CHECK(fixture.controller->ViewModel().objects.front().components.audioSource == source);
        fixture.Process({.command = EditorWorkspaceViewCommand::UndoScene});
        CHECK(fixture.controller->ViewModel().objects.front().components.audioListener == original);
        CHECK(fixture.controller->ViewModel().objects.front().components.audioSource == source);
        fixture.Process({.command = EditorWorkspaceViewCommand::RedoScene});
        CHECK(fixture.controller->ViewModel().objects.front().components.audioListener == authored);
        CHECK(fixture.controller->ViewModel().objects.front().components.audioSource == source);
    }

}  // namespace
