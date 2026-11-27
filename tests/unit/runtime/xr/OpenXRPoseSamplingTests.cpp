#include "OpenXRSessionEvents.h"
#include "support/OpenXRApplicationNativeScriptTestSupport.h"
#include "support/OpenXREventScriptTestSupport.h"

#include <cmath>
#include <limits>

namespace Horo::Tests::OpenXRAction {
    namespace {
        struct TrackingFixture final {
            ActionFixture actions{false};
            const XRWorldOriginRevision origin{IdentityValue<XRWorldOriginRevision>(1)};

            Result<void> Prepare(std::optional<Math::Transform> worldFromBase = std::nullopt, const bool stageAvailable = false,
                                 const std::optional<XRActionSetId> gazeSet = std::nullopt,
                                 const std::optional<float> stageBoundaryWarningMeters = std::nullopt) {
                actions.script.tracking.enabled = true;
                const std::array labels{NativeActionLabel{Input::ActionId{"select"}, "Select"}};
                const std::array setLabels{NativeActionSetLabel{{1}, "Gameplay"}};
                const std::array poses{NativePoseActionPath{{1},
                                                            {1},
                                                            XRTrackedDeviceRole::LeftController,
                                                            XRTrackedPoseKind::Grip,
                                                            "/user/hand/left/input/grip/pose",
                                                            "Grip"},
                                       NativePoseActionPath{{2},
                                                            {1},
                                                            XRTrackedDeviceRole::LeftController,
                                                            XRTrackedPoseKind::Grip,
                                                            "/user/hand/left/input/grip/pose",
                                                            "Grip"}};
                return actions.owner.PrepareAttempt({actions.session.preflight, actions.session.preflight.Attempt()}, gazeSet ? 3 : 2,
                                                    {&ActionScript::GetProc,
                                                     {1, actions.actions, actions.suggestions, {}, {1}, actions.registered},
                                                     actions.controls,
                                                     {},
                                                     actions.profilePaths,
                                                     actions.controlPaths,
                                                     {labels, setLabels},
                                                     poses,
                                                     NativeTrackingSpaceRequest{origin, XRSpaceKind::Local, worldFromBase, stageAvailable,
                                                                                gazeSet, stageBoundaryWarningMeters}});
            }

            void Activate(std::uint32_t maximumSpaces = 3, const bool quad = false, XRSessionLifecycle *lifecycle = nullptr) {
                auto &session = actions.session;
                if (quad)
                    session.capabilities = Capabilities(16, false, true);
                const auto profile = quad ? XRFeatureProfile::FoveatedInsetProjection : XRFeatureProfile::Projection1_0;
                const auto plan = NegotiateXRFeatures(session.capabilities, session.capabilities.System(), session.capabilities.Revision(),
                                                      {.profile = profile,
                                                       .requestedLimits = {.maximumViews = quad ? 4U : 2U,
                                                                           .maximumSpaces = maximumSpaces,
                                                                           .maximumActions = 8,
                                                                           .maximumDevices = 1}});
                REQUIRE(plan.plan.has_value());
                session.plan = *plan.plan;
                auto request = session.Request();
                if (lifecycle) {
                    const auto active = lifecycle->Activate(session.capabilities, session.capabilities.System(),
                                                            session.capabilities.Revision(), session.plan);
                    REQUIRE(active.HasValue());
                    request.candidate = active.Value();
                }
                REQUIRE(session.owner.Activate(request).HasValue());
                REQUIRE(actions.owner.Bind(request.candidate).HasValue());
            }

            XRTimeEvidence Time() const {
                return {IdentityValue<XRRuntimeTime>(100), IdentityValue<XRSimulationTime>(80), {}};
            }

            Result<NativePoseSamples> Sample() {
                const std::array devices{actions.Device()};
                return actions.owner.SamplePoses(actions.session.Request().candidate, Time(), origin, devices);
            }

            void Sync() {
                REQUIRE(actions.owner.Sync(actions.session.owner.Session()).HasValue());
            }
        };

        struct GazeEventResources final : IXRSessionResources {
            Result<void> Prepare(XRSessionPreparation, const XRSessionId &, const XRFeaturePlan &) override {
                return Result<void>::Success();
            }

            void Release(XRSessionPreparation, const XRSessionId &) noexcept override {
                // Native teardown is owned by the tracking fixture.
            }
        };

    }  // namespace

