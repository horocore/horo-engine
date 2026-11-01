#pragma once

#include "EditorViewportShaderParameters.h"
#include "Horo/Runtime/Render/ClusteredLightGrid.h"
#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "MetalViewportClusteredLighting.h"
#include "MetalViewportResourceBridge.h"
#include "editor/renderer/EditorViewportResources.h"
#include "editor/renderer/grid/EditorViewportGridGeometry.h"
#include "editor/screens/workspace/panels/viewport/visualizers/light/LightVisualizerGeometry.h"

namespace Horo::Editor {
    struct MetalViewportGraphAuthor;

    /** @brief Fixed frame-slot graph data. Only topology/resource changes rebuild cold graph ownership. */
    class MetalViewportClusteredFrame final {
    public:
        static constexpr std::size_t MaximumDraws = Render::RenderGraphMaterialRaster::MaximumDraws;
        static constexpr std::size_t MaximumPasses = 9;
        static constexpr std::size_t MaximumUniformDraws = MaximumDraws * 7;
        static constexpr std::size_t MaximumUniformBytes = 4096;

        MetalViewportClusteredFrame(Render::RenderFrontend &frontend, MetalViewportClusteredLighting &programs,
                                    EditorViewportResources &resources, Render::MetalEditorGraphicsBridge &bridge,
                                    const EditorViewportLightingProfile &profile);
        ~MetalViewportClusteredFrame();
        [[nodiscard]] Result<void> Prepare(const Render::StaticMeshPassDescriptor &descriptor, Render::RenderViewId view);
        [[nodiscard]] Result<void> Execute(Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &descriptor,
                                           const Render::PrimaryOutputAttachment &output);
        [[nodiscard]] EditorViewportLightingDiagnostics Diagnostics() const noexcept;
        [[nodiscard]] bool MatchesProfile(const EditorViewportLightingProfile &profile) const noexcept;
        void SetPresentation(const EditorViewportGridOptions &grid, const EditorViewportLightVisualizerOptions &visualizer) noexcept;

    private:
        struct DrawSlot final {
            Render::RenderBufferHandle buffer;
            std::size_t offset{};
            std::shared_ptr<const Render::ResidentMaterialBinding> material;
            std::array<std::byte, MaximumUniformBytes> bytes;
        };

        struct Slot final {
            struct Overlay final {
                Render::RenderMeshHandle mesh;
                Render::RenderMeshDescriptor descriptor;
                std::vector<Render::MeshVertex> vertices;
                std::vector<std::uint32_t> indices;
                Math::Mat4 matrix{Math::Mat4::Identity()};
                Math::Vec3 color;
            };

            std::array<Overlay, 3> overlays;
            std::array<DrawSlot, MaximumUniformDraws> uniforms;
            std::array<Render::RenderBufferHandle, 3> uniformBuffers;

            struct Variant final {
                std::array<std::array<Render::RenderGraphMaterialDraw, MaximumDraws>, 7> draws;
                std::array<std::array<Render::RenderGraphMaterialRead, MaximumDraws + 4>, 7> reads;
                std::array<Render::RenderGraphPassWorkload, MaximumPasses> workloads;
                std::optional<Render::CompiledRenderGraphExecution> graph;
            };

            std::array<Variant, 5> variants;
            Render::UploadedLightFrame uploaded;
            Render::RenderTextureHandle color, depth, atlas;
            std::array<Render::RenderMeshHandle, MaximumDraws> meshes;
            std::size_t meshCount{};
            std::uint64_t atlasGeneration{};
            std::uint64_t materialRevision{};
            void *recordedCommands{}; /**< Compared only against owning native-use evidence; never dereferenced by this owner. */
            std::uint64_t recordedLightRevision{};
            std::uint32_t recordedClusterCount{};
            bool diagnosticsPending{};
        };

