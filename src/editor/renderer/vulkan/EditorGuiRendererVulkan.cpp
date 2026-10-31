#include "EditorGuiRendererVulkan.h"

#include "EditorVulkanGuiFunctionLoader.h"
#include "ImGuiVulkanSlots.h"
#include "RenderFrontendResourceAccess.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
#include <imgui_impl_sdl3.h>

namespace Horo::Editor {
    namespace {
        /** @brief Upstream upload submission is unavailable: only the canonical Horo command owner may submit. */
        VKAPI_ATTR VkResult VKAPI_CALL DenySubmit(VkQueue, std::uint32_t, const VkSubmitInfo *, VkFence) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        /** @brief Neither GUI font upload nor an encoder may introduce a normal-frame queue idle. */
        VKAPI_ATTR VkResult VKAPI_CALL DenyQueueIdle(VkQueue) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        VKAPI_ATTR VkResult VKAPI_CALL DenyDeviceIdle(VkDevice) {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }

        /** @brief Load-time C++ callback failure, caught before returning through the application boundary. */
        struct NativeInitializationFailure {
            VkResult value;
        };

        void CheckNative(VkResult result) {
            if (result < 0)
                throw NativeInitializationFailure{result};
        }
    }  // namespace

    /** @copydoc EditorGuiRendererVulkan::EditorGuiRendererVulkan */
    EditorGuiRendererVulkan::EditorGuiRendererVulkan(SDL_Window &window, Render::RenderFrontend &frontend,
                                                     Render::Detail::VulkanEditorGraphicsBridge &bridge) noexcept
        : window_(window), frontend_(frontend), bridge_(bridge) {}

    EditorGuiRendererVulkan::~EditorGuiRendererVulkan() {
        Shutdown();
    }

