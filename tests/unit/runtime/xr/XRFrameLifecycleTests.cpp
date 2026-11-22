#include "Horo/XR/XRFrameLifecycle.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <thread>

namespace Horo::XR {
    namespace {
        template <typename T> T Generation(const std::uint64_t value) {
            auto created = T::Create(value);
            REQUIRE(created.HasValue());
            return created.Value();
        }

        XRCapabilitySnapshot Capabilities(const std::uint64_t runtime = 1, const std::uint64_t revision = 0, const bool synthesis = false,
                                          const bool quad = false) {
            XRCapabilityDescriptor descriptor{
                .system = {.runtime = Generation<XRRuntimeGeneration>(runtime), .slot = {.index = 2, .generation = 1}},
                .revision = Generation<XRCapabilityRevision>(revision == 0 ? runtime : revision),
                .limits = {.maximumViews = 4, .maximumSpaces = 8, .maximumActions = 8, .maximumDevices = 4},
            };
            descriptor.states.fill(XRCapabilityState::Unsupported);
            for (const XRCapability feature :
                 {XRCapability::Projection, XRCapability::PrimaryOpaqueStereo, XRCapability::OrientationTracking,
                  XRCapability::PositionTracking, XRCapability::ViewSpace, XRCapability::LocalSpace, XRCapability::BooleanActions,
                  XRCapability::FloatActions, XRCapability::Vector2Actions, XRCapability::PoseActions, XRCapability::PredictedFrames,
                  XRCapability::ExternalColorTargets, XRCapability::SessionLossLifecycle, XRCapability::CanonicalInputProjection})
                descriptor.states[static_cast<std::size_t>(feature)] = XRCapabilityState::Available;
            if (synthesis)
                descriptor.states[static_cast<std::size_t>(XRCapability::SpaceWarp)] = XRCapabilityState::Available;
            if (quad)
                descriptor.states[static_cast<std::size_t>(XRCapability::QuadViews)] = XRCapabilityState::Available;
            auto created = XRCapabilitySnapshot::Create(descriptor);
            REQUIRE(created.HasValue());
            return created.Value();
        }

        class NoopResources final : public IXRSessionResources {
        public:
            Result<void> Prepare(XRSessionPreparation, const XRSessionId &, const XRFeaturePlan &) override {
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation, const XRSessionId &) noexcept override {}
        };

        XRSessionId Activate(XRSessionLifecycle &sessions, const XRCapabilitySnapshot &capabilities,
                             std::span<const XRFeatureRequest> features = {}) {
            const XRFeatureNegotiationRequest projection{.profile = XRFeatureProfile::Projection1_0,
                                                         .requestedLimits = {.maximumViews = 2,
                                                                             .maximumSpaces = 2,
                                                                             .maximumActions = 4,
                                                                             .maximumDevices = 1},
                                                         .features = features};
            const auto plan = NegotiateXRFeatures(capabilities, capabilities.System(), capabilities.Revision(), projection);
            REQUIRE(plan.status == XRFeatureNegotiationStatus::Ok);
            auto result = sessions.Activate(capabilities, capabilities.System(), capabilities.Revision(), *plan.plan);
            REQUIRE(result.HasValue());
            const XRSessionId session = result.Value();
            REQUIRE(sessions.ApplyEvent(session, XRSessionEvent::Started).HasValue());
            return session;
        }

        XRViewConfigurationId Configuration(const XRSessionId &session, const std::uint32_t generation = 1) {
            return {session, {.index = 3, .generation = generation}};
        }

        XRSwapchainImageId Image(const XRSessionId &session, const std::uint32_t target, const std::uint32_t image = 1) {
            return {{session, {.index = target, .generation = 1}}, {.index = image, .generation = 1}};
        }

        constexpr XRFrameLimits StereoLimits{.maximumViews = 2, .maximumImages = 2, .maximumLayers = 1};

        XRRenderPredictionTime Prediction(const std::int64_t nanoseconds) {
            auto created = XRRenderPredictionTime::Create(nanoseconds);
            REQUIRE(created.HasValue());
            return created.Value();
        }