        [[nodiscard]] Result<void> PrepareCanonical(const Render::RenderSceneView &scene, Render::RenderViewId view,
                                                    Render::FramebufferExtent extent);
        [[nodiscard]] Result<void> ValidateCanonical(const Render::RenderSceneView &scene) const;
        [[nodiscard]] Result<void> ValidateReadyMeshes(const Render::RenderSceneView &scene) const;
        void CloseSlot(Slot &slot) noexcept;
        [[nodiscard]] Result<void> PrepareSlot(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor);
        [[nodiscard]] Result<void> CollectDiagnostics(Slot &slot);
        [[nodiscard]] Result<void> PrepareGraphMaterials(Slot &slot, const Render::RenderSceneView &scene);
        [[nodiscard]] Result<void> PrepareUniforms(Slot &slot);
        [[nodiscard]] Result<void> WriteUniforms(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor);
        [[nodiscard]] Result<void> WriteDrawUniform(Slot &slot, std::size_t pass, std::size_t draw,
                                                    const Render::StaticMeshPassDescriptor &descriptor);
        [[nodiscard]] Result<void> PrepareOverlays(Slot &slot);
        [[nodiscard]] Result<void> PrepareOverlay(Slot::Overlay &overlay, std::size_t count);
        [[nodiscard]] Result<void> WriteGridOverlay(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor,
                                                    const Math::Mat4 &matrix);
        [[nodiscard]] Result<void> WriteVisualizerOverlay(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor,
                                                          const Math::Mat4 &matrix);
        [[nodiscard]] Result<void> WriteOverlays(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor);
        [[nodiscard]] Result<void> BuildOverlayRaster(Slot &slot, Slot::Variant &variant, MetalViewportGraphAuthor &author,
                                                      Render::RenderGraphMaterialRaster &raster);
        [[nodiscard]] Result<void> BuildGraph(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor);
        [[nodiscard]] Result<void> BuildVariant(Slot &slot, Slot::Variant &variant, std::uint32_t cascadeCount,
                                                const Render::StaticMeshPassDescriptor &descriptor);
        [[nodiscard]] Result<void> BuildRaster(Slot &slot, Slot::Variant &variant, MetalViewportGraphAuthor &author, std::size_t pass,
                                               std::uint32_t cascadeCount, const Render::StaticMeshPassDescriptor &descriptor);
        [[nodiscard]] Result<void> BuildRasterDraws(Slot &slot, Slot::Variant &variant, MetalViewportGraphAuthor &author, std::size_t pass,
                                                    Render::RenderGraphMaterialRaster &raster, const Render::RenderSceneView &scene);
        [[nodiscard]] Result<void> PrepareMaterial(Slot &slot, std::size_t pass, std::size_t draw);
        [[nodiscard]] bool MatchesTopology(const Slot &slot, const Render::RenderSceneView &scene) const noexcept;
        [[nodiscard]] std::uint64_t FrameSignature(const Render::StaticMeshPassDescriptor &descriptor) const noexcept;
        Render::RenderFrontend *frontend_;
        MetalViewportClusteredLighting *programs_;
        EditorViewportResources *resources_;
        Render::MetalEditorGraphicsBridge *bridge_;
        EditorViewportLightingProfile profile_;
        std::unique_ptr<Slot[]> slots_;
        std::array<Render::IdentifiedRenderLight, Render::MaximumSceneLights> lights_;
        std::array<Render::LightCluster, Render::LightCullingBudget::HardMaximumClusters> clusters_;
        std::array<Render::PackedRenderLight, Render::MaximumSceneLights> packedLights_;
        std::array<Render::PackedLightCluster, Render::LightCullingBudget::HardMaximumClusters> packedClusters_;
        std::array<MetalViewportBufferUpdate, MaximumUniformDraws + 3> updates_;
        std::array<MetalViewportBufferUpdate, 6> overlayUpdates_;
        ViewportGridGeometry gridGeometry_;
        LightVisualizerGeometry visualizerGeometry_;
        EditorViewportGridOptions gridOptions_;
        EditorViewportLightVisualizerOptions visualizerOptions_;
        Render::ClusteredLightGrid grid_;
        std::optional<Render::DirectionalShadowFrame> shadow_;
        EditorViewportLightingDiagnostics diagnostics_;
        std::size_t nextSlot_{}, activeSlot_{};
        std::uint64_t preparedSignature_{};
        bool ready_{};
    };
}  // namespace Horo::Editor
