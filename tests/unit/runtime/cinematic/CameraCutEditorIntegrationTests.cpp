#include "Horo/Runtime/Camera/CameraErrors.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "support/CameraCutTestSupport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

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

    TEST_CASE("Controller scrubbing and runtime blend evaluation publish matching camera values", "[unit][cinematic][camera][editor]") {
        const auto curve = GENERATE(Cinematic::CameraBlendCurve::Linear, Cinematic::CameraBlendCurve::SmoothStep);
        PreviewFixture fixture;
        const auto scene = *fixture.scene.ActiveScene();
        auto runtime = Take(Cinematic::CinematicRuntimeService::Create({{1, 1}}));
        auto camera = Take(Runtime::CameraService::Create({{2, 1}, scene.RuntimeId(), Runtime::CameraViewDomain::Runtime}));
        auto playback =
            Take(Cinematic::CinematicCameraPlayback::Activate(runtime, camera,
                                                              TransitionActivation(Cinematic::CameraTransitionTier::SingleViewBlend, curve),
                                                              fixture.Settings(), scene));
        REQUIRE(runtime.Play(playback.Player()).HasValue());
        REQUIRE(fixture.controller
                    .StartCameraCutPreview(TransitionActivation(Cinematic::CameraTransitionTier::SingleViewBlend, curve),
                                           fixture.Settings(), {1, 1})
                    .HasValue());
        std::array<Cinematic::SequenceFrameCameraCutRequest, 64> crossings{};
        for (Cinematic::SequenceTime time = 2; time <= 8; ++time) {
            REQUIRE(playback.Evaluate(time == 2 ? 2 : 1, scene, {{}, {}, crossings}, {}).HasValue());
            REQUIRE(fixture.controller.SeekCameraCutPreview(time).HasValue());
            const auto selection = Take(camera.Commit(static_cast<std::uint64_t>(time), scene, std::nullopt));
            CHECK(fixture.controller.ViewportScene().camera.position == selection.selected.values.position);
            CHECK(fixture.controller.ViewportScene().camera.target == selection.selected.values.target);
            CHECK(fixture.controller.ViewportScene().camera.up == selection.selected.values.up);
            const auto &previewCamera = fixture.controller.ViewportScene().camera;
            const auto &runtimeLens = selection.selected.values.projection;
            const auto expectedProjection = runtimeLens.kind == Render::RenderProjectionKind::Perspective
                                                ? Runtime::CameraProjection::Perspective
                                                : Runtime::CameraProjection::Orthographic;
            CHECK(previewCamera.projection == expectedProjection);
            CHECK(previewCamera.verticalFovRadians == runtimeLens.verticalFovRadians);
            CHECK(previewCamera.orthographicHeight == runtimeLens.orthographicHeight);
            CHECK(previewCamera.nearPlane == runtimeLens.nearPlane);
            CHECK(previewCamera.farPlane == runtimeLens.farPlane);
        }
        fixture.controller.StopCameraCutPreview();
        CHECK(fixture.controller.ViewportScene().camera.IsValid());
    }

    TEST_CASE("Editor and runtime camera blends stay in sync after a scene coordinate shift", "[unit][cinematic][camera][editor][rebase]") {
        const auto curve = GENERATE(Cinematic::CameraBlendCurve::Linear, Cinematic::CameraBlendCurve::SmoothStep);
        PreviewFixture fixture;
        const auto scene = *fixture.scene.ActiveScene();
        auto runtime = Take(Cinematic::CinematicRuntimeService::Create({{1, 1}}));
        auto camera = Take(Runtime::CameraService::Create({{2, 1}, scene.RuntimeId(), Runtime::CameraViewDomain::Runtime}));
        const auto activation = TransitionActivation(Cinematic::CameraTransitionTier::SingleViewBlend, curve);
        auto playback = Take(Cinematic::CinematicCameraPlayback::Activate(runtime, camera, activation, fixture.Settings(), scene));
        REQUIRE(runtime.Play(playback.Player()).HasValue());
        REQUIRE(fixture.controller.StartCameraCutPreview(activation, fixture.Settings(), {1, 1}).HasValue());

        std::array<Cinematic::SequenceFrameCameraCutRequest, 64> crossings{};
        REQUIRE(playback.Evaluate(6, scene, {{}, {}, crossings}, {}).HasValue());
        REQUIRE(fixture.controller.SeekCameraCutPreview(6).HasValue());
        const auto selection = Take(camera.Commit(1, scene, std::nullopt));
        const auto before = fixture.controller.ViewportScene().camera;
        CHECK(before.position.x == 15.0F);
        CHECK(selection.selected.values.position == before.position);

        Runtime::SceneCommandBuffer commands;
        for (std::uint64_t id = 1; id <= 3; ++id)
            commands.SetLocalTransform(*scene.Find({id}), {{static_cast<float>(id * 10) - 100.0F, 0.0F, 0.0F}});
        const auto definition = scene.DefinitionRevision();
        const auto revision = scene.StructuralRevision();
        REQUIRE(fixture.scene.QueueStructuralCommands(std::move(commands)).HasValue());
        fixture.CommitScene();
        const auto current = *fixture.scene.ActiveScene();
        CHECK(current.DefinitionRevision() == definition);
        CHECK(current.StructuralRevision() != revision);
        CHECK_FALSE(scene.IsCurrent());

        REQUIRE(playback.Publish(current).HasValue());
        const auto republished = Take(camera.Commit(2, current, std::nullopt));
        const auto after = fixture.controller.ViewportScene().camera;
        CHECK(after.position.x == -85.0F);
        CHECK(republished.selected.values.position == after.position);
        CHECK(republished.selected.values.target == after.target);
        CHECK(republished.selected.values.up == after.up);
        CHECK(selection.selected.values.position.x == 15.0F);
        REQUIRE(fixture.controller.AdvanceCameraCutPreview(0).HasValue());
        CHECK(fixture.controller.ViewportScene().camera.position == after.position);
    }

    TEST_CASE("Editor preview honors host camera transition capabilities and authored fallback", "[unit][cinematic][camera][editor]") {
        PreviewFixture fixture;
        fixture.Start();
        REQUIRE(fixture.controller.SeekCameraCutPreview(3).HasValue());
        constexpr auto support = Runtime::CameraTransitionSupport::HardCutOnly;
        const auto denied = fixture.controller.StartCameraCutPreview(TransitionActivation(), fixture.Settings(), {2, 1}, support);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == Runtime::CameraErrors::UnsupportedTransition.code.Value());
        CHECK(fixture.controller.ViewportScene().camera.position.x == 10.0F);

        const auto activation = TransitionActivation(Cinematic::CameraTransitionTier::SingleViewBlend, Cinematic::CameraBlendCurve::Linear,
                                                     Cinematic::CameraTransitionFallback::HardCut);
        REQUIRE(fixture.controller.StartCameraCutPreview(activation, fixture.Settings(), {2, 1}, support).HasValue());
        REQUIRE(fixture.controller.SeekCameraCutPreview(6).HasValue());
        CHECK(fixture.controller.ViewportScene().camera.position.x == 20.0F);
    }

    TEST_CASE("Editor preview copies the cross-fade pair through the viewport scene and clears it on handoff",
              "[unit][cinematic][camera][editor][cross-fade]") {
        PreviewFixture fixture;
        const auto scene = *fixture.scene.ActiveScene();
        const Runtime::CameraViewContext context{{1, 1}, scene.RuntimeId(), Runtime::CameraViewDomain::EditorPreview};
        const Runtime::CameraCrossFadeAdmission admission{context, 9, 2};
        auto activation = TransitionActivation(Cinematic::CameraTransitionTier::DualViewCrossFade);
        REQUIRE(fixture.controller
                    .StartCameraCutPreview(std::move(activation), fixture.Settings(), context.view,
                                           Runtime::CameraTransitionSupport::SingleViewBlend, admission)
                    .HasValue());
        REQUIRE(fixture.controller.SeekCameraCutPreview(6).HasValue());
        EditorViewportSceneState presentation;
        presentation.Replace(fixture.controller.ViewportScene());
        const auto retained = presentation.View();
        REQUIRE(retained.cameraCrossFade);
        CHECK(retained.cameraCrossFade->source.position.x == 10.0F);
        CHECK(retained.camera.position.x == 20.0F);
        CHECK(retained.cameraCrossFade->destinationWeight == 0.5F);
        CHECK(retained.cameraCrossFade->admissionGeneration == admission.generation);
        fixture.controller.StopCameraCutPreview();
        CHECK_FALSE(fixture.controller.ViewportScene().View().cameraCrossFade);
        CHECK(retained.cameraCrossFade->source.position.x == 10.0F);
    }

    TEST_CASE("Prepared viewport capacity changes republish the active camera preview without advancing its clock",
              "[unit][cinematic][camera][editor][cross-fade]") {
        const auto policy = GENERATE(Cinematic::CameraTransitionFallback::Fail, Cinematic::CameraTransitionFallback::LinearThenHardCut);
        PreviewFixture fixture;
        const auto scene = *fixture.scene.ActiveScene();
        const Runtime::CameraViewContext context{{1, 1}, scene.RuntimeId(), Runtime::CameraViewDomain::EditorPreview};
        const Runtime::CameraCrossFadeAdmission admission{context, 9, 2};
        auto activation =
            TransitionActivation(Cinematic::CameraTransitionTier::DualViewCrossFade, Cinematic::CameraBlendCurve::SmoothStep, policy, 3);
        REQUIRE(fixture.controller
                    .StartCameraCutPreview(std::move(activation), fixture.Settings(), context.view,
                                           Runtime::CameraTransitionSupport::SingleViewBlend, admission)
                    .HasValue());
        REQUIRE(fixture.controller.SeekCameraCutPreview(6).HasValue());
        const auto first = fixture.controller.ViewportScene();
        REQUIRE(first.cameraCrossFade);
        CHECK(first.cameraCrossFade->destinationWeight == Catch::Approx(7.0F / 27.0F));

        EditorViewportSceneState presentation;
        presentation.SetCameraCrossFadeCapacity({9, 1});
        const auto synchronized = fixture.controller.SynchronizeRuntimeScenePreview(presentation.CameraCrossFadeCapacity());
        if (policy == Cinematic::CameraTransitionFallback::Fail) {
            REQUIRE(synchronized.HasError());
            CHECK(synchronized.ErrorValue().code.Value() == Runtime::CameraErrors::TransitionBudgetExceeded.code.Value());
            presentation.SetCameraPreviewError(synchronized.ErrorValue());
            const auto failure = presentation.TakeCameraPreviewError();
            REQUIRE(failure);
            CHECK(failure->code.Value() == Runtime::CameraErrors::TransitionBudgetExceeded.code.Value());
            CHECK_FALSE(presentation.TakeCameraPreviewError());
            REQUIRE(fixture.controller.ViewportScene().cameraCrossFade);
            CHECK(fixture.controller.ViewportScene().cameraCrossFade->destinationWeight == first.cameraCrossFade->destinationWeight);
            return;
        }
        REQUIRE(synchronized.HasValue());
        presentation.Replace(fixture.controller.ViewportScene());
        CHECK_FALSE(presentation.View().cameraCrossFade);
        CHECK(presentation.View().camera.position.x == Catch::Approx(40.0F / 3.0F));

        presentation.SetCameraCrossFadeCapacity({10, 2});
        REQUIRE(fixture.controller.SynchronizeRuntimeScenePreview(presentation.CameraCrossFadeCapacity()).HasValue());
        presentation.Replace(fixture.controller.ViewportScene());
        const auto recovered = presentation.View();
        REQUIRE(recovered.cameraCrossFade);
        CHECK(recovered.cameraCrossFade->admissionGeneration == 10);
        CHECK(recovered.cameraCrossFade->destinationWeight == first.cameraCrossFade->destinationWeight);
    }

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
