#pragma once
/** @file EditorVulkanXrViewportAccess.h
 * @brief Non-installed explicit App access to the SAME selected viewport's XR resource/graph owner.
 */
#include "../../EditorViewportTextureView.h"
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "VulkanEditorGraphicsBridge.h"

namespace Horo::Render {
    class RenderFrameScope;
    struct PrimaryOutputAttachment;
}  // namespace Horo::Render

namespace Horo::Editor {
    class EditorViewportRendererVulkan;

    /** @brief Static composition boundary, without another viewport, scene, queue or completion authority.
     * @details All borrows use the viewport's render-owner lane and existing resource lifetime. The concrete
     * renderer remains responsible for validation. This header exposes no editor-private source include root.
     */
    class EditorVulkanXrViewportAccess final {
    public:
        /** @brief Borrow the actual selected native device; no creation or selection occurs. */
        [[nodiscard]] static Result<Render::Detail::VulkanEditorDevice> Device(const EditorViewportRendererVulkan &viewport);
        /** @brief Cold-prepare topology over exact canonical meshes and native allocation/release policy.
         * @param viewport Existing selected owner. @param meshes Complete immutable canonical mesh resource projection.
         * @param color Native allocation. @param acquired Actual acquisition state. @param final Required native release state.
         * @return Original owner/resource/graph error or successful preparation before interactive admission.
         */
        [[nodiscard]] static Result<void> Prepare(EditorViewportRendererVulkan &viewport,
                                                  std::span<const Render::RenderMeshResourceView> meshes,
                                                  const Render::RenderTextureDescriptor &color,
                                                  const Render::RenderGraphLogicalState &acquired,
                                                  const Render::RenderGraphLogicalState &final);
        /** @brief Bind one real acquisition before BeginFrame, using fixed owner storage and exact live publications.
         * @param viewport Existing owner. @param color Fresh native import. @param views Exact layer views.
         * @param publication Current Scene/frame/origin/device evidence. @param cameras Actual native view matrices.
         * @param instances Canonical scene instance values. @return Original typed validation/binding outcome.
         */
        [[nodiscard]] static Result<void> Bind(EditorViewportRendererVulkan &viewport, Render::RenderTextureHandle color,
                                               const std::array<Render::RenderTextureViewHandle, 2> &views,
                                               const Render::MultiviewPublication &publication,
                                               std::span<const Render::MultiviewSceneView> cameras,
                                               std::span<const Render::RenderStaticMeshInstance> instances);
        /** @brief Execute through the SAME actual App frame before its existing UI/GUI tail.
         * @param viewport Existing owner. @param frame Existing canonical frame scope. @param output Existing primary output.
         * @return Original admission/encoding outcome; no compositor success is inferred.
         */
        [[nodiscard]] static Result<void> Execute(EditorViewportRendererVulkan &viewport, Render::RenderFrameScope &frame,
                                                  const Render::PrimaryOutputAttachment &output);
        /** @brief Borrow only the current admitted sampled left-eye view, never a desktop-camera fallback.
         * @param viewport Exact selected owner. @param frame Same successfully bound App frame.
         * @param expected Exact retained acquired first-eye generation. @return Full matching GUI binding or empty.
         */
        [[nodiscard]] static EditorViewportTextureView Mirror(const EditorViewportRendererVulkan &viewport, std::uint64_t frame,
                                                              Render::RenderTextureViewHandle expected) noexcept;
        /** @brief Revokes only the borrowed mirror receipt on exact Scene loss/retirement; native queue debt stays owned. */
        static void RevokeMirror(EditorViewportRendererVulkan &viewport) noexcept;
        /** @brief Releases cold XR graph backing only after the App proved every acquisition/scene/GUI reference retired.
         * @param viewport SAME selected owner. @pre Exact native image retirement is already complete; owner lane.
         */
        [[nodiscard]] static Result<void> Retire(EditorViewportRendererVulkan &viewport);
        /** @brief Progress the SAME native queue with a zero-time observation, not an image-release proof. */
        [[nodiscard]] static Result<void> Poll(EditorViewportRendererVulkan &viewport);
    };
}  // namespace Horo::Editor
