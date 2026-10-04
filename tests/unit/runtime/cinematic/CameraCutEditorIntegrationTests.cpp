#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "support/CameraCutTestSupport.h"

namespace Horo::Editor {
    namespace {
        using namespace Tests::CameraCuts;

        struct PreviewFixture final {
            Runtime::RuntimeSceneService scene;
            CancellationSource cancellation;
            EditorWorkspaceController controller{"test-camera-cut-project", scene};
            std::array<Cinematic::CameraCutBinding, 2> bindings{Cinematic::CameraCutBinding{{1, 1}, {1}},
                                                                Cinematic::CameraCutBinding{{2, 1}, {2}}};

            PreviewFixture() {
                REQUIRE(scene.Startup(cancellation.Token()).HasValue());
                CommitScene();
                REQUIRE(scene.QueuePreparation(SceneDefinition()).HasValue());
                CommitScene();
            }

            void CommitScene() {
                const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation.Token()};
                REQUIRE(scene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
                controller.SynchronizeRuntimeScenePreview();
            }

            Cinematic::CameraCutActivation Settings() const {
                return {bindings, 8, Cinematic::CameraCutEndPolicy::HoldLastUntilPlayerEnd, 1, {1, 1}};
            }

            void Start() {
                REQUIRE(controller.StartCameraCutPreview(Activation(), Settings(), {1, 1}).HasValue());
            }
        };
    }  // namespace

    TEST_CASE("Controller extraction uses runtime hard cuts and hands off to the current authoring owner",
              "[unit][cinematic][camera][editor]") {
        PreviewFixture fixture;
        const auto initial = fixture.controller.ViewportScene().camera;
        fixture.Start();
        CHECK(fixture.controller.ViewportScene().camera.position == initial.position);
        REQUIRE(fixture.controller.AdvanceCameraCutPreview(2).HasValue());
        const auto committed = fixture.controller.ViewportScene();
        CHECK(committed.camera.position.x == 10.0F);
        EditorWorkspaceViewCommandData align;
        align.command = EditorWorkspaceViewCommand::AlignViewportToAxis;
        align.viewportAxisPayload = EditorViewportAxisView::NegativeY;
        fixture.controller.ProcessCommand(align);
        CHECK(fixture.controller.ViewportScene().camera.position.x == 10.0F);
        REQUIRE(fixture.controller.SeekCameraCutPreview(5).HasValue());
        CHECK(fixture.controller.ViewportScene().camera.position.x == 20.0F);
        CHECK(committed.camera.position.x == 10.0F);
        fixture.controller.StopCameraCutPreview();
        const auto returned = fixture.controller.ViewportScene().camera;
        CHECK(returned.position != initial.position);
        CHECK(returned.position.y < returned.target.y);
        CHECK(returned.IsValid());
    }

    TEST_CASE("Controller required-camera failures preserve admission atomically and release deleted targets",
              "[unit][cinematic][camera][editor]") {
        PreviewFixture fixture;
        fixture.Start();
        REQUIRE(fixture.controller.SeekCameraCutPreview(3).HasValue());
        fixture.bindings[1].object = {99};
        CHECK(fixture.controller.StartCameraCutPreview(Activation(), fixture.Settings(), {2, 1}).HasError());
        CHECK(fixture.controller.ViewportScene().camera.position.x == 10.0F);
        Runtime::SceneCommandBuffer commands;
        commands.Destroy(*fixture.scene.ActiveScene()->Find({1}));
        REQUIRE(fixture.scene.QueueStructuralCommands(std::move(commands)).HasValue());
        fixture.CommitScene();
        CHECK(fixture.controller.ViewportScene().camera.position.x == 0.0F);
        CHECK(fixture.controller.ViewportScene().camera.IsValid());
        CHECK(fixture.controller.AdvanceCameraCutPreview(1).HasError());
    }
}  // namespace Horo::Editor
