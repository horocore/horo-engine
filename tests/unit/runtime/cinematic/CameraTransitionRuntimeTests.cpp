#include "Horo/Cinematic/CameraCutRuntime.h"
#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"
#include "Horo/Cinematic/TransformTrack.h"
#include "Horo/Runtime/Camera/CameraErrors.h"
#include "support/AllocationProbe.h"
#include "support/CameraCutRuntimeTestFixture.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <limits>
#include <vector>

namespace Horo::Cinematic {
    namespace {
        using namespace Runtime;
        using namespace Tests::CameraCuts;
        using Fixture = Tests::CameraCuts::RuntimeCameraFixture;

        /** @brief Owns sampled transform channels and the exact storage borrowed by their curve views. */
        class CameraTransformCurves final {
        public:
            CameraTransformCurves() {
                const std::array<float, 10> components{10, 0, 0, 0, 0, 0, 1, 1, 1, 1};
                curves_.reserve(storage_.size());
                for (std::size_t index = 0; index < storage_.size(); ++index) {
                    storage_[index] = {ScalarCurveKey{0, components[index]}, ScalarCurveKey{10, components[index]}};
                    curves_.push_back(Take(ScalarCurveView::Create(storage_[index])));
                }
            }

            CameraTransformCurves(const CameraTransformCurves &) = delete;
            CameraTransformCurves(CameraTransformCurves &&) = delete;
            CameraTransformCurves &operator=(const CameraTransformCurves &) = delete;
            CameraTransformCurves &operator=(CameraTransformCurves &&) = delete;

            [[nodiscard]] TransformCurveSet Channels() const {
                return {{curves_[0], curves_[1], curves_[2]},
                        {curves_[3], curves_[4], curves_[5], curves_[6]},
                        {curves_[7], curves_[8], curves_[9]}};
            }

