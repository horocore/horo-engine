#include "AllocationProbe.h"
#include "BehaviorTreeAsyncTestSupport.h"
#include "Horo/Foundation/ModuleHost.h"

namespace Horo::AI {
    namespace {
        using namespace BehaviorTreeAsyncTestSupport;
        using BehaviorTreeTestSupport::ExpectError;
        using BehaviorTreeTestSupport::Id;
        using BehaviorTreeTestSupport::Node;
        using enum AiTaskState;
        using enum BehaviorTreeOperation;
        using enum AiTaskPublicationDisposition;

        TEST_CASE("Async terminal advances the real sequence once at the owner phase", "[unit][ai][bt_async]") {
            Fixture fixture(TaskSequence());
            fixture.probe->outcome[2] = Running;
            auto [async, instance] = StartedInstance{fixture};
            auto continuation = async->continuations[2];
            CHECK(Publish(continuation) == Accepted);
            CHECK(Publish(continuation, Failed) == AlreadyPublished);
            CHECK(instance->RootStatus() == Running);
            CHECK(fixture.probe->starts[3] == 0);
            CHECK(fixture.Evaluate(*instance, 2) == Succeeded);
            CHECK(fixture.Evaluate(*instance, 3) == Succeeded);
            CHECK(fixture.probe->resumes[2] == 0);
            CHECK(fixture.probe->starts[3] == 1);
            CHECK(fixture.probe->cleanups[2] == 1);
            REQUIRE(instance->AbortSubtree(Id(2)).HasValue());
            CHECK(fixture.probe->aborts[2] == 0);
            CHECK(instance->TaskResult(Id(2)).Value()->state == Succeeded);
            CHECK(Publish(continuation) == Stale);
            instance.reset();
            CHECK(continuation.NotifyEvent() == Stale);
        }

        TEST_CASE("Parallel out-of-order candidates retain canonical failure and deterministic flow", "[unit][ai][bt_async]") {
            Fixture fixture(TaskParallel());
            fixture.probe->outcome.fill(Running);
            auto [async, instance] = StartedInstance{fixture};
            CHECK(Publish(async->continuations[3], Failed) == Accepted);
            CHECK(fixture.Evaluate(*instance, 2) == Running);
            CHECK(Publish(async->continuations[2]) == Accepted);
            CHECK(fixture.Evaluate(*instance, 3) == Failed);
            const auto terminal = instance->TaskResult(Id(3));
            REQUIRE(terminal.Value()->failure);
            CHECK(terminal.Value()->failure->cause.code.Value() == AIErrors::RuntimeUnavailable.code.Value());
            CHECK(fixture.probe->cleanups[2] == 1);
            CHECK(fixture.probe->cleanups[3] == 1);
        }

