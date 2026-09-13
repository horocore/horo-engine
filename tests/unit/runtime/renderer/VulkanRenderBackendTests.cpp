#include "Horo/Runtime/Render/RenderFrontend.h"
#include "renderer/RenderBackendContractSuite.h"
#include "runtime/renderer/modules/vulkan/VulkanBackendInternal.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    [[nodiscard]] Error Failure(const char *code) {
        return Error{ErrorCode{code}, ErrorDomainId{"horo.render.test"}, ErrorSeverity::Critical, "Injected failure.", {}};
    }

    enum class FailureStage {
        None,
        AcquireLoader,
        QueryLoader,
        PlatformExtensions,
        CreateInstance,
        Enumerate,
        CreateDevice,
    };

    struct RuntimeState {
        int acquireLoaderCount{0};
        int queryLoaderCount{0};
        int platformExtensionCount{0};
        int createInstanceCount{0};
        int enumerateCount{0};
        std::uint32_t lastMaxAdapters{0};
        int createDeviceCount{0};
        int destroyDeviceCount{0};
        int destroyInstanceCount{0};
        int releaseLoaderCount{0};
        FailureStage failure{FailureStage::None};
        VulkanLoaderCapabilities loader{
            .apiVersion = {1, 3, 280},
            .instanceExtensions = {"VK_EXT_debug_utils", "VK_KHR_surface", "VK_KHR_xcb_surface"},
            .layers = {"VK_LAYER_KHRONOS_validation"},
            .hasInstanceVersionQuery = true,
            .standardApiVariant = true,
        };
        std::vector<std::string> platformExtensions{"VK_KHR_xcb_surface"};
        VulkanAdapterEnumeration enumeration;
        VulkanInstanceRequest instanceRequest;
        VulkanDeviceRequest deviceRequest;
        std::vector<std::string> lifecycle;
    };

    [[nodiscard]] VulkanAdapterCandidate Adapter(std::string id, std::vector<VulkanQueueFamily> queues,
                                                 RenderAdapterKind kind = RenderAdapterKind::Discrete) {
        return VulkanAdapterCandidate{
            .id = RenderAdapterId{std::move(id)},
            .displayName = "Test Vulkan device",
            .kind = kind,
            .dedicatedVideoMemoryBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL,
            .apiVersion = {1, 3, 0},
            .requiredFeatures = {.dynamicRendering = true, .synchronization2 = true, .timelineSemaphore = true},
            .deviceExtensions = {"VK_KHR_swapchain"},
            .queueFamilies = std::move(queues),
        };
    }

    class FakeRuntimePort final : public IVulkanRuntimePort {
    public:
        explicit FakeRuntimePort(RuntimeState &state) noexcept : state_(&state) {}

        Result<void> AcquireLoader() override {
            ++state_->acquireLoaderCount;
            state_->lifecycle.emplace_back("acquire-loader");
            if (state_->failure == FailureStage::AcquireLoader) {
                return Result<void>::Failure(Failure("render.test.acquire_loader"));
            }
            return Result<void>::Success();
        }

        Result<VulkanLoaderCapabilities> QueryLoaderCapabilities() override {
            ++state_->queryLoaderCount;
            if (state_->failure == FailureStage::QueryLoader) {
                return Result<VulkanLoaderCapabilities>::Failure(Failure("render.test.query_loader"));
            }
            return Result<VulkanLoaderCapabilities>::Success(state_->loader);
        }

        Result<std::vector<std::string>> RequiredPresentationExtensions() override {
            ++state_->platformExtensionCount;
            if (state_->failure == FailureStage::PlatformExtensions) {
                return Result<std::vector<std::string>>::Failure(Failure("render.test.platform_extensions"));
            }
            return Result<std::vector<std::string>>::Success(state_->platformExtensions);
        }

        Result<void> CreateInstance(const VulkanInstanceRequest &request) override {
            ++state_->createInstanceCount;
            state_->instanceRequest = request;
            state_->lifecycle.emplace_back("create-instance");
            if (state_->failure == FailureStage::CreateInstance) {
                return Result<void>::Failure(Failure("render.test.create_instance"));
            }
            return Result<void>::Success();
        }

        Result<VulkanAdapterEnumeration> EnumeratePhysicalDevices(std::uint32_t maxAdapters) override {
            ++state_->enumerateCount;
            state_->lastMaxAdapters = maxAdapters;
            if (state_->failure == FailureStage::Enumerate) {
                return Result<VulkanAdapterEnumeration>::Failure(Failure("render.test.enumerate"));
            }
            return Result<VulkanAdapterEnumeration>::Success(state_->enumeration);
        }

        Result<void> CreateDevice(const VulkanDeviceRequest &request) override {
            ++state_->createDeviceCount;
            state_->deviceRequest = request;
            state_->lifecycle.emplace_back("create-device");
            if (state_->failure == FailureStage::CreateDevice) {
                return Result<void>::Failure(Failure("render.test.create_device"));
            }
            return Result<void>::Success();
        }

        void DestroyDevice() noexcept override {
            ++state_->destroyDeviceCount;
            state_->lifecycle.emplace_back("destroy-device");
        }

        void DestroyInstance() noexcept override {
            ++state_->destroyInstanceCount;
            state_->lifecycle.emplace_back("destroy-instance");
        }

        void ReleaseLoader() noexcept override {
            ++state_->releaseLoaderCount;
            state_->lifecycle.emplace_back("release-loader");
        }

    private:
        RuntimeState *state_{nullptr};
    };

    class MissingDispatchLoaderPort final : public IVulkanLoaderPort {
    public:
        Result<void> AcquireLoader() override {
            ++acquireCount;
            return Result<void>::Success();
        }

        PFN_vkGetInstanceProcAddr GetInstanceProcAddr() noexcept override {
            return nullptr;
        }

        Result<std::vector<std::string>> RequiredPresentationExtensions() override {
            return Result<std::vector<std::string>>::Success({});
        }

        Result<bool> SupportsPresentation(VkPhysicalDevice, std::uint32_t) override {
            return Result<bool>::Success(false);
        }

        Result<bool> IsDriverAllowed(const VkPhysicalDeviceProperties &) override {
            return Result<bool>::Success(true);
        }

        void ReleaseLoader() noexcept override {
            ++releaseCount;
        }

        int acquireCount{0};
        int releaseCount{0};
    };

    [[nodiscard]] RuntimeState DefaultState() {
        RuntimeState state;
        state.enumeration = VulkanAdapterEnumeration{
            .revision = 7,
            .adapters = {Adapter("gpu-a", {{0, true, true}})},
        };
        return state;
    }

    [[nodiscard]] std::unique_ptr<IRenderBackend> CreateBackend(FakeRuntimePort &port) {
        RenderBackendRegistry registry;
        REQUIRE(Detail::RegisterVulkanRenderBackendWithRuntimePort(registry, port).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto backend = registry.Create(RenderBackendId{"vulkan"});
        REQUIRE(backend.HasValue());
        return std::move(backend).Value();
    }

    [[nodiscard]] Result<void> InitializeBackend(RuntimeState &state, const RenderBackendConfig &config = {}) {
        FakeRuntimePort port{state};
        return CreateBackend(port)->Initialize(config);
    }

    void RequireInitializationError(RuntimeState &state, const RenderBackendConfig &config, const std::string_view expectedCode) {
        const auto result = InitializeBackend(state, config);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == expectedCode);
    }

    TEST_CASE("Vulkan registration is inert and initialization enables the exact baseline", "[unit][runtime][renderer][vulkan]") {
        RuntimeState state = DefaultState();
        FakeRuntimePort port{state};
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port);
        REQUIRE(state.acquireLoaderCount == 0);

        REQUIRE(backend
                    ->Initialize(RenderBackendConfig{
                        .requirePresentation = true,
                        .enableValidation = true,
                        .maxFramesInFlight = 2,
                        .presentMode = PresentMode::Fifo,
                    })
                    .HasValue());
        REQUIRE(state.instanceRequest.apiVersion == VulkanApiVersion{1, 3, 0});
        REQUIRE(state.instanceRequest.enableValidation);
        REQUIRE(state.instanceRequest.enabledExtensions ==
                std::vector<std::string>{"VK_EXT_debug_utils", "VK_KHR_surface", "VK_KHR_xcb_surface"});
        REQUIRE(state.deviceRequest.adapter == RenderAdapterId{"gpu-a"});
        REQUIRE(state.deviceRequest.graphicsQueueFamily == 0);
        REQUIRE(state.deviceRequest.presentationQueueFamily == 0);
        REQUIRE(state.deviceRequest.enabledFeatures.AreAvailable());
        REQUIRE(state.deviceRequest.enabledExtensions == std::vector<std::string>{"VK_KHR_swapchain"});
        REQUIRE(state.lastMaxAdapters == 64);
        REQUIRE(backend->Capabilities().backend == RenderBackendId{"vulkan"});
        REQUIRE(backend->Capabilities().presentsToWindow);
        REQUIRE_FALSE(backend->Capabilities().supportsCompute);
        REQUIRE(backend->BeginFrame({.frameNumber = 1, .outputExtent = {1280, 720}}).ErrorValue().code.Value() ==
                "render.vulkan.unsupported_operation");

        backend->Shutdown();
        backend->Shutdown();
        REQUIRE(state.lifecycle == std::vector<std::string>{"acquire-loader", "create-instance", "create-device", "destroy-device",
                                                            "destroy-instance", "release-loader"});
    }

    TEST_CASE("Vulkan discovery publishes a bounded sorted backend-neutral snapshot", "[unit][runtime][renderer][vulkan]") {
        RuntimeState state = DefaultState();
        VulkanAdapterCandidate unavailable = Adapter("gpu-z", {{3, true, true}});
        unavailable.portabilitySubset = true;
        state.enumeration.adapters = {unavailable, Adapter("gpu-a", {{2, true, false}, {4, false, true}})};
        FakeRuntimePort port{state};
        std::unique_ptr<IRenderAdapterDiscovery> discovery = Detail::CreateVulkanAdapterDiscoveryWithRuntimePort(port);

        auto result = discovery->Discover({.maxAdapters = 4});
        REQUIRE(result.HasValue());
        REQUIRE(state.lastMaxAdapters == 4);
        REQUIRE(result.Value().revision == 7);
        REQUIRE(result.Value().adapters.size() == 2);
        REQUIRE(result.Value().adapters[0].id == RenderAdapterId{"gpu-a"});
        REQUIRE(result.Value().adapters[0].availability == RenderAdapterAvailability::Available);
        REQUIRE(result.Value().adapters[0].supportsPresentation);
        REQUIRE(result.Value().adapters[1].id == RenderAdapterId{"gpu-z"});
        REQUIRE(result.Value().adapters[1].availability == RenderAdapterAvailability::Unavailable);
        REQUIRE(state.destroyInstanceCount == 1);
        REQUIRE(state.releaseLoaderCount == 1);

        discovery->Stop();
        discovery->Stop();
        const auto stopped = discovery->Discover({});
        REQUIRE(stopped.HasError());
        REQUIRE(stopped.ErrorValue().code.Value() == "render.vulkan.discovery_stopped");
        REQUIRE(state.acquireLoaderCount == 1);
    }

    TEST_CASE("Vulkan discovery preserves an empty bounded snapshot", "[unit][runtime][renderer][vulkan]") {
        RuntimeState state = DefaultState();
        state.enumeration.adapters.clear();
        FakeRuntimePort port{state};
        std::unique_ptr<IRenderAdapterDiscovery> discovery = Detail::CreateVulkanAdapterDiscoveryWithRuntimePort(port);

        const auto result = discovery->Discover({.maxAdapters = 4});

        REQUIRE(result.HasValue());
        REQUIRE(result.Value().revision == 7);
        REQUIRE(result.Value().adapters.empty());
        REQUIRE(state.destroyInstanceCount == 1);
        REQUIRE(state.releaseLoaderCount == 1);
    }

    TEST_CASE("Vulkan production runtime fails closed when loader dispatch is absent", "[unit][runtime][renderer][vulkan]") {
        MissingDispatchLoaderPort loader;
        RenderBackendRegistry registry;
        REQUIRE(RegisterVulkanRenderBackend(registry, loader).HasValue());
        REQUIRE(loader.acquireCount == 0);
        REQUIRE(registry.Seal().HasValue());
        REQUIRE(loader.releaseCount == 0);
        auto created = registry.Create(RenderBackendId{"vulkan"});
        REQUIRE(created.HasValue());

        const auto result = created.Value()->Initialize({});

        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == "render.vulkan.entry_point_missing");
        REQUIRE(loader.acquireCount == 1);
        REQUIRE(loader.releaseCount == 1);
    }

    TEST_CASE("Vulkan loader and instance admission fail closed before native descendants", "[unit][runtime][renderer][vulkan]") {
        SECTION("missing version query") {
            RuntimeState state = DefaultState();
            state.loader.hasInstanceVersionQuery = false;
            RequireInitializationError(state, {}, "render.vulkan.loader_version_unsupported");
            REQUIRE(state.createInstanceCount == 0);
            REQUIRE(state.releaseLoaderCount == 1);
        }
        SECTION("loader below 1.3") {
            RuntimeState state = DefaultState();
            state.loader.apiVersion = {1, 2, 999};
            RequireInitializationError(state, {}, "render.vulkan.loader_version_unsupported");
        }
        SECTION("missing WSI extension") {
            RuntimeState state = DefaultState();
            state.loader.instanceExtensions.erase(state.loader.instanceExtensions.begin() + 2);
            RequireInitializationError(state, RenderBackendConfig{.requirePresentation = true}, "render.vulkan.instance_extension_missing");
            REQUIRE(state.createInstanceCount == 0);
        }
        SECTION("unsupported platform WSI extension") {
            RuntimeState state = DefaultState();
            state.platformExtensions = {"VK_KHR_android_surface"};
            RequireInitializationError(state, RenderBackendConfig{.requirePresentation = true},
                                       "render.vulkan.presentation_extension_unsupported");
            REQUIRE(state.createInstanceCount == 0);
        }
        SECTION("strict validation") {
            RuntimeState state = DefaultState();
            state.loader.layers.clear();
            RequireInitializationError(state, RenderBackendConfig{.enableValidation = true}, "render.vulkan.validation_unavailable");
        }
    }

    TEST_CASE("Vulkan adapter admission is deterministic and explicit selection never falls back", "[unit][runtime][renderer][vulkan]") {
        SECTION("canonical identity order selects an adapter before queue topology") {
            RuntimeState state = DefaultState();
            state.enumeration.adapters = {
                Adapter("gpu-a", {{2, true, false}, {5, false, true}}),
                Adapter("gpu-z", {{9, true, true}}, RenderAdapterKind::Integrated),
            };
            REQUIRE(InitializeBackend(state, RenderBackendConfig{.requirePresentation = true}).HasValue());
            REQUIRE(state.deviceRequest.adapter == RenderAdapterId{"gpu-a"});
            REQUIRE(state.deviceRequest.graphicsQueueFamily == 2);
            REQUIRE(state.deviceRequest.presentationQueueFamily == 5);
        }
        SECTION("split graphics and presentation queues remain eligible") {
            RuntimeState state = DefaultState();
            state.enumeration.adapters = {Adapter("gpu-split", {{4, false, true}, {2, true, false}})};
            REQUIRE(InitializeBackend(state, RenderBackendConfig{.requirePresentation = true}).HasValue());
            REQUIRE(state.deviceRequest.graphicsQueueFamily == 2);
            REQUIRE(state.deviceRequest.presentationQueueFamily == 4);
        }
        SECTION("requested adapter must match current discovery revision") {
            RuntimeState state = DefaultState();
            FakeRuntimePort port{state};
            auto stale = CreateBackend(port)->Initialize(
                RenderBackendConfig{.adapter = RenderAdapterId{"gpu-a"}, .adapterDiscoveryRevision = 6, .requirePresentation = true});
            REQUIRE(stale.HasError());
            REQUIRE(stale.ErrorValue().code.Value() == "render.vulkan.adapter_revision_stale");

            std::unique_ptr<IRenderBackend> backend = CreateBackend(port);
            auto missing = backend->Initialize(
                RenderBackendConfig{.adapter = RenderAdapterId{"gpu-missing"}, .adapterDiscoveryRevision = 7, .requirePresentation = true});
            REQUIRE(missing.HasError());
            REQUIRE(missing.ErrorValue().code.Value() == "render.vulkan.adapter_not_found");
        }
    }

    TEST_CASE("Vulkan rejects device baseline violations and rolls partial state back", "[unit][runtime][renderer][vulkan]") {
        RuntimeState state = DefaultState();
        VulkanAdapterCandidate &candidate = state.enumeration.adapters.front();
        std::string expectedCode;

        SECTION("device API version") {
            candidate.apiVersion = {1, 2, 0};
            expectedCode = "render.vulkan.device_version_unsupported";
        }
        SECTION("required feature") {
            candidate.requiredFeatures.timelineSemaphore = false;
            expectedCode = "render.vulkan.required_features_unavailable";
        }
        SECTION("portability subset") {
            candidate.portabilitySubset = true;
            expectedCode = "render.vulkan.api_variant_unsupported";
        }
        SECTION("driver restriction") {
            candidate.driverAllowed = false;
            expectedCode = "render.vulkan.driver_unsupported";
        }

        const auto result =
            InitializeBackend(state, RenderBackendConfig{.adapter = RenderAdapterId{"gpu-a"}, .adapterDiscoveryRevision = 7});
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == expectedCode);
        REQUIRE(state.createDeviceCount == 0);
        REQUIRE(state.destroyInstanceCount == 1);
        REQUIRE(state.releaseLoaderCount == 1);
    }

    TEST_CASE("Vulkan native failures preserve their typed cause and shared runtime ownership", "[unit][runtime][renderer][vulkan]") {
        SECTION("device creation failure rolls back instance then loader") {
            RuntimeState state = DefaultState();
            state.failure = FailureStage::CreateDevice;
            const auto result = InitializeBackend(state);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == "render.test.create_device");
            REQUIRE(state.destroyDeviceCount == 1);
            REQUIRE(state.lifecycle == std::vector<std::string>{"acquire-loader", "create-instance", "create-device", "destroy-device",
                                                                "destroy-instance", "release-loader"});
        }
        SECTION("overlapping instances cannot share the non-thread-safe loader port") {
            RuntimeState state = DefaultState();
            FakeRuntimePort port{state};
            RenderBackendRegistry registry;
            REQUIRE(Detail::RegisterVulkanRenderBackendWithRuntimePort(registry, port).HasValue());
            REQUIRE(registry.Seal().HasValue());
            auto first = registry.Create(RenderBackendId{"vulkan"});
            auto second = registry.Create(RenderBackendId{"vulkan"});
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());
            REQUIRE(first.Value()->Initialize({}).HasValue());
            const auto overlap = second.Value()->Initialize({});
            REQUIRE(overlap.HasError());
            REQUIRE(overlap.ErrorValue().code.Value() == "render.vulkan.presentation_in_use");
            first.Value()->Shutdown();
            REQUIRE(second.Value()->Initialize({}).HasValue());
        }
    }

    TEST_CASE("Vulkan module info reserves native interactive composition", "[unit][runtime][renderer][vulkan]") {
        Test::CheckModuleInfo(GetVulkanRenderBackendModuleInfo(), {.id = RenderBackendId{"vulkan"}, .presentsToWindow = true},
                              RenderPresentationKind::Vulkan);
    }
}  // namespace
