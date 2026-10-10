#pragma once

#include "Horo/Runtime/Render/PostProcessGraph.h"

namespace Horo::Render::Detail {
    /** @brief Fixed semantic lookup and recipe choices, populated before any graph authoring. */
    struct PostProcessAdmission {
        std::array<const PostProcessTextureInput *, 6> inputs{};
        std::array<std::optional<PostProcessRecipe>, 8> recipes{};
    };

    /** @brief Validates complete generations, settings, inputs, ordered recipes and finite total budgets. */
    [[nodiscard]] Result<PostProcessAdmission> AdmitPostProcessGraph(const PostProcessGraphRequest &request);
    /** @brief Lists semantic inputs required by the given effect without allocations. */
    [[nodiscard]] std::span<const PostProcessSemantic> PostProcessRequiredInputs(PostProcessEffect effect) noexcept;
    /** @brief Reports whether an effect has an authored enabled settings group. */
    [[nodiscard]] bool PostProcessEnabled(const PostProcessSettings &settings, PostProcessEffect effect) noexcept;
    /** @brief Creates the canonical output descriptor for an admitted recipe. */
    [[nodiscard]] RenderTextureDescriptor PostProcessOutputDescriptor(const PostProcessGraphRequest &request,
                                                                      const PostProcessRecipe &recipe) noexcept;
}  // namespace Horo::Render::Detail