        TEST_CASE("Events coalesce in bounded storage and resume only during decision evaluation", "[unit][ai][bt_async]") {
            auto [fixture, async, instance] = StartedTask{};
            const auto before = Tests::AllocationProbe::Count();
            for (std::size_t count = 0; count < 10'000; ++count)
                static_cast<void>(async->continuations[1].NotifyEvent());
            const auto after = Tests::AllocationProbe::Count();
            CHECK(before == after);
            CHECK(fixture.probe->resumes[1] == 0);
            CHECK(fixture.Evaluate(*instance, 1) == Running);
            CHECK(async->reasons[1] == AiTaskResumeReason::Event);
            CHECK(fixture.probe->resumes[1] == 1);
            CHECK(fixture.Evaluate(*instance, 2) == Running);
            CHECK(async->reasons[1] == AiTaskResumeReason::FixedTick);
        }

        TEST_CASE("Abort queued completion restart and loop reentry fence every old generation", "[unit][ai][bt_async]") {
            auto loop = Node(1, Loop, {2});
            loop.iterations = 2;
            Fixture fixture({loop, Node(2, Task)});
            fixture.probe->outcome[2] = Running;
            auto [async, instance] = StartedInstance{fixture};
            auto first = async->continuations[2];
            CHECK(Publish(first) == Accepted);
            REQUIRE(instance->AbortSubtree(Id(2)).HasValue());
            CHECK(fixture.Evaluate(*instance, 2) == Cancelled);
            CHECK(Publish(first) == Stale);
            REQUIRE(instance->Restart().HasValue());
            REQUIRE(fixture.Evaluate(*instance, 3) == Running);
            const auto second = async->continuations[2];
            CHECK(second.Operation().task != first.Operation().task);
            CHECK(Publish(second) == Accepted);
            CHECK(fixture.Evaluate(*instance, 4) == Running);
            CHECK(fixture.Evaluate(*instance, 5) == Running);
            CHECK(Publish(second) == Stale);
            CHECK(Publish(async->continuations[2]) == Accepted);
            CHECK(fixture.Evaluate(*instance, 6) == Succeeded);
            CHECK(fixture.probe->starts[2] == 3);
            CHECK(fixture.probe->cleanups[2] == 3);
        }

        TEST_CASE("Compatible reload retains continuation incompatible reload and timeout revoke it", "[unit][ai][bt_async]") {
            auto timer = Node(1, TimeLimit, {2});
            timer.durationTicks = 2;
            Fixture fixture({timer, Node(2, Task)});
            fixture.probe->outcome[2] = Running;
            auto [async, instance] = StartedInstance{fixture};
            auto old = async->continuations[2];
            REQUIRE(instance->Replace(fixture.Plan()).Value());
            CHECK(old.IsCurrent());
            fixture.nodes[0].durationTicks = 3;
            REQUIRE_FALSE(instance->Replace(fixture.Plan()).Value());
            CHECK(Publish(old) == Stale);
            CHECK(fixture.probe->terminals[2]->cancellationReason == AiTaskCancellationReason::PlanReplaced);
            REQUIRE(fixture.Evaluate(*instance, 2) == Running);
            old = async->continuations[2];
            CHECK(Publish(old) == Accepted);
            CHECK(fixture.Evaluate(*instance, 5) == Failed);
            CHECK(instance->TaskResult(Id(2)).Value()->cancellationReason == AiTaskCancellationReason::TimedOut);
            CHECK(Publish(old) == Stale);
        }

        TEST_CASE("Reactive priority replacement cannot consume the superseded child's queued outcome", "[unit][ai][bt_async]") {
            auto check = Node(2, BlackboardCheck, {4});
            check.abort = BehaviorTreeAbortMode::Both;
            Fixture fixture({Node(1, Selector, {2, 3}), check, Node(3, Task), Node(4, Task)});
            fixture.SetCondition(false);
            fixture.probe->outcome[3] = Running;
            fixture.probe->outcome[4] = Running;
            auto [async, instance] = StartedInstance{fixture};
            const auto old = async->continuations[3];
            CHECK(Publish(old) == Accepted);
            fixture.SetCondition(true);
            CHECK(fixture.Evaluate(*instance, 2) == Running);
            CHECK(Publish(old) == Stale);
            CHECK(fixture.probe->terminals[3]->cancellationReason == AiTaskCancellationReason::Superseded);
            CHECK(fixture.probe->starts[4] == 1);
        }

        TEST_CASE("Scene owner retirement and shutdown discard candidates before callbacks", "[unit][ai][bt_async]") {
            for (const int mode : {0, 1, 2}) {
                Fixture fixture({Node(1, Task)});
                fixture.probe->outcome[1] = Running;
                auto async = std::make_shared<AsyncProbe>();
                CancellationSource scene;
                auto instance = Instance(fixture, async, scene.Token());
                REQUIRE(fixture.Evaluate(*instance, 1) == Running);
                auto old = async->continuations[1];
                CHECK(Publish(old) == Accepted);
                if (mode == 0) {
                    scene.RequestCancellation();
                    CHECK(fixture.Evaluate(*instance, 2) == Cancelled);
                } else if (mode == 1) {
                    auto replacement = fixture.agent;
                    ++replacement.slot.generation;
                    auto snapshot = fixture.blackboard->Snapshot();
                    CHECK(instance->Evaluate(2, snapshot.Value(), replacement).Value() == Cancelled);
                } else {
                    instance->Shutdown();
                }
                CHECK(Publish(old) == Stale);
                CHECK(fixture.probe->resumes[1] == 0);
                CHECK(fixture.probe->cleanups[1] == 1);
                instance.reset();
                CHECK(old.NotifyEvent() == Stale);
            }
        }

        TEST_CASE("Real Foundation jobs complete through the tree and preserve original errors", "[unit][ai][bt_async][jobs]") {
            for (const bool fail : {false, true}) {
                JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
                Fixture fixture(TaskSequence());
                fixture.probe->outcome[2] = Running;
                auto async = std::make_shared<AsyncProbe>();
                CreateService(*async, jobs, 1);
                auto signals = std::make_shared<WorkSignals>();
                signals->fail = fail;
                async->work[2] = {std::make_shared<int>(1), std::make_shared<Work>(signals)};
                auto instance = Instance(fixture, async);
                REQUIRE(fixture.Evaluate(*instance, 1) == Running);
                REQUIRE(signals->started.try_acquire_for(std::chrono::seconds(5)));
                signals->release.release();
                jobs.Shutdown(ShutdownPolicy::Drain);
                CHECK(instance->RootStatus() == Running);
                CHECK(fixture.Evaluate(*instance, 2) == (fail ? Failed : Succeeded));
                CHECK(async->jobs->PendingCount() == 0);
                CHECK(fixture.probe->starts[3] == (fail ? 0 : 1));
                if (fail)
                    CHECK(instance->TaskResult(Id(2)).Value()->failure->cause.code.Value() == AIErrors::RuntimeUnavailable.code.Value());
            }
        }

        TEST_CASE("Task service bounds running work and unload keeps worker and image leases alive", "[unit][ai][bt_async][jobs]") {
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
            Fixture fixture(TaskParallel());
            fixture.probe->outcome.fill(Running);
            auto async = std::make_shared<AsyncProbe>();
            CreateService(*async, jobs, 1);
            auto signals = std::make_shared<WorkSignals>();
            auto image = std::make_shared<int>(7);
            std::weak_ptr<const void> imageObserver = image;
            async->work[2] = {image, std::make_shared<Work>(signals)};
            async->work[3] = async->work[2];
            image.reset();
            auto instance = Instance(fixture, async);
            REQUIRE(fixture.Evaluate(*instance, 1) == Running);
            REQUIRE(signals->started.try_acquire_for(std::chrono::seconds(5)));
            CHECK(instance->TaskResult(Id(3)).Value()->failure->cause.code.Value() == AIErrors::TaskCapacityExceeded.code.Value());
            CHECK(async->jobs->PendingCount() == 1);
            async->work = {};
            CHECK_FALSE(imageObserver.expired());
            instance->Shutdown();
            CHECK(fixture.probe->cleanups[2] == 1);
            async->jobs.reset();
            instance.reset();
            jobs.Shutdown(ShutdownPolicy::Drain);
            CHECK(imageObserver.expired());
            CHECK(Publish(async->continuations[2]) == Stale);
        }

        TEST_CASE("Concurrent detached completions admit one candidate and survive owner destruction", "[unit][ai][bt_async][jobs]") {
            JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 4});
            auto [fixture, async, instance] = StartedTask{};
            const auto continuation = async->continuations[1];
            std::atomic<std::size_t> accepted{};
            std::atomic<std::size_t> duplicates{};
            std::array<JobHandle, 4> handles;
            for (auto &handle : handles) {
                auto submitted = jobs.SubmitResult({}, [continuation, &accepted, &duplicates](const CancellationToken &) {
                    AiTaskTerminalResult candidate{.state = Succeeded};
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (std::chrono::steady_clock::now() < deadline) {
                        const auto result = continuation.PublishTerminal(candidate);
                        if (result.HasError())
                            return Result<void>::Failure(result.ErrorValue());
                        if (result.Value() == Accepted) {
                            ++accepted;
                            return Result<void>::Success();
                        }
                        if (result.Value() == AlreadyPublished) {
                            ++duplicates;
                            return Result<void>::Success();
                        }
                        std::this_thread::yield();
                    }
                    return Result<void>::Failure(MakeError(AIErrors::TaskFailureInvalid));
                });
                REQUIRE(submitted.HasValue());
                handle = std::move(submitted).Value();
            }
            jobs.Shutdown(ShutdownPolicy::Drain);
            CHECK(accepted.load() == 1);
            CHECK(duplicates.load() == 3);
            for (const auto &handle : handles)
                CHECK(handle.Snapshot()->state == JobState::Succeeded);
            CHECK(fixture.Evaluate(*instance, 2) == Succeeded);
            CHECK(fixture.probe->cleanups[1] == 1);
            instance.reset();
            CHECK(Publish(continuation) == Stale);
        }