    /** @copydoc EditorGuiRendererVulkan::Validate */
    Result<void> EditorGuiRendererVulkan::Validate() const {
        if (owner_ != std::this_thread::get_id() || !ImGui::GetCurrentContext() || !bridge_.Port())
            return Result<void>::Failure(MakeError(RendererErrors::GuiOwnerInvalid));
#ifdef IMGUI_HAS_VIEWPORT
        // Docking builds expose platform viewports; each would require its own qualified canonical owner.
        if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
            return Result<void>::Failure(MakeError(RendererErrors::GuiOwnerInvalid));
#endif
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererVulkan::Load */
    PFN_vkVoidFunction EditorGuiRendererVulkan::Load(const char *name, void *context) {
        const auto &device = static_cast<EditorGuiRendererVulkan *>(context)->device_;
        if (std::strcmp(name, "vkQueueSubmit") == 0)
            return std::bit_cast<PFN_vkVoidFunction>(&DenySubmit);
        if (std::strcmp(name, "vkQueueWaitIdle") == 0)
            return std::bit_cast<PFN_vkVoidFunction>(&DenyQueueIdle);
        if (std::strcmp(name, "vkDeviceWaitIdle") == 0)
            return std::bit_cast<PFN_vkVoidFunction>(&DenyDeviceIdle);
        if (std::strcmp(name, "vkCmdBeginRenderingKHR") == 0)
            name = "vkCmdBeginRendering";
        if (std::strcmp(name, "vkCmdEndRenderingKHR") == 0)
            name = "vkCmdEndRendering";
        return Detail::ResolveVulkanGuiFunction(device, name);
    }

    /** @copydoc EditorGuiRendererVulkan::Initialize */
    Result<void> EditorGuiRendererVulkan::Initialize() {
        if (const auto valid = Validate(); valid.HasError())
            return valid;
        if (platformInitialized_ || nativeInitialized_)
            return Result<void>::Failure(MakeError(RendererErrors::GuiOwnerInvalid));
        const auto device = bridge_.Port()->QueryDevice();
        if (device.HasError())
            return Result<void>::Failure(device.ErrorValue());
        device_ = device.Value();
        if (device_.frames == 0 || device_.frames > slots_.size())
            return Result<void>::Failure(MakeError(RendererErrors::GuiNativeInitializationFailed));
        const auto attached = bridge_.Port()->AttachDeviceChild(*this);
        if (attached.HasError())
            return attached;
        childAttached_ = true;
        const auto fail = [this](const Error &error) {
            Shutdown();
            return Result<void>::Failure(error);
        };
        const auto sampler = frontend_.CreateSampler({});
        if (sampler.HasError())
            return fail(sampler.ErrorValue());
        sampler_ = sampler.Value().handle;
        if (const auto ready = Realize(sampler.Value().operation); ready.HasError())
            return fail(ready.ErrorValue());
        unsigned char *pixels{};
        int width{}, height{};
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        if (width <= 0 || height <= 0 || !pixels)
            return fail(MakeError(RendererErrors::GuiTextureInvalid));
        const auto font = CreateTexture({static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                         std::span{pixels, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4}});
        if (font.HasError())
            return fail(font.ErrorValue());
        fontIdentity_ = font.Value();
        if (const auto prepared = InitializeSlots(); prepared.HasError())
            return fail(prepared.ErrorValue());
        if (const auto native = InitializeNative(); native.HasError())
            return fail(native.ErrorValue());
        platformInitialized_ = ImGui_ImplSDL3_InitForVulkan(&window_);
        if (!platformInitialized_)
            return fail(MakeError(RendererErrors::GuiNativeInitializationFailed));
        ImGui::GetIO().Fonts->SetTexID(static_cast<ImTextureID>(fontIdentity_));
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererVulkan::InitializeNative */
    Result<void> EditorGuiRendererVulkan::InitializeNative() {
        if (!ImGui_ImplVulkan_LoadFunctions(&Load, this))
            return Result<void>::Failure(MakeError(RendererErrors::GuiNativeInitializationFailed));
        const auto font = Resolve(fontIdentity_);
        if (font.HasError())
            return Result<void>::Failure(font.ErrorValue());
        const auto view = Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend_, font.Value().view);
        const auto sampler = Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend_, sampler_);
        if (view.HasError())
            return Result<void>::Failure(view.ErrorValue());
        if (sampler.HasError())
            return Result<void>::Failure(sampler.ErrorValue());
        const auto sample = bridge_.Port()->BorrowSample({view.Value(), sampler.Value(), 0});
        if (sample.HasError())
            return Result<void>::Failure(sample.ErrorValue());
        updateDescriptors_ = std::bit_cast<PFN_vkUpdateDescriptorSets>(device_.deviceResolver(device_.device, "vkUpdateDescriptorSets"));
        if (!updateDescriptors_)
            return Result<void>::Failure(MakeError(RendererErrors::GuiNativeInitializationFailed));
        ImGui_ImplVulkan_InitInfo info{};
        info.Instance = device_.instance;
        info.PhysicalDevice = device_.physical;
        info.Device = device_.device;
        info.QueueFamily = device_.graphicsFamily;
        info.Queue = device_.graphicsQueue;
        info.MinImageCount = 2;
        info.ImageCount = 2;  // Upstream swapchain convention; actual encoder slots are separately canonical.
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.DescriptorPoolSize = device_.frames * MaximumTextures;
        info.UseDynamicRendering = true;
        info.PipelineRenderingCreateInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                            .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &device_.outputFormat};
        info.CheckVkResultFn = &CheckNative;
        try {
            nativeInitialized_ = ImGui_ImplVulkan_Init(&info);
            if (!nativeInitialized_ || !HoroImGuiVulkanPrepareSlots(device_.frames))
                return Result<void>::Failure(MakeError(RendererErrors::GuiNativeInitializationFailed));
            for (std::uint32_t slot = 0; slot < device_.frames; ++slot)
                for (auto &descriptor : slots_[slot].descriptors)
                    descriptor =
                        ImGui_ImplVulkan_AddTexture(sample.Value().sampler, sample.Value().view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } catch (const NativeInitializationFailure &) {
            // Upstream publishes its partial backend state before native creation; shutdown owns that rollback.
            nativeInitialized_ = ImGui::GetIO().BackendRendererUserData != nullptr;
            return Result<void>::Failure(MakeError(RendererErrors::GuiNativeInitializationFailed));
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorGuiRendererVulkan::ReleaseNative */
    void EditorGuiRendererVulkan::ReleaseNative() noexcept {
        if (owner_ != std::this_thread::get_id())
            std::terminate();
        if (nativeInitialized_) {
            HoroImGuiVulkanDetachSlots();
            ImGui_ImplVulkan_Shutdown();
            nativeInitialized_ = false;
        }
        childAttached_ = false;
        recordedEver_ = false;
    }

    /** @copydoc EditorGuiRendererVulkan::Shutdown */
    void EditorGuiRendererVulkan::Shutdown() noexcept {
        if (owner_ != std::this_thread::get_id() || (nativeInitialized_ && recordedEver_))
            std::terminate();
        if (childAttached_ && bridge_.Port())
            bridge_.Port()->DetachDeviceChild(*this);
        ReleaseNative();
        composition_.Reset();
        BindViewportTexture({});
        for (auto &texture : textures_)
            ReleaseTexture(texture);
        for (auto &slot : slots_)
            for (auto &buffer : slot.buffers) {
                if (buffer.IsValid())
                    static_cast<void>(frontend_.ReleaseBuffer(buffer));
                buffer = {};
            }
        if (sampler_.IsValid())
            static_cast<void>(frontend_.ReleaseSampler(sampler_));
        sampler_ = {};
        if (platformInitialized_)
            ImGui_ImplSDL3_Shutdown();
        platformInitialized_ = false;
        if (ImGui::GetCurrentContext() && static_cast<std::uintptr_t>(ImGui::GetIO().Fonts->TexID) == fontIdentity_)
            ImGui::GetIO().Fonts->SetTexID(0);
        fontIdentity_ = 0;
    }
}  // namespace Horo::Editor
