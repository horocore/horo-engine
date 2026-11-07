#pragma once
/** @file VulkanVfxInstancedReadback.h @brief Test-only forwarding observation and bounded copies of actual Vulkan draw attachments. */
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Render/Mesh.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"

#include <VulkanBackendModule.h>
#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

namespace Horo::Render::Test::VfxInstanced {
    /** @brief Contains copied native FP16 color and float depth, never fabricated shader output. */
    struct Pixels final {
        std::vector<std::uint16_t> color;
        std::vector<float> depth;
    };

    /** @brief Delegates every native operation unchanged; records only bounded successful outputs on the actual loader owner lane. */
    class ObservedLoader final : public IVulkanLoaderPort {
    public:
        struct Image final {
            VkImage image{};
            VkFormat format{};
            VkImageUsageFlags usage{};
        };

        struct View final {
            VkImageView view{};
            VkImage image{};
        };

        struct Attachments final {
            Image color, depth;
        };

        struct Draw final {
            std::uint32_t indices{}, instances{};
        };

        /** @brief Borrows an existing real system loader; no loader path or driver fact is replaced. */
        explicit ObservedLoader(IVulkanLoaderPort &delegate) noexcept : delegate_(delegate) {}

        /** @brief Releases only the delegated actual loader lease after its backend descendants drain. */
        ~ObservedLoader() override {
            ReleaseLoader();
        }

        /** @copydoc IVulkanLoaderPort::AcquireLoader */
        Result<void> AcquireLoader() override {
            return delegate_.AcquireLoader();
        }

        /** @copydoc IVulkanLoaderPort::GetInstanceProcAddr */
        PFN_vkGetInstanceProcAddr GetInstanceProcAddr() noexcept override {
            if (active_ && active_ != this)
                return nullptr;
            active_ = this;
            instanceResolver_ = delegate_.GetInstanceProcAddr();
            return instanceResolver_ ? &InstanceProc : nullptr;
        }

        /** @copydoc IVulkanLoaderPort::RequiredPresentationExtensions */
        Result<std::vector<std::string>> RequiredPresentationExtensions() override {
            return delegate_.RequiredPresentationExtensions();
        }

        /** @copydoc IVulkanLoaderPort::SupportsPresentation */
        Result<bool> SupportsPresentation(VkPhysicalDevice device, std::uint32_t family) override {
            return delegate_.SupportsPresentation(device, family);
        }

        /** @copydoc IVulkanLoaderPort::IsDriverAllowed */
        Result<bool> IsDriverAllowed(const VkPhysicalDeviceProperties &properties) override {
            return delegate_.IsDriverAllowed(properties);
        }

        /** @copydoc IVulkanLoaderPort::ReleaseLoader */
        void ReleaseLoader() noexcept override {
            if (active_ == this)
                active_ = nullptr;
            delegate_.ReleaseLoader();
        }

        /** @brief Resolves the same selected device's actual function without intercepting the private copy's operations. */
        template <class Function> Function DeviceFunction(const char *name) const {
            REQUIRE(deviceResolver_);
            const auto function = deviceResolver_(device_, name);
            REQUIRE(function);
            return std::bit_cast<Function>(function);
        }

        /** @brief Queries actual selected physical-device memory facts through the same instance. */
        VkPhysicalDeviceMemoryProperties MemoryProperties() const {
            const auto function = instanceResolver_(instance_, "vkGetPhysicalDeviceMemoryProperties");
            REQUIRE(function);
            VkPhysicalDeviceMemoryProperties properties;
            std::bit_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(function)(physical_, &properties);
            return properties;
        }

