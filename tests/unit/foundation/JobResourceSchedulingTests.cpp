#include "../../support/AllocationProbe.h"
#include "../../support/OwnedJobTestThread.h"
#include "Horo/Foundation/JobSystem.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <future>
#include <mutex>
#include <vector>

namespace {
    using namespace Horo;

    /** @brief Bounded barrier whose destruction releases every callback before scheduler teardown. */
    class ResourceGate final {
    public:
        ~ResourceGate() {
            Release();
        }

        void Enter() {
            std::unique_lock lock(mutex_);
            entered_ = true;
            changed_.notify_all();
            changed_.wait_for(lock, std::chrono::seconds(5), [this] {
                return released_;
            });
        }

        bool Started() {
            std::unique_lock lock(mutex_);
            return changed_.wait_for(lock, std::chrono::seconds(2), [this] {
                return entered_;
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
        bool entered_{};
        bool released_{};
    };

    void NoResourceWork(const CancellationToken &) {}

    /** @brief Joins while callback inputs declared before this scope are still alive. */
    struct ShutdownScope final {
        JobSystem &jobs;

        ~ShutdownScope() {
            jobs.Shutdown(ShutdownPolicy::Cancel);
        }
    };

    TEST_CASE("Interactive CPU and I/O jobs progress while background callbacks continuously replenish their lane",
              "[foundation][jobs][resource]") {
        for (const auto resource : {JobResource::Cpu, JobResource::Io}) {
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 8, .ioWorkerCount = 1, .reservedInteractiveJobs = 1}};
            ResourceGate gate;
            std::atomic<std::size_t> runs{};
            std::atomic<std::size_t> interactivePosition{};
            std::atomic<bool> stop{};
            std::promise<Result<JobHandle>> admitted;
            auto admission = admitted.get_future();
            JobFunction background;
            background = [&](const CancellationToken &) {
                const auto position = ++runs;
                if (position == 32)
                    admitted.set_value(
                        jobs.Submit({.priority = JobPriority::Interactive, .resource = resource}, [&](const CancellationToken &) {
                        interactivePosition.store(runs.load());
                        stop.store(true);
                    }));
                if (!stop.load())
                    static_cast<void>(jobs.SubmitResult({.priority = JobPriority::Background, .resource = resource}, background));
                return Result<void>::Success();
            };

            struct StreamScope final {
                JobSystem &jobs;
                ResourceGate &gate;
                std::atomic<bool> &stop;

                ~StreamScope() {
                    stop.store(true);
                    gate.Release();
                    jobs.Shutdown(ShutdownPolicy::Cancel);
                }
            };

            const StreamScope stream{jobs, gate, stop};
            REQUIRE(jobs.Submit({.resource = resource}, [&](const CancellationToken &) {
                gate.Enter();
            }).HasValue());
            REQUIRE(gate.Started());
            for (std::size_t index = 0; index < 6; ++index)
                REQUIRE(jobs.SubmitResult({.priority = JobPriority::Background, .resource = resource}, background).HasValue());
            gate.Release();
            REQUIRE(admission.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
            auto interactive = admission.get();
            REQUIRE(interactive.HasValue());
            // Submitted by the worker: this test thread may wait but cannot help or bypass priority dispatch.
            REQUIRE(interactive.Value()
                        .Wait({.waitPolicy = WaitPolicy::ForbiddenOnOwnerThread, .timeout = Duration::FromMilliseconds(2000)})
                        .HasValue());
            CHECK(interactivePosition.load() >= 32);
            CHECK(interactivePosition.load() <= 38);
            jobs.Shutdown(ShutdownPolicy::Drain);
        }
    }

    TEST_CASE("Partial CPU and I/O worker construction joins owned threads before propagating allocation failure",
              "[foundation][jobs][shutdown]") {
        const JobSystemConfig config{.workerCount = 1, .ioWorkerCount = 1};
        std::size_t constructionRequests{};
        {
            const Tests::AllocationProbe::ScopedMeasurement measurement;
            JobSystem jobs{config};
            constructionRequests = measurement.Snapshot().requests;
        }
        REQUIRE(constructionRequests > 0);
        for (std::size_t index = 0; index < constructionRequests; ++index) {
            const Tests::AllocationProbe::ScopedFailure failure{index};
            CHECK_THROWS_AS(JobSystem{config}, std::bad_alloc);
        }
    }

    TEST_CASE("CPU interactive admission and dispatch survive saturated background and blocked I/O", "[foundation][jobs][resource]") {
        JobSystem jobs{
            {.workerCount = 1, .maxQueuedJobs = 8, .maxRetainedTerminalJobs = 3, .ioWorkerCount = 1, .reservedInteractiveJobs = 2}};
        ResourceGate cpu;
        ResourceGate io;
        std::atomic<std::size_t> backgroundRuns{};
        std::atomic<std::size_t> interactivePosition{999};
        std::atomic<bool> stop{};
        JobFunction background;
        background = [&](const CancellationToken &) {
            ++backgroundRuns;
            if (!stop.load())
                static_cast<void>(jobs.SubmitResult({.priority = JobPriority::Background}, background));
            return Result<void>::Success();
        };

        struct DrainGuard final {
            JobSystem &jobs;
            ResourceGate &cpu;
            ResourceGate &io;
            std::atomic<bool> &stop;

            ~DrainGuard() {
                stop.store(true);
                cpu.Release();
                io.Release();
                jobs.Shutdown(ShutdownPolicy::Cancel);
            }
        };

        const DrainGuard guard{jobs, cpu, io, stop};
        REQUIRE(jobs.Submit({.priority = JobPriority::Background}, [&](const CancellationToken &) {
            cpu.Enter();
        }).HasValue());
        REQUIRE(jobs.Submit({.priority = JobPriority::Background, .resource = JobResource::Io}, [&](const CancellationToken &) {
            io.Enter();
        }).HasValue());
        REQUIRE(cpu.Started());
        REQUIRE(io.Started());
        for (std::size_t index = 0; index < 6; ++index)
            REQUIRE(jobs.SubmitResult({.priority = JobPriority::Background}, background).HasValue());
        CHECK(jobs.Submit({.priority = JobPriority::Background}, NoResourceWork).HasError());
        auto interactive = jobs.Submit({.priority = JobPriority::Interactive}, [&](const CancellationToken &) {
            interactivePosition.store(backgroundRuns.load());
            stop.store(true);
        });
        REQUIRE(interactive.HasValue());
        REQUIRE(jobs.Submit({.priority = JobPriority::Interactive, .resource = JobResource::Io}, NoResourceWork).HasValue());
        CHECK(jobs.Submit({.priority = JobPriority::Interactive}, NoResourceWork).HasError());
        CHECK(jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{2, 0, 6});
        cpu.Release();
        REQUIRE(interactive.Value()
                    .Wait({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = Duration::FromMilliseconds(2000)})
                    .HasValue());
        CHECK(interactivePosition.load() == 0);
        // The I/O callback is still held at a barrier: CPU progress cannot depend on it finishing.
        io.Release();
        jobs.Shutdown(ShutdownPolicy::Drain);
        const auto retained = jobs.SnapshotIfChanged(0);
        REQUIRE(retained.has_value());
        CHECK(retained->jobs.size() <= 3);
        CHECK(jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{});
    }

    TEST_CASE("Resource validation rejects unknown and unconfigured lanes without publishing records", "[foundation][jobs][resource]") {
        JobSystem jobs{{.workerCount = 0, .maxQueuedJobs = 2, .reservedInteractiveJobs = 2}};
        const auto unknown = jobs.Submit({.resource = static_cast<JobResource>(255)}, NoResourceWork);
        const auto unavailable = jobs.Submit({.resource = JobResource::Io}, NoResourceWork);
        REQUIRE(unknown.HasError());
        CHECK(unknown.ErrorValue().code.Value() == "job.submission_invalid");
        REQUIRE(unavailable.HasError());
        CHECK(unavailable.ErrorValue().code.Value() == "job.wait_capacity_deadlock");
        CHECK_FALSE(jobs.SnapshotIfChanged(0).has_value());
        CHECK(jobs.Submit({}, NoResourceWork).HasError());
        auto admitted = jobs.Submit({.priority = JobPriority::Interactive}, NoResourceWork);
        REQUIRE(admitted.HasValue());
        jobs.Shutdown(ShutdownPolicy::Drain);
        CHECK(admitted.Value().Snapshot()->state == JobState::Succeeded);
    }

    TEST_CASE("Already running callbacks can join accepted same-lane children after shutdown closes external helping",
              "[foundation][jobs][shutdown]") {
        for (const auto resource : {JobResource::Cpu, JobResource::Io}) {
            JobSystem jobs{{.workerCount = 1, .ioWorkerCount = 1}};
            ResourceGate parentGate;
            std::optional<JobHandle> child;
            std::atomic<bool> joined{};
            const ShutdownScope scope{jobs};
            auto parent = jobs.SubmitResult({.resource = resource}, [&](const CancellationToken &) {
                parentGate.Enter();
                const auto result = child->Wait({.waitPolicy = WaitPolicy::WorkerOnly, .timeout = Duration::FromMilliseconds(2000)});
                joined.store(result.HasValue());
                return result;
            });
            REQUIRE(parent.HasValue());
            REQUIRE(parentGate.Started());
            auto accepted = jobs.Submit({.resource = resource}, NoResourceWork);
            REQUIRE(accepted.HasValue());
            child.emplace(std::move(accepted).Value());
            jobs.StopAccepting();
            Tests::Jobs::OwnedThread shutdown{Tests::Jobs::NativeThread([&] {
                jobs.Shutdown(ShutdownPolicy::Drain);
            })};
            parentGate.Release();
            shutdown.Join();
            CHECK(joined.load());
            CHECK(parent.Value().Snapshot()->state == JobState::Succeeded);
            CHECK(child->Snapshot()->state == JobState::Succeeded);
        }
    }

    TEST_CASE("Owner helping cannot borrow I/O or exceed occupied CPU execution capacity", "[foundation][jobs][resource]") {
        JobSystem jobs{{.workerCount = 1, .ioWorkerCount = 1}};
        ResourceGate cpu;
        ResourceGate io;
        std::atomic<int> executed{};
        const ShutdownScope scope{jobs};
        REQUIRE(jobs.Submit({}, [&](const CancellationToken &) {
            cpu.Enter();
        }).HasValue());
        REQUIRE(jobs.Submit({.resource = JobResource::Io}, [&](const CancellationToken &) {
            io.Enter();
        }).HasValue());
        REQUIRE(cpu.Started());
        REQUIRE(io.Started());
        auto queuedCpu = jobs.Submit({}, [&](const CancellationToken &) {
            ++executed;
        });
        auto queuedIo = jobs.Submit({.resource = JobResource::Io}, [&](const CancellationToken &) {
            ++executed;
        });
        REQUIRE(queuedCpu.HasValue());
        REQUIRE(queuedIo.HasValue());
        const JoinOptions pump{.waitPolicy = WaitPolicy::MainThreadPumpAllowed, .timeout = Duration::FromMilliseconds(2)};
        CHECK(queuedCpu.Value().Wait(pump).HasError());
        CHECK(queuedIo.Value().Wait(pump).HasError());
        CHECK(executed.load() == 0);
        cpu.Release();
        io.Release();
        jobs.Shutdown(ShutdownPolicy::Drain);
        CHECK(executed.load() == 2);
    }

    TEST_CASE("Saturated CPU and I/O callbacks reject mutual synchronous joins without waiting for timeouts",
              "[foundation][jobs][resource]") {
        JobSystem jobs{{.workerCount = 1, .ioWorkerCount = 1}};
        ResourceGate cpuGate;
        ResourceGate ioGate;
        const JobHandle *cpuHandle{};
        const JobHandle *ioHandle{};
        std::atomic<int> rejected{};
        const auto waitForOtherLane = [&](const JobHandle &other, const JobResource otherResource) {
            const auto bounded = other.Wait({.waitPolicy = WaitPolicy::WorkerOnly, .timeout = Duration::FromMilliseconds(2000)});
            const auto legacy = other.Wait();
            TaskGroup children{jobs};
            const auto spawned = children.Spawn({.resource = otherResource}, [](const CancellationToken &) {
                return Result<void>::Success();
            });
            for (const auto *error :
                 {bounded.HasError() ? &bounded.ErrorValue() : nullptr, legacy.HasError() ? &legacy.ErrorValue() : nullptr,
                  spawned.HasError() ? &spawned.ErrorValue() : nullptr})
                if (error && error->code.Value() == "job.wait_capacity_deadlock")
                    ++rejected;
        };
        const ShutdownScope scope{jobs};
        auto cpu = jobs.Submit({}, [&](const CancellationToken &) {
            cpuGate.Enter();
            if (ioHandle)
                waitForOtherLane(*ioHandle, JobResource::Io);
        });
        auto io = jobs.Submit({.resource = JobResource::Io}, [&](const CancellationToken &) {
            ioGate.Enter();
            if (cpuHandle)
                waitForOtherLane(*cpuHandle, JobResource::Cpu);
        });
        const ShutdownScope handleLifetime{jobs};
        REQUIRE(cpu.HasValue());
        REQUIRE(io.HasValue());
        REQUIRE(cpuGate.Started());
        REQUIRE(ioGate.Started());
        cpuHandle = &cpu.Value();
        ioHandle = &io.Value();
        cpuGate.Release();
        ioGate.Release();
        REQUIRE(cpu.Value().Wait().HasValue());
        REQUIRE(io.Value().Wait().HasValue());
        jobs.Shutdown(ShutdownPolicy::Drain);
        CHECK(rejected.load() == 6);
        CHECK(jobs.SnapshotIfChanged(0)->jobs.size() == 2);
    }

    TEST_CASE("Host rejection and cancellation join CPU I/O and inline captures before service destruction",
              "[foundation][jobs][shutdown]") {
        for (const std::size_t workers : {0U, 1U}) {
            JobSystem jobs{{.workerCount = workers, .maxRetainedTerminalJobs = 0, .ioWorkerCount = 1}};
            std::atomic<int> released{};

            struct BorrowedServiceCapture final {
                std::atomic<int> &released;

                ~BorrowedServiceCapture() {
                    ++released;
                }
            };

            auto capture = std::make_shared<BorrowedServiceCapture>(released);
            std::promise<void> cpuStarted;
            std::promise<void> ioStarted;
            auto startCpu = cpuStarted.get_future();
            auto startIo = ioStarted.get_future();
            const auto work = [](std::promise<void> &started, const CancellationToken &token) {
                started.set_value();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!token.IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                return token.IsCancellationRequested() ? JobCancelled() : Result<void>::Success();
            };
            const ShutdownScope scope{jobs};
            auto cpu = jobs.SubmitResult({}, [&, capture](const CancellationToken &token) {
                static_cast<void>(capture);
                return work(cpuStarted, token);
            });
            auto io = jobs.SubmitResult({.resource = JobResource::Io}, [&, capture](const CancellationToken &token) {
                static_cast<void>(capture);
                return work(ioStarted, token);
            });
            REQUIRE(cpu.HasValue());
            REQUIRE(io.HasValue());
            Tests::Jobs::OwnedThread helper{Tests::Jobs::NativeThread([&] {
                static_cast<void>(
                    cpu.Value().Wait({.waitPolicy = WaitPolicy::MainThreadPumpAllowed, .timeout = Duration::FromMilliseconds(2000)}));
            })};
            REQUIRE(startCpu.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
            REQUIRE(startIo.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
            capture.reset();
            jobs.StopAccepting();
            CHECK(jobs.Submit({}, NoResourceWork).ErrorValue().code.Value() == "job.shutdown");
            auto rejectedIo = jobs.Submit({.resource = JobResource::Io}, NoResourceWork);
            CHECK(rejectedIo.ErrorValue().code.Value() == "job.shutdown");
            jobs.Shutdown(ShutdownPolicy::Cancel);
            helper.Join();
            CHECK(cpu.Value().Snapshot()->state == JobState::Cancelled);
            CHECK(io.Value().Snapshot()->state == JobState::Cancelled);
            CHECK(released.load() == 1);
            jobs.Shutdown(ShutdownPolicy::Cancel);
            jobs.StopAccepting();
        }
    }

    TEST_CASE("Terminal capture cleanup can reenter legacy waits on CPU helpers and I/O workers after store eviction",
              "[foundation][jobs][shutdown]") {
        for (const auto resource : {JobResource::Cpu, JobResource::Io}) {
            JobSystem jobs{{.workerCount = 0, .maxRetainedTerminalJobs = 0, .ioWorkerCount = 1}};
            ResourceGate gate;
            const JobHandle *handle{};
            std::atomic<bool> reentered{};

            struct TerminalCapture final {
                TerminalCapture(const JobHandle *const &job, std::atomic<bool> &observed) : handle(job), reentered(observed) {}

                TerminalCapture(const TerminalCapture &) = delete;
                TerminalCapture &operator=(const TerminalCapture &) = delete;
                TerminalCapture(TerminalCapture &&) = delete;
                TerminalCapture &operator=(TerminalCapture &&) = delete;
                const JobHandle *const &handle;
                std::atomic<bool> &reentered;

                ~TerminalCapture() {
                    reentered.store(handle && handle->Wait().HasValue());
                }
            };

            auto capture = std::make_shared<TerminalCapture>(handle, reentered);
            auto submitted = jobs.Submit({.resource = resource}, [&, capture](const CancellationToken &) {
                static_cast<void>(capture);
                gate.Enter();
            });
            const ShutdownScope scope{jobs};
            REQUIRE(submitted.HasValue());
            handle = &submitted.Value();
            capture.reset();
            gate.Release();
            REQUIRE(submitted.Value()
                        .Wait({.waitPolicy = WaitPolicy::MainThreadPumpAllowed, .timeout = Duration::FromMilliseconds(2000)})
                        .HasValue());
            jobs.Shutdown(ShutdownPolicy::Drain);
            CHECK(reentered.load());
            CHECK_FALSE(jobs.Find(handle->Id()).has_value());
        }
    }
}  // namespace
