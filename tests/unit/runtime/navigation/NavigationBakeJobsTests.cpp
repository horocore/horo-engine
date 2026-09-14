#include "Horo/Navigation/NavigationBakeJobs.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Navigation {
    namespace {
        using namespace std::chrono_literals;

        [[nodiscard]] NavigationBakeJobBudget TestBudget() {
            return {.maximumConcurrentJobs = 2,
                    .maximumResidentBytes = 1'024,
                    .maximumTemporaryBytes = 4'096,
                    .maximumWorkItems = 16,
                    .maximumWorkUnits = 1'024,
                    .childDrainTimeout = Duration::FromMilliseconds(2'000)};
        }

        [[nodiscard]] NavigationBakeWorkItem Work(const NavigationBakeJobStage stage, const std::uint64_t units = 1,
                                                  const std::uint64_t residentBytes = 1, const std::uint64_t temporaryBytes = 1) {
            return {.stage = stage,
                    .workUnits = units,
                    .residentBytes = residentBytes,
                    .temporaryBytes = temporaryBytes,
                    .execute = [](const CancellationToken &) {
                return Result<void>::Success();
            }};
        }

        template <typename Predicate> void RequireEventually(Predicate predicate) {
            for (std::size_t attempt = 0; attempt < 2'000; ++attempt) {
                if (predicate())
                    return;
                std::this_thread::sleep_for(1ms);
            }
            REQUIRE(predicate());
        }

        [[nodiscard]] NavigationBakeJobHandle RequireHandle(Result<NavigationBakeJobHandle> result) {
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        void RequireTerminal(const NavigationBakeJobHandle &handle) {
            RequireEventually([&] {
                return handle.Snapshot()->IsTerminal();
            });
        }

        [[nodiscard]] NavigationBakeJobDescriptor CompleteDescriptor() {
            NavigationBakeJobDescriptor descriptor{.title = "Test navigation bake", .budget = TestBudget()};
            descriptor.work.push_back(Work(NavigationBakeJobStage::PartitionGather, 2));
            descriptor.work.push_back(Work(NavigationBakeJobStage::TileBuild, 3));
            descriptor.work.push_back(Work(NavigationBakeJobStage::TileBuild, 5));
            descriptor.work.push_back(Work(NavigationBakeJobStage::Validation, 7));
            descriptor.work.push_back(Work(NavigationBakeJobStage::Publication, 11));
            return descriptor;
        }
    }  // namespace

    TEST_CASE("Navigation bake jobs complete ordered stages with monotonic operation progress") {
        OperationStore operations(4, 4);
        JobSystem jobs({.workerCount = 4, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 16});

        auto started = StartNavigationBakeJob(operations, jobs, CompleteDescriptor());
        REQUIRE(started.HasValue());
        auto handle = std::move(started).Value();

        double priorProgress{};
        std::uint64_t priorRevision{};
        RequireEventually([&] {
            const auto snapshot = handle.Snapshot();
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->revision >= priorRevision);
            CHECK(snapshot->Progress() >= priorProgress);
            priorRevision = snapshot->revision;
            priorProgress = snapshot->Progress();
            return snapshot->IsTerminal();
        });

        const auto terminal = handle.Snapshot();
        REQUIRE(terminal.has_value());
        CHECK(terminal->state == NavigationBakeJobState::Succeeded);
        CHECK(terminal->Progress() == 1.0);
        CHECK(terminal->acceptedChildJobs == 5);
        CHECK(terminal->terminalChildJobs == terminal->acceptedChildJobs);

        const auto projected = operations.SnapshotIfChanged(0);
        REQUIRE(projected.has_value());
        REQUIRE(projected->operations.size() == 1);
        CHECK(projected->operations.front().id == handle.Id());
        CHECK(projected->operations.front().state == OperationState::Succeeded);
        CHECK(projected->operations.front().progress == 1.0F);
    }

    TEST_CASE("Navigation bake cancellation terminalizes once after accepted children drain") {
        OperationStore operations(4, 4);
        JobSystem jobs({.workerCount = 4, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 16});
        std::atomic<bool> tileEntered{};

        auto descriptor = CompleteDescriptor();
        descriptor.work[1].execute = [&tileEntered](const CancellationToken &cancellation) {
            tileEntered.store(true);
            while (!cancellation.IsCancellationRequested())
                std::this_thread::yield();
            return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
        };
        descriptor.work[2].execute = descriptor.work[1].execute;

        auto handle = RequireHandle(StartNavigationBakeJob(operations, jobs, std::move(descriptor)));
        RequireEventually([&] {
            return tileEntered.load();
        });
        REQUIRE(handle.RequestCancellation());
        REQUIRE(handle.RequestCancellation());
        RequireTerminal(handle);

        const auto terminal = handle.Snapshot();
        REQUIRE(terminal.has_value());
        CHECK(terminal->state == NavigationBakeJobState::Cancelled);
        CHECK(terminal->terminalChildJobs == terminal->acceptedChildJobs);
        const auto terminalRevision = terminal->revision;
        CHECK(handle.RequestCancellation());
        CHECK(handle.Snapshot()->revision == terminalRevision);

        const auto projected = operations.SnapshotIfChanged(0);
        REQUIRE(projected.has_value());
        CHECK(projected->operations.front().state == OperationState::Cancelled);
    }

    TEST_CASE("Navigation bake observes cancellation requested before scheduler execution") {
        OperationStore operations(4, 4);
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8, .maxRetainedTerminalJobs = 8});
        CancellationSource parent;
        parent.RequestCancellation();
        auto descriptor = CompleteDescriptor();
        descriptor.parentCancellation = parent.Token();

        auto handle = RequireHandle(StartNavigationBakeJob(operations, jobs, std::move(descriptor)));
        RequireTerminal(handle);

        const auto terminal = handle.Snapshot();
        REQUIRE(terminal.has_value());
        CHECK(terminal->state == NavigationBakeJobState::Cancelled);
        CHECK(terminal->acceptedChildJobs == 0);
        CHECK(terminal->terminalChildJobs == 0);
    }

    TEST_CASE("Navigation bake budget failure identifies the limiting resource") {
        OperationStore operations(4, 4);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8, .maxRetainedTerminalJobs = 8});
        auto descriptor = CompleteDescriptor();
        descriptor.budget.maximumTemporaryBytes = 3;

        auto started = StartNavigationBakeJob(operations, jobs, std::move(descriptor));
        REQUIRE(started.HasValue());
        const auto snapshot = started.Value().Snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state == NavigationBakeJobState::Failed);
        REQUIRE(snapshot->limitingResource.has_value());
        CHECK(*snapshot->limitingResource == NavigationBakeBudgetResource::TemporaryStorage);
        REQUIRE(snapshot->terminalError.has_value());
        CHECK(snapshot->terminalError->code.Value() == NavigationErrors::BakeJobBudgetExceeded.code.Value());
        CHECK(snapshot->acceptedChildJobs == 0);
    }

    TEST_CASE("Navigation bake drains a resident-memory batch before admitting work that would exceed it") {
        OperationStore operations(4, 4);
        JobSystem jobs({.workerCount = 4, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 16});
        std::atomic<bool> firstEntered{};
        std::atomic<bool> releaseFirst{};
        std::atomic<bool> firstCompleted{};
        std::atomic<bool> laterWorkStartedTooEarly{};
        std::atomic<std::size_t> laterWorkCount{};

        auto descriptor = CompleteDescriptor();
        descriptor.budget.maximumResidentBytes = 100;
        descriptor.budget.maximumConcurrentJobs = 3;
        descriptor.work[1].residentBytes = 60;
        descriptor.work[1].execute = [&](const CancellationToken &) {
            firstEntered.store(true);
            while (!releaseFirst.load())
                std::this_thread::yield();
            firstCompleted.store(true);
            return Result<void>::Success();
        };
        descriptor.work[2].residentBytes = 60;
        descriptor.work[2].execute = [&](const CancellationToken &) {
            laterWorkStartedTooEarly.store(!firstCompleted.load());
            laterWorkCount.fetch_add(1);
            return Result<void>::Success();
        };
        descriptor.work.insert(descriptor.work.begin() + 3, Work(NavigationBakeJobStage::TileBuild, 1, 40));
        descriptor.work[3].execute = descriptor.work[2].execute;

        auto handle = RequireHandle(StartNavigationBakeJob(operations, jobs, std::move(descriptor)));
        RequireEventually([&] {
            return firstEntered.load();
        });
        CHECK(laterWorkCount.load() == 0);
        releaseFirst.store(true);
        RequireTerminal(handle);

        CHECK(handle.Snapshot()->state == NavigationBakeJobState::Succeeded);
        CHECK(laterWorkCount.load() == 2);
        CHECK_FALSE(laterWorkStartedTooEarly.load());
    }

    TEST_CASE("Navigation bake rejects missing stages without creating an operation") {
        OperationStore operations(4, 4);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8, .maxRetainedTerminalJobs = 8});
        NavigationBakeJobDescriptor descriptor{.title = "Incomplete", .budget = TestBudget()};
        descriptor.work.push_back(Work(NavigationBakeJobStage::TileBuild));

        const auto started = StartNavigationBakeJob(operations, jobs, std::move(descriptor));
        REQUIRE(started.HasError());
        CHECK(started.ErrorValue().code.Value() == NavigationErrors::BakeJobInvalid.code.Value());
        CHECK_FALSE(operations.SnapshotIfChanged(0).has_value());
    }
}  // namespace Horo::Navigation
