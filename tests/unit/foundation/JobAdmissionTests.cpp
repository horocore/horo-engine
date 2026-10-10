#include "../../support/AllocationProbe.h"
#include "../../support/OwnedJobTestThread.h"
#include "Horo/Foundation/JobSystem.h"

#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
    using namespace Horo;

    /** @brief Latches worker execution independently of elapsed time, releasing it on every fixture exit. */
    class WorkerGate final {
    public:
        void Enter() {
            std::unique_lock lock(mutex_);
            started_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] {
                return released_;
            });
        }

        [[nodiscard]] bool Started() {
            std::unique_lock lock(mutex_);
            return changed_.wait_for(lock, std::chrono::seconds(2), [this] {
                return started_;
            });
        }

        void Release() {
            std::lock_guard lock(mutex_);
            released_ = true;
            changed_.notify_all();
        }

    private:
        std::mutex mutex_;
        std::condition_variable changed_;
        bool started_{};
        bool released_{};
    };

    [[nodiscard]] JobSystemConfig BlockingConfig() {
        JobSystemConfig config{.workerCount = 1, .maxQueuedJobs = 1};
        config.priorityQueues[1] = {.capacity = 1,
                                    .overloadPolicy = JobOverloadPolicy::Block,
                                    .blockTimeout = Duration::FromMilliseconds(1500)};
        return config;
    }

    class AdmissionFixture final {
    public:
        explicit AdmissionFixture(const JobSystemConfig &config = BlockingConfig()) : jobs(config) {}

        AdmissionFixture(const AdmissionFixture &) = delete;
        AdmissionFixture &operator=(const AdmissionFixture &) = delete;
        AdmissionFixture(AdmissionFixture &&) = delete;
        AdmissionFixture &operator=(AdmissionFixture &&) = delete;

        ~AdmissionFixture() {
            gate.Release();
            jobs.Shutdown(ShutdownPolicy::Cancel);
        }

        [[nodiscard]] bool Start() {
            auto running = jobs.Submit({}, [this](const CancellationToken &) {
                gate.Enter();
            });
            return running.HasValue() && gate.Started();
        }

        [[nodiscard]] bool Waiters(const std::size_t count) const {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            do {
                if (jobs.AdmissionSnapshot().waitingProducers == count)
                    return true;
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }

        WorkerGate gate;
        JobSystem jobs;
    };

    /** @brief Intentionally empty: admission tests observe scheduling decisions, not callback work. */
    void NoOpJob(const CancellationToken &) {
        // These jobs occupy queue slots without introducing unrelated execution behavior.
    }

    using JoiningThread = Tests::Jobs::OwnedThread;
    using NativeTestThread = Tests::Jobs::NativeThread;

#if defined(HORO_JOB_TEST_FORCE_THREAD_FALLBACK)
    static_assert(std::is_same_v<NativeTestThread, std::thread>);
#elif defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L
    static_assert(std::is_same_v<NativeTestThread, std::jthread>);
#endif

    /** @brief Owns one bounded producer operation and its explicit thread; no detached or implicit async lifetime. */
    class ProducerTask final {
    public:
        explicit ProducerTask(const JobSystem &jobs, CancellationToken cancellation)
            : task_([&jobs, cancellation] {
                  const JobProducerScope role{JobProducerRole::NonCritical};
                  return jobs.Submit({.parentCancellation = cancellation}, NoOpJob);
              }),
              result_(task_.get_future()), thread_(NativeTestThread([this] {
                  task_();
              })) {}

        ProducerTask(const ProducerTask &) = delete;
        ProducerTask &operator=(const ProducerTask &) = delete;
        ProducerTask(ProducerTask &&) = delete;
        ProducerTask &operator=(ProducerTask &&) = delete;

        [[nodiscard]] std::future_status wait_for(const std::chrono::milliseconds timeout) const {
            return result_.wait_for(timeout);
        }

        [[nodiscard]] Result<JobHandle> get() {
            return result_.get();
        }

    private:
        std::packaged_task<Result<JobHandle>()> task_;
        std::future<Result<JobHandle>> result_;
        JoiningThread thread_;
    };

    [[nodiscard]] ProducerTask SubmitProducer(const JobSystem &jobs, CancellationToken cancellation = {}) {
        return ProducerTask{jobs, cancellation};
    }

    /** @brief Checks the shared typed admission failure contract without dereferencing a successful result. */
    void CheckSubmissionRejected(const Result<JobHandle> &result, const std::string_view expectedCode) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == expectedCode);
    }

    /** @brief Applies the same typed rejection contract to both producers after each test's wakeup/teardown synchronization. */
    void CheckProducerPairRejected(ProducerTask &first, ProducerTask &second, const std::string_view expectedCode) {
        const auto firstResult = first.get();
        const auto secondResult = second.get();
        CheckSubmissionRejected(firstResult, expectedCode);
        CheckSubmissionRejected(secondResult, expectedCode);
    }

    /** @brief Models a callback capture whose destruction re-enters the scheduler's diagnostic boundary. */
    class ReentrantCapture final {
    public:
        ReentrantCapture(const JobSystem &jobs, bool &observed) : jobs_(jobs), observed_(observed) {}

        ReentrantCapture(const ReentrantCapture &) = delete;
        ReentrantCapture &operator=(const ReentrantCapture &) = delete;
        ReentrantCapture(ReentrantCapture &&) = delete;
        ReentrantCapture &operator=(ReentrantCapture &&) = delete;

        ~ReentrantCapture() {
            observed_ = jobs_.AdmissionSnapshot().waitingProducers == 0;
        }

    private:
        const JobSystem &jobs_;
        bool &observed_;
    };

    /** @brief Creates the same sole-owned, reentrant capture for measured and failing publication paths. */
    [[nodiscard]] std::function<void(const CancellationToken &)> ReentrantWork(const JobSystem &jobs, bool &observed) {
        return [capture = std::make_shared<ReentrantCapture>(jobs, observed)](const CancellationToken &) {
            // The capture validates destruction ordering; execution deliberately performs no other work.
            static_cast<void>(capture);
        };
    }

    /** @brief Verifies the shared terminal-publication contract after either precancellation or a concurrent store cancellation. */
    void CheckCancelledCaptureReleased(const JobSystem &jobs, const JobHandle &handle, const bool observed) {
        const auto snapshot = handle.Snapshot();
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->state == JobState::Cancelled);
        CHECK(observed);
        CHECK(jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{0, 0, 0});
    }

    /** @brief Primes bounded live and terminal deques near native block boundaries before measuring publication. */
    [[nodiscard]] bool PrimePublication(const JobSystem &jobs) {
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        for (std::size_t index = 0; index < 63; ++index) {
            if (jobs.Submit({.parentCancellation = cancellation.Token()}, NoOpJob).HasError())
                return false;
        }
        for (std::size_t index = 0; index < 31; ++index) {
            if (jobs.Submit({}, NoOpJob).HasError())
                return false;
        }
        return true;
    }

    /** @brief Measures this platform's actual submission allocation count without hard-coding allocator call order. */
    [[nodiscard]] std::size_t SubmissionAllocations(const CancellationToken &cancellation) {
        bool observed = false;
        JobSystem jobs{JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 64, .maxRetainedTerminalJobs = 128}};
        REQUIRE(PrimePublication(jobs));
        auto work = ReentrantWork(jobs, observed);
        Tests::AllocationProbe::ScopedMeasurement measurement;
        const auto submitted = jobs.Submit({.parentCancellation = cancellation}, std::move(work));
        const auto requests = measurement.Snapshot().requests;
        REQUIRE(submitted.HasValue());
        return requests;
    }
}  // namespace

