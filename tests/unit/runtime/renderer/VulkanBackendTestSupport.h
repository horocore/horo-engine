#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanBackendInternal.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Render::VulkanBackendTest {
#if defined(_WIN32)
    inline constexpr const char *PresentationExtension = "VK_KHR_win32_surface";
#else
    inline constexpr const char *PresentationExtension = "VK_KHR_xcb_surface";
#endif
    [[nodiscard]] inline Error Failure(const char *code) {
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
            .instanceExtensions = {"VK_EXT_debug_utils", "VK_KHR_surface", PresentationExtension,
                                   VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME},
            .layers = {"VK_LAYER_KHRONOS_validation"},
            .hasInstanceVersionQuery = true,
            .standardApiVariant = true,
        };
        std::vector<std::string> platformExtensions{PresentationExtension};
        VulkanAdapterEnumeration enumeration;
        VulkanInstanceRequest instanceRequest;
        VulkanDeviceRequest deviceRequest;
        std::vector<std::string> lifecycle;
        int queryBufferCostCount{0};
        int createBufferCount{0};
        int destroyBufferCount{0};
    };

    [[nodiscard]] inline VulkanAdapterCandidate Adapter(std::string id, std::vector<VulkanQueueFamily> queues,
                                                        RenderAdapterKind kind = RenderAdapterKind::Discrete) {
        return VulkanAdapterCandidate{
            .id = RenderAdapterId{std::move(id)},
            .displayName = "Test Vulkan device",
            .kind = kind,
            .dedicatedVideoMemoryBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL,
            .apiVersion = {1, 3, 0},
            .requiredFeatures = {.dynamicRendering = true, .synchronization2 = true, .timelineSemaphore = true},
            .deviceExtensions = {"VK_KHR_swapchain", VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME},
            .queueFamilies = std::move(queues),
            .swapchainMaintenance = true,
        };
    }

    /** @brief Synchronous resource fake is composed by the admission runtime, matching the production ownership boundary. */
    class FakeResourceBackend final : public IRenderResourceBackend {
    public:
        explicit FakeResourceBackend(RuntimeState &state) noexcept : state_(&state) {}

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
            ++state_->queryBufferCostCount;
            return Result<RenderMemoryCostPlan>::Success({.memoryClass = descriptor.access == RenderBufferAccess::HostVisible
                                                                             ? RenderMemoryClass::Upload
                                                                             : RenderMemoryClass::PersistentDevice,
                                                          .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                                          .provenance = RenderMemoryCostProvenance::Exact,
                                                          .compatibility = RenderMemoryCompatibilityId{11},
                                                          .payloadBytes = descriptor.byteSize,
                                                          .requiredBytes = descriptor.byteSize,
                                                          .alignment = 16});
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
            const auto bytes = *RenderTextureBaseLevelByteSize(descriptor);
            const RenderMemoryCostPlan plan{.memoryClass = RenderMemoryClass::PersistentDevice,
                                            .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                            .provenance = RenderMemoryCostProvenance::Exact,
                                            .compatibility = RenderMemoryCompatibilityId{12},
                                            .payloadBytes = bytes,
                                            .requiredBytes = bytes,
                                            .alignment = 16};
            return Result<RenderMemoryCostPlan>::Success(plan);
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &, std::span<const std::byte>, const RenderMemoryPlacement &,
                                           RenderResourceNativeLifetime lifetime = {}) override {
            ++state_->createBufferCount;
            return Result<std::uint64_t>::Success(101);
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &, std::uint64_t, std::uint64_t,
                                         RenderResourceNativeLifetime lifetime = {}) override {
            return Result<std::uint64_t>::Success(102);
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &, std::span<const std::byte>, const RenderMemoryPlacement &,
                                            RenderResourceNativeLifetime lifetime = {}) override {
            return Result<std::uint64_t>::Success(103);
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &, std::uint64_t,
                                                RenderResourceNativeLifetime lifetime = {}) override {
            return Result<std::uint64_t>::Success(104);
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &, std::uint64_t, std::uint64_t,
                                                 RenderResourceNativeLifetime lifetime = {}) override {
            return Result<std::uint64_t>::Success(105);
        }

        void DestroyBuffer(std::uint64_t) noexcept override {
            ++state_->destroyBufferCount;
        }

        void DestroyMesh(std::uint64_t) noexcept override {
            state_->lifecycle.emplace_back("destroy-mesh");
        }

        void DestroyTexture(std::uint64_t) noexcept override {
            state_->lifecycle.emplace_back("destroy-texture");
        }

        void DestroyTextureView(std::uint64_t) noexcept override {
            state_->lifecycle.emplace_back("destroy-texture-view");
        }

        void DestroyRenderTarget(std::uint64_t) noexcept override {
            state_->lifecycle.emplace_back("destroy-render-target");
        }

    private:
        RuntimeState *state_;
    };

    class FakeRuntimePort : public IVulkanRuntimePort, public IVulkanResourcePort {
    public:
        explicit FakeRuntimePort(RuntimeState &state) noexcept : state_(&state), resources_(state) {}

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

        IRenderResourceBackend *ResourceBackend() noexcept override {
            return state_->createDeviceCount > 0 ? &resources_ : nullptr;
        }

    private:
        RuntimeState *state_{nullptr};
        FakeResourceBackend resources_;
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

    [[nodiscard]] inline RuntimeState DefaultState() {
        RuntimeState state;
        state.enumeration = VulkanAdapterEnumeration{
            .revision = 7,
            .adapters = {Adapter("gpu-a", {{0, true, true}})},
        };
        return state;
    }

    [[nodiscard]] inline std::unique_ptr<IRenderBackend> CreateBackend(FakeRuntimePort &port) {
        RenderBackendRegistry registry;
        REQUIRE(Detail::RegisterVulkanRenderBackendWithRuntimePort(registry, port).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto backend = registry.Create(RenderBackendId{"vulkan"});
        REQUIRE(backend.HasValue());
        return std::move(backend).Value();
    }

    [[nodiscard]] inline Result<void> InitializeBackend(RuntimeState &state, const RenderBackendConfig &config = {}) {
        FakeRuntimePort port{state};
        return CreateBackend(port)->Initialize(config);
    }

    inline void RequireInitializationError(RuntimeState &state, const RenderBackendConfig &config, const std::string_view expectedCode) {
        const auto result = InitializeBackend(state, config);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == expectedCode);
    }

}  // namespace Horo::Render::VulkanBackendTest
