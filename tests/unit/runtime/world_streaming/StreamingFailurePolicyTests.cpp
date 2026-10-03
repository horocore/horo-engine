#include "Horo/WorldStreaming/StreamingCellState.h"
#include "Horo/WorldStreaming/StreamingFailurePolicy.h"
#include "WorldStreamingTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::Layer;
        using TestSupport::RequireError;
        using TestSupport::World;

        StreamingFailurePolicyRequest Request() {
            StreamingFailurePolicyRequest request;
            request.id = IdentityFrom<StreamingFailurePolicyId>(1);
            request.revision = IdentityFrom<StreamingFailurePolicyRevision>(1);
            return request;
        }

        StreamingFailureContext Context(const std::uint64_t time = 100) {
            return {IdentityFrom<StreamingFailurePolicyId>(1),
                    IdentityFrom<StreamingFailurePolicyRevision>(1),
                    World(),
                    IdentityFrom<PartitionEpoch>(1),
                    IdentityFrom<StreamingContentRevision>(1),
                    IdentityFrom<StreamingProviderRevision>(1),
                    time,
                    0,
                    StreamingFailureLifecycle::Active};
        }

        StreamingCellOperation Queued(const std::uint64_t generation = 1) {
            StreamingCellOperationHandle handle{IdentityFrom<StreamingCellOperationId>(generation),
                                                {World(),
                                                 IdentityFrom<PartitionEpoch>(1),
                                                 {0, 0, 0, 0, Layer(1)},
                                                 IdentityFrom<StreamingGeneration>(generation)}};
            return StreamingCellOperation::Create(handle, StreamingCellOperationKind::Load).Value();
        }

        StreamingCellOperation Failed(const std::uint64_t generation = 1) {
            const auto queued = Queued(generation);
            return queued.Advance(queued.Handle(), StreamingCellOperationTransition::Fail).Value();
        }

        StreamingFailureRecord Record(const StreamingFailureCause cause = StreamingFailureCause::TransientIo) {
            return StreamingFailureRecord::RecordFailure(StreamingFailurePolicy::Create(Request()).Value(), Context(), Failed(), cause,
                                                         std::nullopt)
                .Value();
        }

        TEST_CASE("Failure policy exhausts exactly three automatic retries with 2 4 8 second cooldowns",
                  "[unit][world_streaming][failure_policy]") {
            const auto policy = StreamingFailurePolicy::Create(Request()).Value();
            auto record = Record();
            std::uint64_t time = 100;
            for (std::uint64_t generation = 2; generation <= 4; ++generation) {
                const std::uint64_t delay = 2'000ULL << (generation - 2);
                REQUIRE(record.Snapshot().nextRetryAtServiceMilliseconds == time + delay);
                REQUIRE(record.EvaluateRetry(policy, Context(time + delay - 1)).Value() == StreamingRetryDisposition::CoolingDown);
                time += delay;
                REQUIRE(record.EvaluateRetry(policy, Context(time)).Value() == StreamingRetryDisposition::Eligible);
                const auto issued = record.IssueRetry(policy, Context(time), Queued(generation)).Value();
                REQUIRE(issued.Snapshot().automaticRetriesIssued == generation - 1);
                REQUIRE(issued.Snapshot().attemptCount == generation);
                REQUIRE(issued.EvaluateRetry(policy, Context(time)).Value() == StreamingRetryDisposition::AlreadyIssued);
                RequireError(issued.IssueRetry(policy, Context(time), Queued(generation + 1)),
                             WorldStreamingErrors::FailurePolicyTransitionInvalid);
                record = StreamingFailureRecord::RecordFailure(policy, Context(time), Failed(generation),
                                                               StreamingFailureCause::TransientProvider, issued)
                             .Value();
            }
            REQUIRE(record.Snapshot().attemptCount == 4);
            REQUIRE(record.Snapshot().nextRetryAtServiceMilliseconds == 0);
            REQUIRE(record.EvaluateRetry(policy, Context(time + 100'000)).Value() == StreamingRetryDisposition::Quarantined);
        }

        TEST_CASE("Permanent failure quarantine resets only for newer producer revision or explicit authorization",
                  "[unit][world_streaming][failure_policy]") {
            using enum StreamingFailureCause;
            const auto policy = StreamingFailurePolicy::Create(Request()).Value();
            for (const auto cause : {Integrity, Schema, MissingRequiredProvider, PermanentlyOversized}) {
                const auto record = Record(cause);
                REQUIRE(record.Snapshot().cause == cause);
                REQUIRE(record.EvaluateRetry(policy, Context(900'000)).Value() == StreamingRetryDisposition::Quarantined);
                auto revised = Context();
                revised.contentRevision = IdentityFrom<StreamingContentRevision>(2);
                REQUIRE(record.EvaluateRetry(policy, revised).Value() == StreamingRetryDisposition::Eligible);
                const auto reset = record.IssueRetry(policy, revised, Queued(2)).Value();
                REQUIRE(reset.Snapshot().automaticRetriesIssued == 0);
                REQUIRE(reset.Snapshot().attemptCount == 1);
                REQUIRE(reset.Snapshot().contentRevision == revised.contentRevision);
                const auto authorized = record.IssueRetry(policy, Context(), Queued(2), StreamingRetryAuthorization::Authorized).Value();
                REQUIRE(authorized.Snapshot().attemptCount == 1);
                revised = Context();
                revised.providerRevision = IdentityFrom<StreamingProviderRevision>(2);
                REQUIRE(record.EvaluateRetry(policy, revised).Value() == StreamingRetryDisposition::Eligible);
            }
        }

        TEST_CASE("Failure history requires cleanup and fences duplicate or foreign completions",
                  "[unit][world_streaming][failure_policy]") {
            using enum StreamingFailureCause;
            const auto policy = StreamingFailurePolicy::Create(Request()).Value();
            auto operation = Queued();
            operation = operation.Advance(operation.Handle(), StreamingCellOperationTransition::Admit).Value();
            operation = operation.Advance(operation.Handle(), StreamingCellOperationTransition::Fail).Value();
            RequireError(StreamingFailureRecord::RecordFailure(policy, Context(), operation, TransientIo, std::nullopt),
                         WorldStreamingErrors::FailurePolicyTransitionInvalid);
            operation = operation.Advance(operation.Handle(), StreamingCellOperationTransition::AcknowledgeRetirement).Value();
            const auto record = StreamingFailureRecord::RecordFailure(policy, Context(), operation, TransientIo, std::nullopt).Value();
            RequireError(StreamingFailureRecord::RecordFailure(policy, Context(), operation, TransientIo, record),
                         WorldStreamingErrors::FailurePolicyStale);
            RequireError(record.IssueRetry(policy, Context(2'100), Queued()), WorldStreamingErrors::FailurePolicyStale);
            RequireError(record.IssueRetry(policy, Context(2'099), Queued(2)), WorldStreamingErrors::FailurePolicyTransitionInvalid);
            const auto issued = record.IssueRetry(policy, Context(2'100), Queued(2)).Value();
            RequireError(StreamingFailureRecord::RecordFailure(policy, Context(2'100), Failed(3), TransientIo, issued),
                         WorldStreamingErrors::FailurePolicyStale);
            auto revised = Context(2'100);
            revised.providerRevision = IdentityFrom<StreamingProviderRevision>(2);
            RequireError(StreamingFailureRecord::RecordFailure(policy, revised, Failed(2), TransientIo, issued),
                         WorldStreamingErrors::FailurePolicyStale);
            REQUIRE(record.Snapshot().automaticRetriesIssued == 0);
        }

        TEST_CASE("Failure policy rejects stale contexts capacity and lifecycle without mutating tombstones",
                  "[unit][world_streaming][failure_policy]") {
            auto request = Request();
            request.maximumTrackedCells = 1;
            const auto policy = StreamingFailurePolicy::Create(request).Value();
            const auto record = Record();
            auto context = Context();
            context.trackedCells = 1;
            RequireError(StreamingFailureRecord::RecordFailure(policy, context, Failed(), StreamingFailureCause::TransientIo, std::nullopt),
                         WorldStreamingErrors::FailurePolicyCapacityExceeded);
            REQUIRE(record.EvaluateRetry(policy, context).Value() == StreamingRetryDisposition::CoolingDown);
            for (const auto lifecycle : {StreamingFailureLifecycle::Cancelling, StreamingFailureLifecycle::Closed}) {
                context = Context();
                context.lifecycle = lifecycle;
                RequireError(record.EvaluateRetry(policy, context), WorldStreamingErrors::FailurePolicyLifecycleUnavailable);
            }
            context = Context(99);
            RequireError(record.EvaluateRetry(policy, context), WorldStreamingErrors::FailurePolicyStale);
            context = Context();
            context.policyRevision = IdentityFrom<StreamingFailurePolicyRevision>(2);
            RequireError(record.EvaluateRetry(policy, context), WorldStreamingErrors::FailurePolicyStale);
            context = Context();
            context.epoch = IdentityFrom<PartitionEpoch>(2);
            RequireError(record.EvaluateRetry(policy, context), WorldStreamingErrors::FailurePolicyStale);
            context = Context();
            context.partition = {};
            RequireError(record.EvaluateRetry(policy, context), WorldStreamingErrors::FailurePolicyInvalid);
            context = Context();
            context.lifecycle = StreamingFailureLifecycle::Count;
            RequireError(record.EvaluateRetry(policy, context), WorldStreamingErrors::FailurePolicyUnsupported);
            RequireError(record.EvaluateRetry(policy, Context(), StreamingRetryAuthorization::Count),
                         WorldStreamingErrors::FailurePolicyUnsupported);
            RequireError(StreamingFailureRecord::RecordFailure(policy, Context(), Failed(), StreamingFailureCause::Count, std::nullopt),
                         WorldStreamingErrors::FailurePolicyUnsupported);
        }

        TEST_CASE("Interrupted retries retain consumed allowance and require exact cleanup before requeue",
                  "[unit][world_streaming][failure_policy]") {
            using enum StreamingCellOperationTransition;
            const auto policy = StreamingFailurePolicy::Create(Request()).Value();
            for (const auto transition : {Cancel, Replace, Shutdown}) {
                const auto issued = Record().IssueRetry(policy, Context(2'100), Queued(2)).Value();
                auto operation = Queued(2);
                operation = operation.Advance(operation.Handle(), Admit).Value();
                operation = operation.Advance(operation.Handle(), transition).Value();
                RequireError(issued.ReconcileInterruption(policy, Context(2'100), operation),
                             WorldStreamingErrors::FailurePolicyTransitionInvalid);
                operation = operation.Advance(operation.Handle(), AcknowledgeRetirement).Value();
                const auto retained = issued.ReconcileInterruption(policy, Context(2'100), operation).Value();
                REQUIRE(retained.Snapshot().automaticRetriesIssued == 1);
                REQUIRE(retained.Snapshot().attemptCount == 2);
                REQUIRE(retained.Snapshot().cause == StreamingFailureCause::TransientIo);
                REQUIRE(retained.Snapshot().nextRetryAtServiceMilliseconds == 6'100);
                RequireError(retained.ReconcileInterruption(policy, Context(2'100), operation),
                             WorldStreamingErrors::FailurePolicyTransitionInvalid);
                REQUIRE(retained.EvaluateRetry(policy, Context(6'099)).Value() == StreamingRetryDisposition::CoolingDown);
                const auto next = retained.IssueRetry(policy, Context(6'100), Queued(3)).Value();
                REQUIRE(next.Snapshot().automaticRetriesIssued == 2);
                RequireError(issued.ReconcileInterruption(policy, Context(2'100), Failed(3)), WorldStreamingErrors::FailurePolicyStale);
            }
        }

        TEST_CASE("Load success retains history until exact generation becomes Active and activation failure consumes allowance",
                  "[unit][world_streaming][failure_policy]") {
            const auto policy = StreamingFailurePolicy::Create(Request()).Value();
            const auto record = Record();
            const auto issued = record.IssueRetry(policy, Context(2'100), Queued(2)).Value();
            auto activeHandle = Queued(2).Handle();
            activeHandle.operation = IdentityFrom<StreamingCellOperationId>(100);
            StreamingCellStateRecord residency{activeHandle, StreamingCellState::Resident};
            RequireError(issued.ValidateSuccess(policy, Context(2'100), residency), WorldStreamingErrors::FailurePolicyTransitionInvalid);
            residency.state = StreamingCellState::Active;
            REQUIRE(issued.ValidateSuccess(policy, Context(2'100), residency).HasValue());
            RequireError(record.ValidateSuccess(policy, Context(2'100), residency), WorldStreamingErrors::FailurePolicyStale);
            const auto activation = StreamingCellOperation::Create(activeHandle, StreamingCellOperationKind::Activate).Value();
            const auto failed = activation.Advance(activeHandle, StreamingCellOperationTransition::Fail).Value();
            const auto retained =
                StreamingFailureRecord::RecordFailure(policy, Context(2'100), failed, StreamingFailureCause::TransientProvider, issued)
                    .Value();
            REQUIRE(retained.Snapshot().automaticRetriesIssued == 1);
            REQUIRE(retained.Snapshot().attemptCount == 2);
            REQUIRE(retained.Snapshot().nextRetryAtServiceMilliseconds == 6'100);
        }

        TEST_CASE("Failure policy validates bounded configuration and handles cooldown saturation and time exhaustion",
                  "[unit][world_streaming][failure_policy]") {
            auto request = Request();
            request.contractVersion = 2;
            RequireError(StreamingFailurePolicy::Create(request), WorldStreamingErrors::FailurePolicyUnsupported);
            request = Request();
            request.id = {};
            RequireError(StreamingFailurePolicy::Create(request), WorldStreamingErrors::FailurePolicyInvalid);
            request = Request();
            request.automaticRetries = 17;
            RequireError(StreamingFailurePolicy::Create(request), WorldStreamingErrors::FailurePolicyInvalid);
            request = Request();
            request.initialCooldownMilliseconds = 0;
            RequireError(StreamingFailurePolicy::Create(request), WorldStreamingErrors::FailurePolicyInvalid);
            request = Request();
            request.maximumCooldownMilliseconds = 1'000;
            RequireError(StreamingFailurePolicy::Create(request), WorldStreamingErrors::FailurePolicyInvalid);
            request = Request();
            request.maximumTrackedCells = 0;
            RequireError(StreamingFailurePolicy::Create(request), WorldStreamingErrors::FailurePolicyInvalid);
            request = Request();
            request.automaticRetries = 0;
            auto policy = StreamingFailurePolicy::Create(request).Value();
            auto disabled =
                StreamingFailureRecord::RecordFailure(policy, Context(), Failed(), StreamingFailureCause::TransientIo, std::nullopt)
                    .Value();
            REQUIRE(disabled.EvaluateRetry(policy, Context(999'999)).Value() == StreamingRetryDisposition::Quarantined);
            request = Request();
            request.maximumCooldownMilliseconds = 2'500;
            policy = StreamingFailurePolicy::Create(request).Value();
            auto issued = Record().IssueRetry(policy, Context(2'100), Queued(2)).Value();
            const auto capped =
                StreamingFailureRecord::RecordFailure(policy, Context(2'100), Failed(2), StreamingFailureCause::TransientIo, issued)
                    .Value();
            REQUIRE(capped.Snapshot().nextRetryAtServiceMilliseconds == 4'600);
            RequireError(StreamingFailureRecord::RecordFailure(policy, Context(std::numeric_limits<std::uint64_t>::max()), Failed(),
                                                               StreamingFailureCause::TransientIo, std::nullopt),
                         WorldStreamingErrors::FailurePolicyTimeExhausted);
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
