#include "support/OpenXRCensusTestSupport.h"

#include <bit>

namespace Horo::Tests::OpenXR {
    namespace {

        class CensusLibrary final : public Platform::DynamicLibrary {
        public:
            void *GetSymbol(const std::string_view name) const noexcept override {
                if (name == "xrGetInstanceProcAddr")
                    return std::bit_cast<void *>(&CensusScript::GetProc);
                if (name == "xrDestroyInstance")
                    return std::bit_cast<void *>(&Script::DestroyInstance);
                return nullptr;
            }
        };

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