        TEST_CASE("Copied and replaced task leases retain code through the last work destructor", "[unit][ai][bt_async]") {
            auto image = std::make_shared<int>(1);
            std::weak_ptr<const void> observer = image;
            auto releasedSafely = std::make_shared<std::atomic<bool>>(false);
            AiTaskJobLease first{image, std::make_shared<LeaseBoundWork>(image, releasedSafely)};
            image.reset();
            CHECK(first.IsValid());
            auto copy = first;
            first = {};
            CHECK_FALSE(observer.expired());
            auto transferred = std::move(copy);
            CHECK(transferred.IsValid());
            transferred = {};
            CHECK(observer.expired());
            CHECK(releasedSafely->load());
            CHECK_FALSE(AiTaskJobLease{}.IsValid());
        }

        TEST_CASE("Job admission preserves scheduler errors and validates exact live task identity", "[unit][ai][bt_async][jobs]") {
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
            auto [fixture, async, instance] = StartedTask{};
            CreateService(*async, jobs, 1);
            const auto continuation = async->continuations[1];
            auto signals = std::make_shared<WorkSignals>();
            const AiTaskJobLease lease{std::make_shared<int>(1), std::make_shared<Work>(signals)};
            ExpectError(async->jobs->Start({}, lease), AIErrors::TaskContextInvalid);
            ExpectError(async->jobs->Start(continuation, {}), AIErrors::TaskContextInvalid);
            ExpectError(async->jobs->Cancel({}, AiTaskCancellationReason::Requested), AIErrors::TaskContextInvalid);
            ExpectError(async->jobs->Cancel(continuation.Operation().task, AiTaskCancellationReason::Count), AIErrors::TaskContextInvalid);
            REQUIRE(async->jobs->Start(continuation, lease).HasValue());
            ExpectError(async->jobs->Start(continuation, lease), AIErrors::TaskTransitionInvalid);
            REQUIRE(signals->started.try_acquire_for(std::chrono::seconds(5)));
            REQUIRE(async->jobs->Cancel(continuation.Operation().task, AiTaskCancellationReason::Requested).HasValue());
            jobs.Shutdown(ShutdownPolicy::Drain);
            PumpUntilDrained(*async->jobs);
            CHECK(fixture.Evaluate(*instance, 2) == Cancelled);
            REQUIRE(async->jobs->Cancel(continuation.Operation().task, AiTaskCancellationReason::Requested).HasValue());
            REQUIRE(instance->Restart().HasValue());
            REQUIRE(fixture.Evaluate(*instance, 3) == Running);
            const auto rejected = async->jobs->Start(async->continuations[1], lease);
            REQUIRE(rejected.HasError());
            CHECK(async->jobs->PendingCount() == 0);
            async->jobs->Shutdown();
            async->jobs->Shutdown();
            ExpectError(async->jobs->Start(async->continuations[1], lease), AIErrors::TaskContextInvalid);
        }

