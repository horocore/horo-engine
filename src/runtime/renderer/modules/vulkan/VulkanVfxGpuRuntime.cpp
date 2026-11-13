#include "VulkanVfxGpuInternal.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace Horo::Render::Detail {
    namespace {
        using Resolver = std::function<PFN_vkVoidFunction(VkDevice, const char *)>;

        bool LoadPipelines(VulkanVfxGpuDevice &device, const Resolver &resolve) {
            device.native.createModule = std::bit_cast<PFN_vkCreateShaderModule>(resolve(device.device, "vkCreateShaderModule"));
            device.native.destroyModule = std::bit_cast<PFN_vkDestroyShaderModule>(resolve(device.device, "vkDestroyShaderModule"));
            device.native.createCompute = std::bit_cast<PFN_vkCreateComputePipelines>(resolve(device.device, "vkCreateComputePipelines"));
            device.native.createGraphics =
                std::bit_cast<PFN_vkCreateGraphicsPipelines>(resolve(device.device, "vkCreateGraphicsPipelines"));
            device.native.destroyPipeline = std::bit_cast<PFN_vkDestroyPipeline>(resolve(device.device, "vkDestroyPipeline"));
            return device.native.createModule && device.native.destroyModule && device.native.createCompute &&
                   device.native.createGraphics && device.native.destroyPipeline;
        }

        bool LoadDescriptors(VulkanVfxGpuDevice &device, const Resolver &resolve) {
            device.native.createSetLayout =
                std::bit_cast<PFN_vkCreateDescriptorSetLayout>(resolve(device.device, "vkCreateDescriptorSetLayout"));
            device.native.destroySetLayout =
                std::bit_cast<PFN_vkDestroyDescriptorSetLayout>(resolve(device.device, "vkDestroyDescriptorSetLayout"));
            device.native.createLayout = std::bit_cast<PFN_vkCreatePipelineLayout>(resolve(device.device, "vkCreatePipelineLayout"));
            device.native.destroyLayout = std::bit_cast<PFN_vkDestroyPipelineLayout>(resolve(device.device, "vkDestroyPipelineLayout"));
            device.native.createPool = std::bit_cast<PFN_vkCreateDescriptorPool>(resolve(device.device, "vkCreateDescriptorPool"));
            device.native.destroyPool = std::bit_cast<PFN_vkDestroyDescriptorPool>(resolve(device.device, "vkDestroyDescriptorPool"));
            device.native.allocateSets = std::bit_cast<PFN_vkAllocateDescriptorSets>(resolve(device.device, "vkAllocateDescriptorSets"));
            device.native.updateSets = std::bit_cast<PFN_vkUpdateDescriptorSets>(resolve(device.device, "vkUpdateDescriptorSets"));
            return device.native.createSetLayout && device.native.destroySetLayout && device.native.createLayout &&
                   device.native.destroyLayout && device.native.createPool && device.native.destroyPool && device.native.allocateSets &&
                   device.native.updateSets;
        }

        bool LoadCompute(VulkanVfxGpuDevice &device, const Resolver &resolve) {
            device.native.bindPipeline = std::bit_cast<PFN_vkCmdBindPipeline>(resolve(device.device, "vkCmdBindPipeline"));
            device.native.bindSets = std::bit_cast<PFN_vkCmdBindDescriptorSets>(resolve(device.device, "vkCmdBindDescriptorSets"));
            device.native.dispatch = std::bit_cast<PFN_vkCmdDispatch>(resolve(device.device, "vkCmdDispatch"));
            device.native.barrier = std::bit_cast<PFN_vkCmdPipelineBarrier2>(resolve(device.device, "vkCmdPipelineBarrier2"));
            return device.native.bindPipeline && device.native.bindSets && device.native.dispatch && device.native.barrier;
        }

        bool LoadDraw(VulkanVfxGpuDevice &device, const Resolver &resolve) {
            device.native.draw = std::bit_cast<PFN_vkCmdDraw>(resolve(device.device, "vkCmdDraw"));
            device.native.beginRendering = std::bit_cast<PFN_vkCmdBeginRendering>(resolve(device.device, "vkCmdBeginRendering"));
            device.native.endRendering = std::bit_cast<PFN_vkCmdEndRendering>(resolve(device.device, "vkCmdEndRendering"));
            device.native.viewport = std::bit_cast<PFN_vkCmdSetViewport>(resolve(device.device, "vkCmdSetViewport"));
            device.native.scissor = std::bit_cast<PFN_vkCmdSetScissor>(resolve(device.device, "vkCmdSetScissor"));
            return device.native.draw && device.native.beginRendering && device.native.endRendering && device.native.viewport &&
                   device.native.scissor;
        }

        bool ValidInitialization(VkDevice device, std::uint64_t incarnation, std::uint32_t frames, bool resolver, bool initialized) {
            const std::array valid{device != VK_NULL_HANDLE, incarnation != 0, frames > 0, frames <= 8, resolver, !initialized};
            return std::ranges::all_of(valid, [](bool value) {
                return value;
            });
        }

        bool LoadNative(VulkanVfxGpuDevice &device, const Resolver &resolve) {
            return LoadPipelines(device, resolve) && LoadDescriptors(device, resolve) && LoadCompute(device, resolve) &&
                   LoadDraw(device, resolve);
        }

        bool SupportedLimits(const VkPhysicalDeviceLimits &limits) {
            const std::array admitted{limits.maxComputeWorkGroupInvocations >= 64,
                                      limits.maxComputeWorkGroupSize[0] >= 64,
                                      limits.maxPerStageDescriptorStorageBuffers >= 4,
                                      limits.maxPerStageDescriptorUniformBuffers >= 1,
                                      limits.maxDescriptorSetStorageBuffers >= 4,
                                      limits.maxPerStageResources >= 5,
                                      limits.maxDescriptorSetUniformBuffersDynamic >= 1,
                                      limits.maxUniformBufferRange >= 128,
                                      limits.maxBoundDescriptorSets >= 1};
            return std::ranges::all_of(admitted, [](bool value) {
                return value;
            });
        }
    }  // namespace

    Error VulkanVfxGpuInvalid(const char *text) {
        return MakeError(VulkanBackendErrors::ResourceIdentityInvalid, text);
    }

    Error VulkanVfxGpuNativeFailure(const char *text, VkResult result) {
        return MakeError(result == VK_ERROR_DEVICE_LOST ? VulkanBackendErrors::DeviceLost : VulkanBackendErrors::NativeCallFailed,
                         std::string{text} + " VkResult=" + std::to_string(result));
    }

    VulkanVfxGpuRuntime::VulkanVfxGpuRuntime() : impl_(std::make_unique<Impl>()) {}

    VulkanVfxGpuRuntime::~VulkanVfxGpuRuntime() {
        if (impl_->device && impl_->device->open)
            std::terminate();
    }

    Result<void> VulkanVfxGpuRuntime::Impl::Access(bool admission) const {
        if (!device || !device->open)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::UnsupportedOperation));
        if (owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::WrongThread));
        if (admission && closing)
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        return Result<void>::Success();
    }

    bool VulkanVfxGpuRuntime::Available() const noexcept {
        return impl_->device && impl_->device->open;
    }

    Result<void> VulkanVfxGpuRuntime::Initialize(VkDevice device, const VkPhysicalDeviceProperties &properties, std::uint32_t frames,
                                                 std::uint64_t incarnation, std::span<const VulkanCommandQueue> queues,
                                                 VulkanResourceRuntime &resources, VulkanCommandRuntime &commands,
                                                 const std::function<PFN_vkVoidFunction(VkDevice, const char *)> &resolver) {
        if (!ValidInitialization(device, incarnation, frames, bool(resolver), bool(impl_->device)))
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidConfig));
        if (const bool compute = std::ranges::any_of(queues,
                                                     [](const auto &queue) {
            return queue.supportsGraphics && queue.supportsCompute;
        });
            !compute || !SupportedLimits(properties.limits))
            return Result<void>::Success();
        auto selected = std::make_shared<VulkanVfxGpuDevice>();
        selected->device = device;
        selected->properties = properties;
        selected->incarnation = incarnation;
        selected->frames = frames;
        if (!LoadNative(*selected, resolver))
            return Result<void>::Failure(MakeError(VulkanBackendErrors::EntryPointMissing));
        impl_->kernels.reserve(4096);
        impl_->simulations.reserve(4096);
        selected->open = true;
        impl_->device = std::move(selected);
        impl_->resources = &resources;
        impl_->commands = &commands;
        impl_->owner = std::this_thread::get_id();
        return Result<void>::Success();
    }

    Result<NativeVfxGpuPreparationResources> VulkanVfxGpuRuntime::QueryResources(const VfxGpuSimulationDescriptor &descriptor) const {
        using Outcome = Result<NativeVfxGpuPreparationResources>;
        if (const auto access = impl_->Access(); access.HasError())
            return Outcome::Failure(access.ErrorValue());
        if (!VfxGpuStateBufferCount(descriptor))
            return Outcome::Failure(VulkanVfxGpuInvalid("Invalid pending particle-step bound."));
        const auto alignment = std::max<VkDeviceSize>(16, impl_->device->properties.limits.minUniformBufferOffsetAlignment);
        const auto stride = (VkDeviceSize{128} + alignment - 1) / alignment * alignment;
        const auto slots = std::uint64_t{impl_->device->frames} * RenderGraphLimits::HardMaxPasses + descriptor.maximumPendingSteps;
        if (stride > std::numeric_limits<std::uint32_t>::max() || slots > std::numeric_limits<std::uint32_t>::max() / stride)
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceUnsupported, "Native dynamic-uniform offsets exceed uint32."));
        NativeVfxGpuPreparationResources plan{{static_cast<std::size_t>(slots * stride), RenderBufferUsage::Uniform,
                                               RenderBufferAccess::HostVisible},
                                              static_cast<std::uint32_t>(slots),
                                              static_cast<std::uint32_t>(stride)};
        if (plan.parameterArena.byteSize > descriptor.maximumRetainedBytes)
            return Outcome::Failure(MakeError(VulkanBackendErrors::ResourceCapacityExceeded,
                                              "Declared particle slice cannot hold its fixed native operation arena."));
        return Outcome::Success(plan);
    }

    Result<std::shared_ptr<VulkanVfxGpuSimulation>> VulkanVfxGpuRuntime::Impl::Simulation(
        const std::shared_ptr<IResidentVfxGpuSimulation> &source) const {
        using Outcome = Result<std::shared_ptr<VulkanVfxGpuSimulation>>;
        if (const auto access = Access(false); access.HasError())
            return Outcome::Failure(access.ErrorValue());
        auto simulation = std::dynamic_pointer_cast<VulkanVfxGpuSimulation>(source);
        if (!simulation || simulation->owner != device || !simulation->lease)
            return Outcome::Failure(VulkanVfxGpuInvalid("Particle simulation belongs to another device or has retired."));
        return Outcome::Success(std::move(simulation));
    }

    Result<bool> VulkanVfxGpuRuntime::Impl::Complete(RenderTimelinePoint point, bool refresh) const {
        if (!point.IsValid())
            return Result<bool>::Success(true);
        return commands->ObserveAcceptedNative({.completion = point, .expectedDeviceIncarnation = device->incarnation}, refresh);
    }

    void VulkanVfxGpuRuntime::Close() noexcept {
        impl_->closing = true;
        Abort();
    }

    Result<bool> VulkanVfxGpuRuntime::Drain() {
        if (!Available())
            return Result<bool>::Success(true);
        if (!impl_->closing)
            return Result<bool>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        for (const auto &simulation : impl_->simulations) {
            if (const auto observed = impl_->Observe(*simulation); observed.HasError())
                return Result<bool>::Failure(observed.ErrorValue());
            if (std::ranges::any_of(simulation->operations, [](const auto &op) {
                return op.occupied;
            }))
                return Result<bool>::Success(false);
        }
        for (const auto &simulation : impl_->simulations)
            simulation->ReleaseNative();
        impl_->simulations.clear();
        for (const auto &kernel : impl_->kernels)
            kernel->Reset();
        impl_->kernels.clear();
        impl_->device->open = false;
        return Result<bool>::Success(true);
    }
}  // namespace Horo::Render::Detail
