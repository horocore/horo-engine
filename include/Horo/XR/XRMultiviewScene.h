#pragma once

/** @file XRMultiviewScene.h
 * @brief Explicit XR pose/projection conversion into one immutable renderer multiview presentation.
 */
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "Horo/XR/XRViewRenderPlan.h"

namespace Horo::XR {
    /** @brief Borrowed current owner facts, never a second XR session/image/resource authority. */
    struct XRMultiviewOwnerState final {
        XRSessionId session;
        XRViewConfigurationId configuration;
        XRViewConfigurationRevision configurationRevision;
        XRWorldOriginRevision originRevision;
        std::span<const XRSwapchainImageId> acquiredImages;
        Render::MultiviewPublication renderer;
        Render::MultiviewCapabilities capabilities;
    };

    /** @brief Complete synchronous conversion input copied only after all owner/pose/projection fences pass. */
    struct XRMultiviewSceneInput final {
        const XRViewRenderPlan &plan;
        std::span<const XRPoseSample> referenceToWorld;
        /**< One exact reference-to-canonical-world sample per ordered view, matching that view's prediction/time evidence. */
        std::span<const Render::MultiviewSceneInstance> instances;
        Render::MultiviewPublication publication;
        Math::ClipDepthRange depthRange{Math::ClipDepthRange::ZeroToOne};
        float nearPlane{0.1F};
        float farPlane{1000.0F};
        Render::MultiviewExecutionPolicy policy{Render::MultiviewExecutionPolicy::PreferInstanced};
        Render::MultiviewExtractionLimits limits;
    };

    /** @brief Immutable XR identity/target evidence retained alongside one canonical render presentation.
     * @details Readable after source teardown. Liveness-sensitive consumers must Validate against the actual XR/frame
     * owner before importing targets or submitting. Scene data and poses are presentation copies, never simulation authority.
     */
    class XRMultiviewScene final {
    public:
        /** @brief Converts asymmetric XR views and explicit origin transforms without re-extracting the world per view.
         * @param input Complete ordered view/pose/scene input and finite rendering limits.
         * @param active Current XR and selected-renderer owner facts.
         * @return Immutable scene or original typed XR/render/math failure without partial publication.
         * @post Successful conversion has fixed bounded work/storage, no heap allocation, native calls, I/O or synchronization.
         */
        [[nodiscard]] static Result<XRMultiviewScene> Create(const XRMultiviewSceneInput &input, const XRMultiviewOwnerState &active);
        /** @brief Returns the exact retained XR view/image plan. @return Snapshot-owned immutable plan. */
        [[nodiscard]] const XRViewRenderPlan &Plan() const noexcept;
        /** @brief Returns one extracted renderer presentation. @return Snapshot-owned immutable scene. */
        [[nodiscard]] const Render::MultiviewSceneSnapshot &Scene() const noexcept;
        /** @brief Revalidates the exact session/configuration/origin/acquired-image and renderer publication fences.
         * @param active Current owner facts; invalid after shutdown. @return Success or original typed stale/unavailable failure. */
        [[nodiscard]] Result<void> Validate(const XRMultiviewOwnerState &active) const;
        /** @brief Extracts ready presentation instances using this already converted immutable view set.
         * @param instances Canonical scene presentation with ready exact frame material generations.
         * @param active Current owner fences. @param policy Explicit optimization/fallback policy.
         * @param limits Finite culling/draw envelope.
         * @return New immutable frame or typed failure. View-only preparation can pack/realize frame uniforms first,
         * then extract the actual world values once without redoing origin/projection conversion. */
        [[nodiscard]] Result<XRMultiviewScene> ExtractInstances(std::span<const Render::MultiviewSceneInstance> instances,
                                                                const XRMultiviewOwnerState &active,
                                                                Render::MultiviewExecutionPolicy policy,
                                                                const Render::MultiviewExtractionLimits &limits = {}) const;

    private:
        XRMultiviewScene(const XRViewRenderPlan &plan, Render::MultiviewSceneSnapshot scene) noexcept;
        XRViewRenderPlan plan_;
        Render::MultiviewSceneSnapshot scene_;
    };
}  // namespace Horo::XR
