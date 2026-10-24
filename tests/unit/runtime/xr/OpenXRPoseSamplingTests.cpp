#include "support/OpenXRActionBindingTestSupport.h"

#include <cmath>

namespace Horo::Tests::OpenXRAction {
    namespace {
        struct TrackingFixture final {
            ActionFixture actions{false};
            const XRWorldOriginRevision origin{IdentityValue<XRWorldOriginRevision>(1)};

            Result<void> Prepare(std::optional<Math::Transform> worldFromBase = std::nullopt) {
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
                return actions.owner.PrepareAttempt({actions.session.preflight, actions.session.preflight.Attempt()}, 2,
                                                    {&ActionScript::GetProc,
                                                     {1, actions.actions, actions.suggestions, {}, {1}, actions.registered},
                                                     actions.controls,
                                                     {},
                                                     actions.profilePaths,
                                                     actions.controlPaths,
                                                     {labels, setLabels},
                                                     poses,
                                                     NativeTrackingSpaceRequest{origin, XRSpaceKind::Local, worldFromBase}});
            }

            void Activate(std::uint32_t maximumSpaces = 3) {
                auto &session = actions.session;
                const auto plan = NegotiateXRFeatures(session.capabilities, session.capabilities.System(), session.capabilities.Revision(),
                                                      {.profile = XRFeatureProfile::Projection1_0,
                                                       .requestedLimits = {.maximumViews = 2,
                                                                           .maximumSpaces = maximumSpaces,
                                                                           .maximumActions = 8,
                                                                           .maximumDevices = 1}});
                REQUIRE(plan.plan.has_value());
                session.plan = *plan.plan;
                REQUIRE(session.owner.Activate(session.Request()).HasValue());
                REQUIRE(actions.owner.Bind(session.Request().candidate).HasValue());
            }

            XRTimeEvidence Time() const {
                return {IdentityValue<XRRuntimeTime>(100), IdentityValue<XRSimulationTime>(80), {}};
            }

            Result<NativePoseSamples> Sample() {
                const std::array devices{actions.Device()};
                return actions.owner.SamplePoses(actions.session.Request().candidate, Time(), origin, devices);
            }

            void Sync() {
                REQUIRE(actions.owner.Sync(actions.session.Request().candidate).HasValue());
            }
        };

        /** @brief Official native presentation dispatch with independently controlled complete count/validity evidence. */
        struct PresentationScript final {
            static inline thread_local PresentationScript *active{};
            XrViewStateFlags flags{XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                                   XR_VIEW_STATE_POSITION_TRACKED_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT};
            std::uint32_t count{2};
            XrTime time{};
            XrSpace reference{};
            bool malformedFov{};
            bool staleAfterLocate{};

            PresentationScript() {
                REQUIRE(active == nullptr);
                active = this;
            }

            ~PresentationScript() {
                active = nullptr;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Locate(XrSession session, const XrViewLocateInfo *info, XrViewState *state,
                                                         std::uint32_t capacity, std::uint32_t *count, XrView *views) {
                REQUIRE(session != XR_NULL_HANDLE);
                REQUIRE(info->type == XR_TYPE_VIEW_LOCATE_INFO);
                REQUIRE(info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
                REQUIRE(info->space != XR_NULL_HANDLE);
                REQUIRE(capacity == 2);
                active->time = info->displayTime;
                active->reference = info->space;
                state->viewStateFlags = active->flags;
                *count = active->count;
                for (std::uint32_t index = 0; index < std::min(capacity, active->count); ++index) {
                    REQUIRE(views[index].type == XR_TYPE_VIEW);
                    views[index].pose = {{0, 0, 0, 1}, {index == 0 ? -0.032F : 0.032F, 0, 0}};
                    views[index].fov = {-0.8F, 0.9F, 0.7F, -0.6F};
                    if (active->malformedFov)
                        views[index].fov.angleLeft = std::numeric_limits<float>::quiet_NaN();
                }
                if (active->staleAfterLocate)
                    Script::active->stale = true;
                return Script::Record("locate-views");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Enumerate(XrInstance instance, XrSystemId system, XrViewConfigurationType type,
                                                            std::uint32_t capacity, std::uint32_t *count, XrViewConfigurationView *views) {
                REQUIRE(instance != XR_NULL_HANDLE);
                REQUIRE(system != XR_NULL_SYSTEM_ID);
                REQUIRE(type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
                *count = 2;
                if (capacity) {
                    REQUIRE(capacity == 2);
                    for (std::uint32_t index = 0; index < capacity; ++index) {
                        REQUIRE(views[index].type == XR_TYPE_VIEW_CONFIGURATION_VIEW);
                        views[index].recommendedImageRectWidth = views[index].maxImageRectWidth = 640;
                        views[index].recommendedImageRectHeight = views[index].maxImageRectHeight = 480;
                    }
                }
                return XR_SUCCESS;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance instance, const char *name, PFN_xrVoidFunction *output) {
                if (std::string_view{name} == "xrEnumerateViewConfigurationViews") {
                    *output = std::bit_cast<PFN_xrVoidFunction>(&Enumerate);
                    return XR_SUCCESS;
                }
                if (std::string_view{name} == "xrLocateViews" && Script::active->absent != name) {
                    *output = std::bit_cast<PFN_xrVoidFunction>(&Locate);
                    return XR_SUCCESS;
                }
                return ActionScript::GetProc(instance, name, output);
            }
        };
    }  // namespace

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
        CHECK(grip.pose.time == fixture.Time());
        CHECK(grip.pose.source.worldOriginRevision == fixture.origin);
        CHECK(std::ranges::count(fixture.actions.session.script.calls, "locate-space") == 2);
        CHECK(fixture.Sample().HasError());  // Discovery and legacy sampling consume the SAME opportunity.
        REQUIRE(fixture.actions.owner.Close().HasValue());
        CHECK(fixture.actions.owner.ObservePoses(session, fixture.Time(), fixture.origin).HasError());
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
        REQUIRE(fixture.actions.owner.PreparePresentation(&PresentationScript::GetProc).HasValue());
        const XRTimeEvidence time{IdentityValue<XRRuntimeTime>(100), {}, IdentityValue<XRRenderPredictionTime>(200)};
        const auto located = fixture.actions.owner.LocatePresentation(session, time, 2);
        REQUIRE(located.HasValue());
        REQUIRE(located.Value().count == 2);
        REQUIRE(native.time == 200);
        REQUIRE(fixture.actions.script.tracking.locatedTime == 200);
        REQUIRE(located.Value().views[0].pose.position.x < 0);
        REQUIRE(located.Value().views[1].pose.position.x > 0);
        REQUIRE(located.Value().reference.kind == XRSpaceKind::Local);
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
    }

    TEST_CASE("OpenXR presentation rejects incomplete malformed stale or cross-domain evidence atomically", "[xr][native][views]") {
        TrackingFixture fixture;
        PresentationScript native;
        REQUIRE(fixture.Prepare(Math::Transform{}).HasValue());
        fixture.Activate(6);
        REQUIRE(fixture.actions.owner.PreparePresentation(&PresentationScript::GetProc).HasValue());
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
        CHECK(fixture.actions.owner.BorrowWorldSpace(world.Value()).HasValue());
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
