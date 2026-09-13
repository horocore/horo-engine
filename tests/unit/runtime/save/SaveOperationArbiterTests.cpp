#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveOperationArbiter.h"
#include "unit/runtime/save/SaveTestUtils.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Test;

    SaveNamespaceId NameSpace() {
        return {.product = Id<ProductStorageId>(1),
                .environment = Id<EnvironmentStorageId>(2),
                .owner = ServerWorldOwner{.owner = Id<ServerStorageOwnerId>(3)}};
    }

    SaveArbiterRequest Request(const OperationId operation, const SaveOperationKind kind = SaveOperationKind::Save,
                               const std::uint8_t slot = 1, const SaveArbiterPriority priority = SaveArbiterPriority::Normal,
                               const SaveArbiterConflictPolicy conflict = SaveArbiterConflictPolicy::Queue,
                               const SavePolicyMode mode = SavePolicyMode::Manual) {
        return {.operation = {.operation = operation, .kind = kind, .maximumCompletionCallbacks = 4},
                .mode = mode,
                .address = kind == SaveOperationKind::RefreshCatalog
                               ? std::nullopt
                               : std::optional<SaveArbiterAddress>{{.nameSpace = NameSpace(), .slot = Id<SaveGameSlotId>(slot)}},
                .priority = priority,
                .conflict = conflict};
    }

    SaveOperationArbiter Arbiter(const std::size_t capacity = 8) {
        auto created = CreateSaveOperationArbiter({.maximumRetainedOperations = capacity});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    SaveArbiterAdmission Admit(SaveOperationArbiter &arbiter, SaveArbiterRequest request) {
        auto admitted = arbiter.Admit(std::move(request));
        REQUIRE(admitted.HasValue());
        return std::move(admitted).Value();
    }

    SaveArbiterSnapshot Snapshot(const SaveOperationArbiter &arbiter, const OperationId operation) {
        const auto snapshot = arbiter.Snapshot(operation);
        REQUIRE(snapshot.has_value());
        return *snapshot;
    }

    void RequireError(const Error &error, const ErrorCodeDescriptor &descriptor) {
        CHECK(error.domain.Value() == descriptor.domain.Value());
        CHECK(error.code.Value() == descriptor.code.Value());
    }
}  // namespace

TEST_CASE("Save operation arbiter validates bounded typed admission", "[unit][runtime][save][arbiter]") {
    const auto zeroCapacity = CreateSaveOperationArbiter({});
    REQUIRE(zeroCapacity.HasError());
    RequireError(zeroCapacity.ErrorValue(), SaveErrors::ArbiterInvalid);

    auto arbiter = Arbiter(1);
    auto malformed = Request(0);
    const auto invalid = arbiter.Admit(std::move(malformed));
    REQUIRE(invalid.HasError());
    RequireError(invalid.ErrorValue(), SaveErrors::ArbiterInvalid);

    Admit(arbiter, Request(1));
    const auto duplicate = arbiter.Admit(Request(1));
    REQUIRE(duplicate.HasError());
    RequireError(duplicate.ErrorValue(), SaveErrors::ArbiterInvalid);
    const auto full = arbiter.Admit(Request(2, SaveOperationKind::Save, 2));
    REQUIRE(full.HasError());
    RequireError(full.ErrorValue(), SaveErrors::ArbiterCapacityExceeded);
}