        XRFrameId Wait(XRFrameLifecycle &frames, const XRSessionId &session, const bool shouldRender = true,
                       const std::int64_t time = 10'000) {
            const auto reserved = frames.ReserveWait(session);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            const auto waited = frames.RecordWait(Prediction(time), shouldRender);
            REQUIRE(waited.status == XRFrameStatus::Ok);
            REQUIRE(waited.frame.IsValid());
            REQUIRE(waited.frame == reserved.frame);
            return waited.frame;
        }

        /** @brief Constructs complete stereo evidence against the actual open frame owner. */
        XRViewRenderPlan Projection(const XRFrameLifecycle &frames, const XRFrameId frame, const XRSwapchainImageId color,
                                    const std::uint64_t originValue = 1) {
            const auto session = frame.session;
            const auto origin = Generation<XRWorldOriginRevision>(originValue);
            const auto prediction = frames.Snapshot().predictedDisplayTime;
            const XRCoordinateSpace base{{session, {.index = 1, .generation = 1}}, XRSpaceKind::Local, origin};
            std::array<XRViewDescriptor, 2> views{};
            std::array<XRExternalRenderTargetDescriptor, 2> targets{};
            for (std::uint32_t i = 0; i < views.size(); ++i) {
                const XRCoordinateSpace eye{{session, {.index = 2 + i, .generation = 1}}, XRSpaceKind::View, origin};
                views[i] = {.id = {session, {.index = i, .generation = 1}},
                            .order = i,
                            .role = i == 0 ? XRViewRole::PrimaryLeft : XRViewRole::PrimaryRight,
                            .pose = {session,
                                     eye,
                                     base,
                                     XRPosePurpose::PresentationPrediction,
                                     {.runtimeSample = Generation<XRRuntimeTime>(1), .renderPrediction = prediction},
                                     {.positionMeters = {Math::Vec3{}, XRPoseComponentValidity::Tracked},
                                      .orientation = {Math::Quaternion::Identity(), XRPoseComponentValidity::Tracked},
                                      .confidence = XRTrackingConfidence::High,
                                      .loss = XRTrackingLossState::None}},
                            .projection = {-1, 1, -1, 1},
                            .recommendedExtent = {128, 128},
                            .maximumExtent = {128, 128}};
                targets[i] = {.view = views[i].id,
                              .target = color.target,
                              .image = color,
                              .allocationExtent = {128, 128},
                              .renderRectangle = {0, 0, 128, 128},
                              .arrayLayer = i,
                              .arrayLayerCount = 2};
            }
            const auto revision = Generation<XRViewConfigurationRevision>(1);
            const std::array images{color};
            auto plan = XRViewRenderPlan::Create({{CurrentXRContractVersion, session, frame.configuration, revision,
                                                   XRViewConfigurationType::PrimaryStereo, XREnvironmentBlendMode::Opaque, 2},
                                                  prediction,
                                                  views,
                                                  targets,
                                                  images},
                                                 session, frame.configuration, revision, origin, 2, {});
            INFO((plan.HasError() ? plan.ErrorValue().message : ""));
            REQUIRE(plan.HasValue());
            return std::move(plan).Value();
        }

        /** @brief Opens an actual projection transaction with the ordinary color allocation. */
        void BeginProjection(XRFrameLifecycle &frames, const XRFrameId &frame, const XRSwapchainImageId &color) {
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            REQUIRE(frames.Acquire(frame, color) == XRFrameStatus::Ok);
        }

        constexpr std::array SynthesisRequest{
            XRFeatureRequest{XRCapability::SpaceWarp, XRFeatureRequirementKind::Optional, XRFeatureFallback::BaselineProjection}};

