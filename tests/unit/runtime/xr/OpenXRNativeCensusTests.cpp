#include "support/OpenXRCensusTestSupport.h"

#include <bit>
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
