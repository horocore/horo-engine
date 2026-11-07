#include "DecisionClosureLayout.h"

#include "DecisionCompileLimits.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::AI::Detail {
    namespace {
        template <typename T> Result<T> Limit() {
            return Result<T>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
        }

        bool Add(std::uint64_t &total, std::uint64_t amount) noexcept {
            if (amount > std::numeric_limits<std::uint64_t>::max() - total)
                return false;
            total += amount;
            return true;
        }

        bool AddCount(std::size_t &total, std::size_t amount) noexcept {
            if (amount > std::numeric_limits<std::size_t>::max() - total)
                return false;
            total += amount;
            return true;
        }

        class LayoutBuilder final {
        public:
            LayoutBuilder(std::span<const DecisionCompiledGraph> graphs, const DecisionCompilationLimits &limits)
                : graphs_(graphs), limits_(limits) {
                layout_.occurrences.reserve(limits.maximumOccurrences);
                frames_.reserve(limits.maximumCallDepth);
            }

            Result<DecisionClosureLayout> Run(std::size_t root) {
                if (auto admitted = AdmitGraphs(); admitted.HasError())
                    return Result<DecisionClosureLayout>::Failure(admitted.ErrorValue());
                if (auto begun = Append(root, 0, {}, 1); begun.HasError())
                    return Result<DecisionClosureLayout>::Failure(begun.ErrorValue());
                while (!frames_.empty()) {
                    if (auto advanced = Advance(); advanced.HasError())
                        return Result<DecisionClosureLayout>::Failure(advanced.ErrorValue());
                }
                if (auto bounded = ValidateBudgets(); bounded.HasError())
                    return Result<DecisionClosureLayout>::Failure(bounded.ErrorValue());
                layout_.storage.occurrences = layout_.occurrences.size();
                std::vector<DecisionCallOccurrence>(layout_.occurrences.begin(), layout_.occurrences.end()).swap(layout_.occurrences);
                return Result<DecisionClosureLayout>::Success(std::move(layout_));
            }

        private:
            struct Frame final {
                std::size_t occurrence{};
                std::size_t next{};
                std::size_t depth{};
                std::size_t executionDepth{};
            };

            Result<void> AdmitGraphs() {
                for (const auto &graph : graphs_) {
                    if (!graph.asset.IsValid() || !graph.bindings)
                        return Limit<void>();
                    if (graph.calls.size() > limits_.maximumNodes)
                        return Limit<void>();
                    for (const auto &call : graph.calls)
                        if (!call.node.IsValid() || call.calleePlan >= graphs_.size())
                            return Limit<void>();
                    if (!Add(layout_.storage.compiledBytes, graph.localStorage.compiledBytes))
                        return Limit<void>();
                }
                return Result<void>::Success();
            }

            Result<void> ChargeCore(const DecisionStorageRequirements &local) {
                if (!Add(layout_.storage.coreInstanceBytes, local.coreInstanceBytes))
                    return Limit<void>();
                if (!Add(layout_.storage.scratchBytes, local.scratchBytes))
                    return Limit<void>();
                if (!Add(layout_.storage.taskSlots, local.taskSlots))
                    return Limit<void>();
                if (layout_.storage.taskSlots > limits_.maximumTaskSlots)
                    return Limit<void>();
                return Result<void>::Success();
            }

            Result<void> ChargeProviders(const DecisionStorageRequirements &local) {
                if (!Add(layout_.storage.nativeDeclaredBytes, local.nativeDeclaredBytes))
                    return Limit<void>();
                if (!Add(layout_.storage.enforcedVmBytes, local.enforcedVmBytes))
                    return Limit<void>();
                if (!AddCount(layout_.storage.unknownNativeProviders, local.unknownNativeProviders))
                    return Limit<void>();
                if (!AddCount(layout_.storage.nativeDeclaredProviders, local.nativeDeclaredProviders))
                    return Limit<void>();
                return Result<void>::Success();
            }

            Result<std::size_t> ExecutionDepth(std::size_t graph) const {
                auto depth = frames_.empty() ? 0 : frames_.back().executionDepth;
                if (!AddCount(depth, graphs_[graph].localStorage.maximumExecutionDepth))
                    return Limit<std::size_t>();
                if (depth > limits_.maximumExecutionDepth)
                    return Limit<std::size_t>();
                return Result<std::size_t>::Success(depth);
            }

            Result<void> Append(std::size_t graph, std::size_t parent, DecisionNodeId node, std::size_t depth) {
                if (graph >= graphs_.size() || depth > limits_.maximumCallDepth)
                    return Limit<void>();
                if (layout_.occurrences.size() == limits_.maximumOccurrences)
                    return Limit<void>();
                for (const auto &frame : frames_)
                    if (layout_.occurrences[frame.occurrence].plan == graph)
                        return Result<void>::Failure(MakeError(AIErrors::DecisionAssetDependencyCycle));
                auto measuredDepth = ExecutionDepth(graph);
                if (measuredDepth.HasError())
                    return Result<void>::Failure(measuredDepth.ErrorValue());
                const auto executionDepth = measuredDepth.Value();
                const auto firstSlot = layout_.storage.taskSlots;
                if (auto charged = ChargeCore(graphs_[graph].localStorage); charged.HasError())
                    return charged;
                if (auto charged = ChargeProviders(graphs_[graph].localStorage); charged.HasError())
                    return charged;
                const auto index = layout_.occurrences.size();
                layout_.occurrences.push_back({graph, parent, node, 0, firstSlot});
                frames_.push_back({index, 0, depth, executionDepth});
                layout_.storage.maximumExecutionDepth = std::max(layout_.storage.maximumExecutionDepth, executionDepth);
                layout_.storage.maximumCallDepth = std::max(layout_.storage.maximumCallDepth, depth);
                return Result<void>::Success();
            }

            Result<void> Advance() {
                auto &frame = frames_.back();
                const auto &calls = graphs_[layout_.occurrences[frame.occurrence].plan].calls;
                if (frame.next == calls.size()) {
                    layout_.occurrences[frame.occurrence].subtreeEnd = layout_.occurrences.size();
                    frames_.pop_back();
                    return Result<void>::Success();
                }
                const auto call = calls[frame.next++];
                return Append(call.calleePlan, frame.occurrence, call.node, frame.depth + 1);
            }

            Result<void> ValidateProviderPolicy() const {
                if (limits_.memoryPolicy >= DecisionMemoryPolicy::Count)
                    return Limit<void>();
                if (limits_.memoryPolicy == DecisionMemoryPolicy::CoreBound)
                    return Result<void>::Success();
                if (layout_.storage.unknownNativeProviders != 0)
                    return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                if (limits_.memoryPolicy == DecisionMemoryPolicy::EnforcedTotal && layout_.storage.nativeDeclaredProviders != 0)
                    return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                return Result<void>::Success();
            }

            Result<void> ValidateBudgets() const {
                auto core = layout_.storage.compiledBytes;
                if (!Add(core, layout_.storage.coreInstanceBytes) || !Add(core, layout_.storage.scratchBytes))
                    return Limit<void>();
                if (core > limits_.maximumCoreBytes)
                    return Limit<void>();
                auto provider = layout_.storage.enforcedVmBytes;
                if (!Add(provider, layout_.storage.nativeDeclaredBytes))
                    return Limit<void>();
                if (provider > limits_.maximumProviderBytes)
                    return Limit<void>();
                return ValidateProviderPolicy();
            }

            std::span<const DecisionCompiledGraph> graphs_;
            const DecisionCompilationLimits &limits_;
            DecisionClosureLayout layout_;
            std::vector<Frame> frames_;
        };
    }  // namespace

    Result<DecisionClosureLayout> BuildDecisionClosureLayout(std::span<const DecisionCompiledGraph> graphs, std::size_t root,
                                                             const DecisionCompilationLimits &limits) {
        if (!ValidDecisionLimits(limits) || graphs.empty() || graphs.size() > limits.maximumAssets)
            return Limit<DecisionClosureLayout>();
        try {
            return LayoutBuilder(graphs, limits).Run(root);
        } catch (const std::bad_alloc &) {
            return Result<DecisionClosureLayout>::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }
}  // namespace Horo::AI::Detail
