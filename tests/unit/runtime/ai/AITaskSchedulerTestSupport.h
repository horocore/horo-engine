#pragma once

#include "AiSceneTestSupport.h"
#include "Horo/AI/AITaskScheduler.h"

#include <thread>

namespace Horo::AI::SchedulerTestSupport {
    using TestSupport::MakeIdentity;

    struct Probe final : IAiScheduledDecision {
        std::vector<std::uint64_t> *order{};
        std::uint64_t id{};
        std::size_t remaining{1}, intents{}, committed{}, cancelled{}, evaluations{};
        bool fail{}, exhaust{}, stop{};
        CancellationSource *cancelPeer{};
        CancellationSource *cancelAtCommit{};
        AiTaskScheduler *scheduler{};
        std::optional<AiTaskContinuation> continuation;
        std::optional<AiWorkerSliceLease> worker;
        std::optional<Error> rejection;
        std::size_t attempts{1};
        DecisionWakeReasons reasons;
        std::thread::id owner{std::this_thread::get_id()};

        Result<AiSliceOutcome> Evaluate(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept override {
            CHECK(std::this_thread::get_id() == owner);
            ++evaluations;
            if (cancelPeer)
                cancelPeer->RequestCancellation();
            reasons = context.reasons;
            if (order)
                order->push_back(id);
            if (stop) {
                scheduler->Shutdown();
                return Result<AiSliceOutcome>::Success(AiSliceOutcome::Complete);
            }
            if (continuation && worker) {
                for (std::size_t i = 0; i < attempts; ++i) {
                    auto submitted = scheduler->SubmitWorker(*continuation, *worker);
                    if (submitted.HasError())
                        rejection = submitted.ErrorValue();
                }
            }
            if (fail)
                return Result<AiSliceOutcome>::Failure(MakeError(AIErrors::CapabilityUnavailable));
            while (remaining > 0) {
                if (context.cancellation.IsCancellationRequested())
                    return Result<AiSliceOutcome>::Success(AiSliceOutcome::Yielded);
                if (!budget.Consume())
                    return Result<AiSliceOutcome>::Success(AiSliceOutcome::BudgetExhausted);
                --remaining;
                ++intents;
            }
            if (exhaust)
                static_cast<void>(budget.Consume(budget.Remaining() + 1));
            return Result<AiSliceOutcome>::Success(AiSliceOutcome::Complete);
        }

        Result<bool> Commit(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept override {
            if (cancelAtCommit)
                cancelAtCommit->RequestCancellation();
            CHECK(std::this_thread::get_id() == owner);
            while (intents > 0) {
                if (context.cancellation.IsCancellationRequested())
                    return Result<bool>::Success(false);
                if (!budget.Consume())
                    return Result<bool>::Success(false);
                --intents;
                ++committed;
            }
            return Result<bool>::Success(true);
        }

        void Cancel() noexcept override {
            ++cancelled;
            intents = 0;
        }
    };

    struct DestructionProbe final : IAiScheduledDecision {
        std::weak_ptr<const void> image;
        bool *pinned{};

        DestructionProbe(std::weak_ptr<const void> pin, bool &alive) : image(std::move(pin)), pinned(&alive) {}

        ~DestructionProbe() override {
            *pinned = !image.expired();
        }

        Result<AiSliceOutcome> Evaluate(const AiDecisionSliceContext &, AiWorkBudget &) noexcept override {
            return Result<AiSliceOutcome>::Success(AiSliceOutcome::Complete);
        }

        Result<bool> Commit(const AiDecisionSliceContext &, AiWorkBudget &) noexcept override {
            return Result<bool>::Success(true);
        }

        void Cancel() noexcept override {}
    };

    struct Harness final {
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4}};
        AiRuntimeIncarnation incarnation{MakeIdentity<AiRuntimeIncarnation>(7)};
        AiTaskSchedulerSettings settings;
        std::unique_ptr<AiTaskScheduler> scheduler;
        std::shared_ptr<int> image{std::make_shared<int>(1)};

        Harness(AiTaskSchedulerSettings limits = {}) : settings(limits) {
            auto created = AiTaskScheduler::Create(jobs, incarnation, limits);
            REQUIRE(created.HasValue());
            scheduler = std::move(created).Value();
        }

        AgentHandle Handle(const std::uint32_t slot) const {
            return {incarnation, {slot, 1}};
        }

        std::shared_ptr<Probe> Register(const std::uint32_t slot, const AiAgentSchedulePolicy policy = {},
                                        const CancellationToken cancellation = {}) {
            auto probe = std::make_shared<Probe>();
            probe->id = slot;
            probe->scheduler = scheduler.get();
            REQUIRE(scheduler->Register(Handle(slot), MakeIdentity<AgentId>(slot + 1), policy, image, probe, cancellation).HasValue());
            return probe;
        }
    };

}  // namespace Horo::AI::SchedulerTestSupport
