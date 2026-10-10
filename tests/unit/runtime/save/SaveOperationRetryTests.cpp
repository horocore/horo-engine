#include "SaveOperationArbiterTestSupport.h"

#include <array>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

using namespace Horo::Runtime::ArbiterTestSupport;

namespace {
    using AdmissionMutation = void (*)(SaveArbiterRequest &);
    constexpr std::array<AdmissionMutation, 14> InvalidAdmissions{[](SaveArbiterRequest &request) {
        request.retry->policy.maximumRetries = MaximumSaveStorageAutomaticRetries + 1;
    }, [](SaveArbiterRequest &request) {
        request.retry->policy.initialBackoffMilliseconds = 0;
    }, [](SaveArbiterRequest &request) {
        request.retry->policy.maximumBackoffMilliseconds = 1;
    }, [](SaveArbiterRequest &request) {
        request.retry->policy.maximumElapsedMilliseconds = 0;
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.runtime = {};
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.access.expectedRevision = 0;
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.publicationGeneration = {};
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.slot = Id<SaveGameSlotId>(99);
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.slot = {};
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.compatibilityRevision = 0;
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.bindingState = SaveNamespaceBindingState::NoActiveProfile;
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.bindingState = static_cast<SaveNamespaceBindingState>(255);
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.authorized = false;
    }, [](SaveArbiterRequest &request) {
        request.retry->preconditions.access.expected.environment = Id<EnvironmentStorageId>(99);
    }};

    using EvidenceMutation = void (*)(SaveArbiterRetryPreconditions &);
    constexpr std::array<EvidenceMutation, 12> ChangedEvidence{[](SaveArbiterRetryPreconditions &facts) {
        ++facts.access.expectedRevision;
    }, [](SaveArbiterRetryPreconditions &facts) {
        facts.access.expected.environment = Id<EnvironmentStorageId>(99);
    }, [](SaveArbiterRetryPreconditions &facts) {
        ++facts.runtime.runtime;
    }, [](SaveArbiterRetryPreconditions &facts) {
        ++facts.runtime.scene;
    }, [](SaveArbiterRetryPreconditions &facts) {
        ++facts.runtime.registry;
    }, [](SaveArbiterRetryPreconditions &facts) {
        ++facts.catalogRevision;
    }, [](SaveArbiterRetryPreconditions &facts) {
        facts.expectedGeneration = Id<SlotGenerationId>(12);
    }, [](SaveArbiterRetryPreconditions &facts) {
        facts.publicationGeneration = Id<SlotGenerationId>(13);
    }, [](SaveArbiterRetryPreconditions &facts) {
        facts.authorized = false;
    }, [](SaveArbiterRetryPreconditions &facts) {
        facts.slot = Id<SaveGameSlotId>(99);
    }, [](SaveArbiterRetryPreconditions &facts) {
        facts.bindingState = SaveNamespaceBindingState::NoActiveProfile;
    }, [](SaveArbiterRetryPreconditions &facts) {
        ++facts.compatibilityRevision;
    }};

    SaveArbiterRequest RetryRequest(const OperationId operation = 501) {
        auto request = Request(operation, SaveOperationKind::Save, 1, SaveArbiterPriority::Background, SaveArbiterConflictPolicy::Reject,
                               SavePolicyMode::Auto);
        request.retry = SaveArbiterRetryDescriptor{.policy = {3, 10, 25, 100},
                                                   .preconditions = {.access = {NameSpace(), 7},
                                                                     .runtime = {3, 4, 5},
                                                                     .catalogRevision = 1,
                                                                     .slot = Id<SaveGameSlotId>(1),
                                                                     .publicationGeneration = Id<SlotGenerationId>(9)}};
        return request;
    }

    SaveOperationHandle BeginRetrySave(SaveOperationArbiter &arbiter, SaveArbiterRequest request) {
        const auto operation = request.operation.operation;
        const auto handle = Admit(arbiter, std::move(request)).handle;
        REQUIRE(arbiter.StartNext().has_value());
        REQUIRE(arbiter.Advance(operation, SaveArbiterState::WaitingForSafePoint).HasValue());
        REQUIRE(arbiter.Advance(operation, SaveArbiterState::Capturing, {1, 1}).HasValue());
        REQUIRE(arbiter.Advance(operation, SaveArbiterState::Encoding, {1, 1}).HasValue());
        return handle;
    }