    TEST_CASE("OpenXR gaze requires a separate route and a fresh supported sample", "[xr][native][poses][gaze]") {
        TrackingFixture fixture;
        bool consent = true;
        const auto sample = [&] {
            return fixture.actions.owner.SampleConsentedGaze(fixture.actions.session.Request().candidate, fixture.Time(),
                                                             Duration::FromNanoseconds(10), consent);
        };
        SECTION("withdrawn consent skips the native activity and location calls") {
            REQUIRE(fixture.Prepare(std::nullopt, false, XRActionSetId{1}).HasValue());
            fixture.Activate(4);
            fixture.Sync();
            bool queried = false;
            fixture.actions.script.gaze.onActivity = [&queried] {
                queried = true;
            };
            consent = false;
            const auto denied = sample();
            REQUIRE(denied.HasError());
            CHECK(denied.ErrorValue().code.Value() == XRErrors::PermissionDenied.code.Value());
            CHECK_FALSE(queried);
            CHECK(fixture.actions.script.gaze.locationReads == 0);
        }
        SECTION("ordinary pose tracking never prepares an eye action") {
            REQUIRE(fixture.Prepare().HasValue());
            fixture.Activate();
            fixture.Sync();
            const auto denied = sample();
            REQUIRE(denied.HasError());
            CHECK(denied.ErrorValue().code.Value() == XRErrors::OperationUnsupported.code.Value());
            CHECK(fixture.actions.script.gaze.locationReads == 0);
            CHECK(fixture.actions.script.gaze.action == XR_NULL_HANDLE);
        }
        SECTION("only a current synchronized gaze sample leaves the native owner") {
            REQUIRE(fixture.Prepare(std::nullopt, false, XRActionSetId{1}).HasValue());
            fixture.Activate(4);
            auto &gaze = fixture.actions.script.gaze;
            gaze.sampleTime = 95;
            fixture.Sync();
            const auto admitted = sample();
            REQUIRE(admitted.HasValue());
            REQUIRE(admitted.Value().has_value());
            CHECK(admitted.Value()->Time().runtimeSample.Nanoseconds() == 95);
            CHECK(admitted.Value()->Components().confidence == XRTrackingConfidence::High);
            CHECK_FALSE(admitted.Value()->Components().linearVelocityMetersPerSecond.value.has_value());
            CHECK(fixture.Sample().HasValue());
            CHECK(sample().HasError());

            for (const XrTime time : {0, 89, 101}) {
                gaze.sampleTime = time;
                fixture.Sync();
                const auto stale = sample();
                REQUIRE(stale.HasError());
                CHECK(stale.ErrorValue().code.Value() == XRErrors::TrackingSnapshotStale.code.Value());
            }
            gaze.active = XR_FALSE;
            fixture.Sync();
            const auto reads = gaze.locationReads;
            const auto inactive = sample();
            REQUIRE(inactive.HasValue());
            CHECK_FALSE(inactive.Value().has_value());
            CHECK(gaze.locationReads == reads);

            gaze.active = XR_TRUE;
            gaze.sampleTime = 95;
            fixture.actions.script.tracking.flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
            fixture.Sync();
            const auto malformed = sample();
            REQUIRE(malformed.HasError());
            CHECK(malformed.ErrorValue().code.Value() == XRErrors::PoseInvalid.code.Value());
            REQUIRE(fixture.actions.owner.Close().HasValue());
            CHECK(fixture.actions.script.tracking.spaces == 0);
            CHECK(fixture.actions.script.liveActions == 0);
        }
        SECTION("withdrawn consent skips the native activity and location calls") {
            REQUIRE(fixture.Prepare(std::nullopt, false, XRActionSetId{1}).HasValue());
            fixture.Activate(4);
            fixture.Sync();
            bool queried = false;
            fixture.actions.script.gaze.onActivity = [&queried] {
                queried = true;
            };
            consent = false;
            const auto denied = sample();
            REQUIRE(denied.HasError());
            CHECK(denied.ErrorValue().code.Value() == XRErrors::PermissionDenied.code.Value());
            CHECK_FALSE(queried);
            CHECK(fixture.actions.script.gaze.locationReads == 0);
        }
    }

    TEST_CASE("OpenXR gaze discards an in-flight observation after a native focus interruption", "[xr][native][poses][gaze][revocation]") {
        TrackingFixture fixture;
        GazeEventResources resources;
        XRSessionLifecycle lifecycle{resources};
        REQUIRE(fixture.Prepare(std::nullopt, false, XRActionSetId{1}).HasValue());
        fixture.Activate(4, false, &lifecycle);
        auto &native = fixture.actions.session;
        const auto session = native.owner.Session();
        for (const auto event : {XRSessionEvent::Started, XRSessionEvent::BecameVisible, XRSessionEvent::BecameFocused})
            REQUIRE(lifecycle.ApplyEvent(session, event).HasValue());
        XRFrameLifecycle frames{lifecycle};
        OpenXRSessionEvents events{native.owner, lifecycle, frames};
        REQUIRE(events.Bind({&EventScript::GetProc}, {}).HasValue());
        EventScript script;
        script.session = native.script.lastSession;
        script.Push(XR_SESSION_STATE_VISIBLE);
        script.Push(XR_SESSION_STATE_FOCUSED);
        bool located = false;
        auto &gaze = fixture.actions.script.gaze;
        gaze.sampleTime = 95;
        gaze.onLocation = [&located] {
            located = true;
        };
        native.fence.onValidate = [&located, &script, &events] {
            if (!std::exchange(located, false))
                return;
            EventScript::active = &script;
            const auto observed = events.Poll(true);
            EventScript::active = nullptr;
            REQUIRE(observed.HasValue());
            CHECK(observed.Value().actionsInterrupted);
        };
        fixture.Sync();
        const auto sample = [&fixture, &session] {
            return fixture.actions.owner.SampleConsentedGaze(session, fixture.Time(), Duration::FromNanoseconds(10), true);
        };
        const auto denied = sample();
        native.fence.onValidate = {};
        gaze.onLocation = {};
        RequireFailureIdentity(denied, XRErrors::PermissionDenied);
        CHECK(lifecycle.Snapshot().admitsActions);
        const auto reads = gaze.locationReads;
        fixture.Sync();
        RequireFailureIdentity(sample(), XRErrors::PermissionDenied);
        CHECK(gaze.locationReads == reads);
    }

    TEST_CASE("OpenXR locates the complete quad projection and fences misplaced insets", "[xr][native][poses][quad]") {
        TrackingFixture fixture;
        PresentationScript script;
        script.quad = true;
        script.count = 4;
        REQUIRE(fixture.Prepare(Math::Transform{}).HasValue());
        fixture.Activate(6, true);
        REQUIRE(fixture.actions.owner.PreparePresentation({&PresentationScript::GetProc}).HasValue());
        const auto session = fixture.actions.session.Request().candidate;
        const XRTimeEvidence time{IdentityValue<XRRuntimeTime>(100), {}, IdentityValue<XRRenderPredictionTime>(120)};
        bool valid = false;
        SECTION("Four matching projections publish together") {
            valid = true;
        }
        SECTION("A runtime cannot return only its outer pair") {
            script.count = 2;
        }
        SECTION("An inset cannot change its eye position") {
            script.displacedInset = true;
        }
        SECTION("An inset cannot extend beyond its peripheral projection") {
            script.oversizedInset = true;
        }
        const auto result = fixture.actions.owner.LocatePresentation(session, time, 4);
        if (valid) {
            REQUIRE(result.HasValue());
            CHECK(result.Value().count == 4);
            CHECK(result.Value().views[2].pose.position.x == result.Value().views[0].pose.position.x);
            CHECK(result.Value().views[3].fov.angleRight < result.Value().views[1].fov.angleRight);
        } else {
            REQUIRE(result.HasError());
        }
        REQUIRE(fixture.actions.owner.LocatePresentation(session, time, 2).HasError());
    }

