#pragma once
/** @file EditorGuiRendererVulkan.h
 * @brief Matching SDL/Vulkan GUI encoder using canonical frontend ownership and retired slots. */
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "VulkanEditorGraphicsBridge.h"
#include "editor/renderer/EditorGuiComposition.h"

#include <array>
#include <thread>

struct SDL_Window;

namespace Horo::Editor {
    /** @brief Host-owned encoder; native children are released after canonical drain, before the selected device. */
    class EditorGuiRendererVulkan final : public IEditorGuiRenderer,
                                          public Render::Detail::IVulkanGuiEncoder,
                                          public Render::Detail::IVulkanEditorDeviceChild {
    public:
        EditorGuiRendererVulkan(SDL_Window &window, Render::RenderFrontend &frontend,
                                Render::Detail::VulkanEditorGraphicsBridge &bridge) noexcept;
        ~EditorGuiRendererVulkan() override;
        [[nodiscard]] Result<void> Initialize() override;
        [[nodiscard]] Result<void> BeginFrame() override;
        [[nodiscard]] Result<Render::GuiCompositionPreparation> PrepareComposition(Render::FrameToken frame) override;
        [[nodiscard]] Result<void> RenderDrawData(const Render::GuiCompositionRequest &request) override;
        [[nodiscard]] Result<std::uintptr_t> CreateTexture(const EditorRgba8ImageView &image) override;
        void DestroyTexture(std::uintptr_t identity) noexcept override;
        void Shutdown() noexcept override;
        void ReleaseNative() noexcept override;
        [[nodiscard]] Result<void> Record(VkCommandBuffer command, Render::Detail::VulkanFrameRetentionSlot slot,
                                          std::span<const Render::Detail::VulkanGuiSampleBinding> samples,
                                          std::span<const Render::Detail::VulkanGuiBufferBinding> buffers) override;

    private:
        static constexpr std::size_t MaximumTextures = Render::GuiCompositionPreparation::MaximumResources;
        static constexpr std::uint32_t MaximumVertices = 65536;
        static constexpr std::uint32_t MaximumIndices = 131072;

        struct Texture {
            std::uintptr_t identity{};
            Render::RenderTextureHandle texture;
            Render::RenderTextureViewHandle view;
            Render::RenderBufferHandle staging;
        };

        struct Slot {
            std::array<Render::RenderBufferHandle, 2> buffers;
            std::array<VkDescriptorSet, MaximumTextures> descriptors{};
        };

        [[nodiscard]] Result<void> Validate() const;
        [[nodiscard]] Result<void> Realize(Render::ResourceOperationId operation);
        [[nodiscard]] Result<void> InitializeNative();
        [[nodiscard]] Result<void> InitializeSlots();
        [[nodiscard]] Result<Render::GuiSampledResource> Resolve(std::uintptr_t identity) const;
        void ReleaseTexture(Texture &texture) noexcept;
        static PFN_vkVoidFunction Load(const char *name, void *context);
        static VkDescriptorSet Descriptor(ImTextureID identity, void *context);

        const std::thread::id owner_{std::this_thread::get_id()};
        SDL_Window &window_;
        Render::RenderFrontend &frontend_;
        Render::Detail::VulkanEditorGraphicsBridge &bridge_;
        Render::Detail::VulkanEditorDevice device_;
        EditorGuiComposition composition_;
        std::array<Texture, MaximumTextures> textures_;
        std::array<Slot, 8> slots_;
        std::array<Render::RenderBufferHandle, 2> preparedBuffers_;
        Render::RenderSamplerHandle sampler_;
        PFN_vkUpdateDescriptorSets updateDescriptors_{};
        std::uintptr_t nextIdentity_{1};
        std::uintptr_t fontIdentity_{};
        std::uint32_t recordingSlot_{};
        bool platformInitialized_{};
        bool nativeInitialized_{};
        bool childAttached_{};
        bool recordedEver_{};
    };
}  // namespace Horo::Editor