        private:
            std::array<std::array<ScalarCurveKey, 2>, 10> storage_{};
            std::vector<ScalarCurveView> curves_;
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
            CHECK(Take(owners[index].Commit(1, fixture.scene->View(), std::nullopt)).selected.values.position.x ==
                  static_cast<float>((index + 1) * 10));
        }
        const auto base = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({3})));
        CHECK(owners[0].Submit(leases[1], base).HasError());
        REQUIRE(owners[0].Release(leases[0]).HasValue());
        CHECK_FALSE(Take(owners[0].Commit(2, fixture.scene->View(), base)).overrideOwner.has_value());
        CHECK(Take(owners[1].Commit(2, fixture.scene->View(), base)).overrideOwner.has_value());
        CHECK(Take(owners[2].Commit(2, fixture.scene->View(), base)).overrideOwner.has_value());
    }

    TEST_CASE("Production blend publication and frame commit allocate no storage", "[unit][cinematic][camera][allocation]") {
        Fixture fixture;
        fixture.StartTransition();
        REQUIRE(fixture.runtime.Seek(fixture.playback->Player(), 6).HasValue());
        const auto base = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({3})));
        const auto before = Tests::AllocationProbe::Count();
        const auto published = fixture.playback->Publish(fixture.scene->View());
        const auto committed = fixture.camera.Commit(1, fixture.scene->View(), base);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(published.HasValue());
        REQUIRE(committed.HasValue());
        CHECK(after == before);
        CHECK(committed.Value().selected.values.position.x == 15.0F);
    }

    TEST_CASE("Smooth camera blends use the authored bounded curve and reverse seeks reconstruct their source",
              "[unit][cinematic][camera]") {
        const SequenceTime duration = GENERATE(3, 4);
        const float expected = duration == 3 ? 12.5925926F : 11.5625F;
        Fixture fixture;
        auto activation = TransitionActivation(CameraTransitionTier::SingleViewBlend, CameraBlendCurve::SmoothStep);
        auto keys = std::vector<SequenceFrameCameraCutKey>(activation.plan.CameraKeys().begin(), activation.plan.CameraKeys().end());
        keys[1].transitionDuration = duration;
        activation.plan = Take(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, {}, keys));
        auto settings = fixture.Settings();
        settings.trackEnd = 10;
        fixture.playback.emplace(Take(
            CinematicCameraPlayback::Activate(fixture.runtime, fixture.camera, std::move(activation), settings, fixture.scene->View())));
        REQUIRE(fixture.runtime.Play(fixture.playback->Player()).HasValue());
        fixture.Seek(6);
        CHECK(fixture.Commit().selected.values.position.x == Catch::Approx(expected));
        fixture.Seek(9);
        CHECK(fixture.Commit().selected.values.position.x == 20.0F);
        fixture.Seek(6);
        CHECK(fixture.Commit().selected.values.position.x == Catch::Approx(expected));
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
        const auto selected = fixture.Commit();
        CHECK(selected.selected.values.position.x == 20.0F);
        CHECK(selected.selected.transitionResolution == CameraTransitionResolution::HardCutFallback);
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
        CHECK(Take(baseline.Commit(1, fixture.scene->View(), std::nullopt)).selected.values.position.x == 20.0F);
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

        const CameraTransformCurves channelStorage;
        const auto channels = channelStorage.Channels();
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

    TEST_CASE("Unrepresentable intermediate camera views preserve typed failure and authored fallback", "[unit][cinematic][camera]") {
        const auto fallback = GENERATE(CameraTransitionFallback::Fail, CameraTransitionFallback::HardCut);
        Fixture fixture;
        Math::Transform source;
        source.translation = {100'000'000.0F, 0.0F, 0.0F};
        auto destination = source;
        destination.rotation = Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, Math::Pi);
        SceneCommandBuffer commands;
        commands.SetLocalTransform(*fixture.scene->View().Find({1}), source);
        commands.SetLocalTransform(*fixture.scene->View().Find({2}), destination);
        REQUIRE(fixture.scene->Commit(commands).HasValue());
        fixture.StartTransition(TransitionActivation(CameraTransitionTier::SingleViewBlend, CameraBlendCurve::Linear, fallback));
        fixture.Seek(5);
        fixture.Advance(0);
        const auto before = fixture.Commit();
        CHECK(before.selected.transitionResolution == CameraTransitionResolution::Requested);
        const auto published = fixture.playback->Evaluate(1, fixture.scene->View(), {{}, {}, fixture.crossings}, {});
        if (fallback == CameraTransitionFallback::Fail) {
            REQUIRE(published.HasError());
            CHECK(published.ErrorValue().code.Value() == CameraErrors::IncompatibleTransition.code.Value());
            CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
        } else {
            REQUIRE(published.HasValue());
            const auto selected = fixture.Commit();
            CHECK(selected.overrideOwner.has_value());
            CHECK(selected.selected.camera == *fixture.scene->View().Find({2}));
            CHECK(selected.selected.values.target.z > 0.9F);
            CHECK(selected.selected.transitionResolution == CameraTransitionResolution::HardCutFallback);
            CHECK(selected.selected.discontinuityRevision == before.selected.discontinuityRevision);
            CHECK(selected.selectionEpoch == before.selectionEpoch + 1);
            CHECK(selected.discontinuity);
            REQUIRE(fixture.playback->Publish(fixture.scene->View()).HasValue());
            const auto repeated = fixture.Commit();
            CHECK(repeated.selectionEpoch == selected.selectionEpoch);
            CHECK_FALSE(repeated.discontinuity);
        }
    }

    TEST_CASE("Camera extraction rejects a stale scene epoch and resumes from republished values", "[unit][cinematic][camera][rebase]") {
        Fixture fixture;
        fixture.StartTransition();
        fixture.Seek(6);
        const auto before = fixture.Commit();
        SceneCommandBuffer commands;
        for (std::uint64_t id = 1; id <= 3; ++id)
            commands.SetLocalTransform(*fixture.scene->View().Find({id}), {{static_cast<float>(id * 10) - 100.0F, 0.0F, 0.0F}});
        REQUIRE(fixture.scene->Commit(commands).HasValue());
        const auto scene = fixture.scene->View();
        const auto base = Take(ResolveSceneCamera(scene, *scene.Find({3})));
        const auto staleFrame = fixture.camera.Commit(before.renderedFrame, scene, base);
        REQUIRE(staleFrame.HasError());
        CHECK(staleFrame.ErrorValue().code.Value() == CameraErrors::StaleFrame.code.Value());
        const auto staleProposal = fixture.camera.Commit(before.renderedFrame + 1, scene, base);
        REQUIRE(staleProposal.HasError());
        CHECK(staleProposal.ErrorValue().code.Value() == CameraErrors::StaleFrame.code.Value());
        REQUIRE(fixture.playback->Publish(scene).HasValue());
        CHECK(fixture.camera.Commit(before.renderedFrame, scene, base).HasError());
        const auto after = fixture.Commit();
        CHECK(after.selected.values.position.x == -85.0F);
        CHECK(after.selected.sceneRevision == scene.StructuralRevision());
        CHECK(before.selected.sceneRevision != after.selected.sceneRevision);
        CHECK(before.selected.values.position.x == 15.0F);
    }

    TEST_CASE("The camera owner rejects interpolation between different scene epochs without replacement", "[unit][cinematic][camera]") {
        Fixture fixture;
        const auto old = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({1})));
        SceneCommandBuffer commands;
        commands.SetLocalTransform(*fixture.scene->View().Find({2}), {{25.0F, 0.0F, 0.0F}});
        REQUIRE(fixture.scene->Commit(commands).HasValue());
        const auto current = Take(ResolveSceneCamera(fixture.scene->View(), *fixture.scene->View().Find({2})));
        const auto lease = Take(fixture.camera.Acquire({fixture.camera.Context(), {9, 1}, 1}));
        const auto rejected = fixture.camera.SubmitBlend(lease, old, current, 0.5F);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == CameraErrors::StaleFrame.code.Value());
        CHECK_FALSE(fixture.Commit().overrideOwner.has_value());
    }

}  // namespace Horo::Cinematic
