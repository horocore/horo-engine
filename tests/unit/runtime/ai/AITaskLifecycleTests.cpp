#include "AiTestSupport.h"
#include "AllocationProbe.h"
#include "Horo/AI/AITaskLifecycle.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        struct TaskFixture final {
            AiRuntimeIncarnation incarnation{AiRuntimeIncarnation::Create(71).Value()};
            AgentHandle agent{incarnation, {4, 9}};
            TaskHandle task{incarnation, {6, 3}};
            CancellationSource cancellation;

            [[nodiscard]] AiTaskOperationContext Context() const {
                return {
                    .taskDefinition = MakeIdentity<TaskId>(81),
                    .task = task,
                    .agent = agent,
                    .cancellation = cancellation.Token(),
                };
            }
        };

        [[nodiscard]] Error ExecutionError() {
            return MakeError(AIErrors::BlackboardUnknownValueRejected, "Required task dependency is unavailable.");
        }

        TEST_CASE("AI task start captures a detached generation-fenced context", "[unit][ai][task]") {
            TaskFixture fixture;
            AiTaskLifecycle lifecycle;

            CHECK(lifecycle.State() == AiTaskState::Idle);
            CHECK(lifecycle.Context() == nullptr);
            CHECK(lifecycle.TerminalResult() == nullptr);
            REQUIRE(lifecycle.Start(fixture.Context()).HasValue());
            CHECK(lifecycle.State() == AiTaskState::Running);
            REQUIRE(lifecycle.Context() != nullptr);
            CHECK(lifecycle.Context()->taskDefinition == fixture.Context().taskDefinition);
            CHECK(lifecycle.Context()->task == fixture.task);
            CHECK(lifecycle.Context()->agent == fixture.agent);
            CHECK_FALSE(lifecycle.Context()->diagnostics);
            CHECK(lifecycle.TerminalResult() == nullptr);

            ExpectError(lifecycle.Start(fixture.Context()), AIErrors::TaskTransitionInvalid);
            CHECK(lifecycle.PrepareResume(AiTaskResumeReason::FixedTick, fixture.agent).Value() == AiTaskResumeDisposition::Ready);
            CHECK(lifecycle.PrepareResume(AiTaskResumeReason::Event, fixture.agent).Value() == AiTaskResumeDisposition::Ready);
        }

        TEST_CASE("Internal AI task context copy move and admission allocate no diagnostic storage", "[unit][ai][task]") {
            const TaskFixture fixture;
            AiTaskLifecycle lifecycle;
            bool started{};
            std::size_t requests{};
            {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                auto context = fixture.Context();
                auto copied = context;
                auto moved = std::move(copied);
                started = lifecycle.Start(std::move(moved)).HasValue();
                requests = measurement.Snapshot().requests;
            }
            REQUIRE(started);
            CHECK(requests == 0);
            REQUIRE(lifecycle.Context());
            CHECK_FALSE(lifecycle.Context()->diagnostics);
        }

        TEST_CASE("AI task admission rejects malformed and cross-runtime context", "[unit][ai][task]") {
            TaskFixture fixture;
            auto context = fixture.Context();
            context.taskDefinition = {};
            AiTaskLifecycle invalidDefinition;
            ExpectError(invalidDefinition.Start(context), AIErrors::TaskContextInvalid);

            context = fixture.Context();
            context.task = {};
            AiTaskLifecycle invalidTask;
            ExpectError(invalidTask.Start(context), AIErrors::TaskContextInvalid);

            context = fixture.Context();
            context.agent = {AiRuntimeIncarnation::Create(72).Value(), {4, 9}};
            AiTaskLifecycle crossRuntime;
            ExpectError(crossRuntime.Start(context), AIErrors::TaskContextInvalid);
        }

        TEST_CASE("AI tasks publish exactly one immutable successful terminal result", "[unit][ai][task]") {
            TaskFixture fixture;
            AiTaskLifecycle lifecycle;
            REQUIRE(lifecycle.Start(fixture.Context()).HasValue());
            CHECK(lifecycle.CompleteSuccess(fixture.agent).Value() == AiTaskTransitionDisposition::Applied);
            REQUIRE(lifecycle.TerminalResult() != nullptr);
            CHECK(lifecycle.TerminalResult()->state == AiTaskState::Succeeded);
            CHECK_FALSE(lifecycle.TerminalResult()->failure.has_value());
            CHECK_FALSE(lifecycle.TerminalResult()->cancellationReason.has_value());

            fixture.cancellation.RequestCancellation();
            CHECK(lifecycle.RequestCancellation(AiTaskCancellationReason::Requested).Value() ==
                  AiTaskTransitionDisposition::AlreadyTerminal);
            CHECK(lifecycle.CompleteFailure(fixture.agent, {AiTaskFailureKind::Execution, ExecutionError()}).Value() ==
                  AiTaskTransitionDisposition::AlreadyTerminal);
            CHECK(lifecycle.RetireAgentGeneration() == AiTaskTransitionDisposition::AlreadyTerminal);
            CHECK(lifecycle.TerminalResult()->state == AiTaskState::Succeeded);
        }

        TEST_CASE("AI task cancellation is safe before during and after worker completion", "[unit][ai][task]") {
            TaskFixture beforeFixture;
            AiTaskLifecycle before;
            CHECK(before.RequestCancellation(AiTaskCancellationReason::Requested).Value() == AiTaskTransitionDisposition::Applied);
            CHECK(before.RequestCancellation(AiTaskCancellationReason::OwnerShutdown).Value() == AiTaskTransitionDisposition::NoChange);
            REQUIRE(before.Start(beforeFixture.Context()).HasValue());
            REQUIRE(before.TerminalResult() != nullptr);
            CHECK(before.TerminalResult()->state == AiTaskState::Cancelled);
            CHECK(before.TerminalResult()->cancellationReason == AiTaskCancellationReason::Requested);

            TaskFixture duringFixture;
            AiTaskLifecycle during;
            REQUIRE(during.Start(duringFixture.Context()).HasValue());
            CHECK(during.RequestCancellation(AiTaskCancellationReason::OwnerShutdown).Value() == AiTaskTransitionDisposition::Applied);
            CHECK(during.CompleteSuccess(duringFixture.agent).Value() == AiTaskTransitionDisposition::AlreadyTerminal);
            CHECK(during.TerminalResult()->cancellationReason == AiTaskCancellationReason::OwnerShutdown);

            TaskFixture afterFixture;
            AiTaskLifecycle after;
            REQUIRE(after.Start(afterFixture.Context()).HasValue());
            CHECK(after.CompleteSuccess(afterFixture.agent).Value() == AiTaskTransitionDisposition::Applied);
            afterFixture.cancellation.RequestCancellation();
            CHECK(after.PrepareResume(AiTaskResumeReason::Event, afterFixture.agent).Value() == AiTaskResumeDisposition::AlreadyTerminal);
            CHECK(after.TerminalResult()->state == AiTaskState::Succeeded);
        }

        TEST_CASE("Cancellation token wins at admission and resume boundaries", "[unit][ai][task]") {
            TaskFixture admissionFixture;
            admissionFixture.cancellation.RequestCancellation();
            AiTaskLifecycle admission;
            REQUIRE(admission.Start(admissionFixture.Context()).HasValue());
            CHECK(admission.State() == AiTaskState::Cancelled);
            CHECK(admission.TerminalResult()->cancellationReason == AiTaskCancellationReason::ContextCancelled);

            TaskFixture resumeFixture;
            AiTaskLifecycle resume;
            REQUIRE(resume.Start(resumeFixture.Context()).HasValue());
            resumeFixture.cancellation.RequestCancellation();
            CHECK(resume.PrepareResume(AiTaskResumeReason::FixedTick, resumeFixture.agent).Value() ==
                  AiTaskResumeDisposition::BecameTerminal);
            CHECK(resume.TerminalResult()->cancellationReason == AiTaskCancellationReason::ContextCancelled);
            CHECK(resume.CompleteSuccess(resumeFixture.agent).Value() == AiTaskTransitionDisposition::AlreadyTerminal);
        }

        TEST_CASE("Retired agent generations fence stale resume and completion", "[unit][ai][task]") {
            TaskFixture fixture;
            const AgentHandle replacement{fixture.incarnation, {fixture.agent.slot.index, fixture.agent.slot.generation + 1}};
            AiTaskLifecycle lifecycle;
            REQUIRE(lifecycle.Start(fixture.Context()).HasValue());
            CHECK(lifecycle.PrepareResume(AiTaskResumeReason::Event, replacement).Value() == AiTaskResumeDisposition::BecameTerminal);
            REQUIRE(lifecycle.TerminalResult() != nullptr);
            CHECK(lifecycle.TerminalResult()->state == AiTaskState::Cancelled);
            CHECK(lifecycle.TerminalResult()->cancellationReason == AiTaskCancellationReason::AgentGenerationRetired);
            CHECK(lifecycle.CompleteFailure(fixture.agent, {AiTaskFailureKind::Execution, ExecutionError()}).Value() ==
                  AiTaskTransitionDisposition::AlreadyTerminal);

            TaskFixture retiredBeforeStartFixture;
            AiTaskLifecycle retiredBeforeStart;
            CHECK(retiredBeforeStart.RetireAgentGeneration() == AiTaskTransitionDisposition::Applied);
            REQUIRE(retiredBeforeStart.Start(retiredBeforeStartFixture.Context()).HasValue());
            CHECK(retiredBeforeStart.TerminalResult()->cancellationReason == AiTaskCancellationReason::AgentGenerationRetired);
        }

        TEST_CASE("AI task failure retains typed detail and rejects malformed causes", "[unit][ai][task]") {
            TaskFixture fixture;
            AiTaskLifecycle lifecycle;
            REQUIRE(lifecycle.Start(fixture.Context()).HasValue());
            ExpectError(lifecycle.CompleteFailure(fixture.agent, {AiTaskFailureKind::Execution, {}}), AIErrors::TaskFailureInvalid);
            CHECK(lifecycle.State() == AiTaskState::Running);

            const auto completed = lifecycle.CompleteFailure(fixture.agent, {AiTaskFailureKind::DependencyUnavailable, ExecutionError()});
            REQUIRE(completed.HasValue());
            CHECK(completed.Value() == AiTaskTransitionDisposition::Applied);
            REQUIRE(lifecycle.TerminalResult()->failure.has_value());
            CHECK(lifecycle.TerminalResult()->failure->kind == AiTaskFailureKind::DependencyUnavailable);
            CHECK(lifecycle.TerminalResult()->failure->cause.code.Value() == AIErrors::BlackboardUnknownValueRejected.code.Value());
        }

        TEST_CASE("Terminal publication precedes exactly-once cleanup ownership", "[unit][ai][task]") {
            TaskFixture fixture;
            AiTaskLifecycle lifecycle;
            ExpectError(lifecycle.ClaimCleanup(), AIErrors::TaskTransitionInvalid);
            REQUIRE(lifecycle.Start(fixture.Context()).HasValue());
            ExpectError(lifecycle.ClaimCleanup(), AIErrors::TaskTransitionInvalid);
            REQUIRE(lifecycle.RequestCancellation(AiTaskCancellationReason::Requested).HasValue());
            REQUIRE(lifecycle.TerminalResult() != nullptr);

            const auto firstClaim = lifecycle.ClaimCleanup();
            REQUIRE(firstClaim.HasValue());
            CHECK(firstClaim.Value());
            const auto duplicateClaim = lifecycle.ClaimCleanup();
            REQUIRE(duplicateClaim.HasValue());
            CHECK_FALSE(duplicateClaim.Value());
            CHECK_FALSE(lifecycle.IsCleanupComplete());
            CHECK(lifecycle.CompleteCleanup().Value() == AiTaskTransitionDisposition::Applied);
            CHECK(lifecycle.IsCleanupComplete());
            CHECK(lifecycle.CompleteCleanup().Value() == AiTaskTransitionDisposition::NoChange);
        }

        TEST_CASE("AI task lifecycle rejects unknown control enum values", "[unit][ai][task]") {
            TaskFixture fixture;
            AiTaskLifecycle lifecycle;
            REQUIRE(lifecycle.Start(fixture.Context()).HasValue());
            ExpectError(lifecycle.PrepareResume(static_cast<AiTaskResumeReason>(255), fixture.agent), AIErrors::TaskContextInvalid);
            ExpectError(lifecycle.RequestCancellation(static_cast<AiTaskCancellationReason>(255)), AIErrors::TaskContextInvalid);
            ExpectError(lifecycle.CompleteFailure(fixture.agent, {static_cast<AiTaskFailureKind>(255), ExecutionError()}),
                        AIErrors::TaskFailureInvalid);
            CHECK(lifecycle.State() == AiTaskState::Running);
        }
    }  // namespace
}  // namespace Horo::AI
