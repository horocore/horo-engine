#include "Horo/AI/BehaviorTreeRuntime.h"

#include <type_traits>
#include <utility>

namespace {
    using LegacyReplacement =
        Horo::Result<bool> (Horo::AI::BehaviorTreeInstance::*)(std::shared_ptr<const Horo::AI::BehaviorTreeExecutionPlan>);
    static_assert(std::is_same_v<decltype(static_cast<LegacyReplacement>(&Horo::AI::BehaviorTreeInstance::Replace)), LegacyReplacement>);
}  // namespace

/** @brief Consumes explicit owner reservation using only the AI target's staged public contracts. */
Horo::AI::BehaviorTreeInstanceBinding HoroReserveDecisionReloadSlots(Horo::AI::BehaviorTreeInstanceBinding binding,
                                                                     const std::size_t capacity) {
    binding.availableTaskSlots = capacity;
    return binding;
}

/** @brief Consumes the prepared-executor replacement overload without any internal headers. */
Horo::Result<bool> HoroReplaceDecisionExecutor(Horo::AI::BehaviorTreeInstance &instance,
                                               std::shared_ptr<const Horo::AI::BehaviorTreeExecutionPlan> plan,
                                               std::unique_ptr<Horo::AI::IBehaviorTreeExecutor> executor) {
    return instance.Replace(std::move(plan), std::move(executor));
}
