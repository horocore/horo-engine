#include "support/OpenXRNativeTestSupport.h"

#include <bit>

namespace Horo::Tests::OpenXR {
    namespace {
        struct CensusScript {
            static inline CensusScript *active{};
            std::uint32_t views{2};
            std::uint32_t spaces{2};
            std::uint32_t formats{2};
            bool duplicateSpaces{};
            bool invalidView{};
            std::string_view fail;
            std::vector<std::string_view> calls;

            CensusScript() {
                REQUIRE(active == nullptr);
                active = this;
            }

            ~CensusScript() {
                active = nullptr;
            }

            static XrResult Record(const std::string_view phase) {
                active->calls.push_back(phase);
                return active->fail == phase ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Instance(XrInstance, XrInstanceProperties *value) {
                CopyNativeLiteral(value->runtimeName, "Actual scripted runtime");
                value->runtimeVersion = XR_MAKE_VERSION(9, 8, 7);
                return Record("instance-properties");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL System(XrInstance, XrSystemId system, XrSystemProperties *value) {
                value->systemId = system;
                CopyNativeLiteral(value->systemName, "Observed native system");
                value->graphicsProperties = {2048, 2048, 16};
                value->trackingProperties = {XR_TRUE, XR_TRUE};
                return Record("system-properties");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Configurations(XrInstance, XrSystemId, const std::uint32_t capacity, std::uint32_t *count,
                                                                 XrViewConfigurationType *values) {
                *count = 1;
                if (capacity)
                    values[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                return Record("configurations");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Views(XrInstance, XrSystemId, XrViewConfigurationType, const std::uint32_t capacity,
                                                        std::uint32_t *count, XrViewConfigurationView *values) {
                *count = active->views;
                if (capacity && capacity < *count)
                    return XR_ERROR_SIZE_INSUFFICIENT;
                if (capacity)
                    for (std::uint32_t index = 0; index < *count; ++index) {
                        values[index].recommendedImageRectWidth = active->invalidView ? 4096 : 1024;
                        values[index].maxImageRectWidth = 2048;
                        values[index].recommendedImageRectHeight = 1024;
                        values[index].maxImageRectHeight = 2048;
                        values[index].recommendedSwapchainSampleCount = 1;
                        values[index].maxSwapchainSampleCount = 4;
                    }
                return Record("views");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL BlendModes(XrInstance, XrSystemId, XrViewConfigurationType, const std::uint32_t capacity,
                                                             std::uint32_t *count, XrEnvironmentBlendMode *values) {
                *count = 1;
                if (capacity)
                    values[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                return Record("blend-modes");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Spaces(XrSession session, const std::uint32_t capacity, std::uint32_t *count,
                                                         XrReferenceSpaceType *values) {
                CHECK(Script::active->liveSessions == 1);
                CHECK(session != XR_NULL_HANDLE);
                *count = active->spaces;
                if (capacity && capacity < *count)
                    return XR_ERROR_SIZE_INSUFFICIENT;
                if (capacity && *count > 0) {
                    values[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
                    if (*count > 1)
                        values[1] = active->duplicateSpaces ? XR_REFERENCE_SPACE_TYPE_VIEW : XR_REFERENCE_SPACE_TYPE_LOCAL;
                }
                return Record("spaces");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL Formats(XrSession session, const std::uint32_t capacity, std::uint32_t *count,
                                                          std::int64_t *values) {
                CHECK(Script::active->liveSessions == 1);
                CHECK(session != XR_NULL_HANDLE);
                *count = active->formats;
                if (capacity && capacity < *count)
                    return XR_ERROR_SIZE_INSUFFICIENT;
                if (capacity)
                    for (std::uint32_t index = 0; index < *count; ++index)
                        values[index] = 37 + index;
                return Record("formats");
            }

            static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
                struct Entry {
                    std::string_view name;
                    PFN_xrVoidFunction function;
                };

                const std::array entries{Entry{"xrGetInstanceProperties", std::bit_cast<PFN_xrVoidFunction>(&Instance)},
                                         Entry{"xrGetSystemProperties", std::bit_cast<PFN_xrVoidFunction>(&System)},
                                         Entry{"xrEnumerateViewConfigurations", std::bit_cast<PFN_xrVoidFunction>(&Configurations)},
                                         Entry{"xrEnumerateViewConfigurationViews", std::bit_cast<PFN_xrVoidFunction>(&Views)},
                                         Entry{"xrEnumerateEnvironmentBlendModes", std::bit_cast<PFN_xrVoidFunction>(&BlendModes)},
                                         Entry{"xrEnumerateReferenceSpaces", std::bit_cast<PFN_xrVoidFunction>(&Spaces)},
                                         Entry{"xrEnumerateSwapchainFormats", std::bit_cast<PFN_xrVoidFunction>(&Formats)}};
                const auto found = std::ranges::find(entries, std::string_view{name}, &Entry::name);
                if (found == entries.end())
                    return Script::GetProc(instance, name, function);
                *function = found->function;
                return XR_SUCCESS;
            }
        };

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
