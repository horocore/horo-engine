#pragma once
#include "Horo/AI/DecisionGraphCompiler.h"

namespace Horo::AI::Detail {
    inline bool ValidDecisionCountLimits(const DecisionCompilationLimits &limits) noexcept {
        return limits.maximumAssets > 0 && limits.maximumAssets <= 128 && limits.maximumNodes > 0 && limits.maximumNodes <= 4096 &&
               limits.maximumCallDepth > 0 && limits.maximumCallDepth <= 16 && limits.maximumOccurrences > 0 &&
               limits.maximumOccurrences <= 256;
    }

    inline bool ValidDecisionStorageLimits(const DecisionCompilationLimits &limits) noexcept {
        return limits.maximumExecutionDepth > 0 && limits.maximumExecutionDepth <= 16U * 1024U && limits.maximumTaskSlots <= (1U << 20U) &&
               limits.maximumCoreBytes > 0 && limits.maximumCoreBytes <= 64ULL * 1024ULL * 1024ULL &&
               limits.maximumProviderBytes <= 256ULL * 1024ULL * 1024ULL && limits.memoryPolicy < DecisionMemoryPolicy::Count;
    }

    inline bool ValidDecisionLimits(const DecisionCompilationLimits &limits) noexcept {
        return ValidDecisionCountLimits(limits) && ValidDecisionStorageLimits(limits);
    }
}  // namespace Horo::AI::Detail
