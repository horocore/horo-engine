#include "Horo/Cinematic/CameraCutRuntime.h"
#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"
#include "Horo/Runtime/Camera/CameraErrors.h"
#include "support/AllocationProbe.h"
#include "support/CameraCutTestSupport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Cinematic {
    namespace {
        using namespace Runtime;

        using namespace Tests::CameraCuts;

        struct Fixture final {
            std::unique_ptr<RuntimeScene> scene{Take(RuntimeScene::Create(SceneDefinition(), SceneRuntimeId{1}))};
            CinematicRuntimeService runtime{Take(CinematicRuntimeService::Create({{1, 1}}))};
            CameraService camera{Take(CameraService::Create({{1, 1}, scene->View().RuntimeId(), CameraViewDomain::Runtime}))};
            std::array<CameraCutBinding, 2> bindings{CameraCutBinding{{1, 1}, {1}}, CameraCutBinding{{2, 1}, {2}}};
            std::array<SequenceFrameCameraCutRequest, 64> crossings{};
            std::optional<CinematicCameraPlayback> playback;
            std::uint64_t frame{};

            CameraCutActivation Settings(const CameraCutEndPolicy policy = CameraCutEndPolicy::HoldLastUntilPlayerEnd) const {
                return {bindings, 8, policy, 1, {1, 1}};
            }

            void Start(const SequenceLoopMode loop = SequenceLoopMode::Once,
                       const CameraCutEndPolicy policy = CameraCutEndPolicy::HoldLastUntilPlayerEnd) {
                playback.emplace(
                    Take(CinematicCameraPlayback::Activate(runtime, camera, Activation(loop), Settings(policy), scene->View())));
                REQUIRE(runtime.Play(playback->Player()).HasValue());
                REQUIRE(playback->Publish(scene->View()).HasValue());
            }

            void Advance(const SequenceTime delta) {
                REQUIRE(playback->Evaluate(delta, scene->View(), {{}, {}, crossings}, {}).HasValue());
            }

            void Seek(const SequenceTime position) {
                REQUIRE(runtime.Seek(playback->Player(), position).HasValue());
                REQUIRE(playback->Publish(scene->View()).HasValue());
            }

            CameraSelectionSnapshot Commit() {
                const auto base = Take(ResolveSceneCamera(scene->View(), *scene->View().Find({3})));
                return Take(camera.Commit(++frame, base));
            }
        };
    }  // namespace

    TEST_CASE("Camera-only admission ignores blend windows while mixed scalar plans preserve restore validation",
              "[unit][cinematic][camera]") {
        Fixture fixture;
        const auto activation = Activation(SequenceLoopMode::Loop);
        const auto rejected = fixture.runtime.Activate(activation);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == SequencePlaybackRuntimeErrors::RestoreInvalid.code.Value());
        auto malformed = activation;
        malformed.blend.blendIn.mode = SequenceBlendMode::Count;
        const auto malformedRejected =
            CinematicCameraPlayback::Activate(fixture.runtime, fixture.camera, malformed, fixture.Settings(), fixture.scene->View());
        REQUIRE(malformedRejected.HasError());
        CHECK(malformedRejected.ErrorValue().code.Value() == SequencePlaybackRuntimeErrors::ActivationInvalid.code.Value());
        auto restoreRequired = activation;
        restoreRequired.blend.restorePolicy = SequenceRestorePolicy::RestorePrePlayback;
        const auto restoreRejected =
            CinematicCameraPlayback::Activate(fixture.runtime, fixture.camera, restoreRequired, fixture.Settings(), fixture.scene->View());
        REQUIRE(restoreRejected.HasError());
        CHECK(restoreRejected.ErrorValue().code.Value() == SequencePlaybackRuntimeErrors::RestoreInvalid.code.Value());
        float scalar = 1.0F;
        const std::array tracks{
            SequenceFrameTrackDescriptor{{3, 1}, SequenceApplyStage::Property, &scalar, [](const void *context, SequenceTime) {
            return Result<float>::Success(*static_cast<const float *>(context));
        }, [](void *context, const float value) noexcept {
            *static_cast<float *>(context) = value;
        }}};
        auto mixed = activation;
        mixed.plan = Take(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Loop, 4, tracks, {}, activation.plan.CameraKeys()));
        const auto originalBlend = mixed.blend;
        const auto mixedRejected =
            CinematicCameraPlayback::Activate(fixture.runtime, fixture.camera, mixed, fixture.Settings(), fixture.scene->View());
        REQUIRE(mixedRejected.HasError());
        CHECK(mixedRejected.ErrorValue().code.Value() == SequencePlaybackRuntimeErrors::RestoreInvalid.code.Value());
        CHECK(mixed.blend == originalBlend);
        CHECK(fixture.runtime.ServiceSnapshot().usage.activePlayers == 0);
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
        fixture.Start(SequenceLoopMode::Loop);
        fixture.Advance(2);
        CHECK(fixture.Commit().selected.values.position.x == 10.0F);
        fixture.Advance(3);
        CHECK(fixture.Commit().selected.values.position.x == 20.0F);
    }

    TEST_CASE("Hard cuts select one exact camera at the frame boundary and ignore blend windows", "[unit][cinematic][camera]") {
        Fixture fixture;
        fixture.Start();
        CHECK(fixture.Commit().selected.values.position.x == 30.0F);
        fixture.Advance(1);
        CHECK(fixture.Commit().selected.values.position.x == 30.0F);
        fixture.Advance(1);
        const auto cut = fixture.Commit();
        CHECK(cut.selected.values.position.x == 10.0F);
        CHECK(cut.discontinuity);
        const auto allocations = Tests::AllocationProbe::Count();
        fixture.Advance(3);
        REQUIRE(fixture.playback->Publish(fixture.scene->View()).HasValue());
        CHECK(Tests::AllocationProbe::Count() == allocations);
        CHECK(fixture.Commit().selected.values.position.x == 20.0F);
        fixture.Advance(4);
        CHECK(fixture.Commit().selected.values.position.x == 20.0F);
        fixture.Advance(1);
        CHECK(fixture.Commit().selected.values.position.x == 30.0F);
    }

    TEST_CASE("Camera sampling handles seek reverse and multiple crossed loops using final position", "[unit][cinematic][camera]") {
        Fixture fixture;
        fixture.Start(SequenceLoopMode::Loop);
        fixture.Seek(7);
        CHECK(fixture.Commit().selected.values.position.x == 20.0F);
        REQUIRE(fixture.runtime.SetPlaybackSpeed(fixture.playback->Player(), {-1, 1}).HasValue());
        fixture.Advance(4);
        CHECK(fixture.Commit().selected.values.position.x == 10.0F);
        fixture.Seek(1);
        CHECK(fixture.Commit().selected.values.position.x == 30.0F);
        REQUIRE(fixture.runtime.SetPlaybackSpeed(fixture.playback->Player(), {1, 1}).HasValue());
        fixture.Advance(26);
        CHECK(fixture.Commit().selected.values.position.x == 20.0F);
    }

    TEST_CASE("Declared camera end policy hands off to the current base without saving an old camera", "[unit][cinematic][camera]") {
        Fixture fixture;
        fixture.Start(SequenceLoopMode::Loop, CameraCutEndPolicy::ReleaseAtTrackEnd);
        fixture.Advance(7);
        CHECK(fixture.Commit().overrideOwner.has_value());
        fixture.Advance(1);
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
        auto base = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({3})));
        base.values.position.x = 100.0F;
        base.values.target.x = 100.0F;
        REQUIRE(fixture.playback->Cancel().HasValue());
        CHECK(Take(fixture.camera.Commit(++fixture.frame, base)).selected.values.position.x == 100.0F);
    }

    TEST_CASE("Missing camera admission and target replacement never leave partial authority", "[unit][cinematic][camera]") {
        Fixture fixture;
        fixture.bindings[1].object = {99};
        CHECK(CinematicCameraPlayback::Activate(fixture.runtime, fixture.camera, Activation(), fixture.Settings(), fixture.scene->View())
                  .HasError());
        CHECK(fixture.runtime.ActivePlayerCount() == 0);
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
        fixture.bindings[1].object = {2};
        fixture.Start();
        fixture.Advance(3);
        SceneCommandBuffer commands;
        commands.Destroy(*fixture.scene->View().Find({2}));
        REQUIRE(fixture.scene->Commit(commands).HasValue());
        CHECK(fixture.playback->Publish(fixture.scene->View()).HasError());
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
        CHECK(fixture.runtime.Snapshot(fixture.playback->Player()).Value().state == SequencePlaybackState::Stopped);
    }

    TEST_CASE("Camera leases arbitrate deterministically and committed frames ignore late proposals", "[unit][cinematic][camera]") {
        Fixture fixture;
        const auto context = fixture.camera.Context();
        const auto first = Take(fixture.camera.Acquire({context, {2, 1}, 4}));
        const auto second = Take(fixture.camera.Acquire({context, {1, 1}, 4}));
        const auto left = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({1})));
        const auto right = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({2})));
        REQUIRE(fixture.camera.Submit(first, left).HasValue());
        REQUIRE(fixture.camera.Submit(second, right).HasValue());
        const auto committed = Take(fixture.camera.Commit(1, std::nullopt));
        CHECK(committed.selected.camera == right.camera);
        REQUIRE(fixture.camera.Release(second).HasValue());
        CHECK(Take(fixture.camera.Commit(1, std::nullopt)).selected.camera == right.camera);
        CHECK(Take(fixture.camera.Commit(2, std::nullopt)).selected.camera == left.camera);
        CHECK(fixture.camera.Submit(second, right).HasError());
        fixture.camera.Shutdown();
        CHECK(fixture.camera.Commit(3, right).HasError());
        CHECK(committed.selected.camera == right.camera);
    }

    TEST_CASE("Camera contexts reject foreign scene ownership and suspend instead of inventing a camera", "[unit][cinematic][camera]") {
        Fixture fixture;
        CHECK(fixture.camera.Commit(1, std::nullopt).ErrorValue().code.Value() == CameraErrors::CameraUnavailable.code.Value());
        auto invalid = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({1})));
        invalid.camera.runtime = {99};
        CHECK(fixture.camera.Commit(1, invalid).HasError());
        auto context = fixture.camera.Context();
        context.view.generation++;
        CHECK(fixture.camera.Acquire({context, {1, 1}, 0}).HasError());
        fixture.Start();
        REQUIRE(fixture.runtime.BeginShutdown().HasValue());
        REQUIRE(fixture.playback->Publish(fixture.scene->View()).HasValue());
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
    }

    TEST_CASE("Camera resolution copies hierarchy pose and rejects disabled targets", "[unit][cinematic][camera]") {
        SceneDefinitionBuilder builder{SceneDefinitionId{9}, SceneDefinitionRevision{1}};
        RuntimeEntityDefinition parent;
        parent.object = {1};
        parent.localTransform.translation = {5.0F, 0.0F, 0.0F};
        parent.localTransform.rotation = Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, Math::Pi * 0.5F);
        builder.Add(parent);
        RuntimeEntityDefinition camera;
        camera.object = {2};
        camera.parent = SceneObjectId{1};
        camera.localTransform.translation = {2.0F, 0.0F, 0.0F};
        camera.components.camera = CameraComponent{};
        camera.components.camera->verticalFieldOfViewRadians = 0.8F;
        camera.components.camera->nearPlane = 0.2F;
        camera.components.camera->farPlane = 80.0F;
        builder.Add(camera);
        camera.object = {3};
        camera.components.camera->enabled = false;
        builder.Add(camera);
        const auto scene = Take(RuntimeScene::Create(Take(std::move(builder).Build()), {9}));
        const auto copied = Take(ResolveSceneCamera(scene->View(), *scene->View().Find({2})));
        CHECK(copied.values.position.x == Catch::Approx(5.0F));
        CHECK(copied.values.position.z == Catch::Approx(-2.0F));
        CHECK(copied.values.target.x == Catch::Approx(4.0F));
        CHECK(copied.values.target.z == Catch::Approx(-2.0F));
        CHECK(copied.values.up.y == Catch::Approx(1.0F));
        CHECK(copied.values.projection.verticalFovRadians == 0.8F);
        CHECK(copied.values.projection.nearPlane == 0.2F);
        CHECK(copied.values.projection.farPlane == 80.0F);
        CHECK(ResolveSceneCamera(scene->View(), *scene->View().Find({3})).HasError());
        SceneCommandBuffer commands;
        commands.Destroy(copied.camera);
        REQUIRE(scene->Commit(commands).HasValue());
        CHECK(copied.values.position.x == Catch::Approx(5.0F));
        CHECK(ResolveSceneCamera(scene->View(), copied.camera).HasError());
    }

    TEST_CASE("Scene replacement releases only the adapter lease and bounded claims fail atomically", "[unit][cinematic][camera]") {
        Fixture fixture;
        fixture.Start();
        fixture.Advance(3);
        const auto replacement = Take(RuntimeScene::Create(SceneDefinition(), {2}));
        CHECK(fixture.playback->Publish(replacement->View()).HasError());
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
        for (std::size_t index = 0; index < CameraService::MaximumOverrides; ++index)
            REQUIRE(fixture.camera.Acquire({fixture.camera.Context(), {index + 10, 1}, 1}).HasValue());
        CHECK(fixture.camera.Acquire({fixture.camera.Context(), {999, 1}, 1}).ErrorValue().code.Value() ==
              CameraErrors::CapacityExceeded.code.Value());
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
    }

    TEST_CASE("Committed crossed cuts mark discontinuity even when a loop returns to the same camera", "[unit][cinematic][camera]") {
        Fixture fixture;
        fixture.Start(SequenceLoopMode::Loop);
        fixture.Seek(3);
        const auto first = fixture.Commit();
        const auto failed = fixture.playback->Evaluate(10, fixture.scene->View(), {}, {});
        REQUIRE(failed.HasError());
        REQUIRE(fixture.playback->Publish(fixture.scene->View()).HasValue());
        CHECK(fixture.Commit().selectionEpoch == first.selectionEpoch);
        fixture.Advance(10);
        const auto crossed = fixture.Commit();
        CHECK(crossed.selected.camera == first.selected.camera);
        CHECK(crossed.selectionEpoch == first.selectionEpoch + 1);
        CHECK(crossed.discontinuity);
    }

    TEST_CASE("Same-camera seek marks one committed discontinuity and repeated publication remains stable", "[unit][cinematic][camera]") {
        Fixture fixture;
        fixture.Start();
        fixture.Seek(3);
        const auto first = fixture.Commit();
        fixture.Seek(4);
        const auto seeked = fixture.Commit();
        CHECK(seeked.selected.camera == first.selected.camera);
        CHECK(seeked.selectionEpoch == first.selectionEpoch + 1);
        CHECK(seeked.discontinuity);
        REQUIRE(fixture.playback->Publish(fixture.scene->View()).HasValue());
        const auto repeated = fixture.Commit();
        CHECK(repeated.selectionEpoch == seeked.selectionEpoch);
        CHECK_FALSE(repeated.discontinuity);
    }
}  // namespace Horo::Cinematic