TEST_CASE("Save operation arbiter enforces save state transitions and immutable terminal snapshots", "[unit][runtime][save][arbiter]") {
    auto arbiter = Arbiter();
    Admit(arbiter, Request(11));
    const auto queued = arbiter.StartNext();
    REQUIRE(queued.has_value());
    CHECK(queued->operation.operation == 11);
    CHECK(arbiter.ActiveOperation() == 11);

    const auto skipped = arbiter.Advance(11, SaveArbiterState::Encoding, {1, 1});
    REQUIRE(skipped.HasError());
    RequireError(skipped.ErrorValue(), SaveErrors::ArbiterInvalid);
    REQUIRE(arbiter.Advance(11, SaveArbiterState::WaitingForSafePoint).HasValue());
    REQUIRE(arbiter.Advance(11, SaveArbiterState::Capturing, {1, 1}).HasValue());
    const auto captured = Snapshot(arbiter, 11);
    REQUIRE(arbiter.Advance(11, SaveArbiterState::Encoding, {1, 1}).HasValue());
    REQUIRE(arbiter.Advance(11, SaveArbiterState::Committing, {1, 1}).HasValue());
    REQUIRE(arbiter.Advance(11, SaveArbiterState::Committing, {2, 2}).HasValue());
    REQUIRE(arbiter.Complete(11).HasValue());

    const auto terminal = Snapshot(arbiter, 11);
    CHECK(terminal.state == SaveArbiterState::Terminal);
    CHECK(terminal.operation.state == SaveOperationState::Completed);
    CHECK(terminal.operation.commit == SaveOperationCommitOutcome::Committed);
    CHECK(captured.state == SaveArbiterState::Capturing);
    CHECK(captured.revision < terminal.revision);
    CHECK_FALSE(arbiter.ActiveOperation().has_value());
    CHECK(arbiter.Acknowledge(11));
    CHECK_FALSE(arbiter.Snapshot(11).has_value());
}

TEST_CASE("Save operation arbiter models load activation and query completion", "[unit][runtime][save][arbiter]") {
    auto arbiter = Arbiter();
    Admit(arbiter, Request(21, SaveOperationKind::Load));
    REQUIRE(arbiter.StartNext().has_value());
    REQUIRE(arbiter.Advance(21, SaveArbiterState::Loading, {1, 1}).HasValue());
    REQUIRE(arbiter.Advance(21, SaveArbiterState::WaitingForSafePoint).HasValue());
    REQUIRE(arbiter.Advance(21, SaveArbiterState::Activating, {1, 1}).HasValue());
    REQUIRE(arbiter.Advance(21, SaveArbiterState::Activating, {2, 2}).HasValue());
    REQUIRE(arbiter.Complete(21).HasValue());
    CHECK(Snapshot(arbiter, 21).operation.commit == SaveOperationCommitOutcome::Committed);

    Admit(arbiter, Request(22, SaveOperationKind::RefreshCatalog));
    REQUIRE(arbiter.StartNext().has_value());
    REQUIRE(arbiter.Advance(22, SaveArbiterState::Loading, {1, 1}).HasValue());
    REQUIRE(arbiter.Advance(22, SaveArbiterState::Loading, {2, 2}).HasValue());
    REQUIRE(arbiter.Complete(22).HasValue());
    CHECK(Snapshot(arbiter, 22).operation.commit == SaveOperationCommitOutcome::NotCommitted);
}

