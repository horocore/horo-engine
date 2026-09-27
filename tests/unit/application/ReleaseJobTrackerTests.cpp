#include "Horo/Release/ReleaseErrors.h"
#include "Horo/Release/ReleaseJobTracker.h"

#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::Release::Tests {
    namespace {
        [[nodiscard]] ReleaseJobTracker NewTracker(const ReleaseStagePlan plan = {}) {
            return ReleaseJobTracker{{11}, {21}, 31, plan};
        }

        /** @brief Runs every selected stage while retaining the candidate handoff. */
        void CompletePlan(ReleaseJobTracker &tracker, const ReleaseStagePlan plan) {
            REQUIRE(tracker.Start().HasValue());
            for (std::size_t index = 0; index < ReleaseStageCount; ++index) {
                const auto stage = static_cast<ReleaseStage>(index);
                if (!plan.Includes(stage))
                    continue;
                const auto started = tracker.BeginStage(stage);
                REQUIRE(started.HasValue());
                if (stage == ReleaseStage::FinalizingMetadata)
                    REQUIRE(tracker.CompleteFinalization(started.Value(), {99}).HasValue());
                else if (stage == ReleaseStage::FinalVerifying)
                    REQUIRE(tracker.CompleteFinalVerification(started.Value()).HasValue());
                else
                    REQUIRE(tracker.CompleteStage(stage, started.Value()).HasValue());
            }
        }
    }  // namespace

    TEST_CASE("Release job completes only the ordered planned stages and a verified candidate", "[release][pipeline]") {
        auto tracker = NewTracker({true, true});
        CompletePlan(tracker, {true, true});
        const auto before = tracker.Snapshot();
        REQUIRE(before.candidate);
        CHECK(before.candidate->state == ReleaseCandidateState::FinalVerified);
        REQUIRE(tracker.FinishSuccess().HasValue());
        const auto after = tracker.Snapshot();
        CHECK(after.revision == before.revision + 1);
        CHECK(after.state == ReleaseJobState::Succeeded);
        REQUIRE(after.terminal);
        CHECK(std::get<ReleaseSucceeded>(*after.terminal).candidate == ReleaseCandidateId{99});
        CHECK(tracker.FinishSuccess().HasError());
        CHECK(tracker.Snapshot().revision == after.revision);
    }

    TEST_CASE("Release job rejects skipped, duplicate and optional stage transitions", "[release][pipeline]") {
        auto tracker = NewTracker();
        CHECK(tracker.BeginStage(ReleaseStage::Validating).HasError());
        REQUIRE(tracker.Start().HasValue());
        CHECK(tracker.BeginStage(ReleaseStage::Building).HasError());
        CHECK(tracker.BeginStage(ReleaseStage::Signing).HasError());
        const auto validating = tracker.BeginStage(ReleaseStage::Validating);
        REQUIRE(validating.HasValue());
        CHECK(tracker.BeginStage(ReleaseStage::Configuring).HasError());
        CHECK(tracker.CompleteStage(ReleaseStage::Validating, {validating.Value().value + 1}).HasError());
        REQUIRE(tracker.CompleteStage(ReleaseStage::Validating, validating.Value()).HasValue());
        CHECK(tracker.CompleteStage(ReleaseStage::Validating, validating.Value()).HasError());
        CHECK(tracker.FinishSuccess().HasError());
    }

    TEST_CASE("Release stage failure commits one terminal and retains its exact attempt", "[release][pipeline]") {
        auto tracker = NewTracker();
        REQUIRE(tracker.Start().HasValue());
        const auto attempt = tracker.BeginStage(ReleaseStage::Validating);
        REQUIRE(attempt.HasValue());
        REQUIRE(tracker.FailStage(ReleaseStage::Validating, attempt.Value(), MakeError(ReleaseErrors::ProfileInvalid)).HasValue());
        const auto terminal = tracker.Snapshot();
        CHECK(terminal.state == ReleaseJobState::Failed);
        REQUIRE(terminal.terminal);
        CHECK(std::get<ReleaseFailed>(*terminal.terminal).attempt == attempt.Value());
        CHECK(tracker.RequestCancel().HasError());
        CHECK(tracker.FailStage(ReleaseStage::Validating, attempt.Value(), MakeError(ReleaseErrors::ProfileInvalid)).HasError());
        CHECK(tracker.Snapshot().revision == terminal.revision);
    }

    TEST_CASE("Release cancellation handles queued and active stages without a second terminal", "[release][pipeline]") {
        auto queued = NewTracker();
        REQUIRE(queued.RequestCancel().HasValue());
        const auto queuedTerminal = queued.Snapshot();
        REQUIRE(queued.RequestCancel().HasValue());
        CHECK(queued.Snapshot().revision == queuedTerminal.revision);
        CHECK(queued.Start().HasError());
        CHECK(queued.Snapshot().state == ReleaseJobState::Cancelled);

        auto running = NewTracker();
        REQUIRE(running.Start().HasValue());
        const auto attempt = running.BeginStage(ReleaseStage::Validating);
        REQUIRE(attempt.HasValue());
        REQUIRE(running.RequestCancel().HasValue());
        CHECK(running.BeginStage(ReleaseStage::Configuring).HasError());
        REQUIRE(running.AcknowledgeCancellation().HasValue());
        const auto terminal = running.Snapshot();
        CHECK(terminal.state == ReleaseJobState::Cancelled);
        CHECK(terminal.stages[0].state == ReleaseStageState::Cancelled);
        REQUIRE(terminal.terminal);
        CHECK(std::get<ReleaseCancelled>(*terminal.terminal).attempt == attempt.Value());
        CHECK(running.AcknowledgeCancellation().HasError());
        CHECK(running.Snapshot().revision == terminal.revision);
    }

    TEST_CASE("Release finalization remains ineligible until final verification succeeds", "[release][pipeline]") {
        auto tracker = NewTracker();
        REQUIRE(tracker.Start().HasValue());
        for (std::size_t index = 0; index < static_cast<std::size_t>(ReleaseStage::FinalizingMetadata); ++index) {
            const auto stage = static_cast<ReleaseStage>(index);
            if (stage == ReleaseStage::Signing)
                continue;
            const auto attempt = tracker.BeginStage(stage);
            REQUIRE(attempt.HasValue());
            REQUIRE(tracker.CompleteStage(stage, attempt.Value()).HasValue());
        }
        const auto finalize = tracker.BeginStage(ReleaseStage::FinalizingMetadata);
        REQUIRE(finalize.HasValue());
        CHECK(tracker.CompleteStage(ReleaseStage::FinalizingMetadata, finalize.Value()).HasError());
        REQUIRE(tracker.CompleteFinalization(finalize.Value(), {99}).HasValue());
        CHECK(tracker.FinishSuccess().HasError());
        const auto verify = tracker.BeginStage(ReleaseStage::FinalVerifying);
        REQUIRE(verify.HasValue());
        REQUIRE(tracker.FailStage(ReleaseStage::FinalVerifying, verify.Value(), MakeError(ReleaseErrors::ProfileInvalid)).HasValue());
        REQUIRE(tracker.Snapshot().candidate);
        CHECK(tracker.Snapshot().candidate->state == ReleaseCandidateState::Finalized);
    }

    TEST_CASE("Release progress and diagnostics are stage-bound and bounded", "[release][pipeline]") {
        auto tracker = NewTracker();
        REQUIRE(tracker.Start().HasValue());
        const auto attempt = tracker.BeginStage(ReleaseStage::Validating);
        REQUIRE(attempt.HasValue());
        REQUIRE(tracker.UpdateProgress(ReleaseStage::Validating, attempt.Value(), 1, 3).HasValue());
        CHECK(tracker.UpdateProgress(ReleaseStage::Validating, attempt.Value(), 0, 3).HasError());
        CHECK(tracker.UpdateProgress(ReleaseStage::Validating, attempt.Value(), 4, 3).HasError());
        CHECK(tracker.UpdateProgress(ReleaseStage::Building, attempt.Value(), 2, 3).HasError());
        REQUIRE(tracker.UpdateProgress(ReleaseStage::Validating, attempt.Value(), 3, 3).HasValue());

        ReleaseDiagnosticId first;
        for (std::size_t index = 0; index < MaximumReleaseDiagnostics + 1; ++index) {
            const auto recorded = tracker.AppendDiagnostic(ReleaseStage::Validating, attempt.Value(), ErrorCode{"release.test.bound"},
                                                           ErrorSeverity::Info, "bounded");
            REQUIRE(recorded.HasValue());
            if (index == 0)
                first = recorded.Value();
        }
        const auto snapshot = tracker.Snapshot();
        REQUIRE(snapshot.progress.has_value());
        CHECK(snapshot.progress->completed == 3);
        CHECK(snapshot.recentDiagnostics.size() == MaximumReleaseDiagnostics);
        CHECK_FALSE(tracker.Diagnostic(first).has_value());
        const auto last = tracker.Diagnostic(snapshot.recentDiagnostics.back());
        REQUIRE(last.has_value());
        CHECK(last->job == ReleaseJobId{11});
        CHECK(last->target == ReleaseTargetId{21});
        CHECK(last->operation == 31);
        CHECK(last->stage == ReleaseStage::Validating);
        CHECK(last->attempt == attempt.Value());

        REQUIRE(tracker.CompleteStage(ReleaseStage::Validating, attempt.Value()).HasValue());
        CHECK(tracker.UpdateProgress(ReleaseStage::Validating, attempt.Value(), 3, 3).HasError());
        CHECK(tracker
                  .AppendDiagnostic(ReleaseStage::Validating, attempt.Value(), ErrorCode{"release.test.bound"}, ErrorSeverity::Info,
                                    "too late")
                  .HasError());
    }

    TEST_CASE("Release cancellation and failure race commits exactly one terminal", "[release][pipeline]") {
        auto tracker = NewTracker();
        REQUIRE(tracker.Start().HasValue());
        const auto attempt = tracker.BeginStage(ReleaseStage::Validating);
        REQUIRE(attempt.HasValue());
        REQUIRE(tracker.RequestCancel().HasValue());

        std::barrier ready{3};
        bool failed{};
        bool cancelled{};
        std::thread failureWorker{[&] {
            ready.arrive_and_wait();
            failed = tracker.FailStage(ReleaseStage::Validating, attempt.Value(), MakeError(ReleaseErrors::ProfileInvalid)).HasValue();
        }};
        std::thread cancellationWorker{[&] {
            ready.arrive_and_wait();
            cancelled = tracker.AcknowledgeCancellation().HasValue();
        }};
        ready.arrive_and_wait();
        failureWorker.join();
        cancellationWorker.join();

        CHECK(failed != cancelled);
        const auto terminal = tracker.Snapshot();
        REQUIRE(terminal.terminal.has_value());
        CHECK((terminal.state == ReleaseJobState::Failed || terminal.state == ReleaseJobState::Cancelled));
        CHECK(tracker.FinishSuccess().HasError());
        CHECK(tracker.Snapshot().revision == terminal.revision);
    }
}  // namespace Horo::Release::Tests