        /** @brief Returns the exact pair actually used by a real native render pass, not guessed opaque registry IDs. */
        Attachments Rendered() const {
            REQUIRE_FALSE(invalid_);
            REQUIRE(rendered_.color.image);
            REQUIRE(rendered_.depth.image);
            REQUIRE((rendered_.color.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0);
            REQUIRE((rendered_.depth.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0);
            return rendered_;
        }

        /** @brief Returns actual successfully created device and retrieved graphics queue identities. */
        VkDevice Device() const noexcept {
            return device_;
        }

        VkQueue Queue() const noexcept {
            return queue_;
        }

        std::uint32_t Family() const noexcept {
            return family_;
        }

        /** @brief Starts a bounded observation window without changing any driver command or resource state. */
        void ResetDraws() noexcept {
            drawCount_ = 0;
        }

        /** @brief Returns actual forwarded indexed command counts, checked outside every native callback. */
        std::array<std::uint32_t, 3> DrawCounts() const {
            REQUIRE_FALSE(invalid_);
            std::array<std::uint32_t, 3> counts{0, static_cast<std::uint32_t>(drawCount_), 0};
            for (const auto &draw : std::span{draws_}.first(drawCount_)) {
                REQUIRE(draw.indices == 6);
                counts[0] += draw.instances;
                if (draw.instances > 1)
                    ++counts[2];
            }
            return counts;
        }

    private:
        /** @brief Forwards the actual instance lookup, wrapping only observation-relevant entry points. */
        static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceProc(VkInstance instance, const char *name) noexcept {
            auto &self = *active_;
            if (instance)
                self.instance_ = instance;
            const auto actual = self.instanceResolver_(instance, name);
            if (!actual)
                return actual;
            if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
                self.deviceResolver_ = std::bit_cast<PFN_vkGetDeviceProcAddr>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&DeviceProc);
            }
            if (std::strcmp(name, "vkCreateDevice") == 0) {
                self.createDevice_ = std::bit_cast<PFN_vkCreateDevice>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&CreateDevice);
            }
            return actual;
        }

