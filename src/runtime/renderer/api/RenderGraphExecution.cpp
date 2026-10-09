#include "Horo/Runtime/Render/RenderGraphExecution.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <array>
#include <new>
#include <optional>
#include <utility>

namespace Horo::Render {
    /** @copydoc IRenderBackend::ExecuteGraph */
    Result<void> IRenderBackend::ExecuteGraph(const RenderGraphExecutionRequest &) {
        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
    }

    namespace {
        using RangeMember = RenderGraphExecutionRange RenderGraphExecutionPass::*;

        [[nodiscard]] bool IsKnownPass(const RenderGraphPassRef pass, const RenderGraph &graph) noexcept {
            return pass.owner == graph.Owner() && pass.id.value > 0 && pass.id.value <= graph.Passes().size();
        }

        [[nodiscard]] bool IsKnownResource(const RenderGraphResourceId resource, const RenderGraph &graph) noexcept {
            return resource.owner == graph.Owner() && resource.value > 0 && resource.value <= graph.Resources().size();
        }

        class ExecutionCompiler final {
        public:
            ExecutionCompiler(const RenderGraph &graph, const RenderGraphSchedule &schedule,
                              const RenderGraphSynchronizationPlan &synchronization, const std::span<const RenderQueueAssignment> queues)
                : graph_(graph), schedule_(schedule), synchronization_(synchronization), queues_(queues),
                  passPositions_(graph.Passes().size()) {}

            [[nodiscard]] Result<void> Compile() {
                if (auto valid = ValidateProvenance(); valid.HasError()) {
                    return valid;
                }
                if (auto valid = ResolveQueues(); valid.HasError()) {
                    return valid;
                }
                if (auto valid = PreparePasses(); valid.HasError()) {
                    return valid;
                }
                if (auto valid = ValidateSynchronizationTopology(); valid.HasError()) {
                    return valid;
                }
                return CollectRecords();
            }

            [[nodiscard]] std::vector<RenderGraphExecutionPass> TakePasses() noexcept {
                return std::move(passes_);
            }

            [[nodiscard]] std::vector<RenderGraphResourceUsage> TakeUsages() noexcept {
                return std::move(usages_);
            }

            [[nodiscard]] std::vector<RenderGraphDependency> TakeDependencies() noexcept {
                return std::move(dependencies_);
            }

            [[nodiscard]] std::vector<RenderGraphTransition> TakeTransitions() noexcept {
                return std::move(transitions_);
            }

            [[nodiscard]] std::vector<RenderGraphOwnershipTransfer> TakeReleaseTransfers() noexcept {
                return std::move(releaseTransfers_);
            }

            [[nodiscard]] std::vector<RenderGraphOwnershipTransfer> TakeAcquireTransfers() noexcept {
                return std::move(acquireTransfers_);
            }

