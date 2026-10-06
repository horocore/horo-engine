#pragma once

#include "OpenXRNativeSession.h"
#include "support/TypedIdentityTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace Horo::Tests::OpenXR {
    using namespace Horo::XR;
    using namespace Horo::XR::OpenXRInternal;

    template <typename Handle> Handle NativeHandle(std::uintptr_t value) {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<Handle>(value);
        else
            return static_cast<Handle>(value);
    }

    struct Script final {
        static inline thread_local Script *active{};
        std::string_view fail;
        XrResult failureResult{XR_ERROR_RUNTIME_FAILURE};
        std::string_view absent;
        std::string_view instanceDispatchAbsent;
        std::vector<std::string_view> calls;
        std::uint32_t extensionCount{1};
        bool malformedExtension{};
        bool advertiseLayer{};
        bool enabledLayer{};
        bool failRetirement{};
        bool stale{};
        bool staleAfterSession{};
        bool throwAfterInstance{};
        std::uint32_t liveInstances{};
        std::uint32_t liveSessions{};

        Script() {
            REQUIRE(active == nullptr);
            active = this;
        }

        ~Script() {
            active = nullptr;
        }

        static XrResult Record(std::string_view name) {
            active->calls.push_back(name);
            return active->fail == name ? active->failureResult : XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Layers(std::uint32_t capacity, std::uint32_t *count, XrApiLayerProperties *properties) {
            *count = active->advertiseLayer ? 1U : 0U;
            if (capacity && *count) {
                std::strcpy(properties[0].layerName, "XR_APILAYER_TEST");
                properties[0].specVersion = XR_MAKE_VERSION(1, 1, 0);
                properties[0].layerVersion = 1;
            }
            return Record("layers");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Extensions(const char *layer, std::uint32_t capacity, std::uint32_t *count,
                                                         XrExtensionProperties *properties) {
            *count = active->extensionCount;
            if (capacity && *count <= capacity) {
                for (std::uint32_t index = 0; index < *count; ++index) {
                    std::strcpy(properties[index].extensionName, layer ? "XR_TEST_layer_extension" : "XR_KHR_vulkan_enable2");
                    properties[index].extensionVersion = 1;
                    if (active->malformedExtension)
                        properties[index].type = XR_TYPE_UNKNOWN;
                }
            }
            return Record("extensions");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance) {
            active->enabledLayer = info->enabledApiLayerCount == 1 && std::string_view{info->enabledApiLayerNames[0]} == "XR_APILAYER_TEST";
            const auto result = Record("instance");
            if (XR_SUCCEEDED(result)) {
                *instance = NativeHandle<XrInstance>(++active->liveInstances);
            }
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL DestroyInstance(XrInstance) {
            const auto result = Record("destroy-instance");
            if (XR_SUCCEEDED(result))
                --active->liveInstances;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL System(XrInstance, const XrSystemGetInfo *, XrSystemId *system) {
            const auto result = Record("system");
            if (XR_SUCCEEDED(result))
                *system = 1;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo *, XrSession *session) {
            const auto result = Record("session");
            if (XR_SUCCEEDED(result)) {
                *session = NativeHandle<XrSession>(++active->liveSessions);
                if (active->staleAfterSession)
                    active->stale = true;
            }
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL DestroySession(XrSession) {
            const auto result = Record("destroy-session");
            if (active->failRetirement)
                return XR_ERROR_RUNTIME_FAILURE;
            if (XR_SUCCEEDED(result))
                --active->liveSessions;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
            *function = nullptr;
            if (active->absent == name || (instance != XR_NULL_HANDLE && active->instanceDispatchAbsent == name))
                return XR_ERROR_FUNCTION_UNSUPPORTED;

            struct DispatchEntry {
                std::string_view name;
                PFN_xrVoidFunction function;
            };

            const std::array dispatch{DispatchEntry{"xrEnumerateApiLayerProperties", reinterpret_cast<PFN_xrVoidFunction>(&Layers)},
                                      DispatchEntry{"xrEnumerateInstanceExtensionProperties",
                                                    reinterpret_cast<PFN_xrVoidFunction>(&Extensions)},
                                      DispatchEntry{"xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction>(&CreateInstance)},
                                      DispatchEntry{"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(&DestroyInstance)},
                                      DispatchEntry{"xrGetSystem", reinterpret_cast<PFN_xrVoidFunction>(&System)},
                                      DispatchEntry{"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(&CreateSession)},
                                      DispatchEntry{"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(&DestroySession)}};
            const auto entry = std::ranges::find(dispatch, std::string_view{name}, &DispatchEntry::name);
            if (entry == dispatch.end())
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            *function = entry->function;
            return XR_SUCCESS;
        }
    };

    class Library final : public Platform::DynamicLibrary {
    public:
        bool *destroyed{};

        explicit Library(bool *retired = nullptr) : destroyed(retired) {}

        ~Library() override {
            if (destroyed)
                *destroyed = true;
        }

        void *GetSymbol(std::string_view name) const noexcept override {
            if (Script::active->absent == name)
                return nullptr;
            if (name == "xrGetInstanceProcAddr")
                return reinterpret_cast<void *>(&Script::GetProc);
            if (name == "xrDestroyInstance")
                return reinterpret_cast<void *>(&Script::DestroyInstance);
            return nullptr;
        }
    };

    class Graphics final : public IOpenXRGraphicsBinding {
    public:
        XrBaseInStructure binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr};

        const char *RequiredExtension() const noexcept override {
            return "XR_KHR_vulkan_enable2";
        }

        Result<void> Validate(const XRSessionId &) const override {
            return Script::active->stale ? Result<void>::Failure(MakeError(XRErrors::CapabilityStale)) : Result<void>::Success();
        }

        Result<const XrBaseInStructure *> Prepare(XrInstance, XrSystemId, PFN_xrGetInstanceProcAddr) override {
            if (Script::active->throwAfterInstance)
                throw std::runtime_error("injected private graphics failure");
            const auto result = Script::Record("graphics");
            return XR_FAILED(result) ? Result<const XrBaseInStructure *>::Failure(MakeError(XRErrors::OperationUnavailable))
                                     : Result<const XrBaseInStructure *>::Success(&binding);
        }

        void Release() noexcept override {
            Script::active->calls.push_back("release-graphics");
        }
    };

    class Fence final : public IOpenXRCompositionFence {
    public:
        Result<void> Validate(const XRSessionId &, const XRLoaderPreflightSnapshot &, const XRFeaturePlan &) const override {
            return Script::active->stale ? Result<void>::Failure(MakeError(XRErrors::CapabilityStale)) : Result<void>::Success();
        }
    };

    inline XRLoaderPreflightSnapshot Preflight() {
        const XRLoaderPreflightRequest request{.attempt = IdentityValue<XRLoaderPreflightAttempt>(1),
                                               .backend = IdentityValue<XRBackendId>(2),
                                               .installRecord = IdentityValue<XRInstallRecordId>(3),
                                               .productProfile = IdentityValue<XRProductProfileId>(4),
                                               .admittedLoaderVersions = {{1, 0, 0}, {1, 1, 63}}};
        const XRLoaderProbeEvidence evidence{.attempt = request.attempt,
                                             .loader = XRLoaderAvailability::Available,
                                             .runtime = XRRuntimeAvailability::Available,
                                             .system = XRSystemAvailability::Supported,
                                             .loaderApiVersion = {1, 1, 0},
                                             .runtimeGeneration = IdentityValue<XRRuntimeGeneration>(1),
                                             .consumedProbeSteps = 3};
        const auto result = CreateXRLoaderPreflightSnapshot(request, evidence);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    inline XRCapabilitySnapshot Capabilities(std::uint32_t maximumActions = 16) {
        XRCapabilityDescriptor
            descriptor{.system = {.runtime = IdentityValue<XRRuntimeGeneration>(1), .slot = {.index = 1, .generation = 1}},
                       .revision = IdentityValue<XRCapabilityRevision>(1),
                       .limits = {.maximumViews = 2, .maximumSpaces = 8, .maximumActions = maximumActions, .maximumDevices = 4}};
        descriptor.states.fill(XRCapabilityState::Available);
        const auto result = XRCapabilitySnapshot::Create(descriptor);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    inline XRFeaturePlan Plan(const XRCapabilitySnapshot &capabilities) {
        const auto result =
            NegotiateXRFeatures(capabilities, capabilities.System(), capabilities.Revision(),
                                {.profile = XRFeatureProfile::Projection1_0,
                                 .requestedLimits = {.maximumViews = 2, .maximumSpaces = 2, .maximumActions = 8, .maximumDevices = 1}});
        REQUIRE(result.plan.has_value());
        return *result.plan;
    }

    struct Fixture final {
        Script script;
        Graphics graphics;
        Fence fence;
        XRLoaderPreflightSnapshot preflight{Preflight()};
        XRCapabilitySnapshot capabilities{Capabilities()};
        XRFeaturePlan plan{Plan(capabilities)};
        std::shared_ptr<Library> library{std::make_shared<Library>()};
        OpenXRNativeSession owner{library, graphics, fence};

        NativeSessionRequest Request(std::uint32_t generation = 1) const {
            return {.candidate = {capabilities.System(), {.index = 1, .generation = generation}},
                    .preflight = preflight,
                    .capabilities = capabilities,
                    .plan = plan,
                    .activeAttempt = preflight.Attempt()};
        }
    };
}  // namespace Horo::Tests::OpenXR
