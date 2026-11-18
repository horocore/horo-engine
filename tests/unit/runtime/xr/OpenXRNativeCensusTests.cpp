#include "OpenXRPlaneCapture.h"
#include "OpenXRPlaneSnapshot.h"
#include "support/OpenXRPlaneCaptureTestSupport.h"

#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace Horo::Tests::OpenXR {
    namespace {

        struct CensusFixture {
            Script native;
            CensusScript census;
            Graphics graphics;
            Fence fence;
            XRLoaderPreflightSnapshot preflight{Preflight()};
            OpenXRNativeSession owner{std::make_shared<CensusLibrary>(), graphics, fence};

            NativeDiscoveryRequest Discovery() const {
                return {.selection = {.attempt = preflight.Attempt(),
                                      .backend = preflight.Backend(),
                                      .installRecord = preflight.InstallRecord(),
                                      .productProfile = preflight.ProductProfile(),
                                      .admittedLoaderVersions = {{1, 0, 0}, {1, 1, 63}}},
                        .requestedApi = preflight.LoaderApiVersion()};
            }

            NativeSessionPreparation Preparation() const {
                return {preflight, preflight.Attempt()};
            }
        };

    }  // namespace

    TEST_CASE("OpenXR discovery observes actual pre-session system views and distinct runtime implementation version",
              "[unit][xr][native]") {
        CensusFixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        const auto actual = fixture.owner.DiscoverSystem(fixture.preflight.Attempt());
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().runtime.runtimeVersion == XR_MAKE_VERSION(9, 8, 7));
        CHECK(actual.Value().primaryViewCount == 2);
        CHECK(actual.Value().primaryViews[0].recommendedImageRectWidth == 1024);
        CHECK(actual.Value().primaryBlendModes[0] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
        CHECK(fixture.native.liveSessions == 0);
        CHECK(fixture.owner.DiscoverSession(fixture.Preparation()).HasError());
        CHECK(std::ranges::count(fixture.census.calls, "spaces") == 0);
        CHECK(std::ranges::count(fixture.census.calls, "formats") == 0);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR eye gaze support is a bounded cold census and malformed optional data leaves baseline available",
              "[unit][xr][native][gaze]") {
        using enum XRCapabilityState;
        CensusFixture fixture;
        fixture.native.gazeExtension = true;
        fixture.native.extensionCount = 2;
        fixture.census.gazeSupport = XR_TRUE;
        XRCapabilityState expected = Available;
        bool request = true;
        SECTION("extension advertisement without instance enablement") {
            request = false;
            expected = Unsupported;
        }
        SECTION("runtime reports a supported device") {}
        SECTION("runtime reports an unsupported device") {
            fixture.census.gazeSupport = XR_FALSE;
            expected = Unsupported;
        }
        SECTION("native permission denial grants no authority") {
            fixture.census.gazeResult = XR_ERROR_PERMISSION_INSUFFICIENT;
            expected = Denied;
        }
        SECTION("unavailable optional system property") {
            fixture.census.gazeResult = XR_ERROR_RUNTIME_FAILURE;
            expected = Unavailable;
        }
        SECTION("malformed boolean fails the optional observation") {
            fixture.census.gazeSupport = 9;
            expected = Incompatible;
        }
        const std::array routes{NativeExtensionRequest{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME}};
        auto discovery = fixture.Discovery();
        if (request)
            discovery.extensions = routes;
        REQUIRE(fixture.owner.PrepareDiscovery(discovery).HasValue());
        const auto actual = fixture.owner.DiscoverSystem(fixture.preflight.Attempt());
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().eyeGazeProvider == expected);
        CHECK(actual.Value().system.next == nullptr);
        CHECK(actual.Value().primaryViewCount == 2);
        CHECK(std::ranges::count(fixture.census.calls, "eye-gaze-properties") == static_cast<int>(request));
        CHECK(fixture.native.liveSessions == 0);
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.DiscoverSystem(fixture.preflight.Attempt()).HasError());
    }

    TEST_CASE("OpenXR spatial plane census remains cold and follows exact enabled capabilities", "[unit][xr][native][planes]") {
        using enum XRCapabilityState;
        CensusFixture fixture;
        constexpr std::array<std::string_view, 3> extensions{XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME,
                                                             XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME, XR_EXT_FUTURE_EXTENSION_NAME};
        const std::array routes{NativeExtensionRequest{extensions[0]}, NativeExtensionRequest{extensions[1]},
                                NativeExtensionRequest{extensions[2]}};
        fixture.native.extraExtensions = extensions;
        fixture.native.extensionCount += static_cast<std::uint32_t>(extensions.size());
        auto discovery = fixture.Discovery();
        discovery.extensions = routes;
        auto expected = Available;
        bool labels = true;
        SECTION("all required components exist") {}
        SECTION("semantics are optional") {
            fixture.census.spatialLabels = false;
            labels = false;
        }
        SECTION("require enabled future and spatial entity dependencies") {
            discovery.extensions = std::span{routes}.first(2);
            expected = DependencyMissing;
        }
        SECTION("advertising does not enable") {
            discovery.extensions = {};
            expected = Unsupported;
        }
        SECTION("selected runtime provides another capability set") {
            fixture.census.spatialPlanes = false;
            expected = Unsupported;
        }
        SECTION("bounded geometry is required") {
            fixture.census.spatialBounds = false;
            expected = Incompatible;
        }
        SECTION("runtime has denied permission") {
            fixture.census.spatialComponentsResult = XR_ERROR_PERMISSION_INSUFFICIENT;
            expected = Denied;
        }
        SECTION("large tool publications are not truncated") {
            fixture.census.spatialComponentsResult = XR_ERROR_SIZE_INSUFFICIENT;
            expected = Incompatible;
        }
        REQUIRE(fixture.owner.PrepareDiscovery(discovery).HasValue());
        const auto actual = fixture.owner.DiscoverSystem(fixture.preflight.Attempt());
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().planes.state == expected);
        CHECK(actual.Value().planes.semanticLabels == (expected == Available && labels));
        CHECK(actual.Value().primaryViewCount == 2);
        CHECK(fixture.native.liveSessions == 0);
        CHECK(std::ranges::count(fixture.census.calls, "spatial-capabilities") == (discovery.extensions.size() == routes.size()));
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR spatial snapshot joins tracked native geometry with optional semantic labels", "[unit][xr][native][planes]") {
        PlaneSnapshotFixture fixture;
        auto census = NativePlaneCensus{XRCapabilityState::Available, true};
        SECTION("optional labels are queried separately") {}
        SECTION("unsupported labels remain unknown") {
            census.semanticLabels = false;
        }
        const auto snapshot = NativeHandle<XrSpatialSnapshotEXT>(1);
        const auto result = ReadNativeSpatialPlanes(snapshot, &PlaneSnapshotFixture::Query, census, 4);
        REQUIRE(result.HasValue());
        const auto planes = result.Value().Planes();
        REQUIRE(planes.size() == 2);
        CHECK(planes[0].entity == 5);
        CHECK(planes[1].entity == 20);
        CHECK(planes[0].classification == (census.semanticLabels ? XRPlaneClassification::Platform : XRPlaneClassification::Unknown));
        CHECK(planes[1].classification == XRPlaneClassification::Unknown);
        CHECK(planes[1].confidence == XRTrackingConfidence::High);
        CHECK(planes[1].centerMeters.z == 3.0F);
        CHECK(planes[1].extentMeters.y == 5.0F);
        const auto normal = planes[1].orientation.Rotate({0.0F, 1.0F, 0.0F});
        CHECK(normal.x == Catch::Approx(0.0F).margin(0.00001F));
        CHECK(normal.y == Catch::Approx(0.0F).margin(0.00001F));
        CHECK(normal.z == Catch::Approx(1.0F));
        CHECK(fixture.labelCalls == static_cast<std::uint32_t>(census.semanticLabels));
    }

    TEST_CASE("OpenXR spatial snapshots reject invalid native batches and remove untracked geometry", "[unit][xr][native][planes]") {
        PlaneSnapshotFixture fixture;
        const auto snapshot = NativeHandle<XrSpatialSnapshotEXT>(1);
        const auto read = [&](std::uint32_t capacity = 4) {
            return ReadNativeSpatialPlanes(snapshot, &PlaneSnapshotFixture::Query, {XRCapabilityState::Available, true}, capacity);
        };
        SECTION("native oversized batch is actionable") {
            RequireFailureIdentity(read(2), XRErrors::CapacityExceeded);
        }
        SECTION("duplicated native identity") {
            fixture.duplicate = true;
            RequireFailureIdentity(read(), XRErrors::TrackingSnapshotInvalid);
        }
        SECTION("inconsistent native counts") {
            fixture.countMismatch = true;
            RequireFailureIdentity(read(), XRErrors::TrackingSnapshotInvalid);
        }
        SECTION("unknown labeled plane in the same snapshot") {
            fixture.staleLabel = true;
            RequireFailureIdentity(read(), XRErrors::TrackingSnapshotInvalid);
        }
        SECTION("malformed native rectangle") {
            fixture.malformed = true;
            RequireFailureIdentity(read(), XRErrors::TrackingSnapshotInvalid);
        }
        SECTION("snapshot lost while querying") {
            fixture.result = XR_SESSION_LOSS_PENDING;
            RequireFailureIdentity(read(), XRErrors::IdentityStale);
        }
        SECTION("paused and stopped evidence disappears without inspecting its geometry") {
            fixture.noTracking = true;
            fixture.malformed = true;
            const auto result = read();
            REQUIRE(result.HasValue());
            CHECK(result.Value().Planes().empty());
            CHECK(fixture.calls == 1);
        }
    }

    TEST_CASE("OpenXR plane capture owns asynchronous work and publishes only completed authorized scopes", "[unit][xr][native][planes]") {
        using enum XRCapabilityState;
        CensusFixture fixture;
        SpatialCaptureScript script;
        PlaneSnapshotFixture geometry;
        constexpr std::array<std::string_view, 3> extensions{XR_EXT_FUTURE_EXTENSION_NAME, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME,
                                                             XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME};
        const std::array routes{NativeExtensionRequest{extensions[0]}, NativeExtensionRequest{extensions[1]},
                                NativeExtensionRequest{extensions[2]}};
        fixture.native.extraExtensions = extensions;
        fixture.native.extensionCount += static_cast<std::uint32_t>(extensions.size());
        fixture.census.extraProc = &SpatialCaptureScript::GetProc;
        const auto capabilities = Capabilities();
        const auto plan = Plan(capabilities);
        const XRSessionId session{capabilities.System(), {1, 1}};
        const auto attempt = fixture.preflight.Attempt();
        NativeSessionRequest request{.candidate = session,
                                     .preflight = fixture.preflight,
                                     .capabilities = capabilities,
                                     .plan = plan,
                                     .activeAttempt = attempt,
                                     .extensions = routes};
        REQUIRE(fixture.owner.Create(request).HasValue());
        const XRPlaneScope scope{session,
                                 {{session, {2, 1}}, XRSpaceKind::World, IdentityValue<XRWorldOriginRevision>(1)},
                                 IdentityValue<XRPlaneGrant>(1)};
        const XRPlaneLimits limits{4, Duration::FromNanoseconds(10), Duration::FromNanoseconds(30)};
        OpenXRPlaneCapture capture{fixture.owner};
        const auto time = [](const std::int64_t tick) {
            return XRRuntimeTime::Create(tick).Value();
        };
        const auto world = NativeHandle<XrSpace>(4);
        const auto advance = [&](const std::int64_t tick) {
            return capture.Advance(scope, world, time(tick));
        };
        const auto noPublication = [&](const std::int64_t tick) {
            const auto result = advance(tick);
            REQUIRE(result.HasValue());
            CHECK_FALSE(result.Value());
        };
        const auto publish = [&](const std::int64_t tick) {
            const auto result = advance(tick);
            REQUIRE(result.HasValue());
            CHECK(result.Value());
            const auto snapshot = capture.Read(scope, time(tick));
            REQUIRE(snapshot.HasValue());
            return snapshot.Value();
        };
        REQUIRE(capture.Start(attempt, scope, limits, time(100)).HasValue());
        SECTION("timed-out context cancels its one outstanding future") {
            RequireFailureIdentity(advance(131), XRErrors::TrackingSnapshotStale);
            CHECK(script.cancels == 1);
        }
        SECTION("context and discoveries use the same exact World grant") {
            noPublication(102);
            script.ready = true;
            noPublication(103);
            noPublication(109);
            CHECK(script.scans == 0);
            noPublication(110);
            CHECK(script.scans == 1);
            const auto first = publish(111);
            REQUIRE(first.Planes().size() == 2);
            const auto original = first.Planes()[0].id;
            CHECK(first.Planes()[0].classification == XRPlaneClassification::Platform);
            CHECK_FALSE(script.hasSnapshot);
            noPublication(121);

            SECTION("paused world observations expire and recovered entities use new portable generations") {
                geometry.noTracking = true;
                CHECK(publish(122).Planes().empty());
                geometry.noTracking = false;
                noPublication(132);
                const auto recovered = publish(133);
                REQUIRE(recovered.Planes().size() == 2);
                CHECK(recovered.Planes()[0].id != original);
            }
            SECTION("native permission loss immediately revokes the previously published result") {
                script.completeResult = XR_ERROR_PERMISSION_INSUFFICIENT;
                RequireFailureIdentity(advance(122), XRErrors::PermissionDenied);
                RequireFailureIdentity(capture.Read(scope, time(122)), XRErrors::OperationUnavailable);
            }
            SECTION("recenter rejects the old grant and retirement retains retryable native debt") {
                auto replacement = scope;
                replacement.space.worldOriginRevision = IdentityValue<XRWorldOriginRevision>(2);
                RequireFailureIdentity(capture.Advance(replacement, world, time(122)), XRErrors::IdentityStale);
                RequireFailureIdentity(capture.Read(scope, time(122)), XRErrors::OperationUnavailable);
                script.failDestroy = true;
                RequireFailureIdentity(capture.Close(), XRErrors::OperationUnavailable);
                CHECK(script.cancels == 1);
                CHECK(script.hasContext);
                script.failDestroy = false;
            }
        }
        REQUIRE(capture.Close().HasValue());
        CHECK_FALSE(script.hasContext);
        CHECK_FALSE(script.hasSnapshot);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR reprojection census observes advertised routes without enabling them or issuing sizing calls",
              "[unit][xr][native][reprojection]") {
        CensusFixture fixture;
        NativeReprojectionRoute expected = NativeReprojectionRoute::None;
        SECTION("no route") {}
        SECTION("legacy native offer") {
            fixture.native.spaceWarpExtension = true;
            expected = NativeReprojectionRoute::SpaceWarpFb;
        }
        SECTION("standard offer preferred with both advertised") {
            fixture.native.spaceWarpExtension = true;
            fixture.native.synthesisExtension = true;
            expected = NativeReprojectionRoute::FrameSynthesisExt;
        }
        fixture.native.extensionCount += fixture.native.spaceWarpExtension + fixture.native.synthesisExtension;
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        const auto callCount = fixture.census.calls.size();
        const auto offer = fixture.owner.DiscoverReprojection(fixture.preflight.Attempt());
        REQUIRE(offer.HasValue());
        CHECK(offer.Value().route == expected);
        CHECK(offer.Value().availabilityWithoutProducer ==
              (expected == NativeReprojectionRoute::None ? XRCapabilityState::Unsupported : XRCapabilityState::DependencyMissing));
        CHECK_FALSE(offer.Value().extents[0].IsValid());
        CHECK(fixture.census.calls.size() == callCount);
        CHECK(std::ranges::find(fixture.native.enabledExtensions, XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME) ==
              fixture.native.enabledExtensions.end());
        CHECK(std::ranges::find(fixture.native.enabledExtensions, XR_FB_SPACE_WARP_EXTENSION_NAME) ==
              fixture.native.enabledExtensions.end());
        REQUIRE(fixture.owner.DiscoverSystem(fixture.preflight.Attempt()).HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.owner.DiscoverReprojection(fixture.preflight.Attempt()).HasError());
    }

    TEST_CASE("OpenXR enabled synthesis census checks native sizing without claiming renderer support",
              "[unit][xr][native][reprojection]") {
        const bool extension = GENERATE(false, true);
        CensusFixture fixture;
        fixture.native.synthesisExtension = extension;
        fixture.native.spaceWarpExtension = !extension;
        fixture.native.extensionCount = 2;
        const std::array routes{
            NativeExtensionRequest{extension ? XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME : XR_FB_SPACE_WARP_EXTENSION_NAME}};
        auto discovery = fixture.Discovery();
        discovery.extensions = routes;
        REQUIRE(fixture.owner.PrepareDiscovery(discovery).HasValue());
        const auto offer = fixture.owner.DiscoverReprojection(fixture.preflight.Attempt());
        REQUIRE(offer.HasValue());
        CHECK(offer.Value().route == (extension ? NativeReprojectionRoute::FrameSynthesisExt : NativeReprojectionRoute::SpaceWarpFb));
        CHECK(offer.Value().version == 1);
        CHECK(offer.Value().availabilityWithoutProducer == XRCapabilityState::DependencyMissing);
        CHECK(offer.Value().extents[0] == Render::FramebufferExtent{640, 512});
        CHECK(offer.Value().extents[1] == Render::FramebufferExtent{extension ? 800U : 640U, 512});
        fixture.census.invalidReprojectionExtent = true;
        const auto incompatible = fixture.owner.DiscoverReprojection(fixture.preflight.Attempt());
        REQUIRE(incompatible.HasValue());
        CHECK(incompatible.Value().availabilityWithoutProducer == XRCapabilityState::Incompatible);
        CHECK_FALSE(incompatible.Value().extents[0].IsValid());
        CHECK_FALSE(incompatible.Value().extents[1].IsValid());
        REQUIRE(fixture.owner.DiscoverSystem(fixture.preflight.Attempt()).HasValue());
        fixture.census.fail = "system-properties";
        const auto unavailable = fixture.owner.DiscoverReprojection(fixture.preflight.Attempt());
        REQUIRE(unavailable.HasValue());
        CHECK(unavailable.Value().availabilityWithoutProducer == XRCapabilityState::TemporarilyUnavailable);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR session-dependent census runs only on the actual unpublished prepared session", "[unit][xr][native]") {
        CensusFixture fixture;
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        REQUIRE(fixture.owner.PrepareSession(fixture.Preparation()).HasValue());
        const auto actual = fixture.owner.DiscoverSession(fixture.Preparation());
        REQUIRE(actual.HasValue());
        CHECK(actual.Value().spaceCount == 2);
        CHECK(actual.Value().spaces[0] == XR_REFERENCE_SPACE_TYPE_VIEW);
        CHECK(actual.Value().spaces[1] == XR_REFERENCE_SPACE_TYPE_LOCAL);
        CHECK(actual.Value().formatCount == 2);
        CHECK(actual.Value().formats[0] == 37);
        CHECK_FALSE(fixture.owner.Session().IsValid());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR native census rejects oversized malformed or duplicate publications without truncation", "[unit][xr][native]") {
        CensusFixture fixture;
        SECTION("too many views") {
            fixture.census.views = XRHardLimits::MaximumViews + 1;
        }
        SECTION("malformed view limits") {
            fixture.census.invalidView = true;
        }
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        CHECK(fixture.owner.DiscoverSystem(fixture.preflight.Attempt()).HasError());
        CHECK(fixture.native.liveSessions == 0);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR unpublished session census failures never grant Ready or lose native ownership", "[unit][xr][native]") {
        CensusFixture fixture;
        SECTION("no spaces") {
            fixture.census.spaces = 0;
        }
        SECTION("oversized formats") {
            fixture.census.formats = 129;
        }
        SECTION("duplicate spaces") {
            fixture.census.duplicateSpaces = true;
        }
        SECTION("native failure") {
            fixture.census.fail = "spaces";
        }
        REQUIRE(fixture.owner.PrepareDiscovery(fixture.Discovery()).HasValue());
        REQUIRE(fixture.owner.PrepareSession(fixture.Preparation()).HasValue());
        CHECK(fixture.owner.DiscoverSession(fixture.Preparation()).HasError());
        CHECK_FALSE(fixture.owner.Session().IsValid());
        CHECK(fixture.native.liveSessions == 1);
        CHECK(fixture.native.liveInstances == 1);
        REQUIRE(fixture.owner.Close().HasValue());
        CHECK(fixture.native.liveSessions == 0);
        CHECK(fixture.native.liveInstances == 0);
    }
}  // namespace Horo::Tests::OpenXR