        private:
            [[nodiscard]] Result<void> ValidateProvenance() const {
                if (!graph_.Owner().IsValid()) {
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
                }
                if (schedule_.Owner() != graph_.Owner()) {
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSchedule));
                }
                if (synchronization_.Owner() != graph_.Owner()) {
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSynchronization));
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ResolveQueues() {
                for (const RenderQueueAssignment assignment : queues_) {
                    if (!assignment.IsValid()) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidQueueTopology));
                    }
                    const auto role = static_cast<std::size_t>(assignment.role);
                    if (queuesByRole_[role].has_value()) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidQueueTopology));
                    }
                    queuesByRole_[role] = assignment.queue;
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> AddPass(const RenderGraphPassRef reference) {
                if (!IsKnownPass(reference, graph_) || passPositions_[reference.id.value - 1].has_value()) {
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSchedule));
                }
                const RenderGraphPass &source = graph_.Passes()[reference.id.value - 1];
                const auto role = static_cast<std::size_t>(source.queue);
                if (role >= queuesByRole_.size() || !queuesByRole_[role].has_value()) {
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidQueueTopology));
                }
                passPositions_[reference.id.value - 1] = passes_.size();
                passes_.push_back({reference, source.kind, *queuesByRole_[role], {}, {}, {}, {}, {}});
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> PreparePasses() {
                passes_.reserve(schedule_.OrderedPasses().size());
                for (const RenderGraphPassRef pass : schedule_.OrderedPasses()) {
                    if (auto added = AddPass(pass); added.HasError()) {
                        return added;
                    }
                }
                return Result<void>::Success();
            }

            [[nodiscard]] std::optional<std::size_t> Position(const RenderGraphPassRef pass) const noexcept {
                if (!IsKnownPass(pass, graph_)) {
                    return std::nullopt;
                }
                return passPositions_[pass.id.value - 1];
            }

            [[nodiscard]] std::optional<RenderQueueId> QueueFor(const RenderGraphPassRef pass) const noexcept {
                if (const auto position = Position(pass); position.has_value()) {
                    return passes_[*position].queue;
                }
                return std::nullopt;
            }

            [[nodiscard]] Result<void> ValidateSynchronizationTopology() const {
                std::array<std::optional<RenderQueueId>, 3> synchronizationQueues;
                for (const RenderQueueAssignment assignment : synchronization_.QueueAssignments()) {
                    if (!assignment.IsValid()) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSynchronization));
                    }
                    const auto role = static_cast<std::size_t>(assignment.role);
                    if (synchronizationQueues[role].has_value()) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSynchronization));
                    }
                    synchronizationQueues[role] = assignment.queue;
                }
                for (const RenderGraphPassRef pass : schedule_.OrderedPasses()) {
                    const auto role = static_cast<std::size_t>(graph_.Passes()[pass.id.value - 1].queue);
                    if (!synchronizationQueues[role].has_value() || synchronizationQueues[role] != queuesByRole_[role]) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSynchronization));
                    }
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> CountUsages(std::vector<std::size_t> &counts) const {
                for (const RenderGraphResourceUsage &usage : graph_.Usages()) {
                    if (!IsKnownResource(usage.resource, graph_)) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
                    }
                    if (const auto position = Position(usage.pass); position.has_value()) {
                        ++counts[*position];
                    }
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> CountTransitions(std::vector<std::size_t> &counts) const {
                for (const RenderGraphTransition &transition : synchronization_.Transitions()) {
                    const auto after = Position(transition.after);
                    const auto beforeQueue = QueueFor(transition.before);
                    if (const bool beforeValid =
                            !transition.before.IsValid() || (beforeQueue.has_value() && transition.oldState.queue == *beforeQueue);
                        !after.has_value() || !beforeValid || transition.newState.queue != passes_[*after].queue ||
                        !IsKnownResource(transition.resource, graph_)) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSynchronization));
                    }
                    ++counts[*after];
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> CountDependencies(std::vector<std::size_t> &counts) const {
                for (const RenderGraphDependency &dependency : graph_.Dependencies()) {
                    const auto after = Position(dependency.after);
                    if (after.has_value() && !Position(dependency.before).has_value()) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSchedule));
                    }
                    if (after.has_value()) {
                        ++counts[*after];
                    }
                }
                return Result<void>::Success();
            }

            [[nodiscard]] bool IsTransferValid(const RenderGraphOwnershipTransfer &transfer) const noexcept {
                const auto acquire = Position(transfer.acquireBefore);
                const auto releaseQueue = QueueFor(transfer.releaseAfter);
                const bool releaseValid =
                    !transfer.releaseAfter.IsValid() || (releaseQueue.has_value() && transfer.sourceQueue == *releaseQueue);
                return acquire.has_value() && releaseValid && IsKnownResource(transfer.resource, graph_) &&
                       transfer.sourceQueue.IsValid() && transfer.destinationQueue == passes_[*acquire].queue &&
                       transfer.sourceQueue != transfer.destinationQueue;
            }

            [[nodiscard]] Result<void> CountTransfers(std::vector<std::size_t> &releaseCounts,
                                                      std::vector<std::size_t> &acquireCounts) const {
                for (const RenderGraphOwnershipTransfer &transfer : synchronization_.OwnershipTransfers()) {
                    if (!IsTransferValid(transfer)) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSynchronization));
                    }
                    const auto acquire = Position(transfer.acquireBefore);
                    if (!acquire.has_value()) {
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSynchronization));
                    }
                    if (const auto release = Position(transfer.releaseAfter); release.has_value()) {
                        ++releaseCounts[*release];
                    }
                    ++acquireCounts[*acquire];
                }
                return Result<void>::Success();
            }

            template <typename T>
            void PrepareStorage(const std::vector<std::size_t> &counts, const RangeMember range, std::vector<T> &storage,
                                std::vector<std::size_t> &cursors) {
                std::size_t offset = 0;
                for (std::size_t index = 0; index < passes_.size(); ++index) {
                    passes_[index].*range = {offset, counts[index]};
                    cursors[index] = offset;
                    offset += counts[index];
                }
                storage.resize(offset);
            }

            template <typename T, typename PassSelector>
            void CopyScheduled(const std::span<const T> source, const PassSelector select, std::vector<std::size_t> &cursors,
                               std::vector<T> &destination) const {
                for (const T &record : source) {
                    if (const auto position = Position(select(record))) {
                        destination[cursors[*position]++] = record;
                    }
                }
            }

            [[nodiscard]] Result<void> CollectRecords() {
                std::vector usageCounts(passes_.size(), std::size_t{});
                std::vector dependencyCounts(passes_.size(), std::size_t{});
                std::vector transitionCounts(passes_.size(), std::size_t{});
                std::vector releaseCounts(passes_.size(), std::size_t{});
                std::vector acquireCounts(passes_.size(), std::size_t{});
                if (auto counted = CountUsages(usageCounts); counted.HasError()) {
                    return counted;
                }
                if (auto counted = CountDependencies(dependencyCounts); counted.HasError()) {
                    return counted;
                }
                if (auto counted = CountTransitions(transitionCounts); counted.HasError()) {
                    return counted;
                }
                if (auto counted = CountTransfers(releaseCounts, acquireCounts); counted.HasError()) {
                    return counted;
                }
                PopulateRecords(usageCounts, dependencyCounts, transitionCounts, releaseCounts, acquireCounts);
                return Result<void>::Success();
            }

            void PopulateRecords(const std::vector<std::size_t> &usageCounts, const std::vector<std::size_t> &dependencyCounts,
                                 const std::vector<std::size_t> &transitionCounts, const std::vector<std::size_t> &releaseCounts,
                                 const std::vector<std::size_t> &acquireCounts) {
                std::vector usageCursors(passes_.size(), std::size_t{});
                std::vector dependencyCursors(passes_.size(), std::size_t{});
                std::vector transitionCursors(passes_.size(), std::size_t{});
                std::vector releaseCursors(passes_.size(), std::size_t{});
                std::vector acquireCursors(passes_.size(), std::size_t{});
                PrepareStorage(usageCounts, &RenderGraphExecutionPass::usages, usages_, usageCursors);
                PrepareStorage(dependencyCounts, &RenderGraphExecutionPass::dependencies, dependencies_, dependencyCursors);
                PrepareStorage(transitionCounts, &RenderGraphExecutionPass::transitions, transitions_, transitionCursors);
                PrepareStorage(releaseCounts, &RenderGraphExecutionPass::releaseTransfers, releaseTransfers_, releaseCursors);
                PrepareStorage(acquireCounts, &RenderGraphExecutionPass::acquireTransfers, acquireTransfers_, acquireCursors);
                CopyScheduled<RenderGraphResourceUsage>(graph_.Usages(), [](const auto &usage) {
                    return usage.pass;
                }, usageCursors, usages_);
                CopyScheduled<RenderGraphDependency>(graph_.Dependencies(), [](const auto &dependency) {
                    return dependency.after;
                }, dependencyCursors, dependencies_);
                CopyScheduled<RenderGraphTransition>(synchronization_.Transitions(), [](const auto &transition) {
                    return transition.after;
                }, transitionCursors, transitions_);
                CopyScheduled<RenderGraphOwnershipTransfer>(synchronization_.OwnershipTransfers(), [](const auto &transfer) {
                    return transfer.releaseAfter;
                }, releaseCursors, releaseTransfers_);
                CopyScheduled<RenderGraphOwnershipTransfer>(synchronization_.OwnershipTransfers(), [](const auto &transfer) {
                    return transfer.acquireBefore;
                }, acquireCursors, acquireTransfers_);
            }

            const RenderGraph &graph_;
            const RenderGraphSchedule &schedule_;
            const RenderGraphSynchronizationPlan &synchronization_;
            std::span<const RenderQueueAssignment> queues_;
            std::array<std::optional<RenderQueueId>, 3> queuesByRole_;
            std::vector<std::optional<std::size_t>> passPositions_;
            std::vector<RenderGraphExecutionPass> passes_;
            std::vector<RenderGraphResourceUsage> usages_;
            std::vector<RenderGraphDependency> dependencies_;
            std::vector<RenderGraphTransition> transitions_;
            std::vector<RenderGraphOwnershipTransfer> releaseTransfers_;
            std::vector<RenderGraphOwnershipTransfer> acquireTransfers_;
        };
    }  // namespace

    /** @copydoc CompiledRenderGraphExecution::CompiledRenderGraphExecution */
    CompiledRenderGraphExecution::CompiledRenderGraphExecution(RenderGraphOwnerId owner, Storage storage) noexcept
        : owner_(owner), resources_(std::move(storage.resources)), passes_(std::move(storage.passes)), usages_(std::move(storage.usages)),
          dependencies_(std::move(storage.dependencies)), transitions_(std::move(storage.transitions)),
          releaseTransfers_(std::move(storage.releaseTransfers)), acquireTransfers_(std::move(storage.acquireTransfers)) {}

    /** @copydoc CompiledRenderGraphExecution::CompiledRenderGraphExecution */
    CompiledRenderGraphExecution::CompiledRenderGraphExecution(CompiledRenderGraphExecution &&other) noexcept
        : owner_(std::exchange(other.owner_, {})), resources_(std::move(other.resources_)), passes_(std::move(other.passes_)),
          usages_(std::move(other.usages_)), dependencies_(std::move(other.dependencies_)), transitions_(std::move(other.transitions_)),
          releaseTransfers_(std::move(other.releaseTransfers_)), acquireTransfers_(std::move(other.acquireTransfers_)) {}

    /** @copydoc CompiledRenderGraphExecution::operator= */
    CompiledRenderGraphExecution &CompiledRenderGraphExecution::operator=(CompiledRenderGraphExecution &&other) noexcept {
        if (this != &other) {
            owner_ = std::exchange(other.owner_, {});
            resources_ = std::move(other.resources_);
            passes_ = std::move(other.passes_);
            usages_ = std::move(other.usages_);
            dependencies_ = std::move(other.dependencies_);
            transitions_ = std::move(other.transitions_);
            releaseTransfers_ = std::move(other.releaseTransfers_);
            acquireTransfers_ = std::move(other.acquireTransfers_);
        }
        return *this;
    }

    /** @copydoc CompiledRenderGraphExecution::Owner */
    RenderGraphOwnerId CompiledRenderGraphExecution::Owner() const noexcept {
        return owner_;
    }

    /** @copydoc CompiledRenderGraphExecution::Resources */
    std::span<const RenderGraphResource> CompiledRenderGraphExecution::Resources() const noexcept {
        return resources_;
    }

    /** @copydoc CompiledRenderGraphExecution::Passes */
    std::span<const RenderGraphExecutionPass> CompiledRenderGraphExecution::Passes() const noexcept {
        return passes_;
    }

    /** @copydoc CompiledRenderGraphExecution::Usages */
    std::span<const RenderGraphResourceUsage> CompiledRenderGraphExecution::Usages() const noexcept {
        return usages_;
    }

    /** @copydoc CompiledRenderGraphExecution::Dependencies */
    std::span<const RenderGraphDependency> CompiledRenderGraphExecution::Dependencies() const noexcept {
        return dependencies_;
    }

    /** @copydoc CompiledRenderGraphExecution::Transitions */
    std::span<const RenderGraphTransition> CompiledRenderGraphExecution::Transitions() const noexcept {
        return transitions_;
    }

    /** @copydoc CompiledRenderGraphExecution::ReleaseTransfers */
    std::span<const RenderGraphOwnershipTransfer> CompiledRenderGraphExecution::ReleaseTransfers() const noexcept {
        return releaseTransfers_;
    }

    /** @copydoc CompiledRenderGraphExecution::AcquireTransfers */
    std::span<const RenderGraphOwnershipTransfer> CompiledRenderGraphExecution::AcquireTransfers() const noexcept {
        return acquireTransfers_;
    }

    /** @copydoc CompileRenderGraphExecution */
    Result<CompiledRenderGraphExecution> CompileRenderGraphExecution(const RenderGraph &graph, const RenderGraphSchedule &schedule,
                                                                     const RenderGraphSynchronizationPlan &synchronization,
                                                                     const std::span<const RenderQueueAssignment> queues) {
        try {
            ExecutionCompiler compiler{graph, schedule, synchronization, queues};
            if (auto compiled = compiler.Compile(); compiled.HasError()) {
                return Result<CompiledRenderGraphExecution>::Failure(compiled.ErrorValue());
            }
            std::vector resources(graph.Resources().begin(), graph.Resources().end());
            return Result<CompiledRenderGraphExecution>::Success(
                CompiledRenderGraphExecution{graph.Owner(),
                                             CompiledRenderGraphExecution::Storage{std::move(resources), compiler.TakePasses(),
                                                                                   compiler.TakeUsages(), compiler.TakeDependencies(),
                                                                                   compiler.TakeTransitions(),
                                                                                   compiler.TakeReleaseTransfers(),
                                                                                   compiler.TakeAcquireTransfers()}});
        } catch (const std::bad_alloc &) {
            return Result<CompiledRenderGraphExecution>::Failure(MakeError(RenderGraphExecutionErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
