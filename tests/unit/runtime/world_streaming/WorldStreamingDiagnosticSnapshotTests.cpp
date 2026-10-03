#include "Horo/WorldStreaming/WorldStreamingDiagnosticSnapshot.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::WorldStreaming {
    static_assert(std::is_same_v<decltype(std::declval<const WorldStreamingDiagnosticSnapshot &>().Events()),
                                 std::span<const StreamingDiagnosticDecisionEvent>>);
    static_assert(!std::is_convertible_v<decltype(std::declval<const WorldStreamingDiagnosticSnapshot &>().Events()),
                                         std::span<StreamingDiagnosticDecisionEvent>>);

    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::Layer;
        using TestSupport::RequireError;
        using TestSupport::World;

        StreamingRuntimeOwnerToken RuntimeOwner(const std::uint64_t owner = 7, const std::uint64_t epoch = 3) {
            return {.partition = World(),
                    .epoch = IdentityFrom<PartitionEpoch>(epoch),
                    .owner = IdentityFrom<StreamingRuntimeOwnerId>(owner)};
        }

        StreamingFence Fence(const std::uint64_t generation, const std::int32_t x = 1, const std::uint64_t epoch = 3) {
            return {.partition = World(),
                    .epoch = IdentityFrom<PartitionEpoch>(epoch),
                    .cell = {.x = x, .y = 2, .z = 3, .lod = 0, .layer = Layer()},
                    .generation = IdentityFrom<StreamingGeneration>(generation)};
        }

        StreamingCellOperationHandle Operation(const std::uint64_t operation, const std::uint64_t generation, const std::int32_t x = 1,
                                               const std::uint64_t epoch = 3) {
            return {.operation = IdentityFrom<StreamingCellOperationId>(operation), .fence = Fence(generation, x, epoch)};
        }

        StreamingSourceDesiredState Source(const std::uint64_t id, const std::uint64_t revision = 1, const std::uint64_t epoch = 3) {
            const StreamingSourceDescriptor descriptor{
                .id = IdentityFrom<StreamingSourceId>(id),
                .owner = {.partition = World(), .epoch = IdentityFrom<PartitionEpoch>(epoch), .slot = 1, .generation = 1},
                .intent = StreamingSourceIntent::Camera,
                .priority = StreamingSourcePriority::Create(1.0F).Value(),
                .revision = IdentityFrom<StreamingSourceRevision>(revision),
            };
            return StreamingSourceDesiredState::Create(descriptor, StreamingDesiredResidency::Activated, StreamingRetention::Releasable)
                .Value();
        }

        StreamingBudgetPolicy Policy(const std::uint64_t revision = 1) {
            std::array<StreamingBudgetLimit, StreamingBudgetDimensionCount> limits{};
            for (std::size_t index = 0; index < limits.size(); ++index) {
                limits[index] = {.dimension = static_cast<StreamingBudgetDimension>(index), .softTarget = 50, .hardLimit = 100};
            }
            return StreamingBudgetPolicy::Create(IdentityFrom<StreamingBudgetPolicyRevision>(revision), limits, std::chrono::seconds{1})
                .Value();
        }

        StreamingBudgetSample Sample(const StreamingBudgetPolicy &policy, const std::uint64_t revision = 1) {
            std::array<StreamingBudgetAmount, StreamingBudgetDimensionCount> usage{};
            for (std::size_t index = 0; index < usage.size(); ++index)
                usage[index] = {.dimension = static_cast<StreamingBudgetDimension>(index), .value = index};
            return StreamingBudgetSample::Create(policy, IdentityFrom<StreamingBudgetSampleRevision>(revision), std::chrono::nanoseconds{0},
                                                 std::chrono::nanoseconds{10}, StreamingBudgetAmounts::Create(usage).Value())
                .Value();
        }

        WorldStreamingDiagnosticSnapshotInput Input() {
            return {
                .owner = RuntimeOwner(),
                .revision = IdentityFrom<WorldStreamingDiagnosticRevision>(1),
                .ownerRevision = IdentityFrom<StreamingRuntimeCompositionRevision>(4),
                .lifecycle = WorldStreamingRuntimeCompositionState::Active,
                .limits = {.sources = 4, .cells = 4, .failures = 4},
                .queue = {.owner = IdentityFrom<StreamingSchedulerLedgerId>(8),
                          .limits = {.concurrentOperations = 4,
                                     .capacityUnits = 100,
                                     .concurrency = {.profile = WorldPartitionProjectProfile::Editor,
                                                     .revision = IdentityFrom<StreamingConcurrencyRevision>(1),
                                                     .loads = 4,
                                                     .activations = 4,
                                                     .retirements = 4}},
                          .state = StreamingSchedulerAdmissionState::Accepting,
                          .reservedOperations = 2,
                          .reservedCapacityUnits = 30},
            };
        }

        Result<WorldStreamingDiagnosticSnapshot> CreateSnapshot(const WorldStreamingDiagnosticSnapshotInput &input,
                                                                const StreamingBudgetPolicy &policy, const StreamingBudgetSample &sample,
                                                                const std::span<const StreamingSourceDesiredState> sources = {},
                                                                const std::span<const StreamingCellStateRecord> cells = {},
                                                                const std::span<const StreamingDiagnosticFailureRecord> failures = {},
                                                                const std::span<const StreamingDiagnosticDecisionEvent> events = {}) {
            return WorldStreamingDiagnosticSnapshot::Create(input, policy, sample, {sources, cells, failures, events});
        }

        WorldStreamingDiagnosticSnapshotInput ClosedInput() {
            auto input = Input();
            input.lifecycle = WorldStreamingRuntimeCompositionState::Closed;
            input.queue.state = StreamingSchedulerAdmissionState::Closed;
            input.queue.reservedOperations = 0;
            input.queue.reservedCapacityUnits = 0;
            return input;
        }

        StreamingDiagnosticDecisionEvent Event(const std::uint64_t id, const std::uint64_t sequence,
                                               const StreamingCellOperationHandle operation,
                                               const StreamingDiagnosticEventCategory category) {
            StreamingDiagnosticDecisionEvent event{
                .id = IdentityFrom<StreamingDiagnosticEventId>(id),
                .sequence = IdentityFrom<StreamingDiagnosticEventSequence>(sequence),
                .ownerRevision = IdentityFrom<StreamingRuntimeCompositionRevision>(4),
                .snapshotRevision = IdentityFrom<WorldStreamingDiagnosticRevision>(1),
                .operation = operation,
                .category = category,
                .severity = StreamingDiagnosticEventSeverity::Info,
            };
            if (category == StreamingDiagnosticEventCategory::Admission)
                event.admission = StreamingDiagnosticAdmissionDecision::Accepted;
            else
                event.transition = StreamingDiagnosticCellTransition{StreamingCellState::Unloaded, StreamingCellState::Loading};
            if (category == StreamingDiagnosticEventCategory::Rollback) {
                event.transition = StreamingDiagnosticCellTransition{StreamingCellState::Loading, StreamingCellState::Evicting};
                event.outcome = StreamingCellOperationOutcome::Failed;
            }
            return event;
        }

        TEST_CASE("World Streaming diagnostics own and canonically order complete authority facts",
                  "[unit][world_streaming][diagnostics]") {
            auto input = Input();
            const auto policy = Policy();
            const auto sample = Sample(policy);
            std::vector sources{Source(20), Source(10)};
            std::vector cells{StreamingCellStateRecord{Operation(22, 2, 4), StreamingCellState::Failed},
                              StreamingCellStateRecord{Operation(11, 1, -2), StreamingCellState::Active}};
            std::vector failures{StreamingDiagnosticFailureRecord{cells.front().operation, StreamingCellOperationOutcome::Failed}};

            const auto snapshotResult = CreateSnapshot(input, policy, sample, sources, cells, failures);
            REQUIRE(snapshotResult.HasValue());
            const auto snapshot = snapshotResult.Value();
            REQUIRE(snapshot.Owner() == input.owner);
            REQUIRE(snapshot.Revision() == input.revision);
            REQUIRE(snapshot.OwnerRevision() == input.ownerRevision);
            REQUIRE(snapshot.Lifecycle() == WorldStreamingRuntimeCompositionState::Active);
            REQUIRE(snapshot.Queue().reservedOperations == 2);
            REQUIRE(snapshot.BudgetPolicy().Revision() == policy.Revision());
            REQUIRE(snapshot.BudgetSample().Revision() == sample.Revision());
            REQUIRE(snapshot.Sources().size() == 2);
            REQUIRE(snapshot.Sources()[0].Source().id.Value() == 10);
            REQUIRE(snapshot.Cells().size() == 2);
            REQUIRE(snapshot.Cells()[0].operation.fence.cell.x == -2);
            REQUIRE(snapshot.Failures().front().reason == StreamingCellOperationOutcome::Failed);

            sources.clear();
            cells.clear();
            failures.clear();
            REQUIRE(snapshot.Sources().size() == 2);
            REQUIRE(snapshot.Cells().size() == 2);
            REQUIRE(snapshot.Failures().size() == 1);
        }

        TEST_CASE("World Streaming diagnostics preserve valid empty and closed headless snapshots",
                  "[unit][world_streaming][diagnostics][shutdown]") {
            auto input = ClosedInput();
            const auto policy = Policy();
            const auto result = CreateSnapshot(input, policy, Sample(policy));
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Sources().empty());
            REQUIRE(result.Value().Cells().empty());
            REQUIRE(result.Value().Failures().empty());
            REQUIRE(result.Value().Events().empty());
            REQUIRE(result.Value().Lifecycle() == WorldStreamingRuntimeCompositionState::Closed);
        }

        TEST_CASE("World Streaming diagnostics reject invalid and unsupported aggregate facts transactionally",
                  "[unit][world_streaming][diagnostics]") {
            const auto policy = Policy();
            const auto sample = Sample(policy);
            auto input = Input();
            input.limits.sources = 0;
            RequireError(CreateSnapshot(input, policy, sample), WorldStreamingErrors::DiagnosticProjectionInvalid);

            input = Input();
            input.lifecycle = static_cast<WorldStreamingRuntimeCompositionState>(255);
            RequireError(CreateSnapshot(input, policy, sample), WorldStreamingErrors::DiagnosticProjectionUnsupported);

            input = Input();
            input.queue.reservedOperations = input.queue.limits.concurrentOperations + 1;
            RequireError(CreateSnapshot(input, policy, sample), WorldStreamingErrors::DiagnosticProjectionCapacityExceeded);

            input = Input();
            input.queue.state = StreamingSchedulerAdmissionState::Draining;
            RequireError(CreateSnapshot(input, policy, sample), WorldStreamingErrors::DiagnosticProjectionInvalid);

            input = Input();
            const std::array cells{StreamingCellStateRecord{Operation(1, 1), static_cast<StreamingCellState>(255)}};
            RequireError(CreateSnapshot(input, policy, sample, {}, cells), WorldStreamingErrors::DiagnosticProjectionUnsupported);
            const std::array validCells{StreamingCellStateRecord{Operation(1, 1), StreamingCellState::Failed}};
            const std::array badReason{StreamingDiagnosticFailureRecord{validCells[0].operation, StreamingCellOperationOutcome::Succeeded}};
            RequireError(CreateSnapshot(input, policy, sample, {}, validCells, badReason),
                         WorldStreamingErrors::DiagnosticProjectionUnsupported);
        }

        TEST_CASE("World Streaming diagnostics reject stale partition and budget facts", "[unit][world_streaming][diagnostics][stale]") {
            const auto policy = Policy();
            const auto sample = Sample(policy);
            const auto input = Input();
            const std::array staleSources{Source(1, 1, 4)};
            RequireError(CreateSnapshot(input, policy, sample, staleSources), WorldStreamingErrors::DiagnosticProjectionStale);

            const std::array staleCells{StreamingCellStateRecord{Operation(2, 1, 1, 4), StreamingCellState::Loading}};
            RequireError(CreateSnapshot(input, policy, sample, {}, staleCells), WorldStreamingErrors::DiagnosticProjectionStale);

            const auto otherPolicy = Policy(2);
            RequireError(CreateSnapshot(input, otherPolicy, sample), WorldStreamingErrors::DiagnosticProjectionStale);

            const std::array cells{StreamingCellStateRecord{Operation(3, 1), StreamingCellState::Failed}};
            const std::array orphanFailure{StreamingDiagnosticFailureRecord{Operation(4, 2), StreamingCellOperationOutcome::Failed}};
            RequireError(CreateSnapshot(input, policy, sample, {}, cells, orphanFailure), WorldStreamingErrors::DiagnosticProjectionStale);
        }

        TEST_CASE("World Streaming diagnostic capacity accepts its exact boundary and rejects one more row",
                  "[unit][world_streaming][diagnostics][capacity]") {
            auto input = Input();
            input.limits = {.sources = 2, .cells = 2, .failures = 2};
            const auto policy = Policy();
            const auto sample = Sample(policy);
            const std::array exactSources{Source(1), Source(2)};
            const std::array exactCells{StreamingCellStateRecord{Operation(1, 1, 1), StreamingCellState::Failed},
                                        StreamingCellStateRecord{Operation(2, 1, 2), StreamingCellState::Failed}};
            const std::array exactFailures{StreamingDiagnosticFailureRecord{exactCells[0].operation, StreamingCellOperationOutcome::Failed},
                                           StreamingDiagnosticFailureRecord{exactCells[1].operation,
                                                                            StreamingCellOperationOutcome::Cancelled}};
            REQUIRE(CreateSnapshot(input, policy, sample, exactSources, exactCells, exactFailures).HasValue());

            const std::array tooManySources{Source(1), Source(2), Source(3)};
            RequireError(CreateSnapshot(input, policy, sample, tooManySources), WorldStreamingErrors::DiagnosticProjectionCapacityExceeded);
        }

        TEST_CASE("World Streaming diagnostics reject duplicate source cell and failure identities",
                  "[unit][world_streaming][diagnostics][identity]") {
            const auto input = Input();
            const auto policy = Policy();
            const auto sample = Sample(policy);
            const std::array duplicateSources{Source(1, 1), Source(1, 2)};
            RequireError(CreateSnapshot(input, policy, sample, duplicateSources),
                         WorldStreamingErrors::DiagnosticProjectionIdentityConflict);

            const std::array duplicateCells{StreamingCellStateRecord{Operation(1, 1), StreamingCellState::Loading},
                                            StreamingCellStateRecord{Operation(2, 1), StreamingCellState::Resident}};
            RequireError(CreateSnapshot(input, policy, sample, {}, duplicateCells),
                         WorldStreamingErrors::DiagnosticProjectionIdentityConflict);

            const std::array cells{StreamingCellStateRecord{Operation(1, 1), StreamingCellState::Failed}};
            const std::array duplicateFailures{StreamingDiagnosticFailureRecord{cells[0].operation, StreamingCellOperationOutcome::Failed},
                                               StreamingDiagnosticFailureRecord{cells[0].operation,
                                                                                StreamingCellOperationOutcome::Shutdown}};
            RequireError(CreateSnapshot(input, policy, sample, {}, cells, duplicateFailures),
                         WorldStreamingErrors::DiagnosticProjectionIdentityConflict);
        }

        TEST_CASE("World Streaming diagnostics own canonically ordered lifecycle admission and rollback events",
                  "[unit][world_streaming][diagnostics][events]") {
            const auto input = Input();
            const auto policy = Policy();
            const auto operation = Operation(8, 2);
            const std::array cells{StreamingCellStateRecord{operation, StreamingCellState::Evicting}};
            auto lifecycle = Event(30, 30, operation, StreamingDiagnosticEventCategory::CellLifecycle);
            lifecycle.severity = StreamingDiagnosticEventSeverity::Warning;
            lifecycle.context[0] = {StreamingDiagnosticContextKey::QueueDepth, 3};
            lifecycle.context[1] = {StreamingDiagnosticContextKey::RequestedCapacityUnits, 7};
            lifecycle.contextCount = 2;
            auto admission = Event(10, 10, operation, StreamingDiagnosticEventCategory::Admission);
            admission.admission = StreamingDiagnosticAdmissionDecision::Deferred;
            auto rollback = Event(20, 20, operation, StreamingDiagnosticEventCategory::Rollback);
            rollback.severity = StreamingDiagnosticEventSeverity::Error;
            rollback.outcome = StreamingCellOperationOutcome::Shutdown;
            std::vector events{lifecycle, admission, rollback};

            const auto result = CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, events);
            REQUIRE(result.HasValue());
            const auto snapshot = result.Value();
            REQUIRE(snapshot.Events().size() == 3);
            REQUIRE(snapshot.Events()[0].category == StreamingDiagnosticEventCategory::Admission);
            REQUIRE(snapshot.Events()[1].category == StreamingDiagnosticEventCategory::Rollback);
            REQUIRE(snapshot.Events()[2].category == StreamingDiagnosticEventCategory::CellLifecycle);
            REQUIRE(snapshot.Events()[2].context[0].key == StreamingDiagnosticContextKey::RequestedCapacityUnits);
            events.clear();
            REQUIRE(snapshot.Events().size() == 3);
        }

        TEST_CASE("World Streaming diagnostic events enforce exact and one-over batch and context limits",
                  "[unit][world_streaming][diagnostics][events][capacity]") {
            auto input = Input();
            input.limits.events = 2;
            const auto policy = Policy();
            const auto operation = Operation(4, 1);
            const std::array exact{Event(1, 1, operation, StreamingDiagnosticEventCategory::Admission),
                                   Event(2, 2, operation, StreamingDiagnosticEventCategory::Admission)};
            REQUIRE(CreateSnapshot(input, policy, Sample(policy), {}, {}, {}, exact).HasValue());
            const std::array oneOver{exact[0], exact[1], Event(3, 3, operation, StreamingDiagnosticEventCategory::Admission)};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, {}, {}, oneOver),
                         WorldStreamingErrors::DiagnosticProjectionCapacityExceeded);

            auto contextOver = exact[0];
            contextOver.context[0] = {StreamingDiagnosticContextKey::RequestedCapacityUnits, 1};
            contextOver.context[1] = {StreamingDiagnosticContextKey::ReservedCapacityUnits, 2};
            contextOver.context[2] = {StreamingDiagnosticContextKey::QueueDepth, 3};
            contextOver.context[3] = {StreamingDiagnosticContextKey::BudgetRevision, 4};
            contextOver.contextCount = static_cast<std::uint8_t>(contextOver.context.size());
            const std::array exactContext{contextOver};
            REQUIRE(CreateSnapshot(input, policy, Sample(policy), {}, {}, {}, exactContext).HasValue());
            contextOver.contextCount = static_cast<std::uint8_t>(contextOver.context.size() + 1);
            const std::array badContext{contextOver};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, {}, {}, badContext),
                         WorldStreamingErrors::DiagnosticProjectionCapacityExceeded);
        }

        TEST_CASE("World Streaming diagnostic events reject invalid and stale identities", "[unit][world_streaming][diagnostics][events]") {
            const auto input = Input();
            const auto policy = Policy();
            const auto operation = Operation(5, 1);
            const std::array cells{StreamingCellStateRecord{operation, StreamingCellState::Loading}};

            auto invalid = Event(1, 1, operation, StreamingDiagnosticEventCategory::Admission);
            invalid.id = {};
            const std::array invalidEvents{invalid};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, invalidEvents),
                         WorldStreamingErrors::DiagnosticProjectionInvalid);

            auto stale = Event(1, 1, operation, StreamingDiagnosticEventCategory::CellLifecycle);
            stale.snapshotRevision = IdentityFrom<WorldStreamingDiagnosticRevision>(2);
            const std::array staleEvents{stale};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, staleEvents),
                         WorldStreamingErrors::DiagnosticProjectionStale);

            stale = Event(1, 1, operation, StreamingDiagnosticEventCategory::CellLifecycle);
            stale.ownerRevision = IdentityFrom<StreamingRuntimeCompositionRevision>(5);
            const std::array staleOwnerRevision{stale};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, staleOwnerRevision),
                         WorldStreamingErrors::DiagnosticProjectionStale);

            auto staleAttempt = Event(2, 2, Operation(6, 2), StreamingDiagnosticEventCategory::CellLifecycle);
            const std::array staleAttemptEvents{staleAttempt};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, staleAttemptEvents),
                         WorldStreamingErrors::DiagnosticProjectionStale);
        }

        TEST_CASE("World Streaming diagnostic events reject duplicate unsupported and illegal facts",
                  "[unit][world_streaming][diagnostics][events]") {
            const auto input = Input();
            const auto policy = Policy();
            const auto operation = Operation(5, 1);
            const std::array cells{StreamingCellStateRecord{operation, StreamingCellState::Loading}};

            auto unsupported = Event(3, 3, operation, StreamingDiagnosticEventCategory::Admission);
            unsupported.severity = static_cast<StreamingDiagnosticEventSeverity>(255);
            const std::array unsupportedEvents{unsupported};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, unsupportedEvents),
                         WorldStreamingErrors::DiagnosticProjectionUnsupported);

            auto unsupportedContext = Event(3, 3, operation, StreamingDiagnosticEventCategory::Admission);
            unsupportedContext.context[0] = {static_cast<StreamingDiagnosticContextKey>(255), 1};
            unsupportedContext.contextCount = 1;
            const std::array unsupportedContextEvents{unsupportedContext};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, unsupportedContextEvents),
                         WorldStreamingErrors::DiagnosticProjectionUnsupported);

            auto illegal = Event(4, 4, operation, StreamingDiagnosticEventCategory::CellLifecycle);
            illegal.transition = StreamingDiagnosticCellTransition{StreamingCellState::Unloaded, StreamingCellState::Active};
            const std::array illegalEvents{illegal};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, illegalEvents),
                         WorldStreamingErrors::CellStateTransitionInvalid);

            const std::array duplicateSequence{Event(5, 5, operation, StreamingDiagnosticEventCategory::Admission),
                                               Event(6, 5, operation, StreamingDiagnosticEventCategory::Admission)};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, duplicateSequence),
                         WorldStreamingErrors::DiagnosticProjectionIdentityConflict);
            const std::array duplicateIdentity{Event(7, 6, operation, StreamingDiagnosticEventCategory::Admission),
                                               Event(7, 7, operation, StreamingDiagnosticEventCategory::Admission)};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, duplicateIdentity),
                         WorldStreamingErrors::DiagnosticProjectionIdentityConflict);

            auto duplicateContext = Event(8, 8, operation, StreamingDiagnosticEventCategory::Admission);
            duplicateContext.context[0] = {StreamingDiagnosticContextKey::QueueDepth, 1};
            duplicateContext.context[1] = {StreamingDiagnosticContextKey::QueueDepth, 2};
            duplicateContext.contextCount = 2;
            const std::array duplicateContextEvents{duplicateContext};
            RequireError(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, duplicateContextEvents),
                         WorldStreamingErrors::DiagnosticProjectionIdentityConflict);
        }

        TEST_CASE("World Streaming decision events cover every admission and rollback outcome",
                  "[unit][world_streaming][diagnostics][events][lifecycle]") {
            const auto input = Input();
            const auto policy = Policy();
            const auto operation = Operation(11, 3);
            const std::array cells{StreamingCellStateRecord{operation, StreamingCellState::Evicting}};
            constexpr std::array decisions{StreamingDiagnosticAdmissionDecision::Accepted, StreamingDiagnosticAdmissionDecision::Deferred,
                                           StreamingDiagnosticAdmissionDecision::Rejected};
            std::vector<StreamingDiagnosticDecisionEvent> events;
            std::uint64_t identity = 1;
            for (const auto decision : decisions) {
                auto event = Event(identity, identity, operation, StreamingDiagnosticEventCategory::Admission);
                event.admission = decision;
                events.push_back(event);
                ++identity;
            }
            constexpr std::array outcomes{StreamingCellOperationOutcome::Cancelled, StreamingCellOperationOutcome::Failed,
                                          StreamingCellOperationOutcome::Replaced, StreamingCellOperationOutcome::Shutdown};
            for (const auto outcome : outcomes) {
                auto event = Event(identity, identity, operation, StreamingDiagnosticEventCategory::Rollback);
                event.outcome = outcome;
                events.push_back(event);
                ++identity;
            }
            REQUIRE(CreateSnapshot(input, policy, Sample(policy), {}, cells, {}, events).HasValue());
        }

        TEST_CASE("World Streaming diagnostic replacement is transactional and revision fenced",
                  "[unit][world_streaming][diagnostics][events][replacement]") {
            const auto policy = Policy();
            auto input = Input();
            const auto operation = Operation(9, 1);
            const std::array originalEvents{Event(1, 1, operation, StreamingDiagnosticEventCategory::Admission)};
            const auto originalResult = CreateSnapshot(input, policy, Sample(policy), {}, {}, {}, originalEvents);
            REQUIRE(originalResult.HasValue());
            const auto original = originalResult.Value();

            RequireError(WorldStreamingDiagnosticSnapshot::Replace(original, input, policy, Sample(policy), {{}, {}, {}, originalEvents}),
                         WorldStreamingErrors::DiagnosticProjectionStale);
            REQUIRE(original.Revision().Value() == 1);
            REQUIRE(original.Events().size() == 1);

            input.revision = IdentityFrom<WorldStreamingDiagnosticRevision>(2);
            auto successorEvent = Event(2, 2, operation, StreamingDiagnosticEventCategory::Admission);
            successorEvent.snapshotRevision = input.revision;
            const std::array successorEvents{successorEvent};
            const auto successor =
                WorldStreamingDiagnosticSnapshot::Replace(original, input, policy, Sample(policy), {{}, {}, {}, successorEvents});
            REQUIRE(successor.HasValue());
            REQUIRE(successor.Value().Revision().Value() == 2);
            REQUIRE(original.Revision().Value() == 1);
        }

        TEST_CASE("Closed World Streaming diagnostic snapshots replace idempotently without reviving admission",
                  "[unit][world_streaming][diagnostics][events][shutdown]") {
            auto input = ClosedInput();
            const auto policy = Policy();
            const auto first = CreateSnapshot(input, policy, Sample(policy));
            REQUIRE(first.HasValue());

            input.revision = IdentityFrom<WorldStreamingDiagnosticRevision>(2);
            const auto second = WorldStreamingDiagnosticSnapshot::Replace(first.Value(), input, policy, Sample(policy), {{}, {}, {}, {}});
            REQUIRE(second.HasValue());
            REQUIRE(second.Value().Lifecycle() == WorldStreamingRuntimeCompositionState::Closed);
            REQUIRE(second.Value().Queue().state == StreamingSchedulerAdmissionState::Closed);
            REQUIRE(second.Value().Events().empty());
        }

        TEST_CASE("Disabled World Streaming event instrumentation is allocation-free at the event seam and behavior neutral",
                  "[unit][world_streaming][diagnostics][events][disabled][headless]") {
            auto input = Input();
            input.instrumentation = StreamingDiagnosticInstrumentation::Disabled;
            input.lifecycle = WorldStreamingRuntimeCompositionState::Closed;
            input.queue.state = StreamingSchedulerAdmissionState::Closed;
            input.queue.reservedOperations = 0;
            input.queue.reservedCapacityUnits = 0;
            const auto policy = Policy();
            auto deliberatelyIgnored = Event(1, 1, Operation(1, 1, 1, 99), StreamingDiagnosticEventCategory::Admission);
            deliberatelyIgnored.category = static_cast<StreamingDiagnosticEventCategory>(255);
            const std::array ignoredEvents{deliberatelyIgnored};
            const auto result = CreateSnapshot(input, policy, Sample(policy), {}, {}, {}, ignoredEvents);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Events().empty());
            REQUIRE(result.Value().Lifecycle() == WorldStreamingRuntimeCompositionState::Closed);
            REQUIRE(result.Value().Queue().reservedOperations == 0);
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