TEST_CASE("Admission publication allocation failures leave no orphan records or locked callback destruction",
          "[foundation][jobs][admission]") {
    CancellationSource cancellation;
    SECTION("Live queue publication") {
        // No cancellation: exercise record, retention-map and live-queue publication allocations.
    }
    SECTION("Precancelled terminal publication") {
        cancellation.RequestCancellation();
    }
    const auto requests = SubmissionAllocations(cancellation.Token());
    REQUIRE(requests > 0);
    REQUIRE(requests <= 64);
    for (std::size_t failureIndex = 0; failureIndex < requests; ++failureIndex) {
        bool observed = false;
        JobSystem jobs{JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 64, .maxRetainedTerminalJobs = 128}};
        REQUIRE(PrimePublication(jobs));
        const auto before = jobs.SnapshotIfChanged(0);
        bool allocationFailed = false;
        {
            // A moved-from std::function may retain its capture. Release the caller's owner before checking scheduler cleanup.
            auto work = ReentrantWork(jobs, observed);
            try {
                const Tests::AllocationProbe::ScopedFailure failure{failureIndex};
                static_cast<void>(jobs.Submit({.parentCancellation = cancellation.Token()}, std::move(work)));
            } catch (const std::bad_alloc &) {
                allocationFailed = true;
            }
        }
        CHECK(allocationFailed);
        CHECK(observed);
        CHECK(jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{0, 31, 0});
        CHECK_FALSE(jobs.SnapshotIfChanged(before->revision).has_value());
        const auto recovered = jobs.Submit({}, NoOpJob);
        REQUIRE(recovered.HasValue());
        CHECK(recovered.Value().Id() == 95);
    }
}

