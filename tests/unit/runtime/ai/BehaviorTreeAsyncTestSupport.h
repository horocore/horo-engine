#pragma once

#include "BehaviorTreeRuntimeTestSupport.h"
#include "Horo/AI/AITaskJobService.h"

#include <chrono>
#include <semaphore>
#include <thread>

namespace Horo::AI::BehaviorTreeAsyncTestSupport {
    using BehaviorTreeTestSupport::Executor;
    using BehaviorTreeTestSupport::Fixture;
    using BehaviorTreeTestSupport::Probe;

    struct AsyncProbe final {
        std::array<AiTaskContinuation, Probe::Capacity> continuations;
        std::array<AiTaskResumeReason, Probe::Capacity> reasons{};
        std::array<AiTaskJobLease, Probe::Capacity> work;
        std::unique_ptr<AiTaskJobService> jobs;
    };

    class AsyncExecutor final : public IBehaviorTreeExecutor {
    public:
        AsyncExecutor(std::shared_ptr<Probe> probe, std::shared_ptr<AsyncProbe> async)
            : delegate_(std::move(probe)), async_(std::move(async)) {}

        void Pump() noexcept override {
            if (async_->jobs)
                async_->jobs->Pump();
        }

        Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &context, const AiTaskOperationContext &operation) noexcept override {
            async_->continuations[context.node.Value()] = context.continuation;
            const auto started = delegate_.Start(context, operation);
            if (!async_->jobs || !async_->work[context.node.Value()].IsValid())
                return started;
            if (const auto submitted = async_->jobs->Start(context.continuation, async_->work[context.node.Value()]); submitted.HasError())
                return Result<AiTaskState>::Failure(submitted.ErrorValue());
            return Result<AiTaskState>::Success(AiTaskState::Running);
        }

        Result<AiTaskState> Resume(const BehaviorTreeEvaluationContext &context,
                                   const AiTaskOperationContext &operation) noexcept override {
            async_->reasons[context.node.Value()] = context.reason;
            return delegate_.Resume(context, operation);
        }

        void Abort(const AiTaskOperationContext &operation, const AiTaskCancellationReason reason) noexcept override {
            delegate_.Abort(operation, reason);
            if (async_->jobs)
                static_cast<void>(async_->jobs->Cancel(operation.task, reason));
        }

        void Cleanup(const AiTaskOperationContext &operation, const AiTaskTerminalResult &result) noexcept override {
            delegate_.Cleanup(operation, result);
        }

        Result<bool> Check(const BehaviorTreeEvaluationContext &context) noexcept override {
            return delegate_.Check(context);
        }

        Result<void> Service(const BehaviorTreeEvaluationContext &context) noexcept override {
            return delegate_.Service(context);
        }

