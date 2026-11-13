#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "support/AllocationProbe.h"

#include <array>
#include <atomic>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;

    SaveOperationController Operation(const SaveOperationKind kind = SaveOperationKind::Save, const OperationId id = 41,
                                      const std::size_t callbackCapacity = 4,
                                      const std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt,
                                      const CancellationToken parent = {}) {
        auto created = CreateSaveOperation({.operation = id,
                                            .kind = kind,
                                            .maximumCompletionCallbacks = callbackCapacity,
                                            .deadline = deadline,
                                            .parentCancellation = parent});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    SaveOperationSnapshot Snapshot(const SaveOperationHandle &handle) {
        const auto snapshot = handle.Snapshot();
        REQUIRE(snapshot.has_value());
        return *snapshot;
    }

    void RequireError(const std::optional<Error> &error, const ErrorCodeDescriptor &descriptor) {
        REQUIRE(error.has_value());
        CHECK(error->domain.Value() == descriptor.domain.Value());
        CHECK(error->code.Value() == descriptor.code.Value());
    }

    SaveOperationHandle CountCompletions(SaveOperationController &controller, std::atomic<int> &callbacks) {
        const SaveOperationHandle handle = controller.Handle();
        REQUIRE(handle
                    .OnCompletion([&callbacks](const SaveOperationSnapshot &) {
            ++callbacks;
        }).HasValue());
        return handle;
    }
}  // namespace

TEST_CASE("Save operation admission uses application operation identities", "[unit][runtime][save][operation]") {
    for (const auto descriptor :
         {SaveOperationDescriptor{.maximumCompletionCallbacks = 1}, SaveOperationDescriptor{.operation = 1},
          SaveOperationDescriptor{.operation = 1, .kind = static_cast<SaveOperationKind>(255), .maximumCompletionCallbacks = 1},
          SaveOperationDescriptor{.operation = 1, .maximumCompletionCallbacks = MaximumSaveOperationCompletionCallbacks + 1}}) {
        const auto created = CreateSaveOperation(descriptor);
        REQUIRE(created.HasError());
        CHECK(created.ErrorValue().code.Value() == SaveErrors::OperationInvalid.code.Value());
    }

    auto controller = Operation(SaveOperationKind::Load, 73);
    const SaveOperationHandle handle = controller.Handle();
    CHECK(handle.IsValid());
    CHECK(handle.Id() == 73);
    const auto initial = Snapshot(handle);
    CHECK(initial.kind == SaveOperationKind::Load);
    CHECK(initial.state == SaveOperationState::Queued);
    CHECK(initial.stage == SaveOperationStage::Queued);
    CHECK(initial.cancellable);
    CHECK_FALSE(initial.IsTerminal());

    const SaveOperationHandle invalid;
    CHECK_FALSE(invalid.IsValid());
    CHECK(invalid.Id() == 0);
    CHECK_FALSE(invalid.Snapshot().has_value());
    CHECK(invalid.RequestCancellation() == SaveCancellationRequestResult::InvalidHandle);
    REQUIRE(invalid
                .OnCompletion([](const SaveOperationSnapshot &) {
    }).HasError());
    REQUIRE(handle.OnCompletion({}).HasError());
}

