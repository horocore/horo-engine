#pragma once

#include "Horo/Animation/AnimationGraphInstance.h"

namespace Horo::Animation::Detail {
    /**
     * @brief Animation-owned staged publication seam for the canonical Scene lifecycle adapter.
     * @details Not installed or exported through AnimationApi. The adapter exclusively owns each
     * instance from evaluation through abort/publication. Preparation is fallible and allocation-free;
     * publication is permitted only after the actual scheduler commits the same prepared attempt.
     * No cancellation decision, external callback, replacement, shutdown or other mutation may occur
     * between that fence and publication. Aborting an armed candidate remains valid before the fence.
     */
    struct GraphSceneCommitAccess final {
        /** @brief Checks candidate identity, owner thread and cancellation before arming publication. */
        [[nodiscard]] static GraphInstanceOutcome Prepare(AnimationGraphInstance &instance, AnimationTickId tick);
        /** @brief Publishes an already armed candidate without cancellation or other fallible work. */
        [[nodiscard]] static GraphInstancePublication Publish(AnimationGraphInstance &instance) noexcept;
    };
}  // namespace Horo::Animation::Detail