    Result<bool> Defer(SaveOperationArbiter &arbiter, const std::uint64_t clock,
                       const SaveStorageFailureCategory category = SaveStorageFailureCategory::TransientIo,
                       const SaveOperationCommitOutcome outcome = SaveOperationCommitOutcome::NotCommitted) {
        return arbiter.DeferStorageRetry(501,
                                         {.category = category,
                                          .commitOutcome = outcome,
                                          .completedAutomaticRetries = 0,
                                          .maximumAutomaticRetries = 8,
                                          .nativeCause = MakeError(SaveErrors::StorageTransientIo)},
                                         clock);
    }
}  // namespace

TEST_CASE("Storage retry retains one operation and immutable capture identity with capped exponential backoff",
          "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter(1);
    const auto request = RetryRequest();
    const auto handle = BeginRetrySave(arbiter, request);
    const auto original = *handle.Snapshot();
    std::uint64_t clock = 0;
    for (const auto delay : {10U, 20U, 25U}) {
        const auto deferred = Defer(arbiter, clock);
        REQUIRE(deferred.HasValue());
        REQUIRE(deferred.Value());
        CHECK_FALSE(handle.Snapshot()->IsTerminal());
        CHECK(handle.Id() == original.operation);
        CHECK(handle.Snapshot()->stage == original.stage);
        CHECK_FALSE(arbiter.ActiveOperation());
        CHECK_FALSE(arbiter.StartNext());
        CHECK_FALSE(arbiter.ResumeStorageRetry(501, request.retry->preconditions, clock + delay - 1).Value());
        clock += delay;
        const auto resumed = arbiter.ResumeStorageRetry(501, request.retry->preconditions, clock);
        REQUIRE(resumed.HasValue());
        REQUIRE(resumed.Value());
        CHECK(resumed.Value()->operation.operation == original.operation);
        CHECK(resumed.Value()->retry.eligibleAtMilliseconds == clock);
    }
    const auto exhausted = Defer(arbiter, clock);
    REQUIRE(exhausted.HasValue());
    CHECK_FALSE(exhausted.Value());
    CHECK(handle.Snapshot()->state == SaveOperationState::Failed);
    CHECK(arbiter.Snapshot(501)->retry.completedRetries == 3);
    const auto terminal = *handle.Snapshot();
    CHECK_FALSE(arbiter.ResumeStorageRetry(501, request.retry->preconditions, clock).Value());
    CHECK(handle.Snapshot()->revision == terminal.revision);
    CHECK(arbiter.Acknowledge(501));
    CHECK(handle.Snapshot()->IsTerminal());
}

TEST_CASE("Manual work takes the arbiter before a due background retry and does not discard it", "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    const auto request = RetryRequest();
    BeginRetrySave(arbiter, request);
    REQUIRE(Defer(arbiter, 0).Value());
    Admit(arbiter, Request(502, SaveOperationKind::Save, 2, SaveArbiterPriority::UserBlocking));
    CHECK_FALSE(arbiter.ResumeStorageRetry(501, request.retry->preconditions, 10).Value());
    REQUIRE(arbiter.StartNext()->operation.operation == 502);
    CHECK_FALSE(arbiter.ResumeStorageRetry(501, request.retry->preconditions, 10).Value());
    CHECK(arbiter.Snapshot(501)->state == SaveArbiterState::WaitingForRetry);
    REQUIRE(arbiter.Fail(502, MakeError(SaveErrors::StoragePermanentIo)).HasValue());
    REQUIRE(arbiter.ResumeStorageRetry(501, request.retry->preconditions, 10).Value());
    CHECK(arbiter.ActiveOperation() == 501);
    CHECK(arbiter.StartNext() == std::nullopt);
}

TEST_CASE("Retry revalidation fences every original namespace runtime catalog and publication precondition",
          "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    const auto request = RetryRequest();
    const auto handle = BeginRetrySave(arbiter, request);
    REQUIRE(Defer(arbiter, 0).Value());
    auto current = request.retry->preconditions;
    ChangedEvidence[GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)](current);
    CHECK_FALSE(arbiter.ResumeStorageRetry(501, current, 10).Value());
    REQUIRE(handle.Snapshot()->terminalError);
    RequireError(*handle.Snapshot()->terminalError, SaveErrors::GenerationStale);
    CHECK(handle.Snapshot()->commit == SaveOperationCommitOutcome::NotCommitted);
}

TEST_CASE("Only known uncommitted transient storage failure authorizes a bounded retry", "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    const auto handle = BeginRetrySave(arbiter, RetryRequest());
    const auto category = GENERATE(SaveStorageFailureCategory::DiskFull, SaveStorageFailureCategory::QuotaExceeded,
                                   SaveStorageFailureCategory::PermissionDenied, SaveStorageFailureCategory::ReadOnly,
                                   SaveStorageFailureCategory::VolumeUnavailable, SaveStorageFailureCategory::PermanentIo);
    CHECK_FALSE(Defer(arbiter, 0, category).Value());
    CHECK(handle.Snapshot()->IsTerminal());
    CHECK_FALSE(arbiter.ActiveOperation());
}