    TEST_CASE("OpenXR application dispatch composes canonical projection and preserves missing-entry rejection", "[xr][native][poses]") {
        OpenXRApplication::ApplicationNativeScript script;
        const auto instance = NativeHandle<XrInstance>(1);
        PFN_xrVoidFunction function{};
        SECTION("Missing projection entry remains unavailable") {
            script.native.absent = "xrLocateViews";
            function = std::bit_cast<PFN_xrVoidFunction>(&PresentationScript::Locate);
            CHECK(OpenXRApplication::ApplicationNativeScript::Resolve(instance, "xrLocateViews", &function) ==
                  XR_ERROR_FUNCTION_UNSUPPORTED);
            CHECK(function == nullptr);
        }
        SECTION("Canonical projection callback preserves observed evidence") {
            REQUIRE(OpenXRApplication::ApplicationNativeScript::Resolve(instance, "xrLocateViews", &function) == XR_SUCCESS);
            REQUIRE(function == std::bit_cast<PFN_xrVoidFunction>(&PresentationScript::Locate));
            const auto locate = std::bit_cast<PFN_xrLocateViews>(function);
            XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
            info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            info.displayTime = 100;
            info.space = NativeHandle<XrSpace>(2);
            XrViewState state{XR_TYPE_VIEW_STATE};
            std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
            std::uint32_t count{};
            REQUIRE(locate(NativeHandle<XrSession>(3), &info, &state, 2, &count, views.data()) == XR_SUCCESS);
            CHECK(count == 2);
            CHECK(script.presentation.time == info.displayTime);
            CHECK(script.presentation.reference == info.space);
            CHECK(state.viewStateFlags == script.presentation.flags);
            CHECK(views[0].pose.position.x == -0.032F);
            CHECK(views[1].pose.position.x == 0.032F);
            CHECK(views[0].fov.angleLeft == -0.8F);
        }
    }

    TEST_CASE("OpenXR application dispatch retains Census view dimensions before presentation fallback", "[xr][native][poses]") {
        OpenXRApplication::ApplicationNativeScript script;
        const auto instance = NativeHandle<XrInstance>(1);
        PFN_xrVoidFunction function{};
        REQUIRE(OpenXRApplication::ApplicationNativeScript::Resolve(instance, "xrEnumerateViewConfigurationViews", &function) ==
                XR_SUCCESS);
        REQUIRE(function == std::bit_cast<PFN_xrVoidFunction>(&OpenXR::CensusScript::Views));
        const auto enumerate = std::bit_cast<PFN_xrEnumerateViewConfigurationViews>(function);
        std::array<XrViewConfigurationView, 2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        std::uint32_t count{};
        REQUIRE(enumerate(instance, 1, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views.data()) == XR_SUCCESS);
        REQUIRE(count == 2);
        for (const auto &view : views) {
            CHECK(view.recommendedImageRectWidth == 1024);
            CHECK(view.recommendedImageRectHeight == 1024);
            CHECK(view.maxImageRectWidth == 2048);
            CHECK(view.maxImageRectHeight == 2048);
        }
    }

    TEST_CASE("OpenXR native pose observation discovers actual sources before runtime device identity admission", "[xr][native][poses]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare().HasValue());
        fixture.Activate();
        fixture.Sync();
        const auto session = fixture.actions.session.Request().candidate;
        const auto observed = fixture.actions.owner.ObservePoses(session, fixture.Time(), fixture.origin);
        REQUIRE(observed.HasValue());
        REQUIRE(observed.Value().Poses().size() == 2);
        const auto &head = observed.Value().Poses()[0];
        const auto &grip = observed.Value().Poses()[1];
        CHECK(head.role == XRTrackedDeviceRole::Head);
        CHECK(head.kind == XRTrackedPoseKind::Head);
        CHECK(head.active);
        CHECK(head.pose.source.kind == XRSpaceKind::View);
        CHECK(grip.role == XRTrackedDeviceRole::LeftController);
        CHECK(grip.kind == XRTrackedPoseKind::Grip);
        CHECK(grip.active);
        CHECK(grip.pose.source.kind == XRSpaceKind::Action);
        CHECK(grip.pose.session == session);
        CHECK(grip.pose.target.kind == XRSpaceKind::Local);
        CHECK(grip.pose.time == fixture.Time());
        CHECK(grip.pose.source.worldOriginRevision == fixture.origin);
        CHECK(std::ranges::count(fixture.actions.session.script.calls, "locate-space") == 2);
        CHECK(fixture.Sample().HasError());  // Discovery and legacy sampling consume the SAME opportunity.
        REQUIRE(fixture.actions.owner.Close().HasValue());
        CHECK(fixture.actions.owner.ObservePoses(session, fixture.Time(), fixture.origin).HasError());
    }

