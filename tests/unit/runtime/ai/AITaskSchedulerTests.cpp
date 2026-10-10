#include "AiSceneTestSupport.h"
#include "BehaviorTreeAsyncTestSupport.h"
#include "Horo/AI/AITaskScheduler.h"

#include <limits>
#include <semaphore>
#include <thread>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeFixture;
        using TestSupport::MakeIdentity;
        using TestSupport::Publish;

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

        TEST_CASE("AI scheduler validates finite policies and exact generations before admission", "[unit][ai][scheduler]") {
            Harness h;
            auto bad = h.settings;
            bad.maximumAgents = 0;
            ExpectError(AiTaskScheduler::Create(h.jobs, h.incarnation, bad), AIErrors::SchedulerPolicyInvalid);
            bad = h.settings;
            bad.starvationTicks = 0;
            ExpectError(AiTaskScheduler::Create(h.jobs, h.incarnation, bad), AIErrors::SchedulerPolicyInvalid);
            bad = h.settings;
            bad.mode = AiSchedulingMode::Count;
            ExpectError(AiTaskScheduler::Create(h.jobs, h.incarnation, bad), AIErrors::SchedulerPolicyInvalid);
            auto probe = h.Register(0);
            ExpectError(h.scheduler->Register(h.Handle(0), MakeIdentity<AgentId>(1), {}, h.image, probe), AIErrors::DescriptorConflict);
            auto foreign = h.Handle(1);
            foreign.incarnation = MakeIdentity<AiRuntimeIncarnation>(8);
            ExpectError(h.scheduler->Register(foreign, MakeIdentity<AgentId>(2), {}, h.image, probe), AIErrors::SchedulerPolicyInvalid);
            auto invalid = AiAgentSchedulePolicy{};
            invalid.intervalTicks = 0;
            ExpectError(h.scheduler->Register(h.Handle(1), MakeIdentity<AgentId>(2), invalid, h.image, probe),
                        AIErrors::SchedulerPolicyInvalid);
        }

        TEST_CASE("AI rejected scheduler and scene registration keep code pinned through executor destruction",
                  "[unit][ai][scheduler][lifetime]") {
            Harness h;
            for (const bool inactiveScene : {false, true}) {
                auto image = std::make_shared<int>(1);
                std::weak_ptr<const void> pin = image;
                bool destroyedWhilePinned{};
                auto executor = std::make_shared<DestructionProbe>(pin, destroyedWhilePinned);
                if (inactiveScene) {
                    auto runtime = std::move(AiSceneRuntime::Create()).Value();
                    ExpectError(runtime.RegisterDecisionAtSafePoint(h.Handle(0), {}, std::move(image), std::move(executor)),
                                AIErrors::RuntimeUnavailable);
                } else {
                    ExpectError(h.scheduler->Register({}, MakeIdentity<AgentId>(1), {}, std::move(image), std::move(executor)),
                                AIErrors::SchedulerPolicyInvalid);
                }
                CHECK(destroyedWhilePinned);
                CHECK(pin.expired());
            }
        }

        TEST_CASE("AI deterministic scheduling uses priority then persistent identity regardless of registration order",
                  "[unit][ai][scheduler]") {
            for (const bool reverse : {false, true}) {
                Harness h;
                std::vector<std::uint64_t> order;
                auto a = h.Register(reverse ? 2 : 0);
                auto b = h.Register(reverse ? 0 : 2);
                auto urgent = h.Register(1, {.priority = AiTaskPriority::Urgent});
                a->order = b->order = urgent->order = &order;
                REQUIRE(h.scheduler->EvaluateAtDecision(1).HasValue());
                CHECK(order == std::vector<std::uint64_t>{1, 0, 2});
                CHECK(a->committed == 0);
                REQUIRE(h.scheduler->CommitAtIntentDispatch(1).HasValue());
                CHECK(a->committed == 1);
                REQUIRE(h.scheduler->CommitAtIntentDispatch(1).HasValue());
                CHECK(a->committed == 1);
                ExpectError(h.scheduler->EvaluateAtDecision(1), AIErrors::SchedulerPhaseInvalid);
            }
        }

        TEST_CASE("AI starvation promotion gives bounded service under sustained urgent traffic", "[unit][ai][scheduler]") {
            Harness h({.evaluationsPerTick = 1, .starvationTicks = 2});
            auto urgent = h.Register(0, {.priority = AiTaskPriority::Urgent});
            auto background = h.Register(1, {.priority = AiTaskPriority::Background});
            for (std::uint64_t tick = 1; tick <= 3; ++tick) {
                auto evaluated = h.scheduler->EvaluateAtDecision(tick);
                REQUIRE(evaluated.HasValue());
                REQUIRE(h.scheduler->CommitAtIntentDispatch(tick).HasValue());
                if (tick == 3) {
                    CHECK(evaluated.Value().starved == 1);
                    CHECK(background->evaluations == 1);
                }
            }
            CHECK(urgent->evaluations == 2);
        }

        TEST_CASE("AI same tick calls and wake floods cannot replenish work or grow admission", "[unit][ai][scheduler]") {
            Harness h({.maximumAgents = 1, .evaluationsPerTick = 1, .workUnitsPerTick = 3, .commandsPerTick = 2});
            auto probe = h.Register(0, {.workUnitsPerTick = 3, .commandsPerTick = 2});
            probe->remaining = 7;
            for (std::size_t i = 0; i < 10'000; ++i)
                REQUIRE(h.scheduler->Wake(h.Handle(0), {.perception = true, .task = true}).HasValue());
            const auto first = h.scheduler->EvaluateAtDecision(1);
            REQUIRE(first.HasValue());
            CHECK(first.Value().workUnits == 3);
            CHECK(first.Value().exhausted == 1);
            CHECK(probe->reasons.perception);
            CHECK(probe->reasons.task);
            CHECK(probe->remaining == 4);
            REQUIRE(h.scheduler->EvaluateAtDecision(1).HasValue());
            CHECK(probe->remaining == 4);
            const auto commit = h.scheduler->CommitAtIntentDispatch(1);
            REQUIRE(commit.HasValue());
            CHECK(commit.Value().commands == 2);
            CHECK(probe->committed == 2);
            REQUIRE(h.scheduler->EvaluateAtDecision(2).HasValue());
            CHECK(probe->remaining == 4);  // Pending commands fence overwrite of a bounded result batch.
            REQUIRE(h.scheduler->CommitAtIntentDispatch(2).HasValue());
            CHECK(probe->committed == 3);
            REQUIRE(h.scheduler->EvaluateAtDecision(3).HasValue());
            CHECK(probe->remaining == 1);
            REQUIRE(h.scheduler->CommitAtIntentDispatch(3).HasValue());
            auto extra = std::make_shared<Probe>();
            ExpectError(h.scheduler->Register(h.Handle(1), MakeIdentity<AgentId>(2), {.workUnitsPerTick = 3, .commandsPerTick = 2}, h.image,
                                              extra),
                        AIErrors::AgentCapacityExceeded);
        }

        TEST_CASE("AI frequency coalesces missed intervals and handles clock exhaustion", "[unit][ai][scheduler]") {
            Harness h;
            auto probe = h.Register(0, {.intervalTicks = 10});
            REQUIRE(h.scheduler->EvaluateAtDecision(1).HasValue());
            REQUIRE(h.scheduler->CommitAtIntentDispatch(1).HasValue());
            REQUIRE(h.scheduler->EvaluateAtDecision(9).HasValue());
            CHECK(probe->evaluations == 1);
            REQUIRE(h.scheduler->CommitAtIntentDispatch(9).HasValue());
            REQUIRE(h.scheduler->EvaluateAtDecision(31).HasValue());
            CHECK(probe->evaluations == 2);
            REQUIRE(h.scheduler->CommitAtIntentDispatch(31).HasValue());
            REQUIRE(h.scheduler->Wake(h.Handle(0), {.blackboard = true}).HasValue());
            REQUIRE(h.scheduler->EvaluateAtDecision(32).HasValue());
            CHECK(probe->evaluations == 3);
            REQUIRE(h.scheduler->CommitAtIntentDispatch(32).HasValue());
            ExpectError(h.scheduler->EvaluateAtDecision(31), AIErrors::SchedulerPhaseInvalid);
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            REQUIRE(h.scheduler->EvaluateAtDecision(maximum).HasValue());
            REQUIRE(h.scheduler->CommitAtIntentDispatch(maximum).HasValue());
            ExpectError(h.scheduler->EvaluateAtDecision(0), AIErrors::SchedulerPhaseInvalid);
        }

        TEST_CASE("AI cancellation fences pending intents and failure preserves its original error", "[unit][ai][scheduler]") {
            Harness h;
            CancellationSource cancellation;
            auto probe = h.Register(0, {}, cancellation.Token());
            REQUIRE(h.scheduler->EvaluateAtDecision(1).HasValue());
            cancellation.RequestCancellation();
            const auto commit = h.scheduler->CommitAtIntentDispatch(1);
            REQUIRE(commit.HasValue());
            CHECK(commit.Value().cancelled == 1);
            CHECK(probe->committed == 0);
            CHECK(probe->cancelled == 1);
            auto failing = h.Register(1);
            failing->fail = true;
            const auto report = h.scheduler->EvaluateAtDecision(2);
            REQUIRE(report.HasValue());
            REQUIRE(report.Value().firstFailure);
            CHECK(report.Value().firstFailure->code.Value() == AIErrors::CapabilityUnavailable.code.Value());
            REQUIRE(h.scheduler->CommitAtIntentDispatch(2).HasValue());
            CHECK(failing->committed == 0);
            h.scheduler->Shutdown();
            h.scheduler->Shutdown();
            ExpectError(h.scheduler->EvaluateAtDecision(3), AIErrors::SchedulerPhaseInvalid);
            ExpectError(h.scheduler->Wake(h.Handle(1), {.task = true}), AIErrors::TaskContextInvalid);
        }

        TEST_CASE("AI cancellation published after ordering prevents a later peer from evaluating", "[unit][ai][scheduler]") {
            Harness h;
            CancellationSource cancellation;
            auto first = h.Register(0, {.priority = AiTaskPriority::Urgent});
            auto peer = h.Register(1, {}, cancellation.Token());
            first->cancelPeer = &cancellation;
            const auto report = h.scheduler->EvaluateAtDecision(1);
            REQUIRE(report.HasValue());
            CHECK(report.Value().evaluated == 1);
            CHECK(report.Value().cancelled == 1);
            CHECK(peer->evaluations == 0);
            CHECK(peer->cancelled == 1);
            REQUIRE(h.scheduler->CommitAtIntentDispatch(1).HasValue());
            CHECK(peer->committed == 0);
        }

        TEST_CASE("AI callback cancellation retains charged work and fences pending commit", "[unit][ai][scheduler]") {
            for (const bool duringCommit : {false, true}) {
                Harness h;
                CancellationSource cancellation;
                auto probe = h.Register(0, {}, cancellation.Token());
                if (duringCommit)
                    probe->cancelAtCommit = &cancellation;
                else
                    probe->cancelPeer = &cancellation;
                auto evaluated = h.scheduler->EvaluateAtDecision(1);
                REQUIRE(evaluated.HasValue());
                CHECK(evaluated.Value().workUnits == 64);
                CHECK(evaluated.Value().evaluated == 1);
                auto commit = h.scheduler->CommitAtIntentDispatch(1);
                REQUIRE(commit.HasValue());
                CHECK(commit.Value().cancelled == 1);
                CHECK(commit.Value().commands == (duringCommit ? 16 : 0));
                CHECK(probe->cancelled == 1);
                CHECK(probe->committed == 0);
            }
        }

        TEST_CASE("AI callback shutdown is deferred until callback return without committing intents", "[unit][ai][scheduler]") {
            Harness h;
            auto probe = h.Register(0);
            probe->stop = true;
            REQUIRE(h.scheduler->EvaluateAtDecision(1).HasValue());
            CHECK(probe->cancelled == 1);
            CHECK(probe->committed == 0);
            ExpectError(h.scheduler->CommitAtIntentDispatch(1), AIErrors::SchedulerPhaseInvalid);
        }

        struct Worker final : IAiBudgetedTaskJob {
            std::atomic<std::size_t> *steps{};

            explicit Worker(std::atomic<std::size_t> &count) : IAiBudgetedTaskJob(2), steps(&count) {}

            Result<void> RunSlice(const AiTaskOperationContext &, const CancellationToken &cancellation,
                                  AiWorkBudget &budget) const override {
                for (std::size_t i = 0; i < 3; ++i) {
                    if (cancellation.IsCancellationRequested())
                        return JobCancelled();
                    if (!budget.Consume())
                        break;
                    ++*steps;
                }
                return Result<void>::Success();
            }
        };

        TEST_CASE("AI workers consume the declared owner allowance and retain canonical task failure", "[unit][ai][scheduler][jobs]") {
            using namespace BehaviorTreeAsyncTestSupport;
            for (const auto mode : {AiSchedulingMode::Deterministic, AiSchedulingMode::BestEffort}) {
                Harness h({.mode = mode});
                StartedTask task;
                auto probe = std::make_shared<Probe>();
                const auto continuation = task.async->continuations[1];
                auto scheduler = AiTaskScheduler::Create(h.jobs, continuation.Operation().agent.incarnation, {.mode = mode});
                REQUIRE(scheduler.HasValue());
                auto owned = std::move(scheduler).Value();
                probe->scheduler = owned.get();
                probe->continuation = continuation;
                std::atomic<std::size_t> steps{};
                probe->worker.emplace(h.image, std::make_shared<Worker>(steps));
                probe->attempts = 2;
                REQUIRE(owned->Register(continuation.Operation().agent, MakeIdentity<AgentId>(1), {}, h.image, probe).HasValue());
                auto report = owned->EvaluateAtDecision(1);
                REQUIRE(report.HasValue());
                REQUIRE(probe->rejection);
                CHECK(probe->rejection->code.Value() == (mode == AiSchedulingMode::Deterministic
                                                             ? AIErrors::SchedulerWorkerUnsupported.code.Value()
                                                             : AIErrors::TaskCapacityExceeded.code.Value()));
                h.jobs.Shutdown(ShutdownPolicy::Drain);
                REQUIRE(owned->CommitAtIntentDispatch(1).HasValue());
                REQUIRE(owned->EvaluateAtDecision(2).HasValue());
                if (mode == AiSchedulingMode::BestEffort) {
                    CHECK(steps == 2);
                    CHECK(task.fixture.Evaluate(*task.instance, 2) == AiTaskState::Failed);
                    auto terminal = task.instance->TaskResult(BehaviorTreeTestSupport::Id(1));
                    REQUIRE(terminal.HasValue());
                    REQUIRE(terminal.Value());
                    CHECK(terminal.Value()->failure->cause.code.Value() == AIErrors::SchedulerBudgetExhausted.code.Value());
                } else {
                    CHECK(steps == 0);
                    CHECK(report.Value().workerSubmissions == 0);
                }
            }
        }

        struct WorkerSignals final {
            std::binary_semaphore started{0};
            std::binary_semaphore release{0};
            std::atomic<bool> cancelled{};
        };

        struct CancellableWorker final : IAiBudgetedTaskJob {
            std::shared_ptr<WorkerSignals> signals;

            explicit CancellableWorker(std::shared_ptr<WorkerSignals> state) : IAiBudgetedTaskJob(1), signals(std::move(state)) {}

            Result<void> RunSlice(const AiTaskOperationContext &, const CancellationToken &cancellation,
                                  AiWorkBudget &budget) const override {
                if (!budget.Consume())
                    return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
                signals->started.release();
                signals->release.acquire();  // Test-only worker gate; owner never waits for worker completion.
                signals->cancelled = cancellation.IsCancellationRequested();
                return cancellation.IsCancellationRequested() ? JobCancelled() : Result<void>::Success();
            }
        };

        TEST_CASE("AI scheduler shutdown preserves running worker pins and cancels through Foundation", "[unit][ai][scheduler][jobs]") {
            using namespace BehaviorTreeAsyncTestSupport;
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
            StartedTask task;
            const auto continuation = task.async->continuations[1];
            auto created =
                AiTaskScheduler::Create(jobs, continuation.Operation().agent.incarnation, {.mode = AiSchedulingMode::BestEffort});
            REQUIRE(created.HasValue());
            auto scheduler = std::move(created).Value();
            auto probe = std::make_shared<Probe>();
            auto signals = std::make_shared<WorkerSignals>();
            auto image = std::make_shared<int>(7);
            std::weak_ptr<const void> pin = image;
            probe->scheduler = scheduler.get();
            probe->continuation = continuation;
            probe->worker.emplace(image, std::make_shared<CancellableWorker>(signals));
            REQUIRE(scheduler->Register(continuation.Operation().agent, MakeIdentity<AgentId>(1), {}, image, probe).HasValue());
            REQUIRE(scheduler->EvaluateAtDecision(1).HasValue());
            const bool started = signals->started.try_acquire_for(std::chrono::seconds(5));
            CHECK(started);
            scheduler->Shutdown();
            scheduler.reset();
            probe.reset();
            image.reset();
            if (started)
                CHECK_FALSE(pin.expired());
            signals->release.release();
            jobs.Shutdown(ShutdownPolicy::Drain);
            if (started)
                CHECK(signals->cancelled);
            CHECK(pin.expired());
        }

        TEST_CASE("Scene-owned AI scheduler retires decisions on disable, replacement, restore and shutdown",
                  "[unit][ai][scheduler][scene]") {
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
            TestSupport::RestoreHarness scene;
            REQUIRE(scene.runtime.ConfigureTaskSchedulerAtSafePoint(jobs).HasValue());
            auto image = std::make_shared<int>(1);
            auto probe = std::make_shared<Probe>();
            REQUIRE(scene.runtime.RegisterDecisionAtSafePoint(scene.handle, {}, image, probe).HasValue());
            auto borrowed = scene.runtime.TaskSchedulerAtSafePoint();
            REQUIRE(borrowed.HasValue());
            REQUIRE(borrowed.Value()->EvaluateAtDecision(1).HasValue());
            auto state = scene.runtime.CaptureCanonicalState(scene.scene->View());
            REQUIRE(state.HasValue());
            REQUIRE(scene.runtime.CommitRestoreAtSafePoint(scene.Stage(state.Value())).HasValue());
            CHECK(probe->cancelled == 1);
            ExpectError(scene.runtime.TaskSchedulerAtSafePoint(), AIErrors::RuntimeUnavailable);
            REQUIRE(scene.runtime.ConfigureTaskSchedulerAtSafePoint(jobs).HasValue());
            REQUIRE(scene.runtime.RegisterDecisionAtSafePoint(scene.handle, {}, image, probe).HasValue());
            REQUIRE(scene.runtime.DisableAtSafePoint(scene.handle).HasValue());
            CHECK(probe->cancelled == 2);
            ExpectError(scene.runtime.RegisterDecisionAtSafePoint(scene.handle, {}, image, probe), AIErrors::HandleInvalid);
            scene.runtime.BeginShutdown();
            ExpectError(scene.runtime.TaskSchedulerAtSafePoint(), AIErrors::RuntimeUnavailable);
        }
    }  // namespace
}  // namespace Horo::AI