TEST_CASE("Retry never crosses a commit gate or replays uncertain or committed publication", "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    const auto handle = BeginRetrySave(arbiter, RetryRequest());
    REQUIRE(arbiter.Advance(501, SaveArbiterState::Committing, {1, 1}).HasValue());
    CHECK(Defer(arbiter, 0, SaveStorageFailureCategory::TransientIo, SaveOperationCommitOutcome::Committed).HasError());
    CHECK_FALSE(handle.Snapshot()->IsTerminal());
    CHECK_FALSE(Defer(arbiter, 0, SaveStorageFailureCategory::TransientIo, SaveOperationCommitOutcome::Unknown).Value());
    CHECK(handle.Snapshot()->state == SaveOperationState::Failed);
    CHECK(handle.Snapshot()->commit == SaveOperationCommitOutcome::Unknown);
}

TEST_CASE("Retry cancellation shutdown deadlines elapsed limits and clock rollback settle once", "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    auto request = RetryRequest();
    CancellationSource parent;
    request.operation.parentCancellation = parent.Token();
    const auto scenario = GENERATE(0, 1, 2, 3, 4);
    if (scenario == 4)
        request.retry->policy.maximumElapsedMilliseconds = 9;
    const auto handle = BeginRetrySave(arbiter, request);
    const auto deferred = Defer(arbiter, 5);
    REQUIRE(deferred.HasValue());
    if (scenario == 4) {
        CHECK_FALSE(deferred.Value());
        CHECK(handle.Snapshot()->state == SaveOperationState::Failed);
        return;
    }
    REQUIRE(deferred.Value());
    CHECK(arbiter.ResumeStorageRetry(501, request.retry->preconditions, 4).HasError());
    if (scenario == 0) {
        CHECK(arbiter.Cancel(501) == SaveCancellationRequestResult::Requested);
        CHECK(arbiter.Cancel(501) == SaveCancellationRequestResult::AlreadyTerminal);
    } else if (scenario == 1) {
        REQUIRE(arbiter.BeginShutdown().HasValue());
        REQUIRE(arbiter.BeginShutdown().HasValue());
        CHECK(arbiter.Admit(Request(502)).HasError());
    } else if (scenario == 2) {
        CHECK_FALSE(arbiter.ResumeStorageRetry(501, request.retry->preconditions, 100).Value());
    } else {
        parent.RequestCancellation();
    }
    CHECK_FALSE(arbiter.ResumeStorageRetry(501, request.retry->preconditions, 100).Value());
    CHECK(handle.Snapshot()->IsTerminal());
    if (scenario == 1)
        CHECK(handle.Snapshot()->cancellationReason == SaveCancellationReason::Shutdown);
    if (scenario == 3)
        CHECK(handle.Snapshot()->cancellationReason == SaveCancellationReason::Parent);
    const auto revision = handle.Snapshot()->revision;
    CHECK_FALSE(arbiter.ResumeStorageRetry(501, request.retry->preconditions, 100).Value());
    CHECK(handle.Snapshot()->revision == revision);
}

TEST_CASE("Malformed retry capabilities and terminal failures cannot be reinterpreted as new attempts",
          "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    auto request = RetryRequest();
    InvalidAdmissions[GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13)](request);
    CHECK(arbiter.Admit(request).HasError());
    CHECK_FALSE(arbiter.StartNext());
    const auto valid = RetryRequest();
    const auto handle = BeginRetrySave(arbiter, valid);
    REQUIRE(arbiter.Fail(501, MakeError(SaveErrors::ArchiveChunkHashMismatch)).HasValue());
    const auto terminal = *handle.Snapshot();
    CHECK(Defer(arbiter, 0).HasError());
    CHECK_FALSE(arbiter.ResumeStorageRetry(501, valid.retry->preconditions, 10).Value());
    CHECK(handle.Snapshot()->revision == terminal.revision);
}

