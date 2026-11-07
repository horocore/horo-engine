#pragma once
/** @file EditorViewportForwardLighting.h @brief Fixed-storage per-draw fallback over one complete canonical scene light table. */
#include "EditorViewportLighting.h"
#include "Horo/Runtime/Render/DirectionalShadow.h"

#include <array>

namespace Horo::Editor {
    /** @brief Owner-lane scratch; preparation and selection allocate nothing and retain no input borrows. */
    class EditorViewportForwardLighting final {
    public:
        /** @brief Admits only an exact host-selected CPU recipe, retaining authored completeness and work policy. */
        [[nodiscard]] Result<void> Configure(const EditorViewportLightingProfile &profile,
                                             const Render::LightPreparationSelection &selection);
        /** @brief Copies and validates every stable light, then preflights every actual draw before any native mutation.
         * @details Work is bounded by authored maximumForwardLightTests; no arbitrary draw cap or global light truncation.
         * Failure closes selection until a later complete preparation succeeds. Existing GPU publications remain caller-owned. */
        [[nodiscard]] Result<void> Prepare(const Render::RenderSceneView &scene, const CancellationToken &cancellation = {});
        /** @brief Recomputes one conservative world-AABB selection into caller-owned sixteen-light storage. */
        [[nodiscard]] Result<Render::ForwardDrawLightSelection> SelectDraw(const Render::RenderSceneView &scene,
                                                                           const Render::RenderStaticMeshInstance &instance,
                                                                           std::span<Render::RenderLight> output,
                                                                           const CancellationToken &cancellation = {}) const;
        /** @brief Maps the canonical shadow source identity into a draw-local span; a nonselected source disables only that draw's shadow.
         */
        [[nodiscard]] Result<std::optional<Render::DirectionalShadowFrame>> RemapShadow(
            const std::optional<Render::DirectionalShadowFrame> &shadow, const Render::ForwardDrawLightSelection &selection,
            const Render::RenderSceneView &scene) const;
        [[nodiscard]] EditorViewportLightingDiagnostics Diagnostics() const noexcept;

    private:
        [[nodiscard]] Result<void> CopyCanonicalLights(const Render::RenderSceneView &scene);
        [[nodiscard]] bool MatchesScene(const Render::RenderSceneView &scene) const noexcept;
        [[nodiscard]] Result<Render::ForwardDrawLightSelection> SelectInstance(const Render::RenderStaticMeshInstance &instance,
                                                                               std::span<Render::RenderLight> output,
                                                                               const CancellationToken &cancellation) const;
        EditorViewportLightingProfile profile_;
        EditorViewportLightingDiagnostics diagnostics_;
        Render::LightCullingBudget budget_;
        std::array<Render::IdentifiedRenderLight, Render::LightCullingBudget::HardMaximumLights> lights_;
        std::size_t count_{};
        std::uint64_t domain_{}, revision_{};
        bool admitted_{}, ready_{};
    };
}  // namespace Horo::Editor
