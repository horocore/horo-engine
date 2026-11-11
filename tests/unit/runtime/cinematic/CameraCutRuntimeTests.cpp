#include "Horo/Cinematic/CameraCutRuntime.h"
#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"
#include "Horo/Cinematic/TransformTrack.h"
#include "Horo/Runtime/Camera/CameraErrors.h"
#include "support/AllocationProbe.h"
#include "support/CameraCutTestSupport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <vector>

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

            void StartTransition(SequencePlaybackActivation activation = TransitionActivation()) {
                playback.emplace(
                    Take(CinematicCameraPlayback::Activate(runtime, camera, std::move(activation), Settings(), scene->View())));
                REQUIRE(runtime.Play(playback->Player()).HasValue());
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

    TEST_CASE("Camera blends sample authored time identically after ticks and seeks", "[unit][cinematic][camera]") {
        Fixture stepped;
        Fixture scrubbed;
        stepped.StartTransition();
        scrubbed.StartTransition();
        for (SequenceTime time = 0; time <= 8; ++time) {
            stepped.Advance(time == 0 ? 0 : 1);
            scrubbed.Seek(time);
            const auto tick = stepped.Commit();
            const auto seek = scrubbed.Commit();
            CHECK(tick.selected.camera == seek.selected.camera);
            CHECK(tick.selected.values.position == seek.selected.values.position);
            const float expected = time < 2 ? 30.0F : time <= 5 ? 10.0F : time == 6 ? 15.0F : 20.0F;
            CHECK(tick.selected.values.position.x == Catch::Approx(expected));
        }
    }

    TEST_CASE("Runtime PIE and editor owners isolate equal claims and reject cross domain leases", "[unit][cinematic][camera]") {
        Fixture fixture;
        std::array owners{Take(CameraService::Create({{1, 1}, {1}, CameraViewDomain::Runtime})),
                          Take(CameraService::Create({{2, 1}, {1}, CameraViewDomain::PlayInEditor})),
                          Take(CameraService::Create({{3, 1}, {1}, CameraViewDomain::EditorPreview}))};
        std::array<CameraOverrideLease, 3> leases{};
        for (std::size_t index = 0; index < owners.size(); ++index) {
            leases[index] = Take(owners[index].Acquire({owners[index].Context(), {1, 1}, 1}));
            const auto proposal = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({index + 1})));
            REQUIRE(owners[index].Submit(leases[index], proposal).HasValue());
            CHECK(Take(owners[index].Commit(1, std::nullopt)).selected.values.position.x == static_cast<float>((index + 1) * 10));
        }
        const auto base = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({3})));
        CHECK(owners[0].Submit(leases[1], base).HasError());
        REQUIRE(owners[0].Release(leases[0]).HasValue());
        CHECK_FALSE(Take(owners[0].Commit(2, base)).overrideOwner.has_value());
        CHECK(Take(owners[1].Commit(2, base)).overrideOwner.has_value());
        CHECK(Take(owners[2].Commit(2, base)).overrideOwner.has_value());
    }

    TEST_CASE("Production blend publication and frame commit allocate no storage", "[unit][cinematic][camera][allocation]") {
        Fixture fixture;
        fixture.StartTransition();
        REQUIRE(fixture.runtime.Seek(fixture.playback->Player(), 6).HasValue());
        const auto base = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({3})));
        const auto before = Tests::AllocationProbe::Count();
        const auto published = fixture.playback->Publish(fixture.scene->View());
        const auto committed = fixture.camera.Commit(1, base);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(published.HasValue());
        REQUIRE(committed.HasValue());
        CHECK(after == before);
        CHECK(committed.Value().selected.values.position.x == 15.0F);
    }

    TEST_CASE("Smooth camera blends use the authored bounded curve and reverse seeks reconstruct their source",
              "[unit][cinematic][camera]") {
        Fixture fixture;
        auto activation = TransitionActivation(CameraTransitionTier::SingleViewBlend, CameraBlendCurve::SmoothStep);
        auto keys = std::vector<SequenceFrameCameraCutKey>(activation.plan.CameraKeys().begin(), activation.plan.CameraKeys().end());
        keys[1].transitionDuration = 4;
        activation.plan = Take(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, {}, keys));
        auto settings = fixture.Settings();
        settings.trackEnd = 10;
        fixture.playback.emplace(Take(
            CinematicCameraPlayback::Activate(fixture.runtime, fixture.camera, std::move(activation), settings, fixture.scene->View())));
        REQUIRE(fixture.runtime.Play(fixture.playback->Player()).HasValue());
        fixture.Seek(6);
        CHECK(fixture.Commit().selected.values.position.x == Catch::Approx(11.5625F));
        fixture.Seek(9);
        CHECK(fixture.Commit().selected.values.position.x == 20.0F);
        fixture.Seek(6);
        CHECK(fixture.Commit().selected.values.position.x == Catch::Approx(11.5625F));
    }

    TEST_CASE("Unavailable dual view transitions require an authored hard cut before admitting player state", "[unit][cinematic][camera]") {
        Fixture fixture;
        const auto rejected = CinematicCameraPlayback::Activate(fixture.runtime, fixture.camera,
                                                                TransitionActivation(CameraTransitionTier::DualViewCrossFade),
                                                                fixture.Settings(), fixture.scene->View());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == CameraErrors::UnsupportedTransition.code.Value());
        CHECK(fixture.runtime.ServiceSnapshot().usage.activePlayers == 0);
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
        fixture.StartTransition(
            TransitionActivation(CameraTransitionTier::DualViewCrossFade, CameraBlendCurve::Linear, CameraTransitionFallback::HardCut));
        fixture.Seek(5);
        CHECK(fixture.Commit().selected.values.position.x == 20.0F);
    }

    TEST_CASE("The camera owner interpolates rotation and lens and rejects incompatible projection atomically",
              "[unit][cinematic][camera]") {
        Fixture fixture;
        const auto lease = Take(fixture.camera.Acquire({fixture.camera.Context(), {8, 1}, 1}));
        const auto from = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({1})));
        auto to = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({2})));
        to.values.target = to.values.position + Math::Vec3{0.0F, 0.0F, 1.0F};
        to.values.projection.verticalFovRadians = 1.4F;
        const auto allocations = Tests::AllocationProbe::Count();
        const auto submitted = fixture.camera.SubmitBlend(lease, from, to, 0.5F);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(submitted.HasValue());
        CHECK(after == allocations);
        const auto blended = fixture.Commit();
        CHECK(blended.selected.values.position.x == 15.0F);
        CHECK(Math::Length(blended.selected.values.target - blended.selected.values.position) == Catch::Approx(1.0F));
        CHECK(std::abs((blended.selected.values.target - blended.selected.values.position).x) == Catch::Approx(1.0F));
        CHECK(blended.selected.values.up.y == Catch::Approx(1.0F));
        CHECK(blended.selected.values.projection.verticalFovRadians ==
              Catch::Approx((from.values.projection.verticalFovRadians + 1.4F) / 2));
        to.values.projection.kind = Render::RenderProjectionKind::Orthographic;
        const auto rejected = fixture.camera.SubmitBlend(lease, from, to, 0.5F);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == CameraErrors::IncompatibleTransition.code.Value());
        CHECK(fixture.Commit().selected.values.position == blended.selected.values.position);
        CHECK(fixture.camera.SubmitBlend(lease, from, to, std::numeric_limits<float>::quiet_NaN()).HasError());
        auto orthographic = from;
        orthographic.values.projection.kind = Render::RenderProjectionKind::Orthographic;
        orthographic.values.projection.orthographicHeight = 2.0F;
        to.values.projection.orthographicHeight = 10.0F;
        to.values.projection.nearPlane = 0.5F;
        to.values.projection.farPlane = 200.0F;
        REQUIRE(fixture.camera.SubmitBlend(lease, orthographic, to, 0.25F).HasValue());
        const auto lens = fixture.Commit().selected.values.projection;
        CHECK(lens.kind == Render::RenderProjectionKind::Orthographic);
        CHECK(lens.orthographicHeight == 4.0F);
        CHECK(lens.nearPlane == Catch::Approx(0.2F));
        CHECK(lens.farPlane == 125.0F);
    }

    TEST_CASE("Host capability admission rejects a required blend and permits only authored fallback", "[unit][cinematic][camera]") {
        Fixture fixture;
        auto baseline = Take(CameraService::Create(fixture.camera.Context(), CameraTransitionSupport::HardCutOnly));
        const auto denied =
            CinematicCameraPlayback::Activate(fixture.runtime, baseline, TransitionActivation(), fixture.Settings(), fixture.scene->View());
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == CameraErrors::UnsupportedTransition.code.Value());
        CHECK(fixture.runtime.ServiceSnapshot().usage.activePlayers == 0);
        auto playback =
            Take(CinematicCameraPlayback::Activate(fixture.runtime, baseline,
                                                   TransitionActivation(CameraTransitionTier::SingleViewBlend, CameraBlendCurve::SmoothStep,
                                                                        CameraTransitionFallback::HardCut),
                                                   fixture.Settings(), fixture.scene->View()));
        REQUIRE(fixture.runtime.Play(playback.Player()).HasValue());
        REQUIRE(fixture.runtime.Seek(playback.Player(), 6).HasValue());
        REQUIRE(playback.Publish(fixture.scene->View()).HasValue());
        CHECK(Take(baseline.Commit(1, std::nullopt)).selected.values.position.x == 20.0F);
    }

    TEST_CASE("Camera transition plans reject malformed duration and enum values before activation", "[unit][cinematic][camera]") {
        auto key = TransitionActivation().plan.CameraKeys()[1];
        const auto reject = [](const SequenceFrameCameraCutKey &invalid) {
            const std::array keys{invalid};
            CHECK(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, {}, keys).HasError());
        };
        key.transitionDuration = -1;
        reject(key);
        key.transitionDuration = 6;
        reject(key);
        key.transitionDuration = 0;
        reject(key);
        key.transitionDuration = 2;
        key.time = std::numeric_limits<SequenceTime>::min();
        reject(key);
        key.time = 5;
        key.transitionCurve = CameraBlendCurve::Count;
        reject(key);
        key.transitionCurve = CameraBlendCurve::Linear;
        key.transitionTier = CameraTransitionTier::Count;
        reject(key);
        key.transitionTier = CameraTransitionTier::SingleViewBlend;
        key.fallback = CameraTransitionFallback::Count;
        reject(key);
    }

    TEST_CASE("Camera transition admission rejects missing source and overlapping track end atomically", "[unit][cinematic][camera]") {
        Fixture fixture;
        auto activation = TransitionActivation();
        auto keys = std::vector<SequenceFrameCameraCutKey>(activation.plan.CameraKeys().begin(), activation.plan.CameraKeys().end());
        SECTION("A blend has an earlier authored source") {
            keys.front().transitionTier = CameraTransitionTier::SingleViewBlend;
            keys.front().transitionDuration = 1;
        }
        SECTION("A blend completes by the declared camera track end") {
            keys.back().transitionDuration = 4;
        }
        activation.plan = Take(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, {}, keys));
        const auto rejected = CinematicCameraPlayback::Activate(fixture.runtime, fixture.camera, std::move(activation), fixture.Settings(),
                                                                fixture.scene->View());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == CameraErrors::IncompatibleTransition.code.Value());
        CHECK(fixture.runtime.ServiceSnapshot().usage.activePlayers == 0);
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
    }

    TEST_CASE("Authored hard cut fallback admits unsupported camera transition layouts", "[unit][cinematic][camera]") {
        Fixture fixture;
        auto activation = TransitionActivation();
        auto keys = std::vector<SequenceFrameCameraCutKey>(activation.plan.CameraKeys().begin(), activation.plan.CameraKeys().end());
        SequenceTime position = 6;
        float expected = 20.0F;
        SECTION("Missing source uses the authored hard cut") {
            keys.front().transitionTier = CameraTransitionTier::SingleViewBlend;
            keys.front().transitionDuration = 1;
            keys.front().fallback = CameraTransitionFallback::HardCut;
            position = 3;
            expected = 10.0F;
        }
        SECTION("An overlapping track end uses the authored hard cut") {
            keys.back().transitionDuration = 4;
            keys.back().fallback = CameraTransitionFallback::HardCut;
        }
        activation.plan = Take(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, {}, keys));
        fixture.StartTransition(std::move(activation));
        fixture.Seek(position);
        CHECK(fixture.Commit().selected.values.position.x == expected);
    }

    TEST_CASE("Incompatible authored lenses require explicit fallback and blend cancellation uses the current base",
              "[unit][cinematic][camera]") {
        Fixture fixture;
        SceneDefinitionBuilder builder{SceneDefinitionId{1}, SceneDefinitionRevision{100}};
        for (std::uint64_t id = 1; id <= 3; ++id) {
            RuntimeEntityDefinition entity;
            entity.object = {id};
            entity.localTransform.translation.x = static_cast<float>(id * 10);
            entity.components.camera = CameraComponent{};
            if (id == 2)
                entity.components.camera->projection = CameraProjection::Orthographic;
            builder.Add(entity);
        }
        fixture.scene = Take(RuntimeScene::Create(Take(std::move(builder).Build()), {1}));
        const auto denied = CinematicCameraPlayback::Activate(fixture.runtime, fixture.camera, TransitionActivation(), fixture.Settings(),
                                                              fixture.scene->View());
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == CameraErrors::IncompatibleTransition.code.Value());
        CHECK(fixture.runtime.ServiceSnapshot().usage.activePlayers == 0);
        fixture.StartTransition(
            TransitionActivation(CameraTransitionTier::SingleViewBlend, CameraBlendCurve::Linear, CameraTransitionFallback::HardCut));
        fixture.Seek(6);
        const auto cut = fixture.Commit();
        CHECK(cut.selected.values.position.x == 20.0F);
        CHECK(cut.selected.values.projection.kind == Render::RenderProjectionKind::Orthographic);
        SceneCommandBuffer commands;
        commands.SetLocalTransform(*fixture.scene->View().Find({3}), {{40.0F, 0.0F, 0.0F}});
        REQUIRE(fixture.scene->Commit(commands).HasValue());
        REQUIRE(fixture.playback->Cancel().HasValue());
        CHECK(fixture.Commit().selected.values.position.x == 40.0F);
        CHECK(cut.selected.values.position.x == 20.0F);
    }

    TEST_CASE("Camera blends resample root and child cameras after anchor relative transform publication",
              "[unit][cinematic][camera][rebase]") {
        Fixture fixture;
        SceneDefinitionBuilder builder{SceneDefinitionId{1}, SceneDefinitionRevision{100}};
        for (std::uint64_t id = 1; id <= 4; ++id) {
            RuntimeEntityDefinition entity;
            entity.object = {id};
            entity.localTransform.translation.x = static_cast<float>(id * 10);
            if (id != 4)
                entity.components.camera = CameraComponent{};
            if (id == 2)
                entity.parent = SceneObjectId{4};
            builder.Add(entity);
        }
        fixture.scene = Take(RuntimeScene::Create(Take(std::move(builder).Build()), {1}));
        fixture.StartTransition();
        fixture.Seek(6);

        std::array<std::array<ScalarCurveKey, 2>, 10> storage{};
        const std::array<float, 10> components{10, 0, 0, 0, 0, 0, 1, 1, 1, 1};
        std::vector<ScalarCurveView> curves;
        curves.reserve(storage.size());
        for (std::size_t index = 0; index < storage.size(); ++index) {
            storage[index] = {ScalarCurveKey{0, components[index]}, ScalarCurveKey{10, components[index]}};
            curves.push_back(Take(ScalarCurveView::Create(storage[index])));
        }
        const TransformCurveSet channels{{curves[0], curves[1], curves[2]},
                                         {curves[3], curves[4], curves[5], curves[6]},
                                         {curves[7], curves[8], curves[9]}};
        const std::array childTranslation{ScalarCurveKey{0, 20.0F}, ScalarCurveKey{10, 30.0F}};
        auto childChannels = channels;
        childChannels.translation[0] = Take(ScalarCurveView::Create(childTranslation));
        const auto anchor = Math::WorldCoordinate64::FromMillimeters(1'000'000, 0, 0);
        const std::array bindings{TransformBindingSnapshot{{1, 1}, {}}, TransformBindingSnapshot{{4, 1}, {}},
                                  TransformBindingSnapshot{{2, 1}, TransformBindingId{4, 1}}};
        const std::array
            tracks{TransformTrackDescriptor{CurrentTransformTrackVersion, {1, 1}, {1, 1}, {}, anchor, channels},
                   TransformTrackDescriptor{CurrentTransformTrackVersion, {4, 1}, {4, 1}, {}, anchor, channels},
                   TransformTrackDescriptor{CurrentTransformTrackVersion, {2, 1}, {2, 1}, TransformBindingId{4, 1}, {}, childChannels}};
        const auto plan = Take(TransformEvaluationPlan::Create({1, 1}, tracks, bindings));
        std::array<TransformEvaluationValue, 3> sampled{};
        const auto publishEpoch = [&](const std::int64_t origin, const std::uint64_t epoch) {
            REQUIRE(plan.Evaluate(6, {{1, 1}, Math::WorldCoordinate64::FromMillimeters(origin, 0, 0), epoch}, sampled).HasValue());
            SceneCommandBuffer commands;
            for (const auto &value : sampled)
                commands.SetLocalTransform(*fixture.scene->View().Find({value.binding.stableValue}), value.localTransform);
            REQUIRE(fixture.scene->Commit(commands).HasValue());
            REQUIRE(fixture.playback->Publish(fixture.scene->View()).HasValue());
            return fixture.Commit();
        };
        const auto before = publishEpoch(0, 1);
        const auto after = publishEpoch(400'000, 2);
        CHECK(before.selected.values.position.x == 1023.0F);
        CHECK(after.selected.values.position.x == 623.0F);
        CHECK(before.selected.values.position.x == after.selected.values.position.x + 400.0F);
        CHECK(fixture.runtime.Snapshot(fixture.playback->Player()).Value().position == 6);
        CHECK(fixture.scene->View().Get(*fixture.scene->View().Find({2})).Value().localTransform->translation.x == 26.0F);
    }

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