        TEST_CASE("XR synthesis leaves ordinary projection admissible when its requested provider is unsupported",
                  "[unit][xr][reprojection]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const auto session = Activate(sessions, capabilities, SynthesisRequest);
            CHECK(sessions.AcceptedPlan()->Decision(XRCapability::SpaceWarp).state == XRFeatureDecisionState::BaselineProjection);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const auto frame = Wait(frames, session);
            const auto color = Image(session, 1);
            BeginProjection(frames, frame, color);
            const auto plan = Projection(frames, frame, color);
            CHECK(frames.AcquireReprojectionImage(frame, {}) == XRFrameStatus::Unsupported);
            CHECK(frames.ValidateReprojection(frame, plan, {}) == XRReprojectionStatus::ProjectionInvalid);
            REQUIRE(frames.Submit(frame, std::span{&color, 1}) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(frame, color) == XRFrameStatus::Ok);
            CHECK(frames.ValidateReprojection(frame, plan, {}) == XRReprojectionStatus::BaselineProjection);
            REQUIRE(frames.EndProjection(frame, plan) == XRFrameStatus::Ok);
            CHECK(frames.ValidateReprojection(frame, plan, {}) == XRReprojectionStatus::ProjectionInvalid);
        }

        TEST_CASE("XR synthesis requires released inputs and a compatible completed predecessor", "[unit][xr][reprojection]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            // This is a frame-owner contract fixture, without a native provider or GPU.
            const auto capabilities = Capabilities(1, 1, true);
            const auto session = Activate(sessions, capabilities, SynthesisRequest);
            XRFrameLifecycle frames{sessions};
            constexpr XRReprojectionPolicy policy{.viewExtents = {{{64, 48}, {40, 64}}},
                                                  .viewCount = 2,
                                                  .maximumPredictionIntervalNanoseconds = 1000,
                                                  .motion = XRReprojectionMotionConvention::CurrentNdcMinusPreviousNdcXYZ};
            const auto configuration = Configuration(session);
            REQUIRE(frames.BindConfiguration(configuration, capabilities, {2, 3, 1}) == XRFrameStatus::InvalidInput);
            auto invalid = policy;
            invalid.viewCount = 0;
            CHECK(frames.BindConfiguration(configuration, capabilities, {2, 3, 1}, invalid) == XRFrameStatus::InvalidInput);
            invalid.viewCount = 3;
            CHECK(frames.BindConfiguration(configuration, capabilities, {2, 3, 1}, invalid) == XRFrameStatus::InvalidInput);
            invalid = policy;
            invalid.viewExtents[1].height = 0;
            CHECK(frames.BindConfiguration(configuration, capabilities, {2, 3, 1}, invalid) == XRFrameStatus::InvalidInput);
            REQUIRE(frames.BindConfiguration(configuration, capabilities, {2, 3, 1}, policy) == XRFrameStatus::Ok);
            const auto first = Wait(frames, session);
            const auto color = Image(session, 1);
            BeginProjection(frames, first, color);
            const auto prior = Projection(frames, first, color);
            REQUIRE(frames.Submit(first, std::span{&color, 1}) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(first, color) == XRFrameStatus::Ok);
            CHECK(frames.ValidateReprojection(first, prior, {}) == XRReprojectionStatus::HistoryUnavailable);
            REQUIRE(frames.EndProjection(first, prior) == XRFrameStatus::Ok);

            const auto second = Wait(frames, session, true, 10'500);
            const auto currentColor = Image(session, 1, 2);
            BeginProjection(frames, second, currentColor);
            const auto current = Projection(frames, second, currentColor);
            const auto movedOrigin = Projection(frames, second, currentColor, 2);
            constexpr auto usage = Render::RenderTextureUsage::Sampled | Render::RenderTextureUsage::RenderAttachment;
            XRReprojectionImage motion{Image(session, 2),
                                       XRReprojectionImageKind::Motion,
                                       {.extent = {64, 64},
                                        .format = Render::RenderTextureFormat::Rg16Float,
                                        .layerCount = 2,
                                        .usage = usage}};
            CHECK(frames.AcquireReprojectionImage(second, motion) == XRFrameStatus::InvalidInput);
            motion.allocation.format = policy.motionFormat;
            motion.allocation.extent = policy.viewExtents[0];
            CHECK(frames.AcquireReprojectionImage(second, motion) == XRFrameStatus::InvalidInput);
            motion.allocation.extent = policy.viewExtents[1];
            CHECK(frames.AcquireReprojectionImage(second, motion) == XRFrameStatus::InvalidInput);
            motion.allocation.extent = {64, 64};
            motion.allocation.layerCount = 1;
            CHECK(frames.AcquireReprojectionImage(second, motion) == XRFrameStatus::InvalidInput);
            motion.allocation.layerCount = 2;
            REQUIRE(frames.AcquireReprojectionImage(second, motion) == XRFrameStatus::Ok);
            const XRReprojectionImage depth{Image(session, 3),
                                            XRReprojectionImageKind::Depth,
                                            {.extent = {64, 64}, .format = policy.depthFormat, .layerCount = 2, .usage = usage}};
            REQUIRE(frames.AcquireReprojectionImage(second, depth) == XRFrameStatus::Ok);
            const std::array completed{currentColor, motion.image, depth.image};
            REQUIRE(frames.Submit(second, completed) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(second, currentColor) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(second, motion.image) == XRFrameStatus::Ok);

            std::array<XRReprojectionViewInput, 2> views{};
            for (std::uint32_t i = 0; i < views.size(); ++i) {
                const auto [width, height] = policy.viewExtents[i];
                const XRRenderRectangle rectangle{0, 0, width, height};
                views[i] = {current.Views()[i].id, {motion.image, rectangle, i}, {depth.image, rectangle, i}, 0.0F, 1.0F, 0.1F, 100.0F};
            }
            XRReprojectionInputs inputs{prior.PredictedDisplayTime(), first.sequence,
                                        XRReprojectionSpaceDelta{current.Views().front().pose.target, {}, Math::Quaternion::Identity()},
                                        views};
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::ProjectionInvalid);
            REQUIRE(frames.Release(second, depth.image) == XRFrameStatus::Ok);
            inputs.views = std::span{views}.first(1);
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::InputsInvalid);
            inputs.views = views;
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::CoverageUnavailable);
            std::array<XRReprojectionViewCoverage, 2> observed{};
            for (std::size_t eye = 0; eye < views.size(); ++eye) {
                const auto &view = views[eye];
                const auto [width, height] = policy.viewExtents[eye];
                observed[eye] = {view.view, view.motion, view.depth, std::uint64_t{width} * height, 0};
            }
            inputs.coverage.emplace(XRReprojectionCoverage{.session = session,
                                                           .frameSequence = second.sequence,
                                                           .prediction = current.PredictedDisplayTime(),
                                                           .previousFrameSequence = first.sequence,
                                                           .previousPrediction = prior.PredictedDisplayTime(),
                                                           .views = observed});
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::Ready);
            observed.back().unresolvedSamples = 1;
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::CoverageRejected);
            observed.back().unresolvedSamples = 0;
            --observed.back().classifiedSamples;
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::CoverageRejected);
            ++observed.back().classifiedSamples;
            std::swap(observed[0].depth, observed[1].depth);
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::CoverageRejected);
            std::swap(observed[0].depth, observed[1].depth);
            inputs.coverage->frameSequence = first.sequence;
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::CoverageRejected);
            inputs.coverage->frameSequence = second.sequence;
            inputs.coverage->previousPrediction = current.PredictedDisplayTime();
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::CoverageRejected);
            inputs.coverage->previousPrediction = prior.PredictedDisplayTime();
            inputs.coverage->views = std::span{observed}.first(1);
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::CoverageRejected);
            inputs.coverage->views = observed;
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::Ready);
            CHECK(frames.ValidateReprojection(second, movedOrigin, inputs) == XRReprojectionStatus::HistoryUnavailable);
            inputs.applicationDelta.reset();
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::InputsInvalid);
            inputs.applicationDelta = XRReprojectionSpaceDelta{current.Views().front().pose.target, {}, Math::Quaternion::Identity()};
            inputs.applicationDelta->reference.worldOriginRevision = Generation<XRWorldOriginRevision>(3);
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::InputsInvalid);
            inputs.applicationDelta->reference = current.Views().front().pose.target;
            views[1].motion.arrayLayer = 0;
            CHECK(frames.ValidateReprojection(second, current, inputs) == XRReprojectionStatus::InputsInvalid);
            REQUIRE(frames.EndProjection(second, current) == XRFrameStatus::Ok);

            const auto skipped = Wait(frames, session, false, 11'000);
            REQUIRE(frames.Begin(skipped) == XRFrameStatus::Ok);
            REQUIRE(frames.End(skipped, 0) == XRFrameStatus::Ok);
            const auto next = Wait(frames, session, true, 11'500);
            BeginProjection(frames, next, color);
            const auto afterSkip = Projection(frames, next, color);
            REQUIRE(frames.Submit(next, std::span{&color, 1}) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(next, color) == XRFrameStatus::Ok);
            inputs.previousFrameSequence = second.sequence;
            inputs.previousPrediction = current.PredictedDisplayTime();
            CHECK(frames.ValidateReprojection(next, afterSkip, inputs) == XRReprojectionStatus::HistoryUnavailable);
            REQUIRE(frames.EndProjection(next, afterSkip) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame gate rejects off-thread admission, completion and reset without mutating the owner",
                  "[unit][xr][frame][lifecycle]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const auto session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            const auto configuration = Configuration(session);
            REQUIRE(frames.BindConfiguration(configuration, capabilities, StereoLimits) == XRFrameStatus::Ok);
            const auto reservation = frames.ReserveWait(session);
            REQUIRE(reservation.status == XRFrameStatus::Ok);
            const auto predicted = Prediction(10'000);
            std::array<XRFrameStatus, 5> statuses{};
            XRFrameSnapshot foreignSnapshot;
            std::thread other{[&] {
                statuses[0] = frames.BindConfiguration(configuration, capabilities, StereoLimits);
                statuses[1] = frames.ReserveWait(session).status;
                statuses[2] = frames.CancelWait();
                statuses[3] = frames.RecordWait(predicted, false).status;
                statuses[4] = frames.Begin(reservation.frame);
                foreignSnapshot = frames.Snapshot();
                frames.ResetAfterQuiescence();
                frames.Shutdown();
            }};
            other.join();
            for (const auto status : statuses)
                CHECK(status == XRFrameStatus::WrongThread);
            CHECK_FALSE(foreignSnapshot.frame.IsValid());
            CHECK(frames.Snapshot().phase == XRFramePhase::WaitReserved);
            const auto waited = frames.RecordWait(predicted, false);
            REQUIRE(waited.status == XRFrameStatus::Ok);
            CHECK(waited.frame == reservation.frame);
            REQUIRE(frames.Begin(waited.frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(waited.frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame gate keeps an admitted quad view set intact", "[unit][xr][frame][quad]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            XRFrameLifecycle frames{sessions};
            const auto capabilities = Capabilities(1, 0, false, true);
            const auto plan = NegotiateXRFeatures(capabilities, capabilities.System(), capabilities.Revision(),
                                                  {.profile = XRFeatureProfile::FoveatedInsetProjection, .requestedLimits = {4, 2, 4, 1}});
            REQUIRE(plan.plan);
            const auto active = sessions.Activate(capabilities, capabilities.System(), capabilities.Revision(), *plan.plan);
            REQUIRE(active.HasValue());
            const auto session = active.Value();
            REQUIRE(sessions.ApplyEvent(session, XRSessionEvent::Started).HasValue());
            CHECK(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::InvalidInput);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, {4, 4, 1}) == XRFrameStatus::Ok);
            const auto frame = Wait(frames, session);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            CHECK(frames.LocateViews(frame, 2) == XRFrameStatus::InvalidInput);
            CHECK(frames.LocateViews(frame, 3) == XRFrameStatus::InvalidInput);
            CHECK(frames.Snapshot().phase == XRFramePhase::Begun);
            CHECK(frames.Snapshot().locatedViews == 0);
            CHECK(frames.LocateViews(frame, 4) == XRFrameStatus::Ok);
            CHECK(frames.Snapshot().locatedViews == 4);
            CHECK(frames.End(frame, 1) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Abort(frame) == XRFrameStatus::Ok);
            CHECK(frames.End(frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame contract completes a bounded predicted rendering transaction", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const auto allocations = Horo::Tests::AllocationProbe::Count();
            const XRFrameId frame = Wait(frames, session);
            REQUIRE(frame.capabilityRevision == capabilities.Revision());
            REQUIRE(frames.Snapshot().predictedDisplayTime == Prediction(10'000));
            REQUIRE(frames.Snapshot().shouldRender);
            REQUIRE(frames.Snapshot().renderAdmitted);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            const auto first = Image(session, 1);
            const auto second = Image(session, 2);
            REQUIRE(frames.Acquire(frame, first) == XRFrameStatus::Ok);
            REQUIRE(frames.Acquire(frame, second) == XRFrameStatus::Ok);
            const std::array images{first, second};
            REQUIRE(frames.Submit(frame, images) == XRFrameStatus::Ok);
            REQUIRE(frames.ValidateEnd(frame, 1) == XRFrameStatus::ImageNotReleased);
            REQUIRE(frames.ValidateReleasedImage(frame, first) == XRFrameStatus::ImageNotReleased);
            REQUIRE(frames.Release(frame, second) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(frame, first) == XRFrameStatus::Ok);
            REQUIRE(frames.ValidateReleasedImage(frame, first) == XRFrameStatus::Ok);
            REQUIRE(frames.ValidateReleasedImage(frame, Image(session, 9)) == XRFrameStatus::ImageNotAcquired);
            REQUIRE(frames.ValidateEnd(frame, 1) == XRFrameStatus::Ok);
            REQUIRE(frames.ValidateEnd(frame, 1) == XRFrameStatus::Ok);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::ImagesReleased);
            REQUIRE(frames.End(frame, 1) == XRFrameStatus::Ok);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Idle);
            REQUIRE(Horo::Tests::AllocationProbe::Count() == allocations);
            REQUIRE(frames.End(frame, 1) == XRFrameStatus::Duplicate);
        }

        TEST_CASE("XR should-render false still begins and ends with zero layers and no image work", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, {}) == XRFrameStatus::Ok);
            const auto reserved = frames.ReserveWait(session);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            const auto unsupported = frames.RecordWait(Prediction(10'000), true);
            REQUIRE(unsupported.status == XRFrameStatus::Unsupported);
            REQUIRE(unsupported.frame == reserved.frame);
            REQUIRE(frames.Snapshot().shouldRender);
            REQUIRE_FALSE(frames.Snapshot().renderAdmitted);
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Duplicate);
            REQUIRE(frames.Begin(unsupported.frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(unsupported.frame, 0) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session, false);
            REQUIRE_FALSE(frames.Snapshot().shouldRender);
            REQUIRE_FALSE(frames.Snapshot().renderAdmitted);
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Duplicate);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 1) == XRFrameStatus::Unsupported);
            REQUIRE(frames.Acquire(frame, Image(session, 1)) == XRFrameStatus::Unsupported);
            REQUIRE(frames.Submit(frame, {}) == XRFrameStatus::Unsupported);
            REQUIRE(frames.End(frame, 1) == XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR wait reservation rejects native work early and cancellation never reuses a frame", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            REQUIRE(frames.RecordWait(Prediction(10'000), true).status == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.CancelWait() == XRFrameStatus::OutOfOrder);

            const auto reserved = frames.ReserveWait(session);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::WaitReserved);
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Duplicate);
            REQUIRE(frames.Begin(reserved.frame) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.CancelWait() == XRFrameStatus::Ok);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Idle);
            REQUIRE(frames.RecordWait(Prediction(10'000), true).status == XRFrameStatus::OutOfOrder);

            const auto next = frames.ReserveWait(session);
            REQUIRE(next.status == XRFrameStatus::Ok);
            REQUIRE(next.frame.sequence > reserved.frame.sequence);
            const auto waited = frames.RecordWait(Prediction(20'000), false);
            REQUIRE(waited.status == XRFrameStatus::Ok);
            REQUIRE(waited.frame == next.frame);
            REQUIRE(frames.CancelWait() == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Begin(waited.frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(waited.frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame gate rejects skipped and duplicate operations without advancing state", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Acquire(frame, Image(session, 1)) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Waited);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Abort(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
            const XRFrameId next = Wait(frames, session);
            REQUIRE(next.sequence > frame.sequence);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::StaleFrame);
            REQUIRE(frames.Begin(next) == XRFrameStatus::Ok);
            REQUIRE(frames.Abort(next) == XRFrameStatus::Ok);
            REQUIRE(frames.End(next, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame admission rejects invalid unsupported stale and over-capacity inputs", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Unavailable);
            REQUIRE(frames.BindConfiguration({}, capabilities, StereoLimits) == XRFrameStatus::InvalidInput);
            REQUIRE(frames.BindConfiguration(Configuration(session), Capabilities(2, capabilities.Revision().Value()), StereoLimits) ==
                    XRFrameStatus::StaleSession);
            REQUIRE(frames.BindConfiguration(Configuration(session), Capabilities(1, 99), StereoLimits) == XRFrameStatus::StalePlan);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, {.maximumViews = 5}) == XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, {.maximumViews = 17}) ==
                    XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            REQUIRE(frames.ReserveWait({}).status == XRFrameStatus::InvalidInput);
            const auto reserved = frames.ReserveWait(session);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            const auto invalidPrediction = frames.RecordWait({}, true);
            REQUIRE(invalidPrediction.status == XRFrameStatus::InvalidInput);
            REQUIRE(invalidPrediction.frame == reserved.frame);
            REQUIRE_FALSE(frames.Snapshot().renderAdmitted);
            REQUIRE(frames.Begin(invalidPrediction.frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(invalidPrediction.frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame image and submission failures retain release obligations", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session);
            REQUIRE(frames.BindConfiguration(Configuration(session, 2), capabilities, StereoLimits) == XRFrameStatus::OutOfOrder);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 0) == XRFrameStatus::InvalidInput);
            REQUIRE(frames.LocateViews(frame, 3) == XRFrameStatus::CapacityExceeded);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            const auto first = Image(session, 1);
            const auto second = Image(session, 2);
            REQUIRE(frames.Acquire(frame, {}) == XRFrameStatus::InvalidInput);
            XRSessionId foreign = session;
            ++foreign.slot.generation;
            REQUIRE(frames.Acquire(frame, Image(foreign, 1)) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Acquire(frame, first) == XRFrameStatus::Ok);
            REQUIRE(frames.Acquire(frame, first) == XRFrameStatus::Duplicate);
            REQUIRE(frames.Acquire(frame, second) == XRFrameStatus::Ok);
            REQUIRE(frames.Acquire(frame, Image(session, 3)) == XRFrameStatus::CapacityExceeded);
            const std::array partial{first};
            const std::array wrongOrder{second, first};
            const std::array foreignSubmission{Image(foreign, 1), second};
            REQUIRE(frames.Submit(frame, partial) == XRFrameStatus::IncompleteSubmission);
            REQUIRE(frames.Submit(frame, wrongOrder) == XRFrameStatus::IncompleteSubmission);
            REQUIRE(frames.Submit(frame, foreignSubmission) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::ImagesAcquired);
            REQUIRE(frames.Abort(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::ImageNotReleased);
            REQUIRE(frames.Release(frame, Image(foreign, 1)) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Release(frame, first) == XRFrameStatus::Ok);
            REQUIRE(frames.Release(frame, first) == XRFrameStatus::Duplicate);
            REQUIRE(frames.Release(frame, second) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR session replacement retains completed wait debt until explicit quiescent reset", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto firstCapabilities = Capabilities();
            const XRSessionId firstSession = Activate(sessions, firstCapabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(firstSession), firstCapabilities, StereoLimits) == XRFrameStatus::Ok);
            const auto reserved = frames.ReserveWait(firstSession);
            REQUIRE(reserved.status == XRFrameStatus::Ok);
            const auto replacementCapabilities = Capabilities(2);
            const XRSessionId replacement = Activate(sessions, replacementCapabilities);
            const auto completed = frames.RecordWait(Prediction(10'000), true);
            REQUIRE(completed.status == XRFrameStatus::Ok);
            REQUIRE(completed.frame == reserved.frame);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Waited);
            REQUIRE(frames.Begin(completed.frame) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Waited);
            REQUIRE(frames.ReserveWait(replacement).status == XRFrameStatus::StaleSession);
            // The test resource port is quiescent; a real host must prove native wait/frame and lease retirement first.
            frames.ResetAfterQuiescence();
            REQUIRE(frames.BindConfiguration(Configuration(replacement), replacementCapabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId newFrame = Wait(frames, replacement);
            REQUIRE(newFrame.sequence > completed.frame.sequence);
            REQUIRE(frames.Begin(completed.frame) == XRFrameStatus::StaleSession);
            REQUIRE(frames.Begin(newFrame) == XRFrameStatus::Ok);
            REQUIRE(frames.Abort(newFrame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(newFrame, 0) == XRFrameStatus::Ok);
        }

        TEST_CASE("XR frame abort retains release obligations and shutdown closes admission", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.LocateViews(frame, 2) == XRFrameStatus::Ok);
            const auto image = Image(session, 1);
            REQUIRE(frames.Acquire(frame, image) == XRFrameStatus::Ok);
            SECTION("Cancellation before submission retains image debt") {
                REQUIRE(frames.Abort(frame) == XRFrameStatus::Ok);
                REQUIRE(frames.End(frame, 0) == XRFrameStatus::ImageNotReleased);
                REQUIRE(frames.Release(frame, Image(session, 2)) == XRFrameStatus::ImageNotAcquired);
                REQUIRE(frames.Release(frame, image) == XRFrameStatus::Ok);
                REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
            }
            SECTION("Cancellation after image release suppresses the previously submitted layer") {
                REQUIRE(frames.Submit(frame, std::array{image}) == XRFrameStatus::Ok);
                REQUIRE(frames.Release(frame, image) == XRFrameStatus::Ok);
                REQUIRE(frames.Snapshot().phase == XRFramePhase::ImagesReleased);
                REQUIRE(frames.Abort(frame) == XRFrameStatus::Ok);
                CHECK(frames.ValidateEnd(frame, 1) == XRFrameStatus::OutOfOrder);
                REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
            }
            frames.Shutdown();
            frames.Shutdown();
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Shutdown);
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Shutdown);
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Shutdown);
            REQUIRE(frames.Snapshot().phase == XRFramePhase::Idle);
        }

        TEST_CASE("XR stopping session may finish an open frame but cannot admit another", "[unit][xr][frame]") {
            NoopResources resources;
            XRSessionLifecycle sessions{resources};
            const auto capabilities = Capabilities();
            const XRSessionId session = Activate(sessions, capabilities);
            XRFrameLifecycle frames{sessions};
            REQUIRE(frames.BindConfiguration(Configuration(session), capabilities, StereoLimits) == XRFrameStatus::Ok);
            const XRFrameId frame = Wait(frames, session, false);
            REQUIRE(sessions.ApplyEvent(session, XRSessionEvent::Stopping).HasValue());
            REQUIRE(frames.Begin(frame) == XRFrameStatus::Ok);
            REQUIRE(frames.End(frame, 0) == XRFrameStatus::Ok);
            REQUIRE(frames.ReserveWait(session).status == XRFrameStatus::Unavailable);
        }
    }  // namespace
}  // namespace Horo::XR