TEST_CASE("Save operation publishes exact typed progress without regression", "[unit][runtime][save][operation]") {
    auto controller = Operation();
    const auto handle = controller.Handle();
    CHECK(controller.PublishProgress(SaveOperationStage::CapturingSnapshot, {2, 5}) == SaveOperationTransitionResult::Applied);
    auto progress = Snapshot(handle);
    CHECK(progress.state == SaveOperationState::Running);
    CHECK(progress.stage == SaveOperationStage::CapturingSnapshot);
    CHECK(progress.progress.completedUnits == 2);
    CHECK(progress.progress.totalUnits == 5);
    CHECK(progress.progress.Fraction() == 0.4);

    CHECK(controller.PublishProgress(SaveOperationStage::CapturingSnapshot, {1, 5}) == SaveOperationTransitionResult::InvalidTransition);
    CHECK(controller.PublishProgress(SaveOperationStage::CapturingSnapshot, {0, 0}) == SaveOperationTransitionResult::InvalidTransition);
    CHECK(controller.PublishProgress(SaveOperationStage::Serializing, {6, 5}) == SaveOperationTransitionResult::InvalidTransition);
    CHECK(controller.PublishProgress(SaveOperationStage::Serializing, {0, 0}) == SaveOperationTransitionResult::InvalidTransition);
    CHECK(controller.PublishProgress(SaveOperationStage::PreparingRestore, {0, 1}) == SaveOperationTransitionResult::InvalidTransition);
    CHECK(Snapshot(handle).revision == progress.revision);

    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    CHECK(controller.PublishProgress(SaveOperationStage::Serializing, {maximum - 1, maximum}) == SaveOperationTransitionResult::Applied);
    CHECK(controller.PublishProgress(SaveOperationStage::Serializing, {maximum - 2, maximum - 1}) ==
          SaveOperationTransitionResult::InvalidTransition);
}

TEST_CASE("Cancellation before commit wins and publishes one immutable terminal result", "[unit][runtime][save][operation]") {
    auto controller = Operation();
    const auto handle = controller.Handle();
    std::atomic<int> callbacks{};
    REQUIRE(handle
                .OnCompletion([&callbacks](const SaveOperationSnapshot &snapshot) {
        CHECK(snapshot.state == SaveOperationState::Cancelled);
        ++callbacks;
    }).HasValue());

    CHECK(handle.RequestCancellation() == SaveCancellationRequestResult::Requested);
    CHECK(handle.RequestCancellation() == SaveCancellationRequestResult::AlreadyRequested);
    CHECK(controller.BeginCommit() == SaveCommitGateResult::CancellationWon);
    CHECK(callbacks.load() == 1);
    const auto terminal = Snapshot(handle);
    CHECK(terminal.state == SaveOperationState::Cancelled);
    CHECK(terminal.commit == SaveOperationCommitOutcome::NotCommitted);
    CHECK(terminal.cancellationReason == SaveCancellationReason::Caller);
    CHECK_FALSE(terminal.cancellable);
    RequireError(terminal.terminalError, SaveErrors::OperationCancelled);
    CHECK(controller.Fail(MakeError(SaveErrors::CompositionInjectedFailure), SaveOperationCommitOutcome::NotCommitted) ==
          SaveOperationTransitionResult::AlreadyTerminal);
    CHECK(Snapshot(handle).revision == terminal.revision);
}

TEST_CASE("Commit gate makes late cancellation too late", "[unit][runtime][save][operation]") {
    auto controller = Operation(SaveOperationKind::Load);
    const auto handle = controller.Handle();
    std::atomic<int> callbacks{};
    REQUIRE(handle
                .OnCompletion([&callbacks](const SaveOperationSnapshot &) {
        ++callbacks;
    }).HasValue());
    CHECK(controller.PublishProgress(SaveOperationStage::ReadyToCommit, {1, 1}) == SaveOperationTransitionResult::Applied);
    CHECK(controller.BeginCommit() == SaveCommitGateResult::Entered);
    CHECK(Snapshot(handle).stage == SaveOperationStage::ApplyingState);
    CHECK(handle.RequestCancellation() == SaveCancellationRequestResult::TooLate);
    CHECK(controller.RequestShutdownCancellation() == SaveCancellationRequestResult::TooLate);
    CHECK(callbacks.load() == 0);
    CHECK(controller.Complete(SaveOperationCommitOutcome::Committed) == SaveOperationTransitionResult::Applied);
    CHECK(callbacks.load() == 1);

    const auto terminal = Snapshot(handle);
    CHECK(terminal.state == SaveOperationState::Completed);
    CHECK(terminal.commit == SaveOperationCommitOutcome::Committed);
    CHECK(terminal.progress.Fraction() == 1.0);
    CHECK_FALSE(terminal.terminalError.has_value());
}

