#pragma once

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderParallelWork.h"

#include <memory>
#include <string>
#include <vector>

namespace Horo::Render::Detail {
    /** @brief Finite CPU capture envelope; all source arrays are charged before copying. */
    using RenderFrameInputLimits = RenderParallelWorkLimits;

    /** @brief Owned mesh bytes backing an immutable captured resource view. */
    struct CapturedRenderMesh final {
        CapturedRenderMesh() = default;
        CapturedRenderMesh(const CapturedRenderMesh &) = delete;
        CapturedRenderMesh &operator=(const CapturedRenderMesh &) = delete;
        CapturedRenderMesh(CapturedRenderMesh &&) noexcept = default;
        CapturedRenderMesh &operator=(CapturedRenderMesh &&) noexcept = default;

        std::vector<MeshVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    /** @brief One captured pass; descriptor spans refer only to storage in this value. */
    struct CapturedRenderPass final {
        CapturedRenderPass() = default;
        CapturedRenderPass(const CapturedRenderPass &) = delete;
        CapturedRenderPass &operator=(const CapturedRenderPass &) = delete;
        CapturedRenderPass(CapturedRenderPass &&other) noexcept;
        CapturedRenderPass &operator=(CapturedRenderPass &&other) noexcept;

        /** @brief Rebinds spans after all owner containers reach their final capacity or ownership changes. */
        void Rebind() noexcept;

        RenderPassDescriptor descriptor;
        std::vector<CapturedRenderMesh> meshes;
        std::vector<RenderMeshResourceView> resources;
        std::vector<RenderStaticMeshInstance> instances;
        std::vector<std::string> materials;
        std::vector<RenderLight> lights;
    };

    /** @brief Owned, immutable CPU frame handoff with no executor, frontend, or native pointer. */
    struct CapturedRenderFrame final {
        CapturedRenderFrame() = default;
        CapturedRenderFrame(const CapturedRenderFrame &) = delete;
        CapturedRenderFrame &operator=(const CapturedRenderFrame &) = delete;
        CapturedRenderFrame(CapturedRenderFrame &&) = delete;
        CapturedRenderFrame &operator=(CapturedRenderFrame &&) = delete;

        FrameToken frame;
        std::vector<CapturedRenderPass> passes;
        std::vector<RenderPassId> sortedPassIds;
        std::size_t chargedBytes{};
    };

    /**
     * @brief Charges the complete handoff, then freezes every borrowed payload before job admission.
     * @param frame Exact active frame token, checked again by the owner before execution.
     * @param passes Source storage borrowed only during capture.
     * @param limits Finite pass and total byte envelope including metadata.
     * @param cancellation Cooperative parent cancellation.
     * @return Owned immutable frame or an actionable capacity, cancellation, or descriptor error.
     */
    [[nodiscard]] Result<std::shared_ptr<const CapturedRenderFrame>> CaptureRenderFrameInputs(FrameToken frame,
                                                                                              std::span<const RenderPassDescriptor> passes,
                                                                                              const RenderFrameInputLimits &limits,
                                                                                              const CancellationToken &cancellation);

    /** @brief Validates one captured command on a worker with cancellation checks inside geometry traversal. */
    [[nodiscard]] Result<void> ValidateCapturedRenderPass(const CapturedRenderPass &pass, const CancellationToken &cancellation);
}  // namespace Horo::Render::Detail
