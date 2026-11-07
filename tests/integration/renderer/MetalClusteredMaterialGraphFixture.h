#pragma once
/** @file MetalClusteredMaterialGraphFixture.h @brief Real private-runtime qualification before capability advertisement. */

#include "EditorClusteredLightingAssets.h"
#include "EditorViewportShaderParameters.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/RenderMemoryBudget.h"
#include "runtime/renderer/frontend/RenderResourceRegistry.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"
#include "runtime/renderer/modules/metal/MetalResourceRuntime.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::Render::Test {
    enum class ClusteredQualificationSample : std::uint8_t {
        BottomRight,
        BottomLeft,
        TopFar
    };

    /** @brief Owns an actual optional Metal layer without creating a window or acquiring drawables. */
    class ClusteredQualificationPort final : public IMetalPresentationPort {
    public:
        [[nodiscard]] Result<void> CreateSurface() override;
        [[nodiscard]] void *Layer() const noexcept override;
        void DestroySurface() noexcept override;

    private:
        __strong CAMetalLayer *layer_{nil};
    };

    /** @brief A real native material adapter using only registry-issued resident generations. */
    class ClusteredQualificationMaterials final : public IMaterialBindingBackend {
    public:
        Detail::MetalResourceRuntime *runtime{};
        Detail::RenderResourceRegistry *registry{};
        [[nodiscard]] Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &descriptor) override;
    };

    /** @brief Keeps every issued resource and native command alive until actual completion or explicit unsent abort. */
    class MetalClusteredMaterialGraphFixture final {
    public:
        struct Program final {
            RenderShaderModuleHandle vertex, fragment;
            RenderPipelineHandle pipeline;
            MaterialBindingLayout layout;
            std::size_t uniformBytes{};
        };

        struct Registered final {
            Detail::RenderResourceClass kind;
            Detail::ResourceReservation reservation;
            std::optional<RenderMemoryReservationId> memory;
            RenderMemoryPlacement placement;
            RenderResourceNativeLifetime lifetime;
        };

        MetalClusteredMaterialGraphFixture() = default;
        ~MetalClusteredMaterialGraphFixture();
        /** @brief Initializes the actual selected Metal device and an independent private-runtime registry; no capability override. */
        void Initialize(const Editor::EditorClusteredLightingAssets &assets);
        /** @brief Prepares one actual 3D grid and either 32 or zero live canonical fixture lights. */
        void Upload(std::uint32_t count, std::uint64_t revision,
                    ClusteredQualificationSample sample = ClusteredQualificationSample::BottomRight);
        /** @brief Records and either submits the exact compute→depth→lit graph or discards the unsent native journal. */
        void Execute(bool commit);
        /** @brief Requires actual successful completion and checks native membership, source indices and visible pixels. */
        void CheckCompleted(std::uint32_t count);
        /** @brief Checks copied packing mutations against actual driver reflection from the genuine raster modules. */
        void CheckUniformAbi(const Editor::EditorClusteredLightingAssets &assets);
        /** @brief Proves failed native material publication preserves the previously acquired real generation. */
        void CheckLastGoodMaterial();
        /** @brief Releases issued resources only after terminal native command evidence. */
        void Close();

    private:
        void CreateOwners();
        [[nodiscard]] Registered Reserve(Detail::RenderResourceClass kind, std::span<const Detail::RenderResourceIdentity> dependencies,
                                         const std::optional<RenderMemoryCostPlan> &cost = std::nullopt, bool nativeLifetime = false);
        [[nodiscard]] Detail::RenderResourceIdentity Publish(const Registered &record, std::uint64_t native);
        [[nodiscard]] std::uint64_t Native(Detail::RenderResourceClass kind, Detail::RenderResourceIdentity identity) const;
        void Destroy(Detail::RenderResourceClass kind, std::uint64_t native, Detail::BackendResourceReleaseMode mode) noexcept;
        [[nodiscard]] RenderBufferHandle Buffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> bytes = {});
        [[nodiscard]] RenderTextureHandle Texture(const RenderTextureDescriptor &descriptor);
        [[nodiscard]] RenderTextureViewHandle View(RenderTextureHandle texture, RenderTextureFormat format, RenderTextureAspect aspect);
        void PreparePrograms(const Editor::EditorClusteredLightingAssets &assets);
        void PrepareProgram(const ShaderHotReloadProgramView &view, Program &program, bool depthOnly);
        [[nodiscard]] RenderShaderModuleHandle Stage(const ShaderHotReloadProgramView &view, ShaderStage stage, const char *entry);
        void PrepareScene();
        void PrepareMaterials();
        void PrepareGraph();
        void ResolveGraphInstances();
        void WriteParameters(std::uint32_t count);
        void ConfigureSample(ClusteredQualificationSample sample);
        void ReadPixel();
        void RequireTerminal();
        void CheckAttachmentJournal(bool recorded);
        void ReleaseOwners() noexcept;
        [[nodiscard]] NativeLightFrameUpdate UpdateRequest(std::uint64_t revision) const;

        ClusteredQualificationPort port_;
        MetalEditorGraphicsBridge bridge_;
        RenderBackendRegistry backends_;
        std::unique_ptr<RenderFrontend> frontend_;
        Detail::MetalResourceRuntime runtime_;
        std::unique_ptr<RenderMemoryBudget> memory_;
        std::unique_ptr<Detail::RenderResourceRegistry> registry_;
        ClusteredQualificationMaterials adapter_;
        std::unique_ptr<MaterialBindingTable> materials_;
        std::array<Program, 2> programs_;
        RenderSamplerHandle sampler_;
        RenderMeshHandle mesh_;
        RenderBufferHandle vertices_, indices_, litUniform_, depthUniform_, readback_;
        LightFrameBuffers buffers_;
        RenderTextureHandle color_, depth_, atlas_;
        RenderTextureViewHandle colorView_, depthView_, atlasView_;
        std::shared_ptr<IResidentLightCullingKernel> kernel_;
        std::shared_ptr<const ResidentMaterialBinding> litMaterial_, depthMaterial_;
        std::optional<CompiledRenderGraphExecution> graph_;
        std::array<RenderGraphPassWorkload, 4> workloads_;
        std::vector<RenderGraphResourceInstance> resources_;
        std::array<RenderGraphMeshInstance, 1> meshes_;
        std::array<RenderGraphTextureViewInstance, 3> views_;
        std::array<RenderGraphMaterialDraw, 1> litDraw_, depthDraw_;
        std::array<RenderGraphMaterialRead, 5> litReads_;
        std::array<RenderGraphMaterialRead, 1> depthReads_;
        std::array<RenderLight, 32> lights_;
        std::array<IdentifiedRenderLight, 32> identified_;
        std::array<LightCluster, 16> clusters_;
        std::array<PackedRenderLight, 32> packedLights_;
        std::array<PackedLightCluster, 16> packedClusters_;
        std::array<LightClusterMembership, 16> expectedMembership_;
        std::array<std::uint32_t, 16 * 32> expectedReferences_;
        ClusteredLightGrid grid_;
        LightCullingBudget budget_{32, 16, 32, true};
        RenderCameraView camera_{.position = {0, 0, 3},
                                 .target = {0, 0, 0},
                                 .projection = {.kind = RenderProjectionKind::Orthographic, .orthographicHeight = 4}};
        RenderViewId view_;
        std::uint32_t count_{};
        std::uint32_t pixelX_{40}, pixelY_{40};
        std::array<std::uint32_t, 2> expectedCells_{7, 7}; /**< A single-cell sample repeats its one member. */
        std::uint64_t revision_{};
        __strong id<MTLCommandBuffer> commands_{nil};
        __strong id<MTLSharedEvent> gate_{nil};
        bool committed_{};
        bool closed_{};
        bool productionGraphCapability_{}, productionBatchCapability_{};
    };
}  // namespace Horo::Render::Test