TEST_CASE("Operation stages advance monotonically through per-kind commit predecessors", "[unit][runtime][save][operation]") {
    struct MutationStages final {
        SaveOperationKind kind;
        SaveOperationStage first;
        SaveOperationStage ready;
        SaveOperationStage committed;
    };

    constexpr std::array cases{
        MutationStages{SaveOperationKind::Save, SaveOperationStage::CapturingSnapshot, SaveOperationStage::WritingTemporary,
                       SaveOperationStage::CommitStarted},
        MutationStages{SaveOperationKind::Load, SaveOperationStage::VerifyingArchive, SaveOperationStage::ReadyToCommit,
                       SaveOperationStage::ApplyingState},
        MutationStages{SaveOperationKind::Delete, SaveOperationStage::Deleting, SaveOperationStage::Deleting,
                       SaveOperationStage::CommitStarted},
    };
    OperationId operation = 100;
    for (const auto &stages : cases) {
        auto controller = Operation(stages.kind, operation++);
        CHECK(controller.BeginCommit() == SaveCommitGateResult::NotReady);
        CHECK(controller.PublishProgress(stages.first, {0, 1}) == SaveOperationTransitionResult::Applied);
        CHECK(controller.PublishProgress(stages.committed, {0, 1}) == SaveOperationTransitionResult::InvalidTransition);
        CHECK(controller.PublishProgress(stages.ready, {1, 2}) == SaveOperationTransitionResult::Applied);
        CHECK(controller.BeginCommit() == SaveCommitGateResult::NotReady);
        CHECK(controller.PublishProgress(stages.ready, {2, 2}) == SaveOperationTransitionResult::Applied);
        if (stages.first != stages.ready)
            CHECK(controller.PublishProgress(stages.first, {1, 1}) == SaveOperationTransitionResult::InvalidTransition);
        CHECK(controller.BeginCommit() == SaveCommitGateResult::Entered);
        CHECK(controller.PublishProgress(stages.first, {1, 1}) == SaveOperationTransitionResult::InvalidTransition);
        CHECK(controller.PublishProgress(stages.committed, {1, 1}) == SaveOperationTransitionResult::Applied);
        CHECK(controller.Complete(SaveOperationCommitOutcome::Committed) == SaveOperationTransitionResult::Applied);
    }

    auto query = Operation(SaveOperationKind::RefreshCatalog, operation);
    CHECK(query.BeginCommit() == SaveCommitGateResult::NotRequired);
    CHECK(query.PublishProgress(SaveOperationStage::RefreshingCatalog, {1, 1}) == SaveOperationTransitionResult::Applied);
    CHECK(query.Complete(SaveOperationCommitOutcome::NotCommitted) == SaveOperationTransitionResult::Applied);
}

TEST_CASE("Deadlines and parent cancellation are observed before commit", "[unit][runtime][save][operation]") {
    const auto now = std::chrono::steady_clock::now();
    auto expired = Operation(SaveOperationKind::Save, 41, 2, now);
    const auto expiredHandle = expired.Handle();
    CHECK(expired.ObserveCancellation(now) == SaveCancellationObservation::Cancelled);
    const auto deadlineTerminal = Snapshot(expiredHandle);
    CHECK(deadlineTerminal.cancellationReason == SaveCancellationReason::Deadline);
    RequireError(deadlineTerminal.terminalError, SaveErrors::OperationDeadlineExceeded);

    CancellationSource parent;
    auto child = Operation(SaveOperationKind::Save, 42, 2, std::nullopt, parent.Token());
    const auto childHandle = child.Handle();
    parent.RequestCancellation();
    CHECK(child.PublishProgress(SaveOperationStage::CapturingSnapshot, {0, 1}) == SaveOperationTransitionResult::CancellationWon);
    CHECK(Snapshot(childHandle).cancellationReason == SaveCancellationReason::Parent);
}