        TEST_CASE("Module deactivation drains real task callbacks before releasing their dependencies", "[unit][ai][bt_async][jobs]") {
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
            auto activation = std::make_unique<ModuleActivationContext>(ModuleId{"horo.ai.async.test"}, nullptr);
            auto admitted = activation->AcquireCallbackLease();
            REQUIRE(admitted);
            auto image = std::make_shared<ModuleCallbackLease>(std::move(*admitted));
            std::weak_ptr<ModuleCallbackLease> imageObserver = image;
            Fixture fixture({Node(1, Task)});
            fixture.probe->outcome[1] = Running;
            auto async = std::make_shared<AsyncProbe>();
            CreateService(*async, jobs, 1);
            auto signals = std::make_shared<WorkSignals>();
            async->work[1] = {image, std::make_shared<Work>(signals)};
            auto instance = Instance(fixture, async, activation->Cancellation());
            REQUIRE(fixture.Evaluate(*instance, 1) == Running);
            REQUIRE(signals->started.try_acquire_for(std::chrono::seconds(5)));
            async->work = {};
            image.reset();
            activation.reset();
            jobs.Shutdown(ShutdownPolicy::Drain);
            CHECK(imageObserver.expired());
            CHECK(fixture.Evaluate(*instance, 2) == Cancelled);
            CHECK(fixture.probe->cleanups[1] == 1);
            CHECK(Publish(async->continuations[1]) == Stale);
            PumpUntilDrained(*async->jobs);
        }

        TEST_CASE("Malformed terminal candidates preserve storage and cannot advance flow", "[unit][ai][bt_async]") {
            auto [fixture, async, instance] = StartedTask{};
            for (const auto state : {Idle, Running, Failed, Cancelled}) {
                AiTaskTerminalResult invalid{.state = state};
                ExpectError(async->continuations[1].PublishTerminal(invalid), AIErrors::TaskFailureInvalid);
            }
            CHECK(fixture.Evaluate(*instance, 2) == Running);
            CHECK_FALSE(instance->TaskResult(Id(1)).Value());
            ExpectError(instance->TaskResult(Id(99)), AIErrors::BehaviorTreeTopologyInvalid);
            AiTaskContinuation empty;
            CHECK(Publish(empty) == Stale);
            CHECK(empty.NotifyEvent() == Stale);
            JobSystem jobs;
            ExpectError(AiTaskJobService::Create(jobs, 0), AIErrors::TaskCapacityExceeded);
            ExpectError(AiTaskJobService::Create(jobs, AiTaskJobService::HardCapacity + 1), AIErrors::TaskCapacityExceeded);
        }
    }  // namespace
}  // namespace Horo::AI
