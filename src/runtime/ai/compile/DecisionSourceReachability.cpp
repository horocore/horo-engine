#include "DecisionSourceReachability.h"

#include <algorithm>
#include <array>

namespace Horo::AI::Detail {
    namespace {
        Result<void> Invalid(const ErrorCodeDescriptor &code = StateMachineErrors::TopologyInvalid) {
            return Result<void>::Failure(MakeError(code));
        }

        std::size_t Index(const StateMachineAssetDescriptor &source, DecisionNodeId id) {
            const auto state = std::ranges::find(source.states, id, &StateMachineState::id);
            return static_cast<std::size_t>(state - source.states.begin());
        }

        Result<void> MarkAncestors(const StateMachineAssetDescriptor &source, DecisionNodeId id,
                                   std::array<bool, StateMachineLimits::States> &visited) {
            std::size_t depth{};
            while (id.IsValid()) {
                if (++depth > StateMachineLimits::Depth)
                    return Invalid(StateMachineErrors::HierarchyInvalid);
                const auto index = Index(source, id);
                if (index == source.states.size())
                    return Invalid();
                visited[index] = true;
                id = source.states[index].parent;
            }
            return Result<void>::Success();
        }

        Result<void> MarkPath(const StateMachineAssetDescriptor &source, DecisionNodeId id,
                              std::array<bool, StateMachineLimits::States> &visited) {
            if (auto parents = MarkAncestors(source, id, visited); parents.HasError())
                return parents;
            std::size_t depth{};
            while (id.IsValid()) {
                if (++depth > StateMachineLimits::Depth)
                    return Invalid(StateMachineErrors::HierarchyInvalid);
                const auto index = Index(source, id);
                if (index == source.states.size())
                    return Invalid();
                visited[index] = true;
                id = source.states[index].initialChild;
            }
            return Result<void>::Success();
        }

        Result<void> Expand(const StateMachineAssetDescriptor &source, std::array<bool, StateMachineLimits::States> &visited) {
            for (const auto &transition : source.transitions) {
                const auto origin = Index(source, transition.source);
                if (origin == source.states.size())
                    return Invalid();
                if (!visited[origin])
                    continue;
                if (auto target = MarkPath(source, transition.target, visited); target.HasError())
                    return target;
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ValidateStateSourceReachability(const StateMachineAssetDescriptor &source) {
        if (source.states.empty() || source.states.size() > StateMachineLimits::States ||
            source.transitions.size() > StateMachineLimits::Transitions)
            return Invalid(AIErrors::DecisionAssetLimitExceeded);
        std::array<bool, StateMachineLimits::States> visited{};
        if (auto initial = MarkPath(source, source.initial, visited); initial.HasError())
            return initial;
        for (std::size_t iteration = 0; iteration < source.states.size(); ++iteration) {
            const auto before = visited;
            if (auto expanded = Expand(source, visited); expanded.HasError())
                return expanded;
            if (before == visited)
                break;
        }
        if (std::ranges::count(visited, true) != static_cast<std::ptrdiff_t>(source.states.size()))
            return Invalid();
        return Result<void>::Success();
    }
}  // namespace Horo::AI::Detail