TEST_CASE("Save operation arbiter completes delete and preserves typed failures", "[unit][runtime][save][arbiter]") {
    auto arbiter = Arbiter();
    CHECK_FALSE(arbiter.StartNext().has_value());
    CHECK_FALSE(arbiter.Snapshot(71).has_value());
    CHECK_FALSE(arbiter.Acknowledge(71));
    CHECK(arbiter.Cancel(71) == SaveCancellationRequestResult::InvalidHandle);
    REQUIRE(arbiter.ObserveCancellation(71).HasError());

    Admit(arbiter, Request(71, SaveOperationKind::Delete));
    CHECK_FALSE(arbiter.Acknowledge(71));
    REQUIRE(arbiter.StartNext().has_value());
    REQUIRE(arbiter.Advance(71, SaveArbiterState::Committing, {1, 1}).HasValue());
    CHECK(arbiter.Cancel(71) == SaveCancellationRequestResult::TooLate);
    REQUIRE(arbiter.Complete(71).HasValue());
    CHECK(Snapshot(arbiter, 71).operation.state == SaveOperationState::Completed);

    Admit(arbiter, Request(72, SaveOperationKind::Load, 2));
    REQUIRE(arbiter.StartNext().has_value());
    REQUIRE(arbiter.Advance(72, SaveArbiterState::Loading, {1, 2}).HasValue());
    REQUIRE(arbiter.Fail(72, MakeError(SaveErrors::CompositionInjectedFailure)).HasValue());
    const auto failed = Snapshot(arbiter, 72);
    CHECK(failed.operation.state == SaveOperationState::Failed);
    REQUIRE(failed.operation.terminalError.has_value());
    RequireError(*failed.operation.terminalError, SaveErrors::CompositionInjectedFailure);

    const auto query = Admit(arbiter, Request(73, SaveOperationKind::RefreshCatalog));
    REQUIRE(arbiter.StartNext().has_value());
    REQUIRE(arbiter.Advance(73, SaveArbiterState::Loading, {1, 1}).HasValue());
    CHECK(query.handle.RequestCancellation() == SaveCancellationRequestResult::Requested);
    REQUIRE(arbiter.Complete(73).HasValue());
    CHECK(Snapshot(arbiter, 73).operation.state == SaveOperationState::Cancelled);
}

TEST_CASE("Save operation arbiter selects priority then FIFO without preemption", "[unit][runtime][save][arbiter]") {
    auto arbiter = Arbiter();
    Admit(arbiter, Request(31, SaveOperationKind::Save, 1, SaveArbiterPriority::Background));
    Admit(arbiter, Request(32, SaveOperationKind::Save, 2, SaveArbiterPriority::UserBlocking));
    Admit(arbiter, Request(33, SaveOperationKind::Save, 3, SaveArbiterPriority::UserBlocking));
    CHECK(arbiter.QueuedCount() == 3);
    CHECK(arbiter.StartNext()->operation.operation == 32);
    CHECK(arbiter.StartNext() == std::nullopt);
    CHECK(arbiter.Cancel(32) == SaveCancellationRequestResult::Requested);
    CHECK(Snapshot(arbiter, 32).state == SaveArbiterState::Cancelling);
    REQUIRE(arbiter.ObserveCancellation(32).HasValue());
    CHECK(Snapshot(arbiter, 32).state == SaveArbiterState::Terminal);
    CHECK(arbiter.StartNext()->operation.operation == 33);
    CHECK(arbiter.Cancel(33) == SaveCancellationRequestResult::Requested);
    REQUIRE(arbiter.ObserveCancellation(33).HasValue());
    CHECK(arbiter.StartNext()->operation.operation == 31);
}

TEST_CASE("Save operation arbiter applies explicit rejection coalescing and replacement", "[unit][runtime][save][arbiter]") {
    auto arbiter = Arbiter();
    const auto first = Admit(arbiter, Request(41));
    CHECK(first.disposition == SaveArbiterAdmissionDisposition::Accepted);

    const auto coalesced = Admit(arbiter, Request(42, SaveOperationKind::Save, 1, SaveArbiterPriority::Background,
                                                  SaveArbiterConflictPolicy::CoalesceEquivalent));
    CHECK(coalesced.disposition == SaveArbiterAdmissionDisposition::Coalesced);
    CHECK(coalesced.handle.Id() == 41);
    CHECK(arbiter.QueuedCount() == 1);

    const auto replacement = Admit(arbiter, Request(43, SaveOperationKind::Save, 1, SaveArbiterPriority::UserBlocking,
                                                    SaveArbiterConflictPolicy::ReplaceQueuedEquivalent));
    CHECK(replacement.disposition == SaveArbiterAdmissionDisposition::ReplacedQueued);
    CHECK(replacement.replacedOperation == 41);
    CHECK(Snapshot(arbiter, 41).operation.state == SaveOperationState::Cancelled);
    CHECK(arbiter.QueuedCount() == 1);

    REQUIRE(arbiter.StartNext().has_value());
    const auto rejected =
        arbiter.Admit(Request(44, SaveOperationKind::Load, 1, SaveArbiterPriority::Normal, SaveArbiterConflictPolicy::Reject));
    REQUIRE(rejected.HasError());
    RequireError(rejected.ErrorValue(), SaveErrors::OperationInProgress);
    CHECK(rejected.ErrorValue().message.find("43") != std::string::npos);
}

