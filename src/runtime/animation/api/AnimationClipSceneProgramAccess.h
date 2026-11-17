#pragma once

/** @file AnimationClipSceneProgramAccess.h
 * @brief Non-installed Scene bridge preparing a direct clip through the one existing animation executor.
 */
#include "Horo/Animation/AnimationGraph.h"

namespace Horo::Animation::Detail {
    /** @brief Prepares execution-local operands without inventing a persistent graph asset identity. */
    struct ClipSceneProgramAccess final {
        /**
         * @brief Binds an actual validated absolute clip and its exact host-issued skeleton publication.
         * @param clip Authored source identity. @param context Exactly one real clip and its immutable skeleton.
         * @return Same bounded graph program/executor with Clip lineage and absent GraphId.
         * @details Standalone additive clips require an explicit base/reference operand and fail typed unsupported.
         *          No source parser, compression, I/O, new clock, cursor or pose implementation is introduced.
         */
        [[nodiscard]] static Result<AnimationGraphProgram> Prepare(AnimationClipId clip, const AnimationGraphCompileContext &context);
    };
}  // namespace Horo::Animation::Detail
