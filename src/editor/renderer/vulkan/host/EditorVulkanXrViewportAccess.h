#pragma once
/** @file EditorVulkanXrViewportAccess.h
 * @brief Non-installed explicit App access to the SAME selected viewport's XR resource/graph owner.
 */
#include "../../EditorViewportTextureView.h"
#include "EditorVulkanXrProjectionAlpha.h"
#include "Horo/Runtime/Render/MultiviewMotionHistory.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "VulkanEditorGraphicsBridge.h"

#include <memory>

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
        /** @brief Optional compositor evidence, borrowed only while the exact selected viewport stages its real snapshot. */
        struct MotionInput final {
            Render::MultiviewMotionFrame frame;
            std::span<const Render::RenderMotionObjectId> objects;
        };

        /** @brief Exact allocation and synchronization tuples supplied by the selected acquisition owner. */
        struct AuxiliaryTexture final {
            Render::RenderTextureDescriptor allocation;
            Render::RenderGraphLogicalState acquired;
            Render::RenderGraphLogicalState requiredFinal;
        };

        /** @brief Cold motion/depth tuple plus the ordered native active rectangles. */
        struct AuxiliaryPlan final {
            AuxiliaryTexture motion;
            AuxiliaryTexture depth;
            std::array<Render::FramebufferExtent, 2> viewExtents;
        };

        /** @brief Non-owning receipt for one successfully prepared private viewport graph. */
        class PreparedTargets final {
        public:
            PreparedTargets(const PreparedTargets &) noexcept = default;
            PreparedTargets(PreparedTargets &&) noexcept = default;
            PreparedTargets &operator=(const PreparedTargets &) noexcept = default;
            PreparedTargets &operator=(PreparedTargets &&) noexcept = default;

        private:
            friend class EditorVulkanXrViewportAccess;
            /** @brief Binds an actually prepared graph and its selected capability incarnation. */
            PreparedTargets(std::weak_ptr<const void> graph, std::uint64_t device, std::uint64_t capabilities) noexcept;

            std::weak_ptr<const void> graph_;
            std::uint64_t device_{}, capabilities_{};
        };

        /** @brief Checks whether the selected viewport can attempt cold D32 paired reduction preparation. */
        [[nodiscard]] static bool CanPrepareReduction(const EditorViewportRendererVulkan &viewport,
                                                      Render::RenderTextureFormat depth) noexcept;

        /** @brief Prepares a real paired graph before native activation and captures its exact private owner. */
        [[nodiscard]] static Result<PreparedTargets> PreparePairedTargets(EditorViewportRendererVulkan &viewport,
                                                                          std::span<const Render::RenderMeshResourceView> meshes,
                                                                          const Render::RenderTextureDescriptor &color,
                                                                          const Render::RenderGraphLogicalState &acquired,
                                                                          const Render::RenderGraphLogicalState &requiredFinal,
                                                                          const AuxiliaryPlan &auxiliary, std::size_t coverageDrawCapacity);

        /** @brief Revalidates the exact graph and selected capability incarnation without acquiring native images. */
        [[nodiscard]] static bool IsPrepared(const EditorViewportRendererVulkan &viewport, const PreparedTargets &receipt) noexcept;

        /** @brief Reports a complete sealed paired reducer in both retained history slots of this exact graph. */
        [[nodiscard]] static bool HasPreparedReduction(const EditorViewportRendererVulkan &viewport,
                                                       const PreparedTargets &receipt) noexcept;

        /** @brief Fresh exact imports, with one single-layer view for each eye on both images. */
        struct AuxiliaryImages final {
            Render::RenderTextureHandle motion;
            Render::RenderTextureHandle depth;
            std::array<Render::RenderTextureViewHandle, 2> motionViews;
            std::array<Render::RenderTextureViewHandle, 2> depthViews;
        };

        /** @brief Borrow the actual selected native device; no creation or selection occurs. */
        [[nodiscard]] static Result<Render::Detail::VulkanEditorDevice> Device(const EditorViewportRendererVulkan &viewport);
        /** @brief Cold-prepares the staged optional motion pipeline for the selected format pair.
         * @param viewport Existing
         * selected owner. @param motion Exact signed color format. @param depth Exact forward depth format.
         * @return Resident
         * success, absent optional bundle, or original admission failure.
         */
        [[nodiscard]] static Result<bool> PrepareMotionPipeline(EditorViewportRendererVulkan &viewport, Render::RenderTextureFormat motion,
                                                                Render::RenderTextureFormat depth);
        /** @brief Cold-prepare topology over exact canonical meshes and native allocation/release policy.
         * @param viewport Existing selected owner. @param meshes Complete immutable canonical mesh resource projection.
         * @param color Native allocation. @param acquired Actual acquisition state. @param requiredFinal Required native release state.

         * @param retainPreviousDepth Current optional policy admits retaining sampled predecessor depth.
         * @param auxiliary Exact optional producer tuple; absent keeps the ordinary graph.
         * @param coverageDrawCapacity Initial scene instance bound for cold coverage descriptors; zero disables coverage.
         * @return Original owner/resource/graph error or successful preparation before interactive admission.
         */
        [[nodiscard]] static Result<void> Prepare(EditorViewportRendererVulkan &viewport,
                                                  std::span<const Render::RenderMeshResourceView> meshes,
                                                  const Render::RenderTextureDescriptor &color,
                                                  const Render::RenderGraphLogicalState &acquired,
                                                  const Render::RenderGraphLogicalState &requiredFinal, bool retainPreviousDepth,
                                                  const AuxiliaryPlan *auxiliary = nullptr, std::size_t coverageDrawCapacity = 0);
        /** @brief Bind one real acquisition before BeginFrame, using fixed owner storage and exact live publications.
         * @param viewport Existing owner. @param color Fresh native import. @param views Exact layer views.
         * @param publication Current Scene/frame/origin/device evidence. @param cameras Actual native view matrices.
         * @param instances Canonical scene instance values.
         * @param motion Optional exact compositor prediction and stable object IDs for the selected frame-synthesis route.
         * @param alpha Explicit projection color policy for this acquisition.
         * @param auxiliary Fresh optional motion/depth acquisitions. They require a complete adjacent predecessor.
         * @return Original typed validation/binding outcome. On optional failure the acquisition owner may omit
         * its pair and bind the ordinary projection with no auxiliary images.
         */
        [[nodiscard]] static Result<void> Bind(EditorViewportRendererVulkan &viewport, Render::RenderTextureHandle color,
                                               const std::array<Render::RenderTextureViewHandle, 2> &views,
                                               const Render::MultiviewPublication &publication,
                                               std::span<const Render::MultiviewSceneView> cameras,
                                               std::span<const Render::RenderStaticMeshInstance> instances, const MotionInput *motion,
                                               EditorXrProjectionAlpha alpha = EditorXrProjectionAlpha::Opaque,
                                               const AuxiliaryImages *auxiliary = nullptr);
        /** @brief Takes the renderer's one-shot premultiplied scene evidence after observing the actual queue.
         * @param viewport Same selected renderer owner. @param publication Exact staged Scene/frame/origin/view/device.
         * @param color Exact acquired color target generation. @return Completed evidence or empty.
         * @pre Owner lane. The compositor must independently retain exact acquisition identity and verify image release.
         */
        [[nodiscard]] static std::optional<EditorXrPremultipliedProjection> TakePremultipliedProjection(
            EditorViewportRendererVulkan &viewport, const Render::MultiviewPublication &publication,
            Render::RenderTextureHandle color) noexcept;

        /** @brief The previous successful projection's immutable owned depth and actual committed import state. */
        struct PreviousEyeDepth final {
            Render::MultiviewMotionFrame frame;
            Render::RenderTextureHandle texture;
            std::array<Render::RenderTextureViewHandle, 2> eyes;
            Render::FramebufferExtent extent;
            Render::RenderGraphLogicalState state;
        };

        /** @brief Observes previous depth only while an adjacent compatible current projection is staged.
         * @param viewport Exact
         * live renderer owner. @return The retained sampleable depth or absence.
         * @pre Synchronous control-lane use. A sampling
         * graph imports this owned texture with preserved contents and the given state;
         * it must retain its own graph pin and
         * restore the committed attachment state before releasing the borrow.
         */
        [[nodiscard]] static std::optional<PreviousEyeDepth> PreviousDepth(const EditorViewportRendererVulkan &viewport) noexcept;

        /** @brief Stages explicit diagnostic regions for both eyes before the viewport prepares its Scene graph. */
        [[nodiscard]] static bool RequestCoverageProbe(EditorViewportRendererVulkan &viewport,
                                                       const std::array<Render::MultiviewRenderRectangle, 2> &tiles) noexcept;

        /** @brief Closely bounded native pixels packed in eye, row, then column order. */
        struct CoveragePixels final {
            std::array<Render::MultiviewRenderRectangle, 2> tiles;
            std::array<float, Render::RenderGraphStereoCoverageCopy::MaximumPixels> samples{};
            std::size_t count{};
        };

        /** @brief Exact queue-observed coverage observation for one live frame and its retained predecessor.
         * @details Borrowed renderer identities are valid on the owner lane until the next bind or frame completion.
         * Pixel validity and any native compositor encoding must be verified separately.
         */
        struct CoverageObservation final {
            Render::MultiviewMotionFrame frame;
            Render::MultiviewMotionFrame predecessor;
            Render::RenderTextureHandle texture;
            std::array<Render::RenderTextureViewHandle, 2> eyes;
            Render::FramebufferExtent extent;
            std::array<Render::MultiviewRenderRectangle, 2> rectangles; /**< Exact written area in each mask layer. */
            Render::RenderTimelinePoint completion;
            std::optional<AuxiliaryImages> auxiliary; /**< Current paired acquisitions when coverage replayed the auxiliary raster. */
            std::optional<CoveragePixels> pixels;
        };

        /** @brief Consumes a completed observation once for the exact live frame.
         * @param viewport Selected device owner. @param frame Exact requested frame.
         * @return Observed GPU pass with exact depth ancestry, or absence while pending, stale, or unavailable.
         */
        [[nodiscard]] static std::optional<CoverageObservation> TakeCoverage(EditorViewportRendererVulkan &viewport,
                                                                             const Render::MultiviewMotionFrame &frame) noexcept;

        /** @brief Observed full-eye GPU counters and completion bound to the exact paired writer and predecessor. */
        struct CoverageReduction final {
            Render::MultiviewMotionFrame frame, predecessor;
            AuxiliaryImages auxiliary;
            std::array<Render::MultiviewRenderRectangle, 2> rectangles;
            Render::RenderTimelinePoint completion;
            Render::RenderGraphStereoCoverageReduction::Tallies tallies;

            /** @brief Reports whether all pixels of both active eyes have usable motion and matching previous depth. */
            [[nodiscard]] bool Complete() const noexcept;
        };

        /** @brief Consumes the exact paired reduction result once after queue observation. */
        [[nodiscard]] static std::optional<CoverageReduction> TakeCoverageReduction(EditorViewportRendererVulkan &viewport,
                                                                                    const Render::MultiviewMotionFrame &frame) noexcept;
        /** @brief Records the exact projection or zero-layer completion after successful native end only. */
        static void CompleteMotion(EditorViewportRendererVulkan &viewport, const std::optional<Render::MultiviewMotionFrame> &frame,
                                   bool projected) noexcept;

        /** @brief Exact viewport recording receipt until the selected host submits or cancels its frame. */
        struct ProjectionSubmission final {
            Render::FrameToken frame;
            Render::MultiviewPublication publication;
            Render::RenderTextureHandle color;
            Render::RenderTimelinePoint signal;

            bool operator==(const ProjectionSubmission &) const noexcept = default;
        };
        enum class SubmissionState {
            Pending,
            Accepted,
            Rejected
        };

        /** @brief Execute through the SAME actual App frame before its existing UI/GUI tail.
         * @param viewport Existing owner. @param frame Existing canonical frame scope. @param output Existing primary output.
         * @return Exact encoded graphics work awaiting native queue admission, or the original error.
         */
        [[nodiscard]] static Result<ProjectionSubmission> Execute(EditorViewportRendererVulkan &viewport, Render::RenderFrameScope &frame,
                                                                  const Render::PrimaryOutputAttachment &output);
        /** @brief Resolves native acceptance for this exact recording and discards a closed unissued signal. */
        [[nodiscard]] static Result<SubmissionState> ResolveSubmission(EditorViewportRendererVulkan &viewport,
                                                                       const ProjectionSubmission &submission);
        /** @brief Returns true only after Poll observes completion of this exact live queue-accepted recording. */
        [[nodiscard]] static bool SubmissionCompleted(const EditorViewportRendererVulkan &viewport,
                                                      const ProjectionSubmission &submission) noexcept;
        /** @brief Borrow only the current admitted sampled left-eye view, never a desktop-camera fallback.
         * @param viewport Exact selected owner. @param frame Same successfully bound App frame.
         * @param expected Exact retained acquired first-eye generation. @return Full matching GUI binding or empty.
         */
        [[nodiscard]] static EditorViewportTextureView Mirror(const EditorViewportRendererVulkan &viewport, std::uint64_t frame,
                                                              Render::RenderTextureViewHandle expected) noexcept;
        /** @brief Revokes the mirror and projection evidence on exact Scene loss/retirement; native queue debt stays owned. */
        static void RevokeMirror(const EditorViewportRendererVulkan &viewport) noexcept;
        /** @brief Releases cold XR graph backing only after the App proved every acquisition/scene/GUI reference retired.
         * @param viewport SAME selected owner. @pre Exact native image retirement is already complete; owner lane.
         */
        [[nodiscard]] static Result<void> Retire(EditorViewportRendererVulkan &viewport);
        /** @brief Progress the SAME native queue with a zero-time observation, not an image-release proof. */
        [[nodiscard]] static Result<void> Poll(EditorViewportRendererVulkan &viewport);
    };
}  // namespace Horo::Editor
