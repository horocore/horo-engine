#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/LogContext.h"

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
    const Horo::ErrorCodeDescriptor TestFailure{
        .domain = Horo::ErrorDomainId("test.job_system"),
        .code = Horo::ErrorCode("test.job_system.child_failed"),
        .defaultSeverity = Horo::ErrorSeverity::Error,
        .summary = "Test child failed.",
        .remediationHint = "Inspect the test.",
    };

    struct ReentrantCaptureDestructor {
        explicit ReentrantCaptureDestructor(std::function<void()> callback) : onDestroy(std::move(callback)) {}

        std::function<void()> onDestroy;

        ~ReentrantCaptureDestructor() {
            onDestroy();
        }
    };

    class ManualJobBlocker {
    public:
        void Block() {
            std::unique_lock lock(mutex_);
            started_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this] {
                return released_;
            });
        }

        void WaitUntilStarted() {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return started_;
            });
        }

        void Release() {
            {
                std::lock_guard lock(mutex_);
                released_ = true;
            }
            condition_.notify_all();
        }

    private:
        std::mutex mutex_;
        std::condition_variable condition_;
        bool started_{};
        bool released_{};
    };

    enum class ReentrantReleasePath : std::uint8_t {
        Completed,
        RequestCancel,
        ShutdownCancel,
    };

    void VerifyReentrantCaptureRelease(const ReentrantReleasePath path) {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        std::optional<Horo::JobHandle> handle;
        bool destructorReentered{};
        auto capture = std::make_shared<ReentrantCaptureDestructor>([&] {
            const bool completed = path == ReentrantReleasePath::Completed;
            const auto waited = handle->Wait();
            const Horo::JobState expectedState = completed ? Horo::JobState::Succeeded : Horo::JobState::Cancelled;
            destructorReentered = (completed ? waited.HasValue() : waited.HasError()) && jobs.Query(handle->Id()).state == expectedState &&
                                  jobs.RequestCancel(handle->Id()).HasValue();
        });
        auto submitted = jobs.Submit({}, [capture](const Horo::CancellationToken &) {
        });
        REQUIRE((submitted.HasValue()));
        handle.emplace(std::move(submitted).Value());
        capture.reset();

        if (path == ReentrantReleasePath::Completed) {
            REQUIRE((handle->Wait({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)})
                         .HasValue()));
        } else if (path == ReentrantReleasePath::ShutdownCancel) {
            jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
        } else {
            REQUIRE((jobs.RequestCancel(handle->Id()).HasValue()));
        }
        REQUIRE((destructorReentered));
        jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
    }

    [[nodiscard]] Horo::Result<void> WaitForPublishedWorkerJob(const std::atomic<const Horo::JobHandle *> &published) {
        const Horo::JobHandle *handle = nullptr;
        while ((handle = published.load(std::memory_order_acquire)) == nullptr)
            std::this_thread::yield();
        return handle->Wait({.waitPolicy = Horo::WaitPolicy::WorkerOnly, .timeout = Horo::Duration::FromMilliseconds(100)});
    }

    [[nodiscard]] bool IsCapacityDeadlock(const Horo::Result<void> &result) {
        return result.HasError() && result.ErrorValue().code.Value() == "job.wait_capacity_deadlock";
    }

    [[nodiscard]] Horo::Result<Horo::JobId> SpawnExecutionProbe(Horo::TaskGroup &group, std::atomic<bool> &executed) {
        return group.Spawn({}, [&executed](const Horo::CancellationToken &) {
            executed.store(true, std::memory_order_release);
            return Horo::Result<void>::Success();
        });
    }

    [[nodiscard]] Horo::ConfigurationSnapshot BuildConfiguration(const Horo::ConfigurationRevision revision) {
        Horo::ConfigurationSchema schema;
        REQUIRE(schema
                    .Register({.key = Horo::SettingKey("jobs.test"),
                               .type = Horo::SettingValueType::Integer,
                               .defaultValue = std::int64_t{7},
                               .scope = Horo::SettingScope::Engine,
                               .reloadPolicy = Horo::ReloadPolicy::NextOperation,
                               .sensitivity = Horo::SettingSensitivity::Public})
                    .HasValue());
        REQUIRE(schema.Seal().HasValue());
        auto resolved = Horo::ConfigurationResolver::Resolve(schema, {}, revision);
        REQUIRE(resolved.HasValue());
        return std::move(resolved).Value();
    }

    [[nodiscard]] bool HasContextField(const Horo::Log::LogContextSnapshot &context, const std::string &key, const std::string &value) {
        return std::ranges::any_of(context.Fields(), [&](const Horo::Log::MdcField &field) {
            return field.first == key && field.second == value;
        });
    }

    void RequireTaskGroupClosed(Horo::TaskGroup &group) {
        const auto afterJoin = group.Spawn({}, [](const Horo::CancellationToken &) {
            return Horo::Result<void>::Success();
        });
        REQUIRE((afterJoin.HasError()));
        REQUIRE((afterJoin.ErrorValue().code.Value() == "job.task_group_closed"));
    }

    TEST_CASE("Submitted Job Reaches Succeeded Terminal State", "[unit][foundation]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        std::atomic executed{false};
        const auto submitted = jobs.Submit(Horo::JobDescriptor{}, [&executed](const Horo::CancellationToken &) {
            executed.store(true);
        });
        REQUIRE((submitted.HasValue()));
        REQUIRE((submitted.Value().Wait().HasValue()));

        const Horo::JobSnapshot snapshot = jobs.Query(submitted.Value().Id());
        REQUIRE((executed.load()));
        REQUIRE((snapshot.state == Horo::JobState::Succeeded));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Context Job Captures Configuration Correlation Progress And Diagnostic Context", "[unit][foundation][jobs][context]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4, .maxRetainedTerminalJobs = 4}};
        const Horo::ConfigurationSnapshot captured = BuildConfiguration(17);
        std::atomic<Horo::ConfigurationRevision> observedRevision{};
        std::atomic<Horo::OperationId> observedOperation{};
        std::atomic<bool> observedDiagnostic{};
        std::string regressionError;
        {
            const Horo::Log::LogContext submitContext("request.id", "request-17");
            auto submitted = jobs.SubmitContext({.operationId = Horo::OperationId{42}, .configuration = captured},
                                                [&](const Horo::JobExecutionContext &context) {
                observedRevision.store(context.Configuration()->Revision());
                observedOperation.store(context.Operation().value_or(0));
                observedDiagnostic.store(HasContextField(Horo::Log::CaptureLogContext(), "request.id", "request-17"));
                REQUIRE(context.UpdateProgress(std::string(129, 'x'), 0.5F).ErrorValue().code.Value() == "job.progress_invalid");
                REQUIRE(context.UpdateProgress("load", std::numeric_limits<float>::quiet_NaN()).ErrorValue().code.Value() ==
                        "job.progress_invalid");
                REQUIRE(context.UpdateProgress("load", 0.75F).HasValue());
                REQUIRE(context.UpdateProgress("load").ErrorValue().code.Value() == "job.progress_regressed");
                const auto regressed = context.UpdateProgress("load", 0.5F);
                regressionError = regressed.HasError() ? regressed.ErrorValue().code.Value() : std::string{};
                REQUIRE(context.UpdateProgress("commit", 0.1F).HasValue());
                return Horo::Result<void>::Success();
            });
            REQUIRE(submitted.HasValue());
            REQUIRE(submitted.Value().Wait().HasValue());
            const auto snapshot = submitted.Value().Snapshot();
            REQUIRE(snapshot.has_value());
            REQUIRE(snapshot->progress.phase == "commit");
            REQUIRE(snapshot->progress.value == 0.1F);
            REQUIRE(snapshot->configurationRevision == 17);
            REQUIRE(snapshot->operationId == 42);
            REQUIRE(snapshot->timing.startedAt.has_value());
            REQUIRE(snapshot->timing.finishedAt.has_value());
            REQUIRE(snapshot->terminalResult->state == Horo::JobState::Succeeded);
        }
        REQUIRE(observedRevision.load() == 17);
        REQUIRE(observedOperation.load() == 42);
        REQUIRE(observedDiagnostic.load());
        REQUIRE(regressionError == "job.progress_regressed");

        std::atomic<bool> leaked{};
        auto next = jobs.SubmitContext({}, [&](const Horo::JobExecutionContext &) {
            leaked.store(HasContextField(Horo::Log::CaptureLogContext(), "request.id", "request-17"));
            return Horo::Result<void>::Success();
        });
        REQUIRE(next.HasValue());
        REQUIRE(next.Value().Wait().HasValue());
        REQUIRE_FALSE(leaked.load());
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Job Store Retains A Bounded Terminal Window Independently Of Handles", "[unit][foundation][jobs][store][lifetime]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 3, .maxRetainedTerminalJobs = 2}};
        std::vector<Horo::JobHandle> handles;
        for (int index = 0; index < 3; ++index) {
            auto submitted = jobs.Submit({}, [](const Horo::CancellationToken &) {
            });
            REQUIRE(submitted.HasValue());
            handles.push_back(std::move(submitted).Value());
        }
        for (auto &handle : handles)
            REQUIRE(handle.Wait({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)})
                        .HasValue());

        REQUIRE_FALSE(jobs.Find(handles.front().Id()).has_value());
        REQUIRE(handles.front().Snapshot()->state == Horo::JobState::Succeeded);
        const auto store = jobs.SnapshotIfChanged(0);
        REQUIRE(store.has_value());
        REQUIRE(store->terminalCapacity == 2);
        REQUIRE(store->droppedTerminalCount == 1);
        REQUIRE(store->jobs.size() == 2);
        REQUIRE_FALSE(jobs.SnapshotIfChanged(store->revision).has_value());
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Queued Job Keeps Submission Configuration After Caller Advances", "[unit][foundation][jobs][configuration]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1, .maxRetainedTerminalJobs = 1}};
        Horo::ConfigurationSnapshot active = BuildConfiguration(23);
        Horo::ConfigurationRevision observed{};
        auto submitted = jobs.SubmitContext({.configuration = active}, [&](const Horo::JobExecutionContext &context) {
            observed = context.Configuration()->Revision();
            return Horo::Result<void>::Success();
        });
        REQUIRE(submitted.HasValue());
        active = BuildConfiguration(24);
        REQUIRE(submitted.Value()
                    .Wait({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)})
                    .HasValue());
        REQUIRE(observed == 23);
        REQUIRE(submitted.Value().Snapshot()->configurationRevision == 23);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Terminal Result Remains Immutable Under Late Cancellation", "[unit][foundation][jobs][result]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1, .maxRetainedTerminalJobs = 1}};
        Horo::CancellationToken observedToken;
        auto submitted = jobs.Submit({}, [&observedToken](const Horo::CancellationToken &cancellation) {
            observedToken = cancellation;
        });
        REQUIRE(submitted.HasValue());
        REQUIRE(submitted.Value().Wait().HasValue());
        REQUIRE_FALSE(observedToken.IsCancellationRequested());
        REQUIRE(submitted.Value().RequestCancel().HasValue());
        REQUIRE_FALSE(observedToken.IsCancellationRequested());
        const auto snapshot = submitted.Value().Snapshot();
        REQUIRE(snapshot->state == Horo::JobState::Succeeded);
        REQUIRE(snapshot->terminalResult->state == Horo::JobState::Succeeded);
        REQUIRE_FALSE(snapshot->terminalResult->error.has_value());
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Dropping Caller Handle Does Not Cancel Or Delete Accepted Work", "[unit][foundation][jobs][lifetime]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2, .maxRetainedTerminalJobs = 2}};
        ManualJobBlocker gate;
        auto blocker = jobs.Submit({}, [&gate](const Horo::CancellationToken &) {
            gate.Block();
        });
        REQUIRE(blocker.HasValue());
        gate.WaitUntilStarted();

        std::atomic<bool> executed{};
        Horo::JobId droppedId{};
        {
            auto dropped = jobs.Submit({}, [&](const Horo::CancellationToken &) {
                executed.store(true);
            });
            REQUIRE(dropped.HasValue());
            droppedId = dropped.Value().Id();
        }
        gate.Release();
        REQUIRE(blocker.Value().Wait().HasValue());
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
        REQUIRE(executed.load());
        const auto snapshot = jobs.Find(droppedId);
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->state == Horo::JobState::Succeeded);
    }

    TEST_CASE("Handle Keeps Terminal Record Alive Beyond Scheduler Store Lifetime", "[unit][foundation][jobs][lifetime]") {
        std::optional<Horo::JobHandle> durable;
        {
            Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1, .maxRetainedTerminalJobs = 0}};
            auto submitted = jobs.Submit({}, [](const Horo::CancellationToken &) {
            });
            REQUIRE(submitted.HasValue());
            durable.emplace(std::move(submitted).Value());
            REQUIRE(durable->Wait().HasValue());
            REQUIRE_FALSE(jobs.Find(durable->Id()).has_value());
            jobs.Shutdown(Horo::ShutdownPolicy::Drain);
        }
        REQUIRE(durable->Snapshot()->state == Horo::JobState::Succeeded);
        REQUIRE(durable->RequestCancel().HasValue());
        REQUIRE(durable->Snapshot()->terminalResult->state == Horo::JobState::Succeeded);
    }

    TEST_CASE("Task Group Children Receive Typed Correlation Without Operation Visibility", "[unit][foundation][jobs][group]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1, .maxRetainedTerminalJobs = 1}};
        Horo::TaskGroup group(jobs);
        std::atomic<std::uint64_t> observedGroup{};
        auto child = group.SpawnContext({}, [&](const Horo::JobExecutionContext &context) {
            observedGroup.store(context.Group().value);
            REQUIRE_FALSE(context.Operation().has_value());
            return Horo::Result<void>::Success();
        });
        REQUIRE(child.HasValue());
        REQUIRE(group.Join().HasValue());
        REQUIRE(group.Outcome() == Horo::TaskGroupOutcome::Completed);
        REQUIRE(observedGroup.load() != 0);
        REQUIRE(group.Id().value == observedGroup.load());
        const auto snapshot = jobs.Find(child.Value());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->taskGroupId.value == observedGroup.load());
        REQUIRE_FALSE(snapshot->operationId.has_value());
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Bounded Queue Rejects Overflow", "[unit][foundation]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        REQUIRE((jobs.Submit(Horo::JobDescriptor{}, [](const Horo::CancellationToken &) {
        }).HasValue()));
        const auto rejected = jobs.Submit(Horo::JobDescriptor{}, [](const Horo::CancellationToken &) {
        });
        REQUIRE((rejected.HasError()));
        REQUIRE((rejected.ErrorValue().code.Value() == "job.queue_full"));
        jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
    }

    TEST_CASE("Queued Job Can Be Cancelled By Id", "[unit][foundation]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        const auto submitted = jobs.Submit(Horo::JobDescriptor{}, [](const Horo::CancellationToken &) {
        });
        REQUIRE((submitted.HasValue()));
        REQUIRE((jobs.RequestCancel(submitted.Value().Id()).HasValue()));
        REQUIRE((jobs.Query(submitted.Value().Id()).state == Horo::JobState::Cancelled));
        REQUIRE((submitted.Value().Wait().HasError()));
        jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
    }

    TEST_CASE("Result Returning Job Preserves Typed Failure", "[unit][foundation]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        const auto submitted = jobs.SubmitResult(Horo::JobDescriptor{}, [](const Horo::CancellationToken &) {
            return Horo::Result<void>::Failure(Horo::MakeError(TestFailure));
        });
        REQUIRE((submitted.HasValue()));
        const auto waited = submitted.Value().Wait();
        REQUIRE((waited.HasError()));
        REQUIRE((waited.ErrorValue().code.Value() == "test.job_system.child_failed"));
        REQUIRE((jobs.Query(submitted.Value().Id()).state == Horo::JobState::Failed));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Terminal Jobs Release Callback Ownership", "[unit][foundation][jobs][lifetime]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
        auto completedLifetime = std::make_shared<int>(1);
        const std::weak_ptr completedProbe = completedLifetime;
        auto completed = jobs.Submit({}, [lifetime = std::move(completedLifetime)](const Horo::CancellationToken &) {
            REQUIRE(lifetime != nullptr);
        });
        REQUIRE((completed.HasValue()));
        REQUIRE((completed.Value().Wait().HasValue()));
        REQUIRE((completedProbe.expired()));

        Horo::JobSystem queuedJobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        auto cancelledLifetime = std::make_shared<int>(2);
        const std::weak_ptr cancelledProbe = cancelledLifetime;
        auto cancelled = queuedJobs.Submit({}, [lifetime = std::move(cancelledLifetime)](const Horo::CancellationToken &) {
            REQUIRE(lifetime != nullptr);
        });
        REQUIRE((cancelled.HasValue()));
        REQUIRE((queuedJobs.RequestCancel(cancelled.Value().Id()).HasValue()));
        REQUIRE((cancelled.Value().Wait().HasError()));
        REQUIRE((cancelledProbe.expired()));

        queuedJobs.Shutdown(Horo::ShutdownPolicy::Cancel);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Completed Job Releases Reentrant Capture After Publishing Terminal State", "[unit][foundation][jobs][lifetime]") {
        VerifyReentrantCaptureRelease(ReentrantReleasePath::Completed);
    }

    TEST_CASE("Queued Termination Releases Captures Outside Job System Locks", "[unit][foundation][jobs][lifetime]") {
        VerifyReentrantCaptureRelease(ReentrantReleasePath::RequestCancel);
        VerifyReentrantCaptureRelease(ReentrantReleasePath::ShutdownCancel);
    }

    TEST_CASE("Pre-Cancelled Submission Releases Captures Outside Scheduler Locks", "[unit][foundation][jobs][cancel][lifetime]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        Horo::CancellationSource parent;
        parent.RequestCancellation();
        bool reentered = false;
        auto capture = std::make_shared<ReentrantCaptureDestructor>([&] {
            auto next = jobs.Submit({}, [](const Horo::CancellationToken &) {
            });
            reentered = next.HasValue();
            if (next.HasValue())
                static_cast<void>(next.Value().RequestCancel());
        });
        const std::weak_ptr captureProbe = capture;
        auto cancelled =
            jobs.SubmitContext({.parentCancellation = parent.Token()}, [capture = std::move(capture)](const Horo::JobExecutionContext &) {
            return Horo::Result<void>::Success();
        });
        REQUIRE(cancelled.HasValue());
        REQUIRE(captureProbe.expired());
        REQUIRE(reentered);
        REQUIRE(cancelled.Value().Snapshot()->state == Horo::JobState::Cancelled);
        jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
    }

    TEST_CASE("Bounded Wait Times Out Without Changing Running Job State", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1}};
        std::atomic started{false};
        std::atomic release{false};
        auto submitted = jobs.Submit({}, [&started, &release](const Horo::CancellationToken &) {
            started.store(true);
            while (!release.load())
                std::this_thread::yield();
        });
        REQUIRE((submitted.HasValue()));
        while (!started.load())
            std::this_thread::yield();

        const auto timedOut =
            submitted.Value().Wait({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(1)});
        REQUIRE((timedOut.HasError()));
        REQUIRE((timedOut.ErrorValue().code.Value() == "job.wait_timed_out"));
        REQUIRE((jobs.Query(submitted.Value().Id()).state == Horo::JobState::Running));

        release.store(true);
        REQUIRE((submitted.Value().Wait().HasValue()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Owner Thread Blocking Wait Does Not Execute Queued Work Inline", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        std::atomic executed{false};
        auto submitted = jobs.Submit({}, [&executed](const Horo::CancellationToken &) {
            executed.store(true);
        });
        REQUIRE((submitted.HasValue()));

        const auto timedOut = submitted.Value().Wait(
            {.waitPolicy = Horo::WaitPolicy::OwnerThreadBlockAllowed, .timeout = Horo::Duration::FromMilliseconds(1)});
        REQUIRE((timedOut.HasError()));
        REQUIRE((timedOut.ErrorValue().code.Value() == "job.wait_timed_out"));
        REQUIRE_FALSE((executed.load()));
        jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
    }

    TEST_CASE("Bounded Wait Validates Affinity Before Terminal Observation", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
        auto completed = jobs.Submit({}, [](const Horo::CancellationToken &) {
        });
        REQUIRE((completed.HasValue()));
        REQUIRE((completed.Value().Wait().HasValue()));

        const auto ownerForbidden = completed.Value().Wait(
            {.waitPolicy = Horo::WaitPolicy::ForbiddenOnOwnerThread, .timeout = Horo::Duration::FromMilliseconds(10)});
        REQUIRE((ownerForbidden.HasError()));
        REQUIRE((ownerForbidden.ErrorValue().code.Value() == "job.wait_forbidden"));
        const auto nonWorker =
            completed.Value().Wait({.waitPolicy = Horo::WaitPolicy::WorkerOnly, .timeout = Horo::Duration::FromMilliseconds(10)});
        REQUIRE((nonWorker.HasError()));
        REQUIRE((nonWorker.ErrorValue().code.Value() == "job.wait_forbidden"));
        const auto invalidPolicy =
            completed.Value().Wait({.waitPolicy = static_cast<Horo::WaitPolicy>(255), .timeout = Horo::Duration::FromMilliseconds(10)});
        REQUIRE((invalidPolicy.HasError()));
        REQUIRE((invalidPolicy.ErrorValue().code.Value() == "job.wait_forbidden"));
        REQUIRE((completed.Value()
                     .Wait({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(10)})
                     .HasValue()));

        Horo::JobSystem otherJobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1}};
        auto otherCompleted = otherJobs.Submit({}, [](const Horo::CancellationToken &) {
        });
        REQUIRE((otherCompleted.HasValue()));
        REQUIRE((otherCompleted.Value().Wait().HasValue()));

        std::atomic workerAccepted{false};
        std::atomic foreignWorkerRejected{false};
        auto verifier =
            jobs.Submit({}, [&completed, &otherCompleted, &workerAccepted, &foreignWorkerRejected](const Horo::CancellationToken &) {
            workerAccepted.store(completed.Value()
                                     .Wait({.waitPolicy = Horo::WaitPolicy::WorkerOnly, .timeout = Horo::Duration::FromMilliseconds(10)})
                                     .HasValue());
            const auto foreign =
                otherCompleted.Value().Wait({.waitPolicy = Horo::WaitPolicy::WorkerOnly, .timeout = Horo::Duration::FromMilliseconds(10)});
            foreignWorkerRejected.store(foreign.HasError() && foreign.ErrorValue().code.Value() == "job.wait_forbidden");
        });
        REQUIRE((verifier.HasValue()));
        REQUIRE((verifier.Value().Wait().HasValue()));
        REQUIRE((workerAccepted.load()));
        REQUIRE((foreignWorkerRejected.load()));
        otherJobs.Shutdown(Horo::ShutdownPolicy::Drain);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Bounded Owner Wait Helps Only Its Exact Queued Record", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 2}};
        std::atomic unrelatedExecuted{false};
        std::atomic awaitedExecuted{false};
        const auto unrelated = jobs.Submit({}, [&unrelatedExecuted](const Horo::CancellationToken &) {
            unrelatedExecuted.store(true);
        });
        auto awaited = jobs.Submit({}, [&awaitedExecuted](const Horo::CancellationToken &) {
            awaitedExecuted.store(true);
        });
        REQUIRE((unrelated.HasValue()));
        REQUIRE((awaited.HasValue()));

        const auto result =
            awaited.Value().Wait({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)});
        REQUIRE((result.HasValue()));
        REQUIRE((awaitedExecuted.load()));
        REQUIRE_FALSE((unrelatedExecuted.load()));
        REQUIRE((jobs.Query(unrelated.Value().Id()).state == Horo::JobState::Queued));

        auto replacement = jobs.Submit({}, [](const Horo::CancellationToken &) {
        });
        REQUIRE((replacement.HasValue()));

        REQUIRE((jobs.RequestCancel(unrelated.Value().Id()).HasValue()));
        REQUIRE((jobs.RequestCancel(replacement.Value().Id()).HasValue()));
        jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
    }

    TEST_CASE("Worker Wait Helps Its Exact Queued Dependency", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
        std::atomic<const Horo::JobHandle *> dependencyHandle{};
        std::atomic dependencyExecuted{false};
        std::atomic dependencyWaitSucceeded{false};

        auto parent = jobs.Submit({}, [&](const Horo::CancellationToken &) {
            dependencyWaitSucceeded.store(WaitForPublishedWorkerJob(dependencyHandle).HasValue());
        });
        REQUIRE((parent.HasValue()));
        auto dependency = jobs.Submit({}, [&dependencyExecuted](const Horo::CancellationToken &) {
            dependencyExecuted.store(true);
        });
        REQUIRE((dependency.HasValue()));
        dependencyHandle.store(&dependency.Value());

        REQUIRE((parent.Value().Wait().HasValue()));
        REQUIRE((dependencyWaitSucceeded.load()));
        REQUIRE((dependencyExecuted.load()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Worker Self Wait Returns Capacity Deadlock Without Blocking", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
        std::atomic<const Horo::JobHandle *> selfHandle{};
        std::atomic selfWaitRejected{false};
        auto submitted = jobs.Submit({}, [&](const Horo::CancellationToken &) {
            selfWaitRejected.store(IsCapacityDeadlock(WaitForPublishedWorkerJob(selfHandle)));
        });
        REQUIRE((submitted.HasValue()));
        selfHandle.store(&submitted.Value());

        REQUIRE((submitted.Value().Wait().HasValue()));
        REQUIRE((selfWaitRejected.load()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Worker Helping Rejects A Reentrant Wait Cycle", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 2}};
        std::atomic<const Horo::JobHandle *> parentHandle{};
        std::atomic<const Horo::JobHandle *> childHandle{};
        std::atomic cycleRejected{false};
        std::atomic childWaitSucceeded{false};

        auto parent = jobs.Submit({}, [&](const Horo::CancellationToken &) {
            childWaitSucceeded.store(WaitForPublishedWorkerJob(childHandle).HasValue());
        });
        REQUIRE((parent.HasValue()));
        parentHandle.store(&parent.Value());
        auto child = jobs.Submit({}, [&](const Horo::CancellationToken &) {
            cycleRejected.store(IsCapacityDeadlock(WaitForPublishedWorkerJob(parentHandle)));
        });
        REQUIRE((child.HasValue()));
        childHandle.store(&child.Value());

        REQUIRE((parent.Value().Wait().HasValue()));
        REQUIRE((childWaitSucceeded.load()));
        REQUIRE((cycleRejected.load()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Timed Out Task Group Join Remains Retryable", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 3}};
        ManualJobBlocker gate;
        auto blocker = jobs.Submit({}, [&gate](const Horo::CancellationToken &) {
            gate.Block();
        });
        REQUIRE((blocker.HasValue()));
        gate.WaitUntilStarted();

        Horo::TaskGroup group(jobs);
        std::atomic childExecuted{false};
        REQUIRE((SpawnExecutionProbe(group, childExecuted).HasValue()));

        std::string waitError;
        std::thread waiter([&] {
            const auto joined =
                group.Join({.waitPolicy = Horo::WaitPolicy::ForbiddenOnOwnerThread, .timeout = Horo::Duration::FromMilliseconds(10)});
            if (joined.HasError())
                waitError = joined.ErrorValue().code.Value();
        });
        waiter.join();
        REQUIRE((waitError == "job.wait_timed_out"));
        REQUIRE_FALSE((childExecuted.load()));
        REQUIRE_FALSE(group.Outcome().has_value());

        gate.Release();
        REQUIRE((blocker.Value().Wait().HasValue()));
        REQUIRE((group.Join().HasValue()));
        REQUIRE((group.Join().HasValue()));
        REQUIRE(group.Outcome() == Horo::TaskGroupOutcome::Completed);
        REQUIRE((childExecuted.load()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Forbidden Task Group Join Remains Closed And Retryable", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        Horo::TaskGroup group(jobs);
        std::atomic childExecuted{false};
        REQUIRE((SpawnExecutionProbe(group, childExecuted).HasValue()));

        const auto forbidden = group.Join({.waitPolicy = Horo::WaitPolicy::WorkerOnly, .timeout = Horo::Duration::FromMilliseconds(10)});
        REQUIRE((forbidden.HasError()));
        REQUIRE((forbidden.ErrorValue().code.Value() == "job.wait_forbidden"));
        RequireTaskGroupClosed(group);

        REQUIRE((group.Join({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)})
                     .HasValue()));
        REQUIRE((childExecuted.load()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Cyclic Task Group Join Returns Capacity Deadlock And Remains Retryable", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1}};
        Horo::TaskGroup group(jobs);
        std::atomic joinAttempted{false};
        std::atomic cycleRejected{false};
        REQUIRE((group
                     .Spawn({}, [&group, &joinAttempted, &cycleRejected](const Horo::CancellationToken &) {
            const auto cycle = group.Join({.waitPolicy = Horo::WaitPolicy::WorkerOnly, .timeout = Horo::Duration::FromMilliseconds(100)});
            cycleRejected.store(cycle.HasError() && cycle.ErrorValue().code.Value() == "job.wait_capacity_deadlock");
            joinAttempted.store(true);
            return Horo::Result<void>::Success();
        }).HasValue()));

        while (!joinAttempted.load(std::memory_order_acquire))
            std::this_thread::yield();
        REQUIRE((cycleRejected.load()));
        REQUIRE((group.Join().HasValue()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Bounded Task Group Join Preserves First Failure In Spawn Order", "[unit][foundation][jobs][wait]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 2}};
        Horo::TaskGroup group(jobs, Horo::TaskGroupFailurePolicy::CollectAll);
        Horo::Error firstError = Horo::MakeError(TestFailure);
        firstError.code = Horo::ErrorCode("test.job_system.bounded_first");
        Horo::Error secondError = Horo::MakeError(TestFailure);
        secondError.code = Horo::ErrorCode("test.job_system.bounded_second");
        REQUIRE((group
                     .Spawn({}, [firstError](const Horo::CancellationToken &) {
            return Horo::Result<void>::Failure(firstError);
        }).HasValue()));
        REQUIRE((group
                     .Spawn({}, [secondError](const Horo::CancellationToken &) {
            return Horo::Result<void>::Failure(secondError);
        }).HasValue()));

        const auto joined =
            group.Join({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)});
        REQUIRE((joined.HasError()));
        REQUIRE((joined.ErrorValue().code.Value() == "test.job_system.bounded_first"));
        REQUIRE((group.Join().ErrorValue().code.Value() == "test.job_system.bounded_first"));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Task Group Destructor Cancels And Drains After Bounded Timeout", "[unit][foundation][jobs][wait][lifetime]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1}};
        std::atomic started{false};
        std::atomic stopped{false};
        auto group = std::make_unique<Horo::TaskGroup>(jobs);
        const Horo::JobFunction cancellationProbe = [&](const Horo::CancellationToken &cancellation) {
            started.store(true, std::memory_order_release);
            for (; !cancellation.IsCancellationRequested(); std::this_thread::yield()) {
            }
            stopped.store(true, std::memory_order_release);
            return Horo::Result<void>::Success();
        };
        REQUIRE((group->Spawn({}, cancellationProbe).HasValue()));
        while (!started.load(std::memory_order_acquire))
            std::this_thread::yield();
        const auto timedOut =
            group->Join({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(1)});
        REQUIRE((timedOut.HasError()));
        REQUIRE((timedOut.ErrorValue().code.Value() == "job.wait_timed_out"));
        group.reset();
        REQUIRE((stopped.load()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Task Group Collect All Returns Failures In Spawn Order", "[unit][foundation]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 8}};
        Horo::TaskGroup group(jobs, Horo::TaskGroupFailurePolicy::CollectAll);
        std::atomic completed{0};

        const auto first = group.Spawn({}, [&completed](const Horo::CancellationToken &) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            completed.fetch_add(1);
            Horo::Error error = Horo::MakeError(TestFailure);
            error.code = Horo::ErrorCode("test.job_system.first");
            return Horo::Result<void>::Failure(std::move(error));
        });
        const auto second = group.Spawn({}, [&completed](const Horo::CancellationToken &) {
            completed.fetch_add(1);
            Horo::Error error = Horo::MakeError(TestFailure);
            error.code = Horo::ErrorCode("test.job_system.second");
            return Horo::Result<void>::Failure(std::move(error));
        });
        REQUIRE((first.HasValue()));
        REQUIRE((second.HasValue()));

        const auto joined = group.Join();
        REQUIRE((joined.HasError()));
        REQUIRE((joined.ErrorValue().code.Value() == "test.job_system.first"));
        REQUIRE((completed.load() == 2));
        const auto joinedAgain = group.Join();
        REQUIRE((joinedAgain.HasError()));
        REQUIRE((joinedAgain.ErrorValue().code.Value() == "test.job_system.first"));
        RequireTaskGroupClosed(group);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Task Group Fail Fast Cancels Accepted Siblings", "[unit][foundation]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 8}};
        Horo::TaskGroup group(jobs, Horo::TaskGroupFailurePolicy::FailFast);
        std::mutex mutex;
        std::condition_variable started;
        bool siblingStarted = false;
        std::atomic siblingObservedCancellation{false};

        REQUIRE((group
                     .Spawn({}, [&](const Horo::CancellationToken &cancellation) {
            {
                std::lock_guard lock(mutex);
                siblingStarted = true;
            }
            started.notify_one();
            while (!cancellation.IsCancellationRequested())
                std::this_thread::yield();
            siblingObservedCancellation.store(true);
            return Horo::Result<void>::Failure(Horo::MakeError(TestFailure));
        }).HasValue()));
        REQUIRE((group
                     .Spawn({}, [&](const Horo::CancellationToken &) {
            std::unique_lock lock(mutex);
            started.wait(lock, [&] {
                return siblingStarted;
            });
            return Horo::Result<void>::Failure(Horo::MakeError(TestFailure));
        }).HasValue()));

        REQUIRE((group.Join().HasError()));
        REQUIRE((siblingObservedCancellation.load()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Rejected Task Group Child Is Not Joined", "[unit][foundation]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        Horo::TaskGroup group(jobs, Horo::TaskGroupFailurePolicy::CollectAll);
        REQUIRE((group
                     .Spawn({}, [](const Horo::CancellationToken &) {
            return Horo::Result<void>::Success();
        }).HasValue()));
        const auto rejected = group.Spawn({}, [](const Horo::CancellationToken &) {
            return Horo::Result<void>::Success();
        });
        REQUIRE((rejected.HasError()));
        REQUIRE((rejected.ErrorValue().code.Value() == "job.queue_full"));
        group.RequestCancel();
        REQUIRE((group.Join().HasError()));
        jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
    }

    TEST_CASE("Parent Cancellation Flows To Task Group Children", "[unit][foundation]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        Horo::CancellationSource parent;
        parent.RequestCancellation();
        Horo::TaskGroup group(jobs, Horo::TaskGroupFailurePolicy::FailFast, parent.Token());
        std::atomic executed{false};
        REQUIRE((group
                     .Spawn({}, [&executed](const Horo::CancellationToken &) {
            executed.store(true);
            return Horo::Result<void>::Success();
        }).HasValue()));
        REQUIRE((group.Join().HasError()));
        REQUIRE(group.Outcome() == Horo::TaskGroupOutcome::Cancelled);
        REQUIRE((!executed.load()));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Pre-Cancelled Parent Creates A Cancelled Record Without Queue Execution", "[unit][foundation][jobs][cancel]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        auto occupying = jobs.Submit({}, [](const Horo::CancellationToken &) {
        });
        REQUIRE(occupying.HasValue());
        Horo::CancellationSource parent;
        parent.RequestCancellation();
        bool executed = false;
        auto submitted = jobs.SubmitResult({.parentCancellation = parent.Token()}, [&](const Horo::CancellationToken &) {
            executed = true;
            return Horo::Result<void>::Success();
        });
        REQUIRE(submitted.HasValue());
        const auto waited = submitted.Value().Wait();
        REQUIRE(waited.HasError());
        REQUIRE(waited.ErrorValue().code.Value() == "job.cancelled");
        REQUIRE_FALSE(executed);
        REQUIRE(submitted.Value().Snapshot()->state == Horo::JobState::Cancelled);
        REQUIRE(occupying.Value().RequestCancel().HasValue());
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Parent Cancellation While Queued Wins Exact Record Claim", "[unit][foundation][jobs][cancel][race]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        Horo::CancellationSource parent;
        bool executed = false;
        auto submitted = jobs.SubmitResult({.parentCancellation = parent.Token()}, [&](const Horo::CancellationToken &) {
            executed = true;
            return Horo::Result<void>::Success();
        });
        REQUIRE(submitted.HasValue());
        parent.RequestCancellation();
        const auto waited = submitted.Value().Wait(
            {.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)});
        REQUIRE(waited.HasError());
        REQUIRE(waited.ErrorValue().code.Value() == "job.cancelled");
        REQUIRE_FALSE(executed);
        REQUIRE(submitted.Value().Snapshot()->state == Horo::JobState::Cancelled);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Running Cancellation Cannot Relabel Committed Success Or Failure", "[unit][foundation][jobs][cancel][race]") {
        for (const bool fails : {false, true}) {
            Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1}};
            ManualJobBlocker gate;
            std::atomic observedCancellation{false};
            auto submitted = jobs.SubmitResult({}, [&](const Horo::CancellationToken &cancellation) {
                gate.Block();
                observedCancellation.store(cancellation.IsCancellationRequested());
                return fails ? Horo::Result<void>::Failure(Horo::MakeError(TestFailure)) : Horo::Result<void>::Success();
            });
            REQUIRE(submitted.HasValue());
            gate.WaitUntilStarted();
            REQUIRE(submitted.Value().Snapshot()->state == Horo::JobState::Running);
            REQUIRE(submitted.Value().RequestCancel().HasValue());
            gate.Release();
            const auto waited = submitted.Value().Wait();
            const auto snapshot = submitted.Value().Snapshot();
            REQUIRE(observedCancellation.load());
            REQUIRE(snapshot->state == (fails ? Horo::JobState::Failed : Horo::JobState::Succeeded));
            REQUIRE(snapshot->terminalResult->state == snapshot->state);
            REQUIRE(waited.HasError() == fails);
            if (fails)
                REQUIRE(waited.ErrorValue().code.Value() == "test.job_system.child_failed");
            REQUIRE(submitted.Value().RequestCancel().HasValue());
            REQUIRE(submitted.Value().Snapshot()->state == snapshot->state);
            jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
        }
    }

    TEST_CASE("Explicit Cancellation Retains Its Cause And Never Becomes Failure", "[unit][foundation][jobs][cancel]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1}};
        auto submitted = jobs.SubmitResult({}, [](const Horo::CancellationToken &) {
            return Horo::JobCancelled(Horo::MakeError(TestFailure));
        });
        REQUIRE(submitted.HasValue());
        const auto waited = submitted.Value().Wait();
        REQUIRE(waited.HasError());
        REQUIRE(waited.ErrorValue().code.Value() == "job.cancelled");
        REQUIRE(Horo::IsJobCancelled(waited.ErrorValue()));
        REQUIRE(waited.ErrorValue().cause.Get() != nullptr);
        REQUIRE(waited.ErrorValue().cause.Get()->code.Value() == "test.job_system.child_failed");
        const auto snapshot = submitted.Value().Snapshot();
        REQUIRE(snapshot->state == Horo::JobState::Cancelled);
        REQUIRE(snapshot->terminalResult->state == Horo::JobState::Cancelled);
        REQUIRE(snapshot->error->code.Value() == "job.cancelled");
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Foreign Cancellation-Looking Failure Stays Failed", "[unit][foundation][jobs][cancel]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1}};
        auto submitted = jobs.SubmitResult({}, [](const Horo::CancellationToken &) {
            Horo::Error error = Horo::MakeError(TestFailure);
            error.code = Horo::ErrorCode{"job.cancelled"};
            return Horo::Result<void>::Failure(std::move(error));
        });
        REQUIRE(submitted.HasValue());
        const auto waited = submitted.Value().Wait();
        REQUIRE(waited.HasError());
        REQUIRE(waited.ErrorValue().domain.Value() == "test.job_system");
        REQUIRE_FALSE(Horo::IsJobCancelled(waited.ErrorValue()));
        REQUIRE(submitted.Value().Snapshot()->state == Horo::JobState::Failed);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Task Group Cancellation Does Not Trigger Fail Fast", "[unit][foundation][jobs][group][cancel]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 2}};
        Horo::TaskGroup group(jobs, Horo::TaskGroupFailurePolicy::FailFast);
        bool siblingExecuted = false;
        const auto cancelled = group.Spawn({}, [](const Horo::CancellationToken &) {
            return Horo::JobCancelled();
        });
        const auto sibling = group.Spawn({}, [&](const Horo::CancellationToken &cancellation) {
            siblingExecuted = !cancellation.IsCancellationRequested();
            return Horo::Result<void>::Success();
        });
        REQUIRE(cancelled.HasValue());
        REQUIRE(sibling.HasValue());
        const auto joined =
            group.Join({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)});
        REQUIRE(joined.HasError());
        REQUIRE(joined.ErrorValue().code.Value() == "job.cancelled");
        REQUIRE(group.Outcome() == Horo::TaskGroupOutcome::Cancelled);
        REQUIRE(siblingExecuted);
        REQUIRE(jobs.Find(sibling.Value())->state == Horo::JobState::Succeeded);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Task Group Failure Wins Over Earlier Cancellation And Cancels Queued Siblings", "[unit][foundation][jobs][group][cancel]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 3}};
        Horo::TaskGroup group(jobs, Horo::TaskGroupFailurePolicy::FailFast);
        bool siblingExecuted = false;
        const auto cancelled = group.Spawn({}, [](const Horo::CancellationToken &) {
            return Horo::JobCancelled();
        });
        const auto failed = group.Spawn({}, [](const Horo::CancellationToken &) {
            return Horo::Result<void>::Failure(Horo::MakeError(TestFailure));
        });
        const auto sibling = group.Spawn({}, [&](const Horo::CancellationToken &) {
            siblingExecuted = true;
            return Horo::Result<void>::Success();
        });
        REQUIRE(cancelled.HasValue());
        REQUIRE(failed.HasValue());
        REQUIRE(sibling.HasValue());
        const auto joined =
            group.Join({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)});
        REQUIRE(joined.HasError());
        REQUIRE(joined.ErrorValue().code.Value() == "test.job_system.child_failed");
        REQUIRE(group.Outcome() == Horo::TaskGroupOutcome::Failed);
        REQUIRE_FALSE(siblingExecuted);
        REQUIRE(jobs.Find(cancelled.Value())->state == Horo::JobState::Cancelled);
        REQUIRE(jobs.Find(failed.Value())->state == Horo::JobState::Failed);
        REQUIRE(jobs.Find(sibling.Value())->state == Horo::JobState::Cancelled);
        REQUIRE(group.Join().ErrorValue().code.Value() == "test.job_system.child_failed");
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Thrown Task Group Failure Cancels Queued Siblings", "[unit][foundation][jobs][group][cancel]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 2}};
        Horo::TaskGroup group(jobs, Horo::TaskGroupFailurePolicy::FailFast);
        bool siblingExecuted = false;
        const auto failed = group.Spawn({}, [](const Horo::CancellationToken &) -> Horo::Result<void> {
            throw std::runtime_error("child threw");
        });
        const auto sibling = group.Spawn({}, [&](const Horo::CancellationToken &) {
            siblingExecuted = true;
            return Horo::Result<void>::Success();
        });
        REQUIRE(failed.HasValue());
        REQUIRE(sibling.HasValue());
        const auto joined =
            group.Join({.waitPolicy = Horo::WaitPolicy::MainThreadPumpAllowed, .timeout = Horo::Duration::FromMilliseconds(100)});
        REQUIRE(joined.HasError());
        REQUIRE(joined.ErrorValue().code.Value() == "job.failed");
        REQUIRE(group.Outcome() == Horo::TaskGroupOutcome::Failed);
        REQUIRE_FALSE(siblingExecuted);
        REQUIRE(jobs.Find(sibling.Value())->state == Horo::JobState::Cancelled);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Shutdown Cancels Queued Work Before Joining Running Work", "[unit][foundation][jobs][cancel][shutdown][race]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 1}};
        std::atomic started{false};
        std::atomic observedCancellation{false};
        bool queuedExecuted = false;
        auto running = jobs.SubmitResult({}, [&](const Horo::CancellationToken &cancellation) {
            started.store(true, std::memory_order_release);
            while (!cancellation.IsCancellationRequested())
                std::this_thread::yield();
            observedCancellation.store(true);
            return Horo::JobCancelled();
        });
        REQUIRE(running.HasValue());
        while (!started.load(std::memory_order_acquire))
            std::this_thread::yield();
        auto queued = jobs.SubmitResult({}, [&](const Horo::CancellationToken &) {
            queuedExecuted = true;
            return Horo::Result<void>::Success();
        });
        REQUIRE(queued.HasValue());
        std::thread shutdown([&] {
            jobs.Shutdown(Horo::ShutdownPolicy::Cancel);
        });
        shutdown.join();
        REQUIRE(observedCancellation.load());
        REQUIRE_FALSE(queuedExecuted);
        REQUIRE(running.Value().Snapshot()->state == Horo::JobState::Cancelled);
        REQUIRE(queued.Value().Snapshot()->state == Horo::JobState::Cancelled);
        REQUIRE(running.Value().Wait().ErrorValue().code.Value() == "job.cancelled");
        REQUIRE(queued.Value().Wait().ErrorValue().code.Value() == "job.cancelled");
        REQUIRE(jobs.Submit({},
                            [](const Horo::CancellationToken &) {
        })
                    .ErrorValue()
                    .code.Value() == "job.shutdown");
    }

    TEST_CASE("Destructor Cancels And Joins Children", "[unit][foundation]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        std::atomic started{false};
        std::atomic stopped{false};
        Horo::JobId childId{};
        {
            Horo::TaskGroup group(jobs);
            const auto child = group.Spawn({}, [&started, &stopped](const Horo::CancellationToken &cancellation) {
                started.store(true);
                while (!cancellation.IsCancellationRequested())
                    std::this_thread::yield();
                stopped.store(true);
                return Horo::JobCancelled();
            });
            REQUIRE(child.HasValue());
            childId = child.Value();
            while (!started.load())
                std::this_thread::yield();
        }
        REQUIRE((stopped.load()));
        REQUIRE(jobs.Find(childId)->state == Horo::JobState::Cancelled);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Task Group Destructor Cancels Queued Children Without Running Them", "[unit][foundation][jobs][group][cancel][lifetime]") {
        Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        Horo::JobId childId{};
        bool executed = false;
        {
            Horo::TaskGroup group(jobs);
            const auto child = group.Spawn({}, [&](const Horo::CancellationToken &) {
                executed = true;
                return Horo::Result<void>::Success();
            });
            REQUIRE(child.HasValue());
            childId = child.Value();
        }
        REQUIRE_FALSE(executed);
        REQUIRE(jobs.Find(childId)->state == Horo::JobState::Cancelled);
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }
}  // namespace