TEST_CASE("Completion callbacks are bounded reentrant and immediate after terminal", "[unit][runtime][save][operation]") {
    auto controller = Operation(SaveOperationKind::RefreshCatalog, 41, 1);
    const auto handle = controller.Handle();
    std::atomic<int> calls{};
    std::atomic<int> reentrantCalls{};
    std::atomic<bool> reentrantRegistrationSucceeded{};
    REQUIRE(handle
                .OnCompletion([handle, &calls, &reentrantCalls, &reentrantRegistrationSucceeded](const SaveOperationSnapshot &terminal) {
        CHECK(terminal.IsTerminal());
        CHECK(handle.Snapshot()->revision == terminal.revision);
        reentrantRegistrationSucceeded = handle
                                             .OnCompletion([&reentrantCalls](const SaveOperationSnapshot &nestedTerminal) {
            if (nestedTerminal.IsTerminal())
                ++reentrantCalls;
        }).HasValue();
        ++calls;
    }).HasValue());
    const auto excess = handle.OnCompletion([](const SaveOperationSnapshot &) {
    });
    REQUIRE(excess.HasError());
    CHECK(excess.ErrorValue().code.Value() == SaveErrors::OperationCallbackCapacityExceeded.code.Value());
    CHECK(controller.Complete(SaveOperationCommitOutcome::NotCommitted) == SaveOperationTransitionResult::Applied);
    CHECK(calls.load() == 1);
    CHECK(reentrantRegistrationSucceeded.load());
    CHECK(reentrantCalls.load() == 1);

    REQUIRE(handle
                .OnCompletion([&calls](const SaveOperationSnapshot &) {
        ++calls;
    }).HasValue());
    CHECK(calls.load() == 2);
    CHECK(handle.RequestCancellation() == SaveCancellationRequestResult::AlreadyTerminal);
}

TEST_CASE("Late callback retains terminal state across self-release", "[unit][runtime][save][operation]") {
    auto controller = Operation(SaveOperationKind::RefreshCatalog, 45);
    SaveOperationHandle handle = controller.Handle();
    CHECK(controller.Complete(SaveOperationCommitOutcome::NotCommitted) == SaveOperationTransitionResult::Applied);
    std::atomic<bool> observed{};
    REQUIRE(handle
                .OnCompletion([&handle, &observed](const SaveOperationSnapshot &terminal) {
        handle = {};
        observed = terminal.IsTerminal() && terminal.operation == 45;
    }).HasValue());
    CHECK(observed.load());
    CHECK_FALSE(handle.IsValid());
}

TEST_CASE("Completion callback exceptions do not suppress later observers", "[unit][runtime][save][operation]") {
    auto controller = Operation(SaveOperationKind::RefreshCatalog, 46, 2);
    const auto handle = controller.Handle();
    std::atomic<int> calls{};
    REQUIRE(handle
                .OnCompletion([](const SaveOperationSnapshot &) {
        throw 7;
    }).HasValue());
    REQUIRE(handle
                .OnCompletion([&calls](const SaveOperationSnapshot &) {
        ++calls;
    }).HasValue());
    CHECK(controller.Complete(SaveOperationCommitOutcome::NotCommitted) == SaveOperationTransitionResult::Applied);
    CHECK(calls.load() == 1);
    CHECK(handle
              .OnCompletion([](const SaveOperationSnapshot &) {
        throw 9;
    }).HasValue());
}

TEST_CASE("Controller move assignment installs replacement before abandonment callbacks", "[unit][runtime][save][operation]") {
    auto target = Operation(SaveOperationKind::RefreshCatalog, 47);
    auto source = Operation(SaveOperationKind::RefreshCatalog, 48);
    const auto abandoned = target.Handle();
    const auto replaced = source.Handle();
    SaveOperationHandle reentrant;
    REQUIRE(abandoned
                .OnCompletion([&target, &reentrant](const SaveOperationSnapshot &) {
        auto nested = Operation(SaveOperationKind::RefreshCatalog, 49);
        reentrant = nested.Handle();
        target = std::move(nested);
    }).HasValue());

    target = std::move(source);
    CHECK(Snapshot(abandoned).state == SaveOperationState::Failed);
    CHECK(Snapshot(replaced).state == SaveOperationState::Failed);
    CHECK(target.Handle().Id() == 49);
    CHECK(reentrant.Id() == 49);
}

