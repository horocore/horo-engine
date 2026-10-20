#include "Horo/Navigation/NavigationBakeJobs.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <thread>

using namespace Horo;
using namespace Horo::Navigation;

namespace {
    /** @brief Declares only real validation work, with no builder or publication callback. */
    NavigationBakeJobDescriptor ValidationDescriptor() {
        NavigationBakeJobDescriptor descriptor{.title = "Validate navigation source",
                                               .budget = {.maximumConcurrentJobs = 1,
                                                          .maximumResidentBytes = 1024,
                                                          .maximumTemporaryBytes = 1024,
                                                          .maximumWorkItems = 4,
                                                          .maximumWorkUnits = 1024,
                                                          .childDrainTimeout = Duration::FromMilliseconds(2000)},
                                               .execution = NavigationBakeExecutionMode::ValidateOnly};
        for (const auto stage : {NavigationBakeJobStage::PartitionGather, NavigationBakeJobStage::Validation})
            descriptor.work.push_back({.stage = stage, .execute = [](const CancellationToken &) {
                return Result<void>::Success();
            }});
        return descriptor;
    }

    /** @brief Bounds observation while preserving actual cancellation and job-drain ownership. */
    template <typename Predicate> bool Eventually(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return predicate();
    }

    /** @brief Cancels captured work on assertion unwinding before the scheduler is destroyed. */
    struct CancelOnExit final {
        NavigationBakeJobHandle handle;

        explicit CancelOnExit(NavigationBakeJobHandle value) : handle(std::move(value)) {}

        CancelOnExit(const CancelOnExit &) = delete;
        CancelOnExit &operator=(const CancelOnExit &) = delete;

        ~CancelOnExit() {
            static_cast<void>(handle.RequestCancellation());
        }
    };
}  // namespace

TEST_CASE("Validation navigation jobs execute exactly capture and validation with one typed operation", "[navigation][jobs][validation]") {
    OperationStore operations{4, 4};
    JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 8}};
    auto admitted = StartNavigationBakeJob(operations, jobs, ValidationDescriptor());
    REQUIRE(admitted.HasValue());
    const auto handle = std::move(admitted).Value();
    REQUIRE(Eventually([&handle] {
        return handle.Snapshot()->IsTerminal();
    }));
    const auto snapshot = handle.Snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->state == NavigationBakeJobState::Succeeded);
    CHECK(snapshot->execution == NavigationBakeExecutionMode::ValidateOnly);
    CHECK(snapshot->stage == NavigationBakeJobStage::Validation);
    CHECK(snapshot->acceptedChildJobs == 2);
    CHECK(snapshot->terminalChildJobs == 2);
    CHECK(snapshot->Progress() == 1.0);
    const auto projected = operations.SnapshotIfChanged(0);
    REQUIRE(projected);
    REQUIRE(projected->operations.size() == 1);
    CHECK(projected->operations.front().kind == OperationKind::Validation);
}

TEST_CASE("Validation navigation jobs reject missing work builder work publication and commit receipts before admission",
          "[navigation][jobs][validation]") {
    OperationStore operations{4, 4};
    JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 8}};
    auto descriptor = ValidationDescriptor();
    SECTION("capture is required") {
        descriptor.work.erase(descriptor.work.begin());
    }
    SECTION("validation is required") {
        descriptor.work.pop_back();
    }
    SECTION("builder work is forbidden") {
        auto builder = descriptor.work.front();
        builder.stage = NavigationBakeJobStage::TileBuild;
        descriptor.work.insert(descriptor.work.begin() + 1, std::move(builder));
    }
    SECTION("publication is forbidden") {
        auto publication = descriptor.work.back();
        publication.stage = NavigationBakeJobStage::Publication;
        descriptor.work.push_back(std::move(publication));
    }
    SECTION("publication acknowledgement is forbidden") {
        descriptor.publicationReceipt = std::make_shared<NavigationBakePublicationReceipt>();
    }
    SECTION("unknown mode is forbidden") {
        descriptor.execution = NavigationBakeExecutionMode::Count;
    }
    const auto rejected = StartNavigationBakeJob(operations, jobs, std::move(descriptor));
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == NavigationErrors::BakeJobInvalid.code.Value());
    CHECK_FALSE(operations.SnapshotIfChanged(0).has_value());
}

TEST_CASE("Validation navigation cancellation drains capture without invoking the next stage", "[navigation][jobs][validation]") {
    auto entered = std::make_shared<std::atomic_bool>();
    auto validated = std::make_shared<std::atomic_bool>();
    OperationStore operations{4, 4};
    JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 8}};
    auto descriptor = ValidationDescriptor();
    descriptor.work.front().execute = [entered](const CancellationToken &stop) {
        entered->store(true);
        while (!stop.IsCancellationRequested())
            std::this_thread::yield();
        return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
    };
    descriptor.work.back().execute = [validated](const CancellationToken &) {
        validated->store(true);
        return Result<void>::Success();
    };
    auto admitted = StartNavigationBakeJob(operations, jobs, std::move(descriptor));
    REQUIRE(admitted.HasValue());
    const CancelOnExit owner{std::move(admitted).Value()};
    REQUIRE(Eventually([&entered] {
        return entered->load();
    }));
    REQUIRE(owner.handle.RequestCancellation());
    REQUIRE(Eventually([&owner] {
        return owner.handle.Snapshot()->IsTerminal();
    }));
    const auto terminal = owner.handle.Snapshot();
    REQUIRE(terminal);
    CHECK(terminal->state == NavigationBakeJobState::Cancelled);
    CHECK(terminal->acceptedChildJobs == terminal->terminalChildJobs);
    CHECK_FALSE(validated->load());
}

TEST_CASE("Validation jobs preserve matching queued kind and reject a Cook operation before child admission",
          "[navigation][jobs][validation]") {
    const auto parent = std::make_shared<CancellationSource>();
    OperationStore operations{4, 4};
    JobSystem jobs{{.workerCount = 2, .maxQueuedJobs = 8}};
    const bool matching = GENERATE(true, false);
    const auto queued = operations.Begin({.kind = matching ? OperationKind::Validation : OperationKind::Cook,
                                          .title = "Queued source validation",
                                          .cancellable = true,
                                          .requestCancel = [parent] {
        parent->RequestCancellation();
    }});
    REQUIRE(queued);
    auto descriptor = ValidationDescriptor();
    descriptor.parentCancellation = parent->Token();
    descriptor.queuedOperation = queued;
    const auto admitted = StartNavigationBakeJob(operations, jobs, std::move(descriptor));
    if (matching) {
        REQUIRE(admitted.HasValue());
        const CancelOnExit owner{admitted.Value()};
        REQUIRE(Eventually([&owner] {
            return owner.handle.Snapshot()->IsTerminal();
        }));
        CHECK(owner.handle.Snapshot()->operation == *queued);
        CHECK(owner.handle.Snapshot()->state == NavigationBakeJobState::Succeeded);
    } else {
        CHECK(admitted.HasError());
    }
    const auto snapshot = operations.SnapshotIfChanged(0);
    REQUIRE(snapshot);
    REQUIRE(snapshot->operations.size() == 1);
    CHECK(snapshot->operations.front().kind == (matching ? OperationKind::Validation : OperationKind::Cook));
    if (!matching)
        CHECK(snapshot->operations.front().state == OperationState::Queued);
}