    TEST_CASE("OpenXR scalar sampling withdraws pose-dependent controls until the current controller is tracked",
              "[xr][native][poses][actions]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare().HasValue());
        fixture.Activate();
        const auto session = fixture.actions.session.Request().candidate;
        const auto plan = fixture.actions.Plan();
        std::array devices{fixture.actions.Device()};
        const auto sample = [&] {
            fixture.Sync();
            return fixture.actions.owner.Sample(session, plan, devices);
        };
        devices.front().state = XRTrackedDeviceState::Tracked;
        const auto original = sample();
        REQUIRE(original.HasValue());
        REQUIRE(original.Value().Actions().size() == 1);
        CHECK(original.Value().Actions().front().active);

        for (const auto unavailable : {XRTrackedDeviceState::TrackingLost, XRTrackedDeviceState::Disconnected}) {
            devices.front().state = unavailable;
            const auto nativeQueries = std::ranges::count(fixture.actions.session.script.calls, "sample-boolean");
            const auto neutral = sample();
            REQUIRE(neutral.HasValue());
            CHECK(neutral.Value().Actions().empty());
            CHECK(std::ranges::count(fixture.actions.session.script.calls, "sample-boolean") == nativeQueries);
        }

        ++devices.front().id.slot.generation;
        devices.front().state = XRTrackedDeviceState::Tracked;
        const auto replacement = sample();
        REQUIRE(replacement.HasValue());
        REQUIRE(replacement.Value().Actions().size() == 1);
        CHECK(replacement.Value().Actions().front().device == devices.front().id);
        CHECK(replacement.Value().Actions().front().device != original.Value().Actions().front().device);
    }