TEST_CASE("Observer registration racing completion dispatches every accepted callback exactly once", "[unit][runtime][save][operation]") {
    constexpr int observerCount = 32;
    auto controller = Operation(SaveOperationKind::RefreshCatalog, 44, observerCount);
    const auto handle = controller.Handle();
    std::atomic<int> callbacks{};
    std::atomic<int> invalidSnapshots{};
    std::atomic<int> registrationFailures{};
    std::barrier start{observerCount + 1};
    std::vector<std::thread> observers;
    observers.reserve(observerCount);
    for (int observer = 0; observer < observerCount; ++observer) {
        observers.emplace_back([&] {
            start.arrive_and_wait();
            const auto registered = handle.OnCompletion([&](const SaveOperationSnapshot &terminal) {
                if (!terminal.IsTerminal() || !handle.Snapshot()->IsTerminal())
                    ++invalidSnapshots;
                ++callbacks;
            });
            if (registered.HasError())
                ++registrationFailures;
        });
    }

    start.arrive_and_wait();
    CHECK(controller.Complete(SaveOperationCommitOutcome::NotCommitted) == SaveOperationTransitionResult::Applied);
    for (auto &observer : observers)
        observer.join();

    CHECK(registrationFailures.load() == 0);
    CHECK(invalidSnapshots.load() == 0);
    CHECK(callbacks.load() == observerCount);
}

TEST_CASE("Post-gate failure preserves unknown publication evidence and the original cause", "[unit][runtime][save][operation]") {
    auto controller = Operation(SaveOperationKind::Delete);
    const auto handle = controller.Handle();
    CHECK(controller.PublishProgress(SaveOperationStage::Deleting, {1, 1}) == SaveOperationTransitionResult::Applied);
    CHECK(controller.BeginCommit() == SaveCommitGateResult::Entered);
    CHECK(controller.Fail(MakeError(SaveErrors::CompositionInjectedFailure), SaveOperationCommitOutcome::Unknown) ==
          SaveOperationTransitionResult::Applied);

    const auto terminal = Snapshot(handle);
    CHECK(terminal.state == SaveOperationState::Failed);
    CHECK(terminal.commit == SaveOperationCommitOutcome::Unknown);
    RequireError(terminal.terminalError, SaveErrors::CompositionInjectedFailure);

    auto preCommit = Operation(SaveOperationKind::Save, 42);
    CHECK(preCommit.Fail(MakeError(SaveErrors::CompositionInjectedFailure), SaveOperationCommitOutcome::Unknown) ==
          SaveOperationTransitionResult::InvalidTransition);
}

TEST_CASE("Invalid commit outcome representations never publish terminal state", "[unit][runtime][save][operation]") {
    constexpr auto invalidOutcome = static_cast<SaveOperationCommitOutcome>(0xffU);
    auto query = Operation(SaveOperationKind::RefreshCatalog, 50);
    const auto queryHandle = query.Handle();
    CHECK(query.Complete(invalidOutcome) == SaveOperationTransitionResult::InvalidTransition);
    CHECK(query.Fail(MakeError(SaveErrors::CompositionInjectedFailure), invalidOutcome) ==
          SaveOperationTransitionResult::InvalidTransition);
    CHECK_FALSE(Snapshot(queryHandle).IsTerminal());

    auto mutation = Operation(SaveOperationKind::Delete, 52);
    const auto mutationHandle = mutation.Handle();
    REQUIRE(mutation.PublishProgress(SaveOperationStage::Deleting, {1, 1}) == SaveOperationTransitionResult::Applied);
    REQUIRE(mutation.BeginCommit() == SaveCommitGateResult::Entered);
    CHECK(mutation.Complete(invalidOutcome) == SaveOperationTransitionResult::InvalidTransition);
    CHECK(mutation.Fail(MakeError(SaveErrors::CompositionInjectedFailure), invalidOutcome) ==
          SaveOperationTransitionResult::InvalidTransition);
    CHECK_FALSE(Snapshot(mutationHandle).IsTerminal());
    CHECK(mutation.Fail(MakeError(SaveErrors::CompositionInjectedFailure), SaveOperationCommitOutcome::Unknown) ==
          SaveOperationTransitionResult::Applied);
}