TEST_CASE("Save operation arbiter reports active conflicts while coalescing the queued equivalent", "[unit][runtime][save][arbiter]") {
    auto arbiter = Arbiter();
    Admit(arbiter, Request(45, SaveOperationKind::Load, 1, SaveArbiterPriority::Background));
    Admit(arbiter, Request(46, SaveOperationKind::Save, 1, SaveArbiterPriority::UserBlocking));
    CHECK(arbiter.StartNext()->operation.operation == 46);

    const auto coalesced =
        Admit(arbiter, Request(47, SaveOperationKind::Load, 1, SaveArbiterPriority::Normal, SaveArbiterConflictPolicy::CoalesceEquivalent));
    CHECK(coalesced.disposition == SaveArbiterAdmissionDisposition::Coalesced);
    CHECK(coalesced.handle.Id() == 45);

    const auto rejected =
        arbiter.Admit(Request(48, SaveOperationKind::Load, 1, SaveArbiterPriority::Normal, SaveArbiterConflictPolicy::Reject));
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().message.find("46") != std::string::npos);
}

TEST_CASE("Save operation arbiter keeps producer ownership independent from observers", "[unit][runtime][save][arbiter]") {
    SaveOperationHandle survivor;
    std::atomic<int> completions{};
    {
        auto arbiter = Arbiter();
        auto admission = Admit(arbiter, Request(51));
        survivor = admission.handle;
        REQUIRE(admission.handle
                    .OnCompletion([&completions](const SaveOperationSnapshot &) {
            ++completions;
        }).HasValue());
        admission.handle = {};
        REQUIRE(arbiter.StartNext().has_value());
        REQUIRE(arbiter.Advance(51, SaveArbiterState::WaitingForSafePoint).HasValue());
        REQUIRE(arbiter.Advance(51, SaveArbiterState::Capturing, {1, 1}).HasValue());
        REQUIRE(arbiter.Advance(51, SaveArbiterState::Encoding, {1, 1}).HasValue());
        REQUIRE(arbiter.Advance(51, SaveArbiterState::Committing, {1, 1}).HasValue());
        REQUIRE(arbiter.Complete(51).HasValue());
        CHECK(completions.load() == 1);
    }
    REQUIRE(survivor.Snapshot().has_value());
    CHECK(survivor.Snapshot()->state == SaveOperationState::Completed);
    CHECK(completions.load() == 1);
}

TEST_CASE("Destroying the arbiter terminalizes every still accepted request", "[unit][runtime][save][arbiter]") {
    SaveOperationHandle active;
    SaveOperationHandle queued;
    {
        auto arbiter = Arbiter();
        active = Admit(arbiter, Request(61)).handle;
        queued = Admit(arbiter, Request(62, SaveOperationKind::Save, 2)).handle;
        REQUIRE(arbiter.StartNext().has_value());
        REQUIRE(arbiter.Advance(61, SaveArbiterState::WaitingForSafePoint).HasValue());
    }
    REQUIRE(active.Snapshot().has_value());
    REQUIRE(queued.Snapshot().has_value());
    CHECK(active.Snapshot()->state == SaveOperationState::Failed);
    CHECK(queued.Snapshot()->state == SaveOperationState::Failed);
    RequireError(*active.Snapshot()->terminalError, SaveErrors::OperationAbandoned);
    RequireError(*queued.Snapshot()->terminalError, SaveErrors::OperationAbandoned);
}
