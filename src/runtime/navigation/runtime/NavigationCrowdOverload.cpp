#include "Horo/Navigation/NavigationCrowdOverload.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <limits>
#include <tuple>

namespace Horo::Navigation {
    namespace {
        /** @brief Advances a bounded feedback dimension without wrapping at representational limits. */
        std::size_t Recover(const std::size_t attempted, const bool saturated) noexcept {
            return saturated ? std::max(std::size_t{1}, attempted / 2)
                             : attempted + static_cast<std::size_t>(attempted != std::numeric_limits<std::size_t>::max());
        }
    }  // namespace

    /** @copydoc NavigationCrowdWorkBudget::IsValid */
    bool NavigationCrowdWorkBudget::IsValid() const noexcept {
        return maximumAgentsPerGroup != 0 && maximumAgentsPerGroup <= maximumAgents;
    }

    /** @copydoc NavigationCrowdRecoveryBudget::Limit */
    NavigationCrowdCapacity NavigationCrowdRecoveryBudget::Limit(const NavigationCrowdCapacity maximum) const noexcept {
        return {next_.agents ? std::min(maximum.agents, next_.agents) : maximum.agents,
                next_.jobs ? std::min(maximum.jobs, next_.jobs) : maximum.jobs};
    }

    /** @copydoc NavigationCrowdRecoveryBudget::Observe */
    void NavigationCrowdRecoveryBudget::Observe(const NavigationCrowdCapacity attempted, const bool saturated) noexcept {
        if (saturated)
            next_ = {Recover(attempted.agents, true), Recover(attempted.jobs, true)};
        else if (next_.agents != 0)
            next_ = {Recover(next_.agents, false), Recover(next_.jobs, false)};
    }

    /** @copydoc SelectNavigationCrowdDemand */
    Result<std::size_t> SelectNavigationCrowdDemand(const std::span<NavigationCrowdDemand> demands,
                                                    const std::span<std::size_t> groupCounts, const NavigationCrowdWorkBudget budget) {
        using Selection = Result<std::size_t>;
        if (!budget.IsValid())
            return Selection::Failure(MakeError(NavigationErrors::CapabilityDescriptorInvalid));
        if (std::ranges::any_of(demands, [groupCounts](const auto &demand) {
            return demand.groupIndex >= groupCounts.size() || demand.waitingSince == 0;
        }))
            return Selection::Failure(MakeError(NavigationErrors::AgentDescriptorInvalid));
        std::ranges::fill(groupCounts, 0);
        std::ranges::sort(demands, {}, [](const auto &demand) {
            return std::tuple{demand.waitingSince, demand.groupIndex, demand.agentIndex};
        });
        std::size_t selected{};
        for (const auto &demand : demands) {
            if (selected == budget.maximumAgents)
                break;
            auto &group = groupCounts[demand.groupIndex];
            if (group == budget.maximumAgentsPerGroup)
                continue;
            ++group;
            demands[selected++] = demand;
        }
        return Selection::Success(selected);
    }
}  // namespace Horo::Navigation