TEST_CASE("Operation admission maps every allocation point to typed failure", "[unit][runtime][save][operation]") {
    const SaveOperationDescriptor descriptor{.operation = 51, .kind = SaveOperationKind::Save, .maximumCompletionCallbacks = 1};
    const auto admissionAllocations = [&] {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        auto created = CreateSaveOperation(descriptor);
        const auto requests = measurement.Snapshot().requests;
        REQUIRE(created.HasValue());
        return requests;
    }();
    REQUIRE(admissionAllocations > 0);
    bool admitted = false;
    for (std::size_t successfulAllocations = 0; successfulAllocations <= admissionAllocations; ++successfulAllocations) {
        INFO(successfulAllocations);
        auto created = [&] {
            Tests::AllocationProbe::ScopedFailure failure{successfulAllocations};
            return CreateSaveOperation(descriptor);
        }();
        admitted = created.HasValue();
        CHECK(admitted == (successfulAllocations == admissionAllocations));
        if (!admitted)
            CHECK(created.ErrorValue().code.Value() == SaveErrors::OperationAllocationFailed.code.Value());
    }
    CHECK(admitted);
}

TEST_CASE("Terminal transitions and abandonment allocate no storage after admission", "[unit][runtime][save][operation]") {
    auto cancelled = Operation(SaveOperationKind::Save, 53);
    const auto cancelledHandle = cancelled.Handle();
    SaveCancellationRequestResult request;
    SaveCommitGateResult cancelledGate;
    std::size_t cancellationAllocations{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        Tests::AllocationProbe::ScopedFailure failure;
        request = cancelledHandle.RequestCancellation();
        cancelledGate = cancelled.BeginCommit();
        cancellationAllocations = measurement.Snapshot().requests;
    }
    CHECK(cancellationAllocations == 0);
    CHECK(request == SaveCancellationRequestResult::Requested);
    CHECK(cancelledGate == SaveCommitGateResult::CancellationWon);

    auto completed = Operation(SaveOperationKind::RefreshCatalog, 54);
    SaveOperationTransitionResult completion;
    std::size_t completionAllocations{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        Tests::AllocationProbe::ScopedFailure failure;
        completion = completed.Complete(SaveOperationCommitOutcome::NotCommitted);
        completionAllocations = measurement.Snapshot().requests;
    }
    CHECK(completionAllocations == 0);
    CHECK(completion == SaveOperationTransitionResult::Applied);

    auto failed = Operation(SaveOperationKind::Save, 55);
    SaveOperationTransitionResult failureResult;
    Error failureError = MakeError(SaveErrors::CompositionInjectedFailure);
    std::size_t failureAllocations{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        Tests::AllocationProbe::ScopedFailure failure;
        failureResult = failed.Fail(std::move(failureError), SaveOperationCommitOutcome::NotCommitted);
        failureAllocations = measurement.Snapshot().requests;
    }
    CHECK(failureAllocations == 0);
    CHECK(failureResult == SaveOperationTransitionResult::Applied);

    auto abandonedController = Operation(SaveOperationKind::Save, 56);
    const SaveOperationHandle abandoned = abandonedController.Handle();
    std::optional<SaveOperationController> owner{std::move(abandonedController)};
    std::size_t abandonmentAllocations{};
    {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        Tests::AllocationProbe::ScopedFailure failure;
        owner.reset();
        abandonmentAllocations = measurement.Snapshot().requests;
    }
    CHECK(abandonmentAllocations == 0);
    CHECK(Snapshot(abandoned).state == SaveOperationState::Failed);
}

