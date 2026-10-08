#include "Horo/AI/DecisionWakePolicy.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::AI {
    /** @copydoc DecisionWakeReasons::Any */
    bool DecisionWakeReasons::Any() const noexcept {
        return activation || blackboard || perception || task || polling || explicitRequest;
    }

    /** @copydoc DecisionWakePolicy::CreateAtBlackboardSync */
    Result<std::unique_ptr<DecisionWakePolicy>> DecisionWakePolicy::CreateAtBlackboardSync(
        std::shared_ptr<const DecisionAssetPlan> plan, BlackboardInstance &blackboard, const TaskHandle observerOwner,
        const std::span<const DecisionWakePollingRule> polling, const std::span<const DecisionWakePerceptionDependency> perception,
        const DecisionWakeBudget &budget, CancellationToken cancellation) {
        if (!plan || plan->Nodes().empty() || !blackboard.IsActive() || !observerOwner.IsValid() ||
            observerOwner.incarnation != blackboard.Binding().agent.incarnation ||
            plan->BlackboardSchema()->Identity() != blackboard.Binding().schema ||
            plan->BlackboardSchema()->Version() != blackboard.Binding().schemaVersion)
            return Result<std::unique_ptr<DecisionWakePolicy>>::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        if (budget.maximumEvaluationsPerTick == 0 || budget.maximumEvaluationsPerTick > DecisionWakeBudget::HardEvaluationsPerTick ||
            budget.maximumPollingWakeupsPerUpdate == 0 ||
            budget.maximumPollingWakeupsPerUpdate > DecisionAssetValidationHardLimits::NodesPerAsset ||
            polling.size() > plan->Nodes().size() || perception.size() > DecisionAssetValidationHardLimits::NodesPerAsset)
            return Result<std::unique_ptr<DecisionWakePolicy>>::Failure(MakeError(AIErrors::DecisionAssetLimitExceeded));
        try {
            auto policy = std::unique_ptr<DecisionWakePolicy>(new DecisionWakePolicy());
            policy->plan_ = std::move(plan);
            policy->blackboard_ = &blackboard;
            policy->binding_ = blackboard.Binding();
            policy->budget_ = budget;
            policy->cancellation_ = std::move(cancellation);
            if (const auto prepared = policy->Prepare(polling, perception); prepared.HasError())
                return Result<std::unique_ptr<DecisionWakePolicy>>::Failure(prepared.ErrorValue());
            for (auto &watch : policy->watches_) {
                auto registered =
                    blackboard.RegisterObserverAtBlackboardSync({policy->binding_.agent, observerOwner, watch.key, OnBlackboard, &watch});
                if (registered.HasError())
                    return Result<std::unique_ptr<DecisionWakePolicy>>::Failure(registered.ErrorValue());
                watch.token = std::move(registered).Value();
            }
            return Result<std::unique_ptr<DecisionWakePolicy>>::Success(std::move(policy));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<DecisionWakePolicy>>::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc DecisionWakePolicy::Prepare */
    Result<void> DecisionWakePolicy::Prepare(const std::span<const DecisionWakePollingRule> polling,
                                             const std::span<const DecisionWakePerceptionDependency> perception) {
        PrepareWatches();
        if (const auto result = PreparePolling(polling); result.HasError())
            return result;
        return PreparePerception(perception);
    }

    /** @copydoc DecisionWakePolicy::PrepareWatches */
    void DecisionWakePolicy::PrepareWatches() {
        nodes_.resize(plan_->Nodes().size());
        admitted_.reserve(nodes_.size());
        for (std::size_t node = 0; node < nodes_.size(); ++node) {
            nodes_[node].pending.activation = true;
            for (const auto &binding : plan_->BindingsForNode(plan_->Nodes()[node].id)) {
                auto watch = std::ranges::find(watches_, binding.key, &Watch::key);
                if (watch == watches_.end()) {
                    watches_.emplace_back(this, binding.key);
                    watch = watches_.end() - 1;
                }
                watch->nodes.push_back(node);
            }
        }
    }

    /** @copydoc DecisionWakePolicy::PreparePolling */
    Result<void> DecisionWakePolicy::PreparePolling(const std::span<const DecisionWakePollingRule> polling) {
        polls_.reserve(polling.size());
        for (const auto &rule : polling) {
            const auto node = Find(rule.node);
            if (node == nodes_.size() || rule.firstTick == 0 || rule.intervalTicks == 0)
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
            polls_.emplace_back(node, rule.firstTick, rule.intervalTicks, false, rule.enabled);
        }
        std::ranges::sort(polls_, {}, &Poll::node);
        if (std::ranges::adjacent_find(polls_, [](const Poll &left, const Poll &right) {
            return left.node == right.node;
        }) != polls_.end())
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        RefreshDeadline();
        return Result<void>::Success();
    }

    /** @copydoc DecisionWakePolicy::PreparePerception */
    Result<void> DecisionWakePolicy::PreparePerception(const std::span<const DecisionWakePerceptionDependency> perception) {
        perception_.assign(perception.begin(), perception.end());
        for (const auto &dependency : perception_)
            if (!dependency.listener.IsValid() || Find(dependency.node) == nodes_.size())
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        std::ranges::sort(perception_, [](const auto &left, const auto &right) {
            return left.listener == right.listener ? left.node < right.node : left.listener < right.listener;
        });
        if (std::ranges::adjacent_find(perception_, [](const auto &left, const auto &right) {
            return left.listener == right.listener && left.node == right.node;
        }) != perception_.end())
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        return Result<void>::Success();
    }

    /** @copydoc DecisionWakePolicy::Find */
    std::size_t DecisionWakePolicy::Find(const DecisionNodeId node) const noexcept {
        const auto records = plan_->Nodes();
        const auto found = std::ranges::lower_bound(records, node, {}, &DecisionPlanNode::id);
        return found != records.end() && found->id == node ? static_cast<std::size_t>(found - records.begin()) : records.size();
    }

    /** @copydoc DecisionWakePolicy::Current */
    bool DecisionWakePolicy::Current() const noexcept {
        return !closed_ && !cancellation_.IsCancellationRequested() && blackboard_->IsActive() && blackboard_->Binding() == binding_;
    }

    /** @copydoc DecisionWakePolicy::OnBlackboard */
    void DecisionWakePolicy::OnBlackboard(void *context, const BlackboardNotificationBatch &notification) noexcept {
        auto &watch = *static_cast<Watch *>(context);
        if (!watch.owner->Current() || notification.Binding() != watch.owner->binding_)
            return;
        for (const auto node : watch.nodes)
            watch.owner->nodes_[node].pending.blackboard = true;
        watch.owner->pending_ = true;
    }

    /** @copydoc DecisionWakePolicy::AdmitPolling */
    void DecisionWakePolicy::AdmitPolling(const std::uint64_t tick) noexcept {
        if (!hasPoll_ || tick < nextPoll_)
            return;
        std::size_t admitted{};
        const auto start = pollCursor_;
        for (std::size_t offset = 0; offset < polls_.size() && admitted < budget_.maximumPollingWakeupsPerUpdate; ++offset) {
            const auto index = (start + offset) % polls_.size();
            auto &poll = polls_[index];
            if (!poll.enabled || poll.exhausted || tick < poll.next)
                continue;
            nodes_[poll.node].pending.polling = true;
            pending_ = true;
            ++admitted;
            pollCursor_ = (index + 1) % polls_.size();
            // Overflow retires this cadence after its final representable wake; it never wraps to an earlier tick.
            poll.exhausted = tick > std::numeric_limits<std::uint64_t>::max() - poll.interval;
            if (!poll.exhausted)
                poll.next = tick + poll.interval;
        }
        RefreshDeadline();
    }

    /** @copydoc DecisionWakePolicy::RefreshDeadline */
    void DecisionWakePolicy::RefreshDeadline() noexcept {
        hasPoll_ = false;
        for (const auto &poll : polls_) {
            if (!poll.enabled || poll.exhausted)
                continue;
            nextPoll_ = hasPoll_ ? std::min(nextPoll_, poll.next) : poll.next;
            hasPoll_ = true;
        }
    }

    /** @copydoc DecisionWakePolicy::SetPollingEnabled */
    Result<void> DecisionWakePolicy::SetPollingEnabled(const DecisionNodeId node, const bool enabled, const std::uint64_t nextTick) {
        if (!Current() || (enabled && (nextTick == 0 || nextTick < tick_)))
            return Result<void>::Failure(MakeError(AIErrors::TaskTransitionInvalid));
        const auto found = std::ranges::find(polls_, Find(node), &Poll::node);
        if (found == polls_.end())
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        found->enabled = enabled;
        if (enabled) {
            found->next = nextTick;
            found->exhausted = false;
        }
        RefreshDeadline();
        return Result<void>::Success();
    }

    /** @copydoc DecisionWakePolicy::BeginUpdate */
    Result<std::span<const DecisionWakeRequest>> DecisionWakePolicy::BeginUpdate(const std::uint64_t tick, const AgentHandle activeAgent) {
        if (closed_ || evaluating_ || tick == 0 || tick < tick_)
            return Result<std::span<const DecisionWakeRequest>>::Failure(MakeError(AIErrors::TaskTransitionInvalid));
        if (activeAgent != binding_.agent || !Current()) {
            if (const auto closed = CloseAtBlackboardSync(); closed.HasError())
                return Result<std::span<const DecisionWakeRequest>>::Failure(closed.ErrorValue());
            return Result<std::span<const DecisionWakeRequest>>::Failure(MakeError(AIErrors::BlackboardInstanceStale));
        }
        if (tick != tick_) {
            tick_ = tick;
            evaluations_ = 0;
        }
        if (evaluations_ == budget_.maximumEvaluationsPerTick)
            return Result<std::span<const DecisionWakeRequest>>::Success({});
        AdmitPolling(tick);
        if (!pending_)
            return Result<std::span<const DecisionWakeRequest>>::Success({});
        admitted_.clear();
        for (std::size_t node = 0; node < nodes_.size(); ++node) {
            auto &reasons = nodes_[node].pending;
            if (reasons.Any())
                admitted_.emplace_back(plan_->Nodes()[node].id, reasons);
            reasons = {};
        }
        pending_ = false;
        evaluating_ = true;
        ++evaluations_;
        return Result<std::span<const DecisionWakeRequest>>::Success(admitted_);
    }

    /** @copydoc DecisionWakePolicy::FinishUpdate */
    Result<void> DecisionWakePolicy::FinishUpdate() {
        if (closed_ || !evaluating_)
            return Result<void>::Failure(MakeError(AIErrors::TaskTransitionInvalid));
        evaluating_ = false;
        admitted_.clear();
        return Result<void>::Success();
    }

    /** @copydoc DecisionWakePolicy::Request */
    Result<void> DecisionWakePolicy::Request(const DecisionNodeId node) {
        if (!Current())
            return Result<void>::Failure(MakeError(AIErrors::BlackboardInstanceStale));
        const auto index = Find(node);
        if (index == nodes_.size())
            return Result<void>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        nodes_[index].pending.explicitRequest = true;
        pending_ = true;
        return Result<void>::Success();
    }

    /** @copydoc DecisionWakePolicy::NotifyPerception */
    Result<bool> DecisionWakePolicy::NotifyPerception(const AgentHandle agent, const PerceptionListenerTypeId listener) {
        if (!Current() || agent != binding_.agent || !listener.IsValid())
            return Result<bool>::Failure(MakeError(AIErrors::PerceptionEventStale));
        bool matched{};
        for (const auto &dependency : perception_) {
            if (dependency.listener != listener)
                continue;
            nodes_[Find(dependency.node)].pending.perception = true;
            matched = true;
        }
        pending_ = pending_ || matched;
        return Result<bool>::Success(matched);
    }

    /** @copydoc DecisionWakePolicy::BindTask */
    Result<void> DecisionWakePolicy::BindTask(const DecisionNodeId node, const std::optional<TaskHandle> &task) {
        const auto index = Find(node);
        if (!Current() || index == nodes_.size() ||
            (task.has_value() && (!task->IsValid() || task->incarnation != binding_.agent.incarnation)))
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        if (task.has_value())
            for (std::size_t other = 0; other < nodes_.size(); ++other)
                if (other != index && nodes_[other].task == task)
                    return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        nodes_[index].task = task;
        return Result<void>::Success();
    }

    /** @copydoc DecisionWakePolicy::NotifyTask */
    Result<bool> DecisionWakePolicy::NotifyTask(const AgentHandle agent, const DecisionNodeId node, const TaskHandle task) {
        const auto index = Find(node);
        if (!Current() || agent != binding_.agent || index == nodes_.size() || !task.IsValid())
            return Result<bool>::Failure(MakeError(AIErrors::TaskContextInvalid));
        if (nodes_[index].task != task)
            return Result<bool>::Success(false);
        nodes_[index].pending.task = true;
        pending_ = true;
        return Result<bool>::Success(true);
    }

    /** @copydoc DecisionWakePolicy::CloseAtBlackboardSync */
    Result<void> DecisionWakePolicy::CloseAtBlackboardSync() {
        if (closed_)
            return Result<void>::Success();
        // Replacement and teardown invalidate the entire old registry before releasing callback contexts.
        const bool registryCurrent = blackboard_->IsActive() && blackboard_->Binding() == binding_;
        for (auto &watch : watches_) {
            if (!watch.token.has_value())
                continue;
            if (registryCurrent) {
                if (const auto removed = blackboard_->RemoveObserverAtBlackboardSync(*watch.token); removed.HasError())
                    return Result<void>::Failure(removed.ErrorValue());
            }
            watch.token.reset();
        }
        closed_ = true;
        evaluating_ = false;
        admitted_.clear();
        blackboard_ = nullptr;
        return Result<void>::Success();
    }

    /** @copydoc DecisionWakePolicy::~DecisionWakePolicy */
    DecisionWakePolicy::~DecisionWakePolicy() {
        static_cast<void>(CloseAtBlackboardSync());
    }
}  // namespace Horo::AI