    private:
        Executor delegate_;
        std::shared_ptr<AsyncProbe> async_;
    };

    inline std::unique_ptr<BehaviorTreeInstance> Instance(const Fixture &fixture, const std::shared_ptr<AsyncProbe> &async,
                                                          const CancellationToken cancellation = {}) {
        const auto snapshot = fixture.blackboard->Snapshot();
        REQUIRE(snapshot.HasValue());
        const BehaviorTreeInstanceBinding binding{fixture.agent, cancellation, 100, 1, snapshot.Value().Binding()};
        auto created = BehaviorTreeInstance::Create(fixture.Plan(), binding, std::make_unique<AsyncExecutor>(fixture.probe, async));
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    struct StartedInstance final {
        std::shared_ptr<AsyncProbe> async{std::make_shared<AsyncProbe>()};
        std::unique_ptr<BehaviorTreeInstance> instance;

        explicit StartedInstance(const Fixture &fixture) : instance(Instance(fixture, async)) {
            REQUIRE(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
        }
    };

    struct StartedTask final {
        Fixture fixture{{BehaviorTreeTestSupport::Node(1, BehaviorTreeOperation::Task)}};
        std::shared_ptr<AsyncProbe> async{std::make_shared<AsyncProbe>()};
        std::unique_ptr<BehaviorTreeInstance> instance;

        StartedTask() {
            fixture.probe->outcome[1] = AiTaskState::Running;
            instance = Instance(fixture, async);
            REQUIRE(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
        }
    };

    inline std::vector<BehaviorTreeExecutionNode> TaskSequence() {
        using BehaviorTreeTestSupport::Node;
        using enum BehaviorTreeOperation;
        return {Node(1, Sequence, {2, 3}), Node(2, Task), Node(3, Task)};
    }

    inline std::vector<BehaviorTreeExecutionNode> TaskParallel() {
        using BehaviorTreeTestSupport::Node;
        using enum BehaviorTreeOperation;
        return {Node(1, Parallel, {2, 3}), Node(2, Task), Node(3, Task)};
    }

    inline AiTaskPublicationDisposition Publish(const AiTaskContinuation &continuation, const AiTaskState state = AiTaskState::Succeeded) {
        AiTaskTerminalResult result{.state = state};
        if (state == AiTaskState::Failed)
            result.failure = AiTaskFailureDetail{AiTaskFailureKind::Execution, MakeError(AIErrors::RuntimeUnavailable)};
        if (state == AiTaskState::Cancelled)
            result.cancellationReason = AiTaskCancellationReason::Requested;
        const auto published = continuation.PublishTerminal(result);
        REQUIRE(published.HasValue());
        return published.Value();
    }

    struct WorkSignals final {
        std::binary_semaphore started{0};
        std::binary_semaphore release{0};
        bool fail{};
    };

    class Work final : public IAiTaskJob {
    public:
        explicit Work(std::shared_ptr<WorkSignals> signals) : signals_(std::move(signals)) {}

        Result<void> Run(const AiTaskOperationContext &, const CancellationToken &cancellation) const override {
            signals_->started.release();
            while (!signals_->release.try_acquire_for(std::chrono::milliseconds(10)))
                if (cancellation.IsCancellationRequested())
                    return JobCancelled();
            if (cancellation.IsCancellationRequested())
                return JobCancelled();
            return signals_->fail ? Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable)) : Result<void>::Success();
        }

    private:
        std::shared_ptr<WorkSignals> signals_;
    };

    class LeaseBoundWork final : public IAiTaskJob {
    public:
        LeaseBoundWork(std::weak_ptr<const void> image, std::shared_ptr<std::atomic<bool>> releasedSafely)
            : image_(std::move(image)), releasedSafely_(std::move(releasedSafely)) {}

        ~LeaseBoundWork() override {
            releasedSafely_->store(!image_.expired());
        }

        LeaseBoundWork(const LeaseBoundWork &) = delete;
        LeaseBoundWork &operator=(const LeaseBoundWork &) = delete;
        LeaseBoundWork(LeaseBoundWork &&) = delete;
        LeaseBoundWork &operator=(LeaseBoundWork &&) = delete;

        Result<void> Run(const AiTaskOperationContext &, const CancellationToken &) const override {
            return Result<void>::Success();
        }

    private:
        std::weak_ptr<const void> image_;
        std::shared_ptr<std::atomic<bool>> releasedSafely_;
    };

    inline void CreateService(AsyncProbe &async, JobSystem &jobs, const std::size_t capacity) {
        auto created = AiTaskJobService::Create(jobs, capacity);
        REQUIRE(created.HasValue());
        async.jobs = std::move(created).Value();
    }

    inline void PumpUntilDrained(AiTaskJobService &service) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (service.PendingCount() > 0 && std::chrono::steady_clock::now() < deadline) {
            service.Pump();
            std::this_thread::yield();
        }
        REQUIRE(service.PendingCount() == 0);
    }
}  // namespace Horo::AI::BehaviorTreeAsyncTestSupport