TEST_CASE("Precancelled admission releases reentrant callback captures outside scheduler lock", "[foundation][jobs][admission]") {
    bool observed = false;
    JobSystem jobs{JobSystemConfig{.workerCount = 0}};
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    const auto submitted = jobs.Submit({.parentCancellation = cancellation.Token()}, ReentrantWork(jobs, observed));
    REQUIRE(submitted.HasValue());
    CheckCancelledCaptureReleased(jobs, submitted.Value(), observed);
}

TEST_CASE("Concurrent store observer cancellation releases the initialized callback before handoff", "[foundation][jobs][admission]") {
    for (std::size_t repetition = 0; repetition < 32; ++repetition) {
        bool observed = false;
        JobSystem jobs{JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
        std::promise<void> ready;
        auto started = ready.get_future();
        std::packaged_task<bool()> cancelObserved{[&jobs, &ready] {
            ready.set_value();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            do {
                if (jobs.Find(1).has_value())
                    return jobs.RequestCancel(1).HasValue();
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }};
        auto cancelled = cancelObserved.get_future();
        JoiningThread observer{NativeTestThread([&cancelObserved] {
            cancelObserved();
        })};
        // The observer is active before submission, not started by the returned handle.
        REQUIRE(started.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const auto submitted = jobs.Submit({}, ReentrantWork(jobs, observed));
        REQUIRE(submitted.HasValue());
        REQUIRE(cancelled.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        REQUIRE(cancelled.get());
        observer.Join();
        CheckCancelledCaptureReleased(jobs, submitted.Value(), observed);
    }
}

TEST_CASE("Priority admission enforces class and global limits without consuming rejected identities", "[foundation][jobs][admission]") {
    JobSystemConfig config{.workerCount = 0, .maxQueuedJobs = 2};
    config.priorityQueues[0].capacity = 1;
    config.priorityQueues[2].capacity = std::numeric_limits<std::size_t>::max();
    JobSystem jobs{config};
    auto interactive = jobs.Submit({.priority = JobPriority::Interactive}, NoOpJob);
    REQUIRE(interactive.HasValue());
    const auto classFull = jobs.Submit({.priority = JobPriority::Interactive}, NoOpJob);
    CheckSubmissionRejected(classFull, "job.queue_full");
    auto background = jobs.Submit({.priority = JobPriority::Background}, NoOpJob);
    REQUIRE(background.HasValue());
    CHECK(background.Value().Id() == interactive.Value().Id() + 1);
    const auto globallyFull = jobs.Submit({}, NoOpJob);
    CheckSubmissionRejected(globallyFull, "job.queue_full");
    const auto pressure = jobs.AdmissionSnapshot();
    CHECK(pressure.queued == std::array<std::size_t, 3>{1, 0, 1});
    CHECK(pressure.rejected == std::array<std::uint64_t, 3>{1, 1, 0});
}

TEST_CASE("Shed drops only optional incoming work at class or global capacity", "[foundation][jobs][admission]") {
    JobSystemConfig config{.workerCount = 0, .maxQueuedJobs = 2};
    config.priorityQueues[2] = {.capacity = 1, .overloadPolicy = JobOverloadPolicy::Shed};
    SECTION("Class capacity") {
        config.priorityQueues[2].capacity = 1;
    }
    SECTION("Global capacity") {
        config.maxQueuedJobs = 1;
        config.priorityQueues[2].capacity = 2;
    }
    JobSystem jobs{config};
    auto first = jobs.Submit({.priority = JobPriority::Background}, NoOpJob);
    REQUIRE(first.HasValue());
    const auto required = jobs.Submit({.priority = JobPriority::Background}, NoOpJob);
    CheckSubmissionRejected(required, "job.queue_full");
    CHECK(jobs.AdmissionSnapshot().shed[2] == 0);
    const auto dropped = jobs.Submit({.priority = JobPriority::Background, .requirement = JobRequirement::Optional}, NoOpJob);
    CheckSubmissionRejected(dropped, "job.queue_shed");
    CHECK(first.Value().Snapshot()->state == JobState::Queued);
    REQUIRE(first.Value().RequestCancel().HasValue());
    const auto replacement = jobs.Submit({.priority = JobPriority::Background}, NoOpJob);
    REQUIRE(replacement.HasValue());
    CHECK(replacement.Value().Id() == first.Value().Id() + 1);
    CHECK(jobs.AdmissionSnapshot().rejected[2] == 2);
    CHECK(jobs.AdmissionSnapshot().shed[2] == 1);
}

TEST_CASE("Identical queued pressure has stable weighted FIFO dispatch without starving background", "[foundation][jobs][admission]") {
    for (int repetition = 0; repetition < 3; ++repetition) {
        std::vector<int> order;
        AdmissionFixture fixture{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 14}};
        REQUIRE(fixture.Start());
        for (int index = 0; index < 7; ++index) {
            REQUIRE(fixture.jobs
                        .Submit({.priority = JobPriority::Interactive}, [&order, index](const CancellationToken &) {
                order.push_back(10 + index);
            }).HasValue());
        }
        for (int index = 0; index < 4; ++index) {
            REQUIRE(fixture.jobs
                        .Submit({}, [&order, index](const CancellationToken &) {
                order.push_back(20 + index);
            }).HasValue());
        }
        for (int index = 0; index < 3; ++index) {
            REQUIRE(fixture.jobs
                        .Submit({.priority = JobPriority::Background}, [&order, index](const CancellationToken &) {
                order.push_back(30 + index);
            }).HasValue());
        }
        fixture.gate.Release();
        fixture.jobs.Shutdown(ShutdownPolicy::Drain);
        // The initial Normal gate consumed slot 4 in the fixed cycle; dispatch resumes at slot 5.
        CHECK(order == std::vector<int>{20, 30, 10, 11, 12, 13, 21, 22, 31, 14, 15, 16, 23, 32});
    }
}

TEST_CASE("Critical and unknown producer scopes cannot gain blocking rights through nesting", "[foundation][jobs][admission]") {
    using enum JobProducerRole;
    AdmissionFixture fixture;
    REQUIRE(fixture.Start());
    REQUIRE(fixture.jobs.Submit({}, NoOpJob).HasValue());
    for (const auto role : {MainEditor, RenderOwner, TransportOwner, Worker, IoService, ExternalUnknown}) {
        const JobProducerScope outer{role};
        const JobProducerScope attemptedWidening{JobProducerRole::NonCritical};
        const auto rejected = fixture.jobs.Submit({}, NoOpJob);
        CheckSubmissionRejected(rejected, "job.wait_forbidden");
    }
    const auto unknown = fixture.jobs.Submit({}, NoOpJob);
    CheckSubmissionRejected(unknown, "job.wait_forbidden");
    CHECK(fixture.jobs.AdmissionSnapshot().waitingProducers == 0);
}

TEST_CASE("Scheduler callbacks cannot block admission even when declaring a noncritical scope", "[foundation][jobs][admission]") {
    std::string outcome;
    AdmissionFixture fixture;
    auto parent = fixture.jobs.Submit({}, [&fixture, &outcome](const CancellationToken &) {
        fixture.gate.Enter();
        const JobProducerScope attemptedPermission{JobProducerRole::NonCritical};
        const auto result = fixture.jobs.Submit({}, NoOpJob);
        outcome = result.HasError() ? std::string{result.ErrorValue().code.Value()} : "accepted";
    });
    REQUIRE(parent.HasValue());
    REQUIRE(fixture.gate.Started());
    REQUIRE(fixture.jobs.Submit({}, NoOpJob).HasValue());
    fixture.gate.Release();
    REQUIRE(
        parent.Value().Wait({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = Duration::FromMilliseconds(1000)}).HasValue());
    fixture.jobs.Shutdown(ShutdownPolicy::Drain);
    CHECK(outcome == "job.wait_forbidden");
}

TEST_CASE("Cancellation wakes all producers sharing an ancestor without admitting records", "[foundation][jobs][admission]") {
    AdmissionFixture fixture;
    REQUIRE(fixture.Start());
    REQUIRE(fixture.jobs.Submit({}, NoOpJob).HasValue());
    CancellationSource parent;
    CancellationSource child{parent.Token()};
    auto first = SubmitProducer(fixture.jobs, child.Token());
    auto second = SubmitProducer(fixture.jobs, child.Token());
    REQUIRE(fixture.Waiters(2));
    parent.RequestCancellation();
    REQUIRE(first.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    REQUIRE(second.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    CheckProducerPairRejected(first, second, "job.cancelled");
    CHECK(fixture.jobs.AdmissionSnapshot().waitingProducers == 0);
    CHECK(fixture.jobs.SnapshotIfChanged(0)->jobs.size() == 2);
}

TEST_CASE("Shutdown wakes all bounded producers before joining running callbacks", "[foundation][jobs][admission]") {
    AdmissionFixture fixture;
    REQUIRE(fixture.Start());
    REQUIRE(fixture.jobs.Submit({}, NoOpJob).HasValue());
    auto first = SubmitProducer(fixture.jobs);
    auto second = SubmitProducer(fixture.jobs);
    REQUIRE(fixture.Waiters(2));
    NativeTestThread shutdownThread{[&fixture] {
        fixture.jobs.Shutdown(ShutdownPolicy::Cancel);
    }};
    JoiningThread shutdown{std::move(shutdownThread)};
    const bool firstReady = first.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    const bool secondReady = second.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    // Release the worker before assertions so a failed wakeup cannot strand the shutdown future.
    fixture.gate.Release();
    shutdown.Join();
    CHECK(firstReady);
    CHECK(secondReady);
    CheckProducerPairRejected(first, second, "job.shutdown");
    CHECK(fixture.jobs.AdmissionSnapshot().waitingProducers == 0);
}

TEST_CASE("Host rejection wakes producers while accepted work remains available for owner drain", "[foundation][jobs][admission]") {
    AdmissionFixture fixture;
    REQUIRE(fixture.Start());
    auto queued = fixture.jobs.Submit({}, NoOpJob);
    REQUIRE(queued.HasValue());
    auto producer = SubmitProducer(fixture.jobs);
    REQUIRE(fixture.Waiters(1));
    fixture.jobs.StopAccepting();
    REQUIRE(producer.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready);
    CheckSubmissionRejected(producer.get(), "job.shutdown");
    CHECK(queued.Value().Snapshot()->state == JobState::Queued);
    fixture.gate.Release();
    fixture.jobs.Shutdown(ShutdownPolicy::Drain);
    CHECK(queued.Value().Wait().HasValue());
    CHECK(fixture.jobs.AdmissionSnapshot().waitingProducers == 0);
}

TEST_CASE("Producer wait count and deadlines stay bounded", "[foundation][jobs][admission]") {
    auto config = BlockingConfig();
    config.maxWaitingProducers = 1;
    AdmissionFixture fixture{config};
    REQUIRE(fixture.Start());
    auto queued = fixture.jobs.Submit({}, NoOpJob);
    REQUIRE(queued.HasValue());
    auto first = SubmitProducer(fixture.jobs);
    REQUIRE(fixture.Waiters(1));
    const JobProducerScope scope{JobProducerRole::NonCritical};
    const auto excess = fixture.jobs.Submit({}, NoOpJob);
    CheckSubmissionRejected(excess, "job.queue_full");
    REQUIRE(queued.Value().RequestCancel().HasValue());
    const auto admitted = first.get();
    REQUIRE(admitted.HasValue());
    CHECK(fixture.jobs.AdmissionSnapshot().waitingProducers == 0);

    const auto timedOut = fixture.jobs.Submit({}, NoOpJob);
    CheckSubmissionRejected(timedOut, "job.wait_timed_out");
    CHECK(fixture.jobs.AdmissionSnapshot().timedOut[1] == 1);
}

TEST_CASE("Invalid queue policy and disabled capacity have typed outcomes", "[foundation][jobs][admission]") {
    SECTION("Invalid requirement") {
        JobSystem jobs{JobSystemConfig{.workerCount = 0}};
        const auto result = jobs.Submit({.requirement = static_cast<JobRequirement>(255)}, NoOpJob);
        CheckSubmissionRejected(result, "job.submission_invalid");
        CHECK(jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{});
        CHECK(jobs.AdmissionSnapshot().rejected == std::array<std::uint64_t, 3>{});
    }
    SECTION("Invalid priority") {
        JobSystem jobs{JobSystemConfig{.workerCount = 0}};
        const auto result = jobs.Submit({.priority = static_cast<JobPriority>(255)}, NoOpJob);
        CheckSubmissionRejected(result, "job.submission_invalid");
    }
    SECTION("Block requires a positive timeout") {
        auto config = BlockingConfig();
        config.priorityQueues[1].blockTimeout = {};
        JobSystem jobs{config};
        const auto result = jobs.Submit({}, NoOpJob);
        CheckSubmissionRejected(result, "job.submission_invalid");
    }
    SECTION("Zero execution capacity never waits") {
        auto config = BlockingConfig();
        config.workerCount = 0;
        config.maxQueuedJobs = 0;
        JobSystem jobs{config};
        const JobProducerScope scope{JobProducerRole::NonCritical};
        const auto result = jobs.Submit({}, NoOpJob);
        CheckSubmissionRejected(result, "job.wait_capacity_deadlock");
    }
}
