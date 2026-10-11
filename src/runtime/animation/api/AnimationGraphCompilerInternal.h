#pragma once
/** @file AnimationGraphCompilerInternal.h
 * @brief Target-private graph compiler diagnostics, lookup and detached binding/access plan.
 */
#include "Horo/Animation/AnimationGraph.h"

#include <algorithm>

namespace Horo::Animation::GraphCompileDetail {
    /** @brief Produces one stable diagnostic without candidate content or project paths. */
    Error Failure(const ErrorCodeDescriptor &code, GraphSourceLocation location = {});
    /** @brief Checks captured admission/cancellation at bounded compiler checkpoints. */
    Result<void> Admission(const AnimationGraphCompileContext &context);

    /** @brief Finds a stable id in a canonical collection. */
    template <typename Collection, typename Id> const typename Collection::value_type *Find(const Collection &values, Id id) {
        using Value = typename Collection::value_type;
        const auto found = std::ranges::lower_bound(values, id, {}, &Value::id);
        return found != values.end() && found->id == id ? &*found : nullptr;
    }

    /** @brief Complete detached immutable dependency/access plan before program publication. */
    struct BoundPlan final {
        GraphSkeletonBinding skeleton{};
        std::vector<AnimationClipDescriptor> clips{};
        GraphMemoryRequirements memory{};
        std::vector<GraphInstructionOccurrence> occurrences{};
    };

    /** @brief Captures exact dependencies and produces finite occurrence-to-storage/state accesses. */
    Result<BoundPlan> Bind(const AnimationGraphProgram &program, const AnimationGraphCompileContext &context);
}  // namespace Horo::Animation::GraphCompileDetail