TEST_CASE("Original operation deadline wins during deferred retry without dispatching or replacing a handle",
          "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    auto request = RetryRequest();
    const auto now = std::chrono::steady_clock::now();
    request.operation.deadline = now + std::chrono::hours(1);
    const auto handle = BeginRetrySave(arbiter, request);
    REQUIRE(Defer(arbiter, 0).Value());
    CHECK(handle.Snapshot()->deadline == request.operation.deadline);
    const auto resumed = arbiter.ResumeStorageRetry(501, request.retry->preconditions, 10, *request.operation.deadline);
    REQUIRE(resumed.HasValue());
    CHECK_FALSE(resumed.Value());
    CHECK(handle.Snapshot()->state == SaveOperationState::Cancelled);
    CHECK(handle.Snapshot()->cancellationReason == SaveCancellationReason::Deadline);
    CHECK(arbiter.Snapshot(501)->retry.completedRetries == 0);
}

TEST_CASE("Retry clock overflow and zero admitted budget cannot create another attempt", "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    auto request = RetryRequest();
    const auto overflow = GENERATE(false, true);
    if (overflow)
        request.retry->admittedAtMilliseconds = std::numeric_limits<std::uint64_t>::max() - 5;
    else
        request.retry->policy.maximumRetries = 0;
    const auto clock = request.retry->admittedAtMilliseconds;
    const auto handle = BeginRetrySave(arbiter, request);
    CHECK_FALSE(Defer(arbiter, clock).Value());
    CHECK(handle.Snapshot()->state == SaveOperationState::Failed);
    CHECK(arbiter.Snapshot(501)->retry.completedRetries == 0);
}

TEST_CASE("A transient label cannot override typed corruption incompatibility permissions or cancellation",
          "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    const auto handle = BeginRetrySave(arbiter, RetryRequest());
    const auto *cause = GENERATE(&SaveErrors::ArchiveChunkHashMismatch, &SaveErrors::VersionUnsupportedNewer,
                                 &SaveErrors::StoragePermissionDenied, &SaveErrors::OperationCancelled);
    const auto deferred =
        arbiter.DeferStorageRetry(501, {.category = SaveStorageFailureCategory::TransientIo, .nativeCause = MakeError(*cause)}, 0);
    REQUIRE(deferred.HasValue());
    CHECK_FALSE(deferred.Value());
    REQUIRE(handle.Snapshot()->terminalError);
    RequireError(*handle.Snapshot()->terminalError, *cause);
    CHECK(arbiter.Snapshot(501)->retry.completedRetries == 0);
}

TEST_CASE("Shutdown preserves an already entered publication and cancels active precommit work cooperatively",
          "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    const auto handle = BeginRetrySave(arbiter, RetryRequest());
    const auto committing = GENERATE(false, true);
    if (committing)
        REQUIRE(arbiter.Advance(501, SaveArbiterState::Committing, {1, 1}).HasValue());
    REQUIRE(arbiter.BeginShutdown().HasValue());
    CHECK_FALSE(handle.Snapshot()->IsTerminal());
    if (committing) {
        REQUIRE(arbiter.Complete(501).HasValue());
        CHECK(handle.Snapshot()->commit == SaveOperationCommitOutcome::Committed);
    } else {
        REQUIRE(arbiter.PollCancellation(501).Value());
        CHECK(handle.Snapshot()->cancellationReason == SaveCancellationReason::Shutdown);
    }
}

TEST_CASE("Typed terminal failure advances retry clock while invalid publication evidence changes no operation",
          "[unit][runtime][save][arbiter][retry]") {
    auto arbiter = Arbiter();
    const auto handle = BeginRetrySave(arbiter, RetryRequest());
    const auto invalid = arbiter.DeferStorageRetry(501,
                                                   {.category = SaveStorageFailureCategory::TransientIo,
                                                    .commitOutcome = static_cast<SaveOperationCommitOutcome>(255),
                                                    .nativeCause = MakeError(SaveErrors::ArchiveChunkHashMismatch)},
                                                   100);
    REQUIRE(invalid.HasError());
    CHECK_FALSE(handle.Snapshot()->IsTerminal());
    const auto failed = arbiter.DeferStorageRetry(501,
                                                  {.category = SaveStorageFailureCategory::TransientIo,
                                                   .nativeCause = MakeError(SaveErrors::ArchiveChunkHashMismatch)},
                                                  10);
    REQUIRE(failed.HasValue());
    CHECK_FALSE(failed.Value());
    auto next = RetryRequest(502);
    next.retry->admittedAtMilliseconds = 5;
    BeginRetrySave(arbiter, next);
    const SaveStorageFailureInput failure{.category = SaveStorageFailureCategory::TransientIo,
                                          .nativeCause = MakeError(SaveErrors::StorageTransientIo)};
    CHECK(arbiter.DeferStorageRetry(502, failure, 5).HasError());
    CHECK(arbiter.DeferStorageRetry(502, failure, 10).Value());
    CHECK(handle.Snapshot()->IsTerminal());
}
