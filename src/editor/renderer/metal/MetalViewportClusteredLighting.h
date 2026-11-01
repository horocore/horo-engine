#pragma once

#include "EditorViewportLighting.h"
#include "Horo/Runtime/Render/LightCullingKernel.h"
#include "Horo/Runtime/Render/LightFrameBufferPool.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/ShaderHotReloadTarget.h"

namespace Horo::Editor {
    /** @brief Cold-owned complete cooked family used exclusively by the qualified Metal graph recipe. */
    class MetalViewportClusteredLighting final {
    public:
        struct Program final {
            Render::RenderShaderModuleHandle vertex, fragment;
            Render::RenderPipelineHandle pipeline;
            Render::MaterialBindingLayout layout;
            std::unique_ptr<Render::MaterialBindingTable> materials;
            std::size_t uniformBytes{};
        };

        explicit MetalViewportClusteredLighting(Render::RenderFrontend &frontend) noexcept;
        ~MetalViewportClusteredLighting();
        MetalViewportClusteredLighting(const MetalViewportClusteredLighting &) = delete;
        MetalViewportClusteredLighting &operator=(const MetalViewportClusteredLighting &) = delete;

        /** @brief Creates actual native stages, pipelines and finite frame storage; failure rolls back this unpublished owner. */
        [[nodiscard]] Result<void> Prepare(std::span<const Render::ShaderHotReloadProgramView> family,
                                           const Render::CookedLightCullingKernel &kernel, const EditorViewportLightingProfile &profile);
        [[nodiscard]] Result<void> Close();
        /** @brief Rebuilds all raster roles while retaining the exact already-admitted compute kernel lease. */
        [[nodiscard]] Result<void> PrepareReplacement(std::span<const Render::ShaderHotReloadProgramView> family,
                                                      const MetalViewportClusteredLighting &previous,
                                                      const EditorViewportLightingProfile &profile);

        [[nodiscard]] const Program &Role(std::size_t role) const noexcept {
            return programs_[role];
        }

        [[nodiscard]] Render::LightFrameBufferPool &Frames() noexcept {
            return *frames_;
        }

        [[nodiscard]] const std::shared_ptr<Render::IResidentLightCullingKernel> &Kernel() const noexcept {
            return kernel_;
        }

        [[nodiscard]] std::uint32_t SlotCount() const noexcept {
            return slots_;
        }

        [[nodiscard]] Render::RenderSamplerHandle Sampler() const noexcept {
            return sampler_;
        }

    private:
        [[nodiscard]] Result<void> PrepareProgram(const Render::ShaderHotReloadProgramView &view, std::size_t role);
        [[nodiscard]] Result<void> PreparePipeline(Program &program, const Render::ShaderHotReloadProgramView &view,
                                                   const Render::CompiledShaderArtifact &artifact, std::size_t role);
        [[nodiscard]] Result<void> PrepareMaterialOwner(Program &program, const Render::NormalizedShaderReflection &reflection);
        [[nodiscard]] Result<void> PrepareStorage(const EditorViewportLightingProfile &profile);
        Render::RenderFrontend *frontend_;
        std::array<Program, 3> programs_;
        std::unique_ptr<Render::LightFrameBufferPool> frames_;
        std::shared_ptr<Render::IResidentLightCullingKernel> kernel_;
        Render::RenderSamplerHandle sampler_;
        std::uint32_t slots_{};
    };
}  // namespace Horo::Editor
