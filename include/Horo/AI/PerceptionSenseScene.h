#pragma once

/** @file PerceptionSenseScene.h
 * @brief Scene-owner composition for bounded custom spatial senses and atomic memory staging.
 */

#include "Horo/AI/PerceptionSenseProvider.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"

namespace Horo::AI {
    /** @brief Bounded dispatch evidence; omitted or truncated candidates do not imply sensing loss. */
    struct PerceptionSenseSceneResult final {
        std::size_t candidates{};
        std::size_t samples{};
        bool truncated{};
    };

    /** @brief Evaluates one custom sense and stages its complete memory transaction at PerceptionSensePoll.
     * @param scene Current owner-thread Scene view, borrowed through this synchronous operation.
     * @param spatial Actual committed spatial owner; its publication is revalidated after the callback.
     * @param instance Exact admitted agent/listener provider instance.
     * @param query Declared listener, sense and filters; its candidate bound must fit the provider contract.
     * @param tick Committed monotonic simulation tick.
     * @param memory Exact recipient agent memory; changed only after all samples pass validation.
     * @param agentBudget Agent allowance. @param globalBudget Global allowance; aliases are charged once.
     * @return Candidate/sample counts and truncation, or typed failure leaving memory unchanged.
     * @details Reserves one bounded spatial-query unit before gathering at most Sources records, in addition to
     * the provider/validation allowance. Providers receive frozen facts only. Publication replacement,
     * Scene changes, dead candidates, cancellation and module retirement prevent staging. Blackboard publication
     * remains the separate owner-controlled BlackboardSync operation. Call only on the acquiring owner thread.
     */
    [[nodiscard]] Result<PerceptionSenseSceneResult> RunPerceptionSenseAtSensePoll(const Runtime::RuntimeSceneView &scene,
                                                                                   const PerceptionSpatialBroadphase &spatial,
                                                                                   PerceptionSenseInstance &instance,
                                                                                   const PerceptionSpatialQuery &query, std::uint64_t tick,
                                                                                   AIPerceptionMemory &memory, AiWorkBudget &agentBudget,
                                                                                   AiWorkBudget &globalBudget);
}  // namespace Horo::AI
