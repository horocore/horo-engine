#include "OpenXRPlaneSnapshot.h"
#include "support/OpenXRCensusTestSupport.h"

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

        struct PlaneSnapshotFixture final {
            static inline PlaneSnapshotFixture *current{};
            XrResult result{XR_SUCCESS};
            bool duplicate{};
            bool staleLabel{};
            bool malformed{};
            bool countMismatch{};
            bool noTracking{};
            std::uint32_t calls{};
            std::uint32_t labelCalls{};

            PlaneSnapshotFixture() {
                current = this;
            }

            ~PlaneSnapshotFixture() {
                current = nullptr;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Query(XrSpatialSnapshotEXT snapshot,
                                                        const XrSpatialComponentDataQueryConditionEXT *condition,
                                                        XrSpatialComponentDataQueryResultEXT *output) {
                REQUIRE(snapshot != XR_NULL_HANDLE);
                REQUIRE(condition->componentTypes[0] == XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT);
                ++current->calls;
                if (current->result != XR_SUCCESS)
                    return current->result;
                if (condition->componentTypeCount == 2) {
                    ++current->labelCalls;
                    REQUIRE(condition->componentTypes[1] == XR_SPATIAL_COMPONENT_TYPE_PLANE_SEMANTIC_LABEL_EXT);
                    auto &labels = *static_cast<XrSpatialComponentPlaneSemanticLabelListEXT *>(output->next);
                    REQUIRE(labels.semanticLabelCount >= 1);
                    output->entityIdCountOutput = output->entityStateCountOutput = 1;
                    output->entityIds[0] = current->staleLabel ? 42 : 5;
                    output->entityStates[0] = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
                    labels.semanticLabels[0] = XR_SPATIAL_PLANE_SEMANTIC_LABEL_TABLE_EXT;
                    return XR_SUCCESS;
                }
                auto &rectangles = *static_cast<XrSpatialComponentBounded2DListEXT *>(output->next);
                if (output->entityIdCapacityInput < 3)
                    return XR_ERROR_SIZE_INSUFFICIENT;
                REQUIRE(rectangles.boundCount == output->entityIdCapacityInput);
                output->entityIdCountOutput = 3;
                output->entityStateCountOutput = current->countMismatch ? 2 : 3;
                output->entityIds[0] = 20;
                output->entityIds[1] = current->duplicate ? 20 : 11;
                output->entityIds[2] = 5;
                output->entityStates[0] =
                    current->noTracking ? XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT : XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
                output->entityStates[1] = XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT;
                output->entityStates[2] = output->entityStates[0];
                rectangles.bounds[0] = {{.orientation = {0, 0, 0, 1}, .position = {1, 2, 3}}, {4, 5}};
                rectangles.bounds[2] = {{.orientation = {0, 0, 0, 1}, .position = {7, 8, 9}}, {current->malformed ? 0.0F : 4.0F, 5}};
                return XR_SUCCESS;
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