    TEST_CASE("OpenXR native source loss retains invalid evidence without inferring connection from prepared bindings",
              "[xr][native][poses]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare().HasValue());
        fixture.Activate();
        fixture.actions.script.sample.active = XR_FALSE;
        fixture.actions.script.tracking.flags = 0;
        fixture.actions.script.tracking.velocityFlags = 0;
        fixture.Sync();
        const auto observed =
            fixture.actions.owner.ObservePoses(fixture.actions.session.Request().candidate, fixture.Time(), fixture.origin);
        REQUIRE(observed.HasValue());
        REQUIRE(observed.Value().Poses().size() == 2);
        for (const auto &source : observed.Value().Poses()) {
            CHECK_FALSE(source.active);
            CHECK_FALSE(source.pose.components.positionMeters.value.has_value());
            CHECK_FALSE(source.pose.components.orientation.value.has_value());
            CHECK(source.pose.components.loss == XRTrackingLossState::FullyLost);
        }
        CHECK(std::ranges::count(fixture.actions.session.script.calls, "locate-space") == 1);  // Real head query only.
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR native discovery rejects partial malformed stale and reentrant opportunities", "[xr][native][poses]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare().HasValue());
        fixture.Activate();
        fixture.Sync();
        const auto session = fixture.actions.session.Request().candidate;
        auto origin = fixture.origin;
        SECTION("malformed actual component flags") {
            fixture.actions.script.tracking.flags = XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        }
        SECTION("native failure after a successful head observation") {
            fixture.actions.session.script.fail = "sample-pose";
        }
        SECTION("stale origin") {
            origin = IdentityValue<XRWorldOriginRevision>(2);
        }
        CHECK(fixture.actions.owner.ObservePoses(session, fixture.Time(), origin).HasError());
        CHECK(fixture.actions.owner.ObservePoses(session, fixture.Time(), fixture.origin).HasError());
        fixture.actions.session.script.fail = {};
        fixture.Sync();
        fixture.actions.script.tracking.flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        fixture.actions.script.sample.reentrantOwner = &fixture.actions.owner;
        REQUIRE(fixture.actions.owner.ObservePoses(session, fixture.Time(), fixture.origin).HasValue());
        CHECK(fixture.actions.script.sample.closeRejected);
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR presentation locates actual views and calibrated reference at the same native prediction", "[xr][native][views]") {
        TrackingFixture fixture;
        PresentationScript native;
        REQUIRE(fixture.Prepare(Math::Transform{}).HasValue());
        fixture.Activate(6);
        const auto session = fixture.actions.session.Request().candidate;
        REQUIRE(fixture.actions.owner.PreparePresentation({&PresentationScript::GetProc}).HasValue());
        const XRTimeEvidence time{IdentityValue<XRRuntimeTime>(100), {}, IdentityValue<XRRenderPredictionTime>(200)};
        const auto located = fixture.actions.owner.LocatePresentation(session, time, 2);
        REQUIRE(located.HasValue());
        REQUIRE(located.Value().count == 2);
        REQUIRE(native.time == 200);
        REQUIRE(fixture.actions.script.tracking.locatedTime == 200);
        REQUIRE(located.Value().views[0].pose.position.x < 0);
        REQUIRE(located.Value().views[1].pose.position.x > 0);
        REQUIRE(located.Value().reference.kind == XRSpaceKind::Local);
        const auto reference = located.Value().reference;
        const auto borrowed = fixture.actions.owner.BorrowPresentationReference(reference);
        REQUIRE(borrowed.HasValue());
        CHECK(borrowed.Value() == native.reference);
        CHECK(fixture.actions.owner.BorrowWorldSpace(reference).HasError());
        REQUIRE(located.Value().eyes[0].kind == XRSpaceKind::View);
        REQUIRE(located.Value().eyes[1].kind == XRSpaceKind::View);
        REQUIRE(located.Value().eyes[0].id != located.Value().eyes[1].id);
        REQUIRE(located.Value().eyes[0].id.session == session);
        REQUIRE(located.Value().eyes[0].worldOriginRevision == fixture.origin);
        REQUIRE(fixture.actions.owner.BorrowWorldSpace(located.Value().eyes[0]).HasError());
        REQUIRE(fixture.actions.owner.BorrowWorldSpace(located.Value().eyes[1]).HasError());
        REQUIRE(located.Value().referenceToWorld.source == located.Value().reference);
        const auto world = fixture.actions.owner.WorldSpace(session);
        REQUIRE(world.HasValue());
        REQUIRE(located.Value().referenceToWorld.target == world.Value());
        REQUIRE(located.Value().referenceToWorld.purpose == XRPosePurpose::PresentationPrediction);
        REQUIRE(located.Value().referenceToWorld.time == time);
        REQUIRE_FALSE(located.Value().referenceToWorld.time.simulation);
        native.flags = 0;
        const auto lost = fixture.actions.owner.LocatePresentation(session, time, 2);
        REQUIRE(lost.HasValue());
        REQUIRE(lost.Value().flags == 0);  // No stale/default pose-validity bit is synthesized.
        REQUIRE(fixture.actions.owner.Close().HasValue());
        REQUIRE(fixture.actions.owner.LocatePresentation(session, time, 2).HasError());
        CHECK(fixture.actions.owner.BorrowPresentationReference(reference).HasError());
    }

    TEST_CASE("OpenXR cold reference calibration observes only the exact bound runtime spaces", "[xr][native][poses][reference]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare(Math::Transform{.translation = {1.0F, 2.0F, 3.0F}}).HasValue());
        fixture.Activate(6);
        auto &owner = fixture.actions.owner;
        auto &script = fixture.actions.script;
        const auto session = fixture.actions.session.Request().candidate;
        const auto references = owner.References(session);
        const auto world = owner.WorldSpace(session);
        REQUIRE(references.HasValue());
        REQUIRE(world.HasValue());
        auto reference = references.Value().local;
        auto target = world.Value();
        const auto sample = IdentityValue<XRRuntimeTime>(503);

        SECTION("current references yield a runtime-domain relation without Sync or a frame") {
            script.sample.reentrantOwner = &owner;
            const auto received = owner.ObserveReferenceCalibration(reference, target, sample);
            REQUIRE(received.HasValue());
            CHECK(received.Value().reference == reference);
            CHECK(received.Value().world == target);
            CHECK(received.Value().observedAt == sample);
            CHECK(received.Value().worldFromReference.translation == Math::Vec3{1.0F, 2.0F, 3.0F});
            CHECK(script.tracking.locatedTime == 503);
            const auto nativeWorld = owner.BorrowWorldSpace(target);
            REQUIRE(nativeWorld.HasValue());
            CHECK(script.tracking.locatedBase == nativeWorld.Value());
            CHECK(script.sample.closeRejected);
        }
        SECTION("retired space generation and replaced origin fail before native work") {
            ++reference.id.slot.generation;
            REQUIRE(owner.ObserveReferenceCalibration(reference, target, sample).HasError());
            reference = references.Value().local;
            target.worldOriginRevision = IdentityValue<XRWorldOriginRevision>(2);
            const auto stale = owner.ObserveReferenceCalibration(reference, target, sample);
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == XRErrors::IdentityStale.code.Value());
            CHECK(script.tracking.locatedTime == 0);
        }
        SECTION("an invalid runtime timestamp cannot authorize a locate") {
            const auto invalid = owner.ObserveReferenceCalibration(reference, target, {});
            REQUIRE(invalid.HasError());
            CHECK(invalid.ErrorValue().code.Value() == XRErrors::TimeDomainIncompatible.code.Value());
            CHECK(script.tracking.locatedTime == 0);
        }
        SECTION("inferred components do not qualify the relation") {
            script.tracking.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
            const auto inferred = owner.ObserveReferenceCalibration(reference, target, sample);
            REQUIRE(inferred.HasError());
            CHECK(inferred.ErrorValue().code.Value() == XRErrors::OperationUnavailable.code.Value());
        }
        SECTION("failed native location cannot publish a partial relation") {
            fixture.actions.session.script.fail = "locate-space";
            const auto unavailable = owner.ObserveReferenceCalibration(reference, target, sample);
            REQUIRE(unavailable.HasError());
            CHECK(unavailable.ErrorValue().code.Value() == XRErrors::OperationUnavailable.code.Value());
            fixture.actions.session.script.fail = {};
        }
        REQUIRE(owner.Close().HasValue());
        CHECK(owner.ObserveReferenceCalibration(references.Value().local, world.Value(), sample).HasError());
    }

    TEST_CASE("OpenXR presentation reference borrows reject every distinct owner identity", "[xr][native][views]") {
        TrackingFixture fixture;
        PresentationScript native;
        REQUIRE(fixture.Prepare(Math::Transform{}).HasValue());
        fixture.Activate(6);
        const auto session = fixture.actions.session.Request().candidate;
        REQUIRE(fixture.actions.owner.PreparePresentation({&PresentationScript::GetProc}).HasValue());
        const XRTimeEvidence time{IdentityValue<XRRuntimeTime>(100), {}, IdentityValue<XRRenderPredictionTime>(200)};
        const auto located = fixture.actions.owner.LocatePresentation(session, time, 2);
        REQUIRE(located.HasValue());
        auto stale = located.Value().reference;
        SECTION("kind") {
            stale.kind = XRSpaceKind::World;
        }
        SECTION("origin") {
            stale.worldOriginRevision = IdentityValue<XRWorldOriginRevision>(2);
        }
        SECTION("session") {
            ++stale.id.session.slot.generation;
        }
        SECTION("generation") {
            ++stale.id.slot.generation;
        }
        SECTION("eye") {
            stale = located.Value().eyes[0];
        }
        SECTION("calibrated World") {
            const auto world = fixture.actions.owner.WorldSpace(session);
            REQUIRE(world.HasValue());
            stale = world.Value();
        }
        CHECK(fixture.actions.owner.BorrowPresentationReference(stale).HasError());
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR presentation rejects incomplete malformed stale or cross-domain evidence atomically", "[xr][native][views]") {
        TrackingFixture fixture;
        PresentationScript native;
        REQUIRE(fixture.Prepare(Math::Transform{}).HasValue());
        fixture.Activate(6);
        REQUIRE(fixture.actions.owner.PreparePresentation({&PresentationScript::GetProc}).HasValue());
        XRTimeEvidence time{IdentityValue<XRRuntimeTime>(100), {}, IdentityValue<XRRenderPredictionTime>(200)};
        const auto session = fixture.actions.session.Request().candidate;
        SECTION("incomplete native count") {
            native.count = 1;
        }
        SECTION("tracked without valid") {
            native.flags = XR_VIEW_STATE_POSITION_TRACKED_BIT;
        }
        SECTION("nonfinite native projection") {
            native.malformedFov = true;
        }
        SECTION("native replacement during query") {
            native.staleAfterLocate = true;
        }
        SECTION("simulation evidence is not presentation authority") {
            time.simulation = IdentityValue<XRSimulationTime>(0);
        }
        REQUIRE(fixture.actions.owner.LocatePresentation(session, time, 2).HasError());
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR calibrated World is a distinct native rigid inverse with exact retirement fences", "[xr][native][poses]") {
        TrackingFixture fixture;
        const Math::Transform worldFromBase{{2.0F, 3.0F, 4.0F},
                                            Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, 1.57079632679F),
                                            {1.0F, 1.0F, 1.0F}};
        REQUIRE(fixture.Prepare(worldFromBase).HasValue());
        REQUIRE(fixture.actions.script.tracking.references.size() == 3);
        const auto &native = fixture.actions.script.tracking.references.back();
        CHECK(native.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL);
        const Math::Quaternion rotation{native.poseInReferenceSpace.orientation.x, native.poseInReferenceSpace.orientation.y,
                                        native.poseInReferenceSpace.orientation.z, native.poseInReferenceSpace.orientation.w};
        const Math::Vec3 translation{native.poseInReferenceSpace.position.x, native.poseInReferenceSpace.position.y,
                                     native.poseInReferenceSpace.position.z};
        const auto origin = worldFromBase.rotation.Rotate(translation) + worldFromBase.translation;
        CHECK(Math::Length(origin) < 0.00001F);
        const auto product = worldFromBase.rotation * rotation;
        CHECK(std::abs(product.x) < 0.00001F);
        CHECK(std::abs(product.y) < 0.00001F);
        CHECK(std::abs(product.z) < 0.00001F);
        CHECK(std::abs(product.w - 1.0F) < 0.00001F);
        CHECK(fixture.actions.owner.WorldSpace(fixture.actions.session.Request().candidate).HasError());
        fixture.Activate(4);
        const auto world = fixture.actions.owner.WorldSpace(fixture.actions.session.Request().candidate);
        REQUIRE(world.HasValue());
        CHECK(world.Value().kind == XRSpaceKind::World);
        const auto nativeWorld = fixture.actions.owner.BorrowWorldSpace(world.Value());
        REQUIRE(nativeWorld.HasValue());
        fixture.Sync();
        const auto poses =
            fixture.actions.owner.ObserveWorldPoses(fixture.actions.session.Request().candidate, fixture.Time(), fixture.origin);
        REQUIRE(poses.HasValue());
        REQUIRE(poses.Value().Poses().size() == 2);
        for (const auto &pose : poses.Value().Poses())
            CHECK(pose.pose.target == world.Value());
        CHECK(fixture.actions.script.tracking.locatedBase == nativeWorld.Value());
        CHECK(std::ranges::count(fixture.actions.session.script.calls, "locate-space") == 2);
        CHECK(fixture.Sample().HasError());
        auto stale = world.Value();
        stale.worldOriginRevision = IdentityValue<XRWorldOriginRevision>(2);
        CHECK(fixture.actions.owner.BorrowWorldSpace(stale).HasError());
        stale = world.Value();
        stale.kind = XRSpaceKind::Local;
        CHECK(fixture.actions.owner.BorrowWorldSpace(stale).HasError());
        REQUIRE(fixture.actions.owner.Close().HasValue());
        CHECK(fixture.actions.script.tracking.spaces == 0);
        CHECK(fixture.actions.owner.BorrowWorldSpace(world.Value()).HasError());
    }

    TEST_CASE("OpenXR Stage bounds are cold, finite, and owned by the issued reference", "[xr][native][spaces]") {
        TrackingFixture fixture;
        auto &tracking = fixture.actions.script.tracking;
        bool valid = true;
        SECTION("finite room") {}
        SECTION("real runtime has no measured rectangle") {
            tracking.boundsResult = XR_SPACE_BOUNDS_UNAVAILABLE;
            tracking.bounds = {};
        }
        SECTION("runtime data is malformed") {
            tracking.bounds.width = std::numeric_limits<float>::infinity();
            valid = false;
        }
        SECTION("Stage was withdrawn after census") {
            tracking.boundsResult = XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
            valid = false;
        }
        const auto prepared = fixture.Prepare(std::nullopt, true);
        REQUIRE(tracking.boundsReads == 1);
        if (!valid) {
            REQUIRE(prepared.HasError());
            CHECK(prepared.ErrorValue().code.Value() == (tracking.boundsResult == XR_SUCCESS
                                                             ? XRErrors::OperationInvalid.code.Value()
                                                             : XRErrors::OperationUnsupported.code.Value()));
            CHECK(tracking.spaces == 0);
            return;
        }
        REQUIRE(prepared.HasValue());
        fixture.Activate(4);
        const auto session = fixture.actions.session.Request().candidate;
        const auto references = fixture.actions.owner.References(session);
        REQUIRE(references.HasValue());
        CHECK(references.Value().view.kind == XRSpaceKind::View);
        CHECK(references.Value().local.kind == XRSpaceKind::Local);
        REQUIRE(references.Value().stage.has_value());
        CHECK(references.Value().stage->kind == XRSpaceKind::Stage);
        CHECK(references.Value().stage->id.session == session);
        CHECK(references.Value().stage->id != references.Value().local.id);
        CHECK(references.Value().stageBounds.has_value() == (tracking.boundsResult == XR_SUCCESS));
        if (references.Value().stageBounds) {
            CHECK(references.Value().stageBounds->widthMeters == 3.0F);
            CHECK(references.Value().stageBounds->depthMeters == 4.0F);
        }
        CHECK(fixture.actions.owner.BorrowPresentationReference(references.Value().local).HasValue());
        CHECK(fixture.actions.owner.BorrowPresentationReference(*references.Value().stage).HasError());
        REQUIRE(fixture.actions.owner.Close().HasValue());
        CHECK(fixture.actions.owner.References(session).HasError());
        CHECK(tracking.spaces == 0);
    }

    TEST_CASE("OpenXR Stage advisory is a bounded side observation from the World sample", "[xr][native][spaces][comfort]") {
        TrackingFixture fixture;
        bool available = true;
        bool tracked = true;
        SECTION("none when Stage has no metric rectangle") {
            auto &tracking = fixture.actions.script.tracking;
            tracking.boundsResult = XR_SPACE_BOUNDS_UNAVAILABLE;
            tracking.bounds = {};
            available = false;
        }
        SECTION("classified when the native head position is tracked") {}
        SECTION("no classified value for inferred position") {
            fixture.actions.script.tracking.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
            tracked = false;
        }
        REQUIRE(fixture.Prepare(Math::Transform{}, true, std::nullopt, 0.30F).HasValue());
        fixture.Activate(5);
        fixture.Sync();
        const auto packet =
            fixture.actions.owner.ObserveWorldPoses(fixture.actions.session.Request().candidate, fixture.Time(), fixture.origin);
        REQUIRE(packet.HasValue());
        REQUIRE(packet.Value().Poses().size() == 2);
        for (const auto &pose : packet.Value().Poses())
            CHECK(pose.pose.target.kind == XRSpaceKind::World);
        const auto boundary = packet.Value().StageBoundary();
        CHECK(boundary.has_value() == (available && tracked));
        CHECK(std::ranges::count(fixture.actions.session.script.calls, "locate-space") == (available ? 3 : 2));
    }

    TEST_CASE("OpenXR calibrated World rejects scale before creating native children", "[xr][native][poses]") {
        TrackingFixture fixture;
        Math::Transform world;
        world.scale.x = -1.0F;
        CHECK(fixture.Prepare(world).HasError());
        CHECK(fixture.actions.script.tracking.references.empty());
        CHECK(fixture.actions.script.liveActions == 0);
        world.scale = {1.0F, 2.0F, 1.0F};
        CHECK(fixture.Prepare(world).HasError());
        CHECK(fixture.actions.script.tracking.references.empty());
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR action spaces retain exact native parents and publish Action meaning after canonical binding",
              "[xr][native][poses]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare().HasValue());
        CHECK(fixture.actions.script.liveActions == 2);
        CHECK(fixture.actions.script.tracking.spaces == 3);
        CHECK_FALSE(fixture.actions.session.owner.Session().IsValid());
        CHECK(fixture.Sample().HasError());
        fixture.Activate();
        fixture.Sync();
        const auto poses = fixture.Sample();
        REQUIRE(poses.HasValue());
        REQUIRE(poses.Value().Poses().size() == 1);
        const auto &pose = poses.Value().Poses().front();
        CHECK(pose.kind == XRTrackedPoseKind::Grip);
        CHECK(pose.pose.source.kind == XRSpaceKind::Action);
        CHECK(pose.pose.target.kind == XRSpaceKind::Local);
        CHECK(pose.pose.source.id.session == fixture.actions.session.Request().candidate);
        CHECK(pose.pose.source.worldOriginRevision == fixture.origin);
        CHECK(fixture.actions.script.tracking.locatedTime == 100);
        CHECK(pose.pose.components.positionMeters.validity == XRPoseComponentValidity::Tracked);
        CHECK(pose.pose.components.orientation.validity == XRPoseComponentValidity::Tracked);
        CHECK(fixture.Sample().HasError());
        REQUIRE(fixture.actions.owner.Close().HasValue());
        CHECK(fixture.actions.script.tracking.spaces == 0);
        CHECK(fixture.actions.script.liveActions == 0);
    }

    TEST_CASE("OpenXR space location preserves independent validity and ignores invalid native payloads", "[xr][native][poses]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare().HasValue());
        fixture.Activate();
        fixture.actions.script.tracking.flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        fixture.Sync();
        const auto poses = fixture.Sample();
        REQUIRE(poses.HasValue());
        const auto &components = poses.Value().Poses().front().pose.components;
        CHECK_FALSE(components.positionMeters.value.has_value());
        CHECK_FALSE(components.linearVelocityMetersPerSecond.value.has_value());
        REQUIRE(components.orientation.value.has_value());
        CHECK(components.orientation.validity == XRPoseComponentValidity::Inferred);
        CHECK(components.loss == XRTrackingLossState::PositionLost);
        CHECK(components.confidence == XRTrackingConfidence::Low);
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR inactive pose action emits loss without locating or reviving stale tracking", "[xr][native][poses]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare().HasValue());
        fixture.Activate();
        fixture.actions.script.sample.active = XR_FALSE;
        fixture.Sync();
        const auto poses = fixture.Sample();
        REQUIRE(poses.HasValue());
        const auto &components = poses.Value().Poses().front().pose.components;
        CHECK(components.loss == XRTrackingLossState::FullyLost);
        CHECK_FALSE(components.positionMeters.value.has_value());
        CHECK_FALSE(components.orientation.value.has_value());
        CHECK(std::ranges::count(fixture.actions.session.script.calls, "locate-space") == 0);
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR pose sampling rejects contradictory native flags stale origin and presentation-only time", "[xr][native][poses]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare().HasValue());
        fixture.Activate();
        fixture.Sync();
        const std::array devices{fixture.actions.Device()};
        SECTION("tracked without valid") {
            fixture.actions.script.tracking.flags = XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
            CHECK(fixture.Sample().HasError());
        }
        SECTION("stale origin") {
            CHECK(fixture.actions.owner
                      .SamplePoses(fixture.actions.session.Request().candidate, fixture.Time(), IdentityValue<XRWorldOriginRevision>(2),
                                   devices)
                      .HasError());
        }
        SECTION("presentation evidence") {
            auto time = fixture.Time();
            time.simulation.reset();
            time.renderPrediction = IdentityValue<XRRenderPredictionTime>(100);
            CHECK(fixture.actions.owner.SamplePoses(fixture.actions.session.Request().candidate, time, fixture.origin, devices).HasError());
        }
        CHECK(fixture.Sample().HasError());  // Failed opportunity cannot be retried against old Sync.
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR tracking retirement preserves space debt before destroying borrowed pose actions", "[xr][native][poses]") {
        TrackingFixture fixture;
        REQUIRE(fixture.Prepare().HasValue());
        fixture.Activate();
        fixture.Sync();
        fixture.actions.script.sample.reentrantOwner = &fixture.actions.owner;
        REQUIRE(fixture.Sample().HasValue());
        CHECK(fixture.actions.script.sample.closeRejected);
        fixture.actions.session.script.fail = "destroy-space";
        CHECK(fixture.actions.owner.Close().HasError());
        CHECK(fixture.actions.script.tracking.spaces == 3);
        CHECK(fixture.actions.script.liveActions == 2);
        CHECK(fixture.Sample().HasError());
        fixture.actions.session.script.fail = {};
        REQUIRE(fixture.actions.owner.Close().HasValue());
        CHECK(fixture.actions.script.tracking.spaces == 0);
        CHECK(fixture.actions.script.liveActions == 0);
    }

    TEST_CASE("OpenXR space creation keeps loss-pending handles for rollback without publishing tracking", "[xr][native][spaces]") {
        TrackingFixture fixture;
        auto &script = fixture.actions.session.script;
        auto &tracking = fixture.actions.script.tracking;
        std::size_t expectedSpaces = 0;
        SECTION("local reference") {
            tracking.loseOnReferenceCall = expectedSpaces = 1;
        }
        SECTION("view reference") {
            tracking.loseOnReferenceCall = expectedSpaces = 2;
        }
        SECTION("optional stage reference") {
            tracking.loseOnReferenceCall = expectedSpaces = 3;
        }
        SECTION("calibrated world reference") {
            tracking.loseOnReferenceCall = expectedSpaces = 4;
        }
        SECTION("controller action space") {
            script.fail = "create-action-space";
            script.failureResult = XR_SESSION_LOSS_PENDING;
            expectedSpaces = 5;
        }
        const Math::Transform calibration{.translation = {5.0F, 0.0F, 0.0F}};
        const auto prepared = fixture.Prepare(calibration, true);
        REQUIRE(prepared.HasError());
        CHECK(prepared.ErrorValue().code.Value() == XRErrors::RuntimeUnavailable.code.Value());
        CHECK(std::ranges::count(script.calls, "destroy-space") == expectedSpaces);
        CHECK(tracking.spaces == 0);
        CHECK(fixture.actions.script.liveActions == 0);
        CHECK(fixture.actions.script.liveSets == 0);
        CHECK(fixture.actions.owner.References(fixture.actions.session.Request().candidate).HasError());
        CHECK_FALSE(fixture.actions.session.owner.Session().IsValid());
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR loss-pending creation retains handles when native rollback must be retried", "[xr][native][spaces]") {
        TrackingFixture fixture;
        auto &script = fixture.actions.session.script;
        auto &tracking = fixture.actions.script.tracking;
        tracking.loseOnReferenceCall = 2;
        script.fail = "destroy-space";
        REQUIRE(fixture.Prepare().HasError());
        CHECK(tracking.spaces == 2);
        CHECK(fixture.actions.owner.References(fixture.actions.session.Request().candidate).HasError());
        script.fail = {};
        REQUIRE(fixture.actions.owner.Close().HasValue());
        CHECK(tracking.spaces == 0);
        CHECK(fixture.actions.script.liveActions == 0);
        CHECK(fixture.actions.script.liveSets == 0);
    }

    TEST_CASE("OpenXR maps a native reference-space capacity refusal to the XR budget error", "[xr][native][spaces]") {
        TrackingFixture fixture;
        auto &script = fixture.actions.session.script;
        script.fail = "create-reference";
        script.failureResult = XR_ERROR_LIMIT_REACHED;
        const auto prepared = fixture.Prepare();
        REQUIRE(prepared.HasError());
        CHECK(prepared.ErrorValue().code.Value() == XRErrors::CapacityExceeded.code.Value());
        CHECK(fixture.actions.script.tracking.spaces == 0);
        CHECK(fixture.actions.script.liveActions == 0);
        CHECK(fixture.actions.script.liveSets == 0);
    }

    TEST_CASE("OpenXR partial action-space failure rolls back references before pose actions", "[xr][native][poses]") {
        TrackingFixture fixture;
        fixture.actions.session.script.fail = "create-action-space";
        CHECK(fixture.Prepare().HasError());
        CHECK(fixture.actions.script.tracking.spaces == 0);
        CHECK(fixture.actions.script.liveActions == 0);
        CHECK(fixture.actions.script.liveSets == 0);
        CHECK_FALSE(fixture.actions.session.owner.Session().IsValid());
        REQUIRE(fixture.actions.owner.Close().HasValue());
    }
}  // namespace Horo::Tests::OpenXRAction