TEST_CASE("Producer release terminalizes an abandoned operation", "[unit][runtime][save][operation]") {
    SaveOperationHandle handle;
    std::atomic<int> calls{};
    std::atomic<int> reentrantCalls{};
    {
        auto controller = Operation();
        handle = controller.Handle();
        REQUIRE(handle
                    .OnCompletion([handle, &calls, &reentrantCalls](const SaveOperationSnapshot &terminal) {
            if (terminal.IsTerminal() && handle.Snapshot()->revision == terminal.revision) {
                static_cast<void>(handle.OnCompletion([&reentrantCalls](const SaveOperationSnapshot &) {
                    ++reentrantCalls;
                }));
            }
            ++calls;
        }).HasValue());
    }
    const auto terminal = Snapshot(handle);
    CHECK(terminal.state == SaveOperationState::Failed);
    CHECK(terminal.commit == SaveOperationCommitOutcome::NotCommitted);
    RequireError(terminal.terminalError, SaveErrors::OperationAbandoned);
    CHECK(calls.load() == 1);
    CHECK(reentrantCalls.load() == 1);
}

TEST_CASE("Cancellation and completion races produce exactly one terminal callback", "[unit][runtime][save][operation]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto controller = Operation(SaveOperationKind::RefreshCatalog, static_cast<OperationId>(iteration + 1));
        std::atomic<int> callbacks{};
        const SaveOperationHandle handle = CountCompletions(controller, callbacks);
        std::barrier start{2};
        std::thread cancellation([&] {
            start.arrive_and_wait();
            static_cast<void>(handle.RequestCancellation());
        });
        start.arrive_and_wait();
        const auto completion = controller.Complete(SaveOperationCommitOutcome::NotCommitted);
        cancellation.join();

        const auto terminal = Snapshot(handle);
        CHECK(terminal.IsTerminal());
        CHECK((terminal.state == SaveOperationState::Completed || terminal.state == SaveOperationState::Cancelled));
        CHECK((completion == SaveOperationTransitionResult::Applied || completion == SaveOperationTransitionResult::CancellationWon));
        CHECK(callbacks.load() == 1);
    }
}

TEST_CASE("Cancellation and commit gate races have one atomic winner", "[unit][runtime][save][operation]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto controller = Operation(SaveOperationKind::Delete, static_cast<OperationId>(iteration + 200));
        std::atomic<int> callbacks{};
        const SaveOperationHandle handle = CountCompletions(controller, callbacks);
        REQUIRE(controller.PublishProgress(SaveOperationStage::Deleting, {1, 1}) == SaveOperationTransitionResult::Applied);
        std::barrier start{2};
        SaveCancellationRequestResult cancellation = SaveCancellationRequestResult::InvalidHandle;
        std::thread requester([&] {
            start.arrive_and_wait();
            cancellation = handle.RequestCancellation();
        });
        start.arrive_and_wait();
        const SaveCommitGateResult gate = controller.BeginCommit();
        requester.join();

        if (gate == SaveCommitGateResult::Entered) {
            CHECK(cancellation == SaveCancellationRequestResult::TooLate);
            CHECK_FALSE(Snapshot(handle).cancellable);
            CHECK(controller.Fail(MakeError(SaveErrors::CompositionInjectedFailure), SaveOperationCommitOutcome::Unknown) ==
                  SaveOperationTransitionResult::Applied);
        } else {
            CHECK(gate == SaveCommitGateResult::CancellationWon);
            CHECK(cancellation == SaveCancellationRequestResult::Requested);
            CHECK(Snapshot(handle).state == SaveOperationState::Cancelled);
        }
        CHECK(callbacks.load() == 1);
    }
}

TEST_CASE("Operation errors expose stable actionable descriptors", "[unit][runtime][save][operation]") {
    const ErrorCodeDescriptor *descriptors[]{&SaveErrors::OperationInvalid,           &SaveErrors::OperationAllocationFailed,
                                             &SaveErrors::OperationTransitionInvalid, &SaveErrors::OperationCallbackCapacityExceeded,
                                             &SaveErrors::OperationCallbackInvalid,   &SaveErrors::OperationCancelled,
                                             &SaveErrors::OperationDeadlineExceeded,  &SaveErrors::OperationAbandoned};
    for (const auto *descriptor : descriptors) {
        CHECK(descriptor->domain.Value() == "horo.save");
        CHECK_FALSE(descriptor->code.Value().empty());
        CHECK_FALSE(descriptor->summary.empty());
        CHECK_FALSE(descriptor->remediationHint.empty());
    }
}