        /** @brief Forwards all device operations and records bounded outputs without allocation, assertions or exceptions in callbacks. */
        static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceProc(VkDevice device, const char *name) noexcept {
            auto &self = *active_;
            const auto actual = self.deviceResolver_(device, name);
            if (!actual)
                return actual;
            if (std::strcmp(name, "vkGetDeviceQueue") == 0) {
                self.getQueue_ = std::bit_cast<PFN_vkGetDeviceQueue>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&GetQueue);
            }
            if (std::strcmp(name, "vkCreateImage") == 0) {
                self.createImage_ = std::bit_cast<PFN_vkCreateImage>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&CreateImage);
            }
            if (std::strcmp(name, "vkDestroyImage") == 0) {
                self.destroyImage_ = std::bit_cast<PFN_vkDestroyImage>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&DestroyImage);
            }
            if (std::strcmp(name, "vkCreateImageView") == 0) {
                self.createView_ = std::bit_cast<PFN_vkCreateImageView>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&CreateView);
            }
            if (std::strcmp(name, "vkDestroyImageView") == 0) {
                self.destroyView_ = std::bit_cast<PFN_vkDestroyImageView>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&DestroyView);
            }
            if (std::strcmp(name, "vkCmdBeginRendering") == 0) {
                self.beginRendering_ = std::bit_cast<PFN_vkCmdBeginRendering>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&BeginRendering);
            }
            if (std::strcmp(name, "vkCmdDrawIndexed") == 0) {
                self.drawIndexed_ = std::bit_cast<PFN_vkCmdDrawIndexed>(actual);
                return std::bit_cast<PFN_vkVoidFunction>(&DrawIndexed);
            }
            return actual;
        }

        /** @brief Forwards real indexed draws unchanged and records only bounded arguments, never a GPU verdict. */
        static VKAPI_ATTR void VKAPI_CALL DrawIndexed(VkCommandBuffer command, std::uint32_t indices, std::uint32_t instances,
                                                      std::uint32_t firstIndex, std::int32_t vertexOffset,
                                                      std::uint32_t firstInstance) noexcept {
            auto &self = *active_;
            self.drawIndexed_(command, indices, instances, firstIndex, vertexOffset, firstInstance);
            if (self.drawCount_ == self.draws_.size()) {
                self.invalid_ = true;
                return;
            }
            self.draws_[self.drawCount_++] = {indices, instances};
        }

        /** @brief Records only successful actual selected device creation. */
        static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                           const VkAllocationCallbacks *allocator, VkDevice *device) noexcept {
            auto &self = *active_;
            const auto result = self.createDevice_(physical, info, allocator, device);
            if (result == VK_SUCCESS) {
                self.physical_ = physical;
                self.device_ = *device;
            }
            return result;
        }

        /** @brief Captures only the actual explicitly aliased graphics queue returned by the real driver. */
        static VKAPI_ATTR void VKAPI_CALL GetQueue(VkDevice device, std::uint32_t family, std::uint32_t index, VkQueue *queue) noexcept {
            auto &self = *active_;
            self.getQueue_(device, family, index, queue);
            if (self.queue_ && self.queue_ != *queue)
                self.invalid_ = true;
            self.queue_ = *queue;
            self.family_ = family;
        }

        /** @brief Retains only real live 64x32 single-sample color/depth identities in a fixed mapping. */
        void RecordImage(VkImage image, const VkImageCreateInfo &info) noexcept {
            if (info.extent.width != 64 || info.extent.height != 32 || info.samples != VK_SAMPLE_COUNT_1_BIT)
                return;
            if (info.format != VK_FORMAT_R16G16B16A16_SFLOAT && info.format != VK_FORMAT_D32_SFLOAT)
                return;
            const auto free = std::ranges::find(images_, VkImage{}, &Image::image);
            if (free == images_.end()) {
                invalid_ = true;
                return;
            }
            *free = {image, info.format, info.usage};
        }

        /** @brief Forwards creation first and records only a genuine successful result. */
        static VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo *info,
                                                          const VkAllocationCallbacks *allocator, VkImage *image) noexcept {
            auto &self = *active_;
            const auto result = self.createImage_(device, info, allocator, image);
            if (result == VK_SUCCESS)
                self.RecordImage(*image, *info);
            return result;
        }

        /** @brief Removes the exact native generation when the real resource owner destroys it. */
        static VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) noexcept {
            auto &self = *active_;
            for (auto &entry : self.images_)
                if (entry.image == image)
                    entry = {};
            self.destroyImage_(device, image, allocator);
        }

        /** @brief Preserves real view-to-image association without adopting resource lifetime. */
        void RecordView(VkImageView view, VkImage image) noexcept {
            if (std::ranges::find(images_, image, &Image::image) == images_.end())
                return;
            const auto free = std::ranges::find(views_, VkImageView{}, &View::view);
            if (free == views_.end()) {
                invalid_ = true;
                return;
            }
            *free = {view, image};
        }

        /** @brief Records only successfully created actual views. */
        static VKAPI_ATTR VkResult VKAPI_CALL CreateView(VkDevice device, const VkImageViewCreateInfo *info,
                                                         const VkAllocationCallbacks *allocator, VkImageView *view) noexcept {
            auto &self = *active_;
            const auto result = self.createView_(device, info, allocator, view);
            if (result == VK_SUCCESS)
                self.RecordView(*view, info->image);
            return result;
        }

        /** @brief Removes only the exact destroyed native view generation. */
        static VKAPI_ATTR void VKAPI_CALL DestroyView(VkDevice device, VkImageView view, const VkAllocationCallbacks *allocator) noexcept {
            auto &self = *active_;
            for (auto &entry : self.views_)
                if (entry.view == view)
                    entry = {};
            self.destroyView_(device, view, allocator);
        }

        /** @brief Resolves a real render attachment through both successful native creation maps. */
        Image Attachment(VkImageView view) const noexcept {
            const auto found = std::ranges::find(views_, view, &View::view);
            if (found == views_.end())
                return {};
            const auto image = std::ranges::find(images_, found->image, &Image::image);
            return image == images_.end() ? Image{} : *image;
        }

        /** @brief Observes the actual draw pass pair while forwarding the identical real command. */
        static VKAPI_ATTR void VKAPI_CALL BeginRendering(VkCommandBuffer command, const VkRenderingInfo *info) noexcept {
            auto &self = *active_;
            self.beginRendering_(command, info);
            if (info->colorAttachmentCount != 1 || !info->pDepthAttachment)
                return;
            const auto color = self.Attachment(info->pColorAttachments[0].imageView);
            const auto depth = self.Attachment(info->pDepthAttachment->imageView);
            if (color.format == VK_FORMAT_R16G16B16A16_SFLOAT && depth.format == VK_FORMAT_D32_SFLOAT)
                self.rendered_ = {color, depth};
        }

        IVulkanLoaderPort &delegate_;
        inline static thread_local ObservedLoader *active_{};
        PFN_vkGetInstanceProcAddr instanceResolver_{};
        PFN_vkGetDeviceProcAddr deviceResolver_{};
        PFN_vkCreateDevice createDevice_{};
        PFN_vkGetDeviceQueue getQueue_{};
        PFN_vkCreateImage createImage_{};
        PFN_vkDestroyImage destroyImage_{};
        PFN_vkCreateImageView createView_{};
        PFN_vkDestroyImageView destroyView_{};
        PFN_vkCmdBeginRendering beginRendering_{};
        PFN_vkCmdDrawIndexed drawIndexed_{};
        VkInstance instance_{};
        VkPhysicalDevice physical_{};
        VkDevice device_{};
        VkQueue queue_{};
        std::uint32_t family_{};
        std::array<Image, 32> images_;
        std::array<View, 32> views_;
        Attachments rendered_;
        std::array<Draw, 32> draws_;
        std::size_t drawCount_{};
        bool invalid_{};
    };

    /** @brief Owns only private diagnostic staging children; no production image, queue or registry ownership is fabricated. */
    class NativeReadback final {
    public:
        /** @brief Borrows the actual live backend observer after ordinary frontend completion has been proved. */
        explicit NativeReadback(const ObservedLoader &loader) noexcept : loader_(loader) {}

        /** @brief Contains every native wait to five seconds and destroys partial children before the frontend/device parent. */
        ~NativeReadback() {
            if (submitted_ && Wait() != VK_SUCCESS)
                std::terminate();
            const auto device = loader_.Device();
            if (fence_)
                loader_.DeviceFunction<PFN_vkDestroyFence>("vkDestroyFence")(device, fence_, nullptr);
            if (pool_)
                loader_.DeviceFunction<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool_, nullptr);
            if (buffer_)
                loader_.DeviceFunction<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, buffer_, nullptr);
            if (memory_)
                loader_.DeviceFunction<PFN_vkFreeMemory>("vkFreeMemory")(device, memory_, nullptr);
        }

        /** @brief Allocates a real coherent staging destination and a one-use same-queue diagnostic command. */
        void Prepare() {
            const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                            .size = TotalBytes,
                                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
            REQUIRE(loader_.DeviceFunction<PFN_vkCreateBuffer>("vkCreateBuffer")(loader_.Device(), &buffer, nullptr, &buffer_) ==
                    VK_SUCCESS);
            VkMemoryRequirements requirements;
            loader_.DeviceFunction<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(loader_.Device(), buffer_,
                                                                                                       &requirements);
            const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                  .allocationSize = requirements.size,
                                                  .memoryTypeIndex = MemoryType(requirements.memoryTypeBits)};
            REQUIRE(loader_.DeviceFunction<PFN_vkAllocateMemory>("vkAllocateMemory")(loader_.Device(), &allocation, nullptr, &memory_) ==
                    VK_SUCCESS);
            REQUIRE(loader_.DeviceFunction<PFN_vkBindBufferMemory>("vkBindBufferMemory")(loader_.Device(), buffer_, memory_, 0) ==
                    VK_SUCCESS);
            const VkCommandPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                               .queueFamilyIndex = loader_.Family()};
            REQUIRE(loader_.DeviceFunction<PFN_vkCreateCommandPool>("vkCreateCommandPool")(loader_.Device(), &pool, nullptr, &pool_) ==
                    VK_SUCCESS);
            const VkCommandBufferAllocateInfo commands{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                       .commandPool = pool_,
                                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                       .commandBufferCount = 1};
            REQUIRE(loader_.DeviceFunction<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(loader_.Device(), &commands,
                                                                                                     &commands_) == VK_SUCCESS);
        }

        /** @brief Transitions actual completed attachments, copies their contents, and restores the canonical graph's final layouts. */
        Pixels Read() {
            const auto images = loader_.Rendered();
            const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            REQUIRE(loader_.DeviceFunction<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands_, &begin) == VK_SUCCESS);
            Copy(images.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            Copy(images.depth.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, ColorBytes,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            const VkBufferMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                             .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                             .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                             .buffer = buffer_,
                                             .offset = 0,
                                             .size = TotalBytes};
            loader_.DeviceFunction<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                                                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0,
                                                                                     nullptr);
            REQUIRE(loader_.DeviceFunction<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands_) == VK_SUCCESS);
            const VkFenceCreateInfo fence{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            REQUIRE(loader_.DeviceFunction<PFN_vkCreateFence>("vkCreateFence")(loader_.Device(), &fence, nullptr, &fence_) == VK_SUCCESS);
            const VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands_};
            REQUIRE(loader_.DeviceFunction<PFN_vkQueueSubmit>("vkQueueSubmit")(loader_.Queue(), 1, &submit, fence_) == VK_SUCCESS);
            submitted_ = true;
            REQUIRE(Wait() == VK_SUCCESS);
            return Mapped();
        }

    private:
        static constexpr VkDeviceSize ColorBytes = 64 * 32 * 8, TotalBytes = ColorBytes + 64 * 32 * 4;

        /** @brief Chooses only a real host-visible/coherent memory type allowed by this exact native allocation. */
        std::uint32_t MemoryType(std::uint32_t bits) const {
            const auto properties = loader_.MemoryProperties();
            constexpr auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
                if ((bits & (1U << i)) && (properties.memoryTypes[i].propertyFlags & required) == required)
                    return i;
            FAIL("Actual device has no compatible coherent diagnostic staging type.");
            std::terminate();
        }

        /** @brief Returns only the real native diagnostic fence result under a fixed five-second test budget. */
        VkResult Wait() const {
            return loader_.DeviceFunction<PFN_vkWaitForFences>("vkWaitForFences")(loader_.Device(), 1, &fence_, VK_TRUE, 5'000'000'000ULL);
        }

        /** @brief Encodes reversible native copy transitions without changing the frontend's logical state authority. */
        void Copy(VkImage image, VkImageAspectFlags aspect, VkImageLayout layout, VkDeviceSize offset, VkPipelineStageFlags stage,
                  VkAccessFlags access) {
            VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                         .srcAccessMask = access,
                                         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                         .oldLayout = layout,
                                         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = image,
                                         .subresourceRange = {aspect, 0, 1, 0, 1}};
            const auto transition = loader_.DeviceFunction<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
            transition(commands_, stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            const VkBufferImageCopy copy{.bufferOffset = offset, .imageSubresource = {aspect, 0, 0, 1}, .imageExtent = {64, 32, 1}};
            loader_.DeviceFunction<PFN_vkCmdCopyImageToBuffer>(
                "vkCmdCopyImageToBuffer")(commands_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &copy);
            std::swap(barrier.oldLayout, barrier.newLayout);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = access;
            transition(commands_, VK_PIPELINE_STAGE_TRANSFER_BIT, stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        /** @brief Copies only after actual coherent staging completion; bytes remain owned by the test result. */
        Pixels Mapped() {
            Pixels result;
            result.color.resize(64 * 32 * 4);
            result.depth.resize(64 * 32);
            void *mapped{};
            REQUIRE(loader_.DeviceFunction<PFN_vkMapMemory>("vkMapMemory")(loader_.Device(), memory_, 0, VK_WHOLE_SIZE, 0, &mapped) ==
                    VK_SUCCESS);
            std::memcpy(result.color.data(), mapped, ColorBytes);
            std::memcpy(result.depth.data(), static_cast<const std::byte *>(mapped) + ColorBytes, TotalBytes - ColorBytes);
            loader_.DeviceFunction<PFN_vkUnmapMemory>("vkUnmapMemory")(loader_.Device(), memory_);
            return result;
        }

        const ObservedLoader &loader_;
        VkBuffer buffer_{};
        VkDeviceMemory memory_{};
        VkCommandPool pool_{};
        VkCommandBuffer commands_{};
        VkFence fence_{};
        bool submitted_{};
    };
}  // namespace Horo::Render::Test::VfxInstanced
