#pragma once
/** @file EditorViewportRendererVulkan.h
 * @brief Selected Vulkan viewport using canonical cooked material, frontend resources and frame submission. */
#include "EditorVulkanViewportShader.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "VulkanEditorGraphicsBridge.h"
#include "editor/renderer/EditorViewportResources.h"

#include <array>
#include <thread>

namespace Horo::Editor {
    class EditorViewportRendererVulkan final : public IEditorViewportRenderer, public Render::Detail::IVulkanEditorDeviceChild {
    public:
        EditorViewportRendererVulkan(Render::RenderFrontend &frontend, Render::Detail::VulkanEditorGraphicsBridge &bridge,
                                     EditorVulkanViewportCook cooked) noexcept;
        ~EditorViewportRendererVulkan() override;
        [[nodiscard]] Result<void> Initialize();
        void Shutdown() noexcept;
        void ReleaseNative() noexcept override;
        [[nodiscard]] Result<void> ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                        std::span<const Render::RenderPassDescriptor> passes) override;
        [[nodiscard]] Result<void> ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &) override;
        [[nodiscard]] Result<std::optional<Render::RenderTargetHandle>> PrepareResources(Render::RenderFrontend &frontend,
                                                                                         const Render::RenderSceneView &scene) override;
        void RequestExtent(EditorViewportExtent extent) noexcept override;
        void RequestGrid(const EditorViewportGridOptions &options) noexcept override;
        void RequestLightVisualizer(const EditorViewportLightVisualizerOptions &options) noexcept override;
        [[nodiscard]] EditorViewportExtent RequestedExtent() const noexcept override;
        [[nodiscard]] Math::ClipDepthRange ClipDepthRange() const noexcept override;
        [[nodiscard]] EditorViewportTextureView TextureView() const noexcept override;
        [[nodiscard]] bool IsReady() const noexcept override;

    private:
        static constexpr std::size_t MaximumUniformBytes = 1024;

        struct DrawSlot {
            Render::RenderBufferHandle buffer;
            std::uint64_t instance{};
            std::shared_ptr<const Render::ResidentMaterialBinding> material;
        };

        struct FrameSlot {
            std::array<DrawSlot, Render::RenderMaterialPass::MaximumDraws> draws;
        };

        struct EmptyGraph {
            Render::CompiledRenderGraphExecution graph;
            std::array<Render::RenderGraphPassWorkload, 1> workloads;
        };

        /** @brief One cooked-program admission and its dependent material resources, released after device drain. */
        struct ShaderResources {
            EditorVulkanViewportCook cooked;
            Render::RenderShaderModuleHandle vertex, fragment;
            Render::RenderPipelineHandle pipeline;
            Render::MaterialBindingLayout layout;
            std::unique_ptr<Render::MaterialBindingTable> materials;
            std::size_t uniformBytes{};
        };

        [[nodiscard]] Result<void> CreatePipeline();
        /** @brief Admits the reflected uniform layout and creates its one binding table. */
        [[nodiscard]] Result<void> CreateMaterialLayout(const Render::NormalizedShaderReflection &reflection);
        [[nodiscard]] Result<void> CreateDrawSlots();
        [[nodiscard]] Result<void> CreateEmptyGraph();
        /** @brief Submits the cached empty-scene clear through the exact current frontend frame. */
        [[nodiscard]] Result<void> ExecuteEmptyViewport(Render::RenderFrameScope &frame, Render::ClearColor clearColor);
        [[nodiscard]] Result<void> ExecuteViewport(Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &pass);
        [[nodiscard]] Result<void> WriteDraw(Render::FrameToken frame, const DrawSlot &slot,
                                             const Render::RenderStaticMeshInstance &instance, const Render::RenderSceneView &scene);
        [[nodiscard]] Result<void> WriteParameter(std::span<std::byte> bytes, Render::ShaderParameterId parameter,
                                                  std::span<const float> values) const;
        const std::thread::id owner_{std::this_thread::get_id()};
        Render::RenderFrontend &frontend_;
        Render::Detail::VulkanEditorGraphicsBridge &bridge_;
        ShaderResources shader_;
        EditorViewportResources resources_;
        Render::Detail::VulkanEditorDevice device_;
        std::array<FrameSlot, 8> slots_;
        std::array<Render::RenderMaterialDraw, Render::RenderMaterialPass::MaximumDraws> preparedDraws_;
        std::optional<EmptyGraph> emptyGraph_;
        Render::RenderTextureHandle emptyGraphTexture_;
        EditorViewportExtent requestedExtent_;
        EditorViewportGridOptions requestedGrid_;
        EditorViewportLightVisualizerOptions requestedLightVisualizer_;
        bool initialized_{};
        bool childAttached_{};
        bool recordedEver_{};
    };
}  // namespace Horo::Editor
