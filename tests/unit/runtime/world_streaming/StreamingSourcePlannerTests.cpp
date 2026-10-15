#include "Horo/WorldStreaming/StreamingSourcePlanner.h"
#include "StreamingPlannerFixture.h"
#include "WorldStreamingTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::WorldStreaming {
    namespace {
        using namespace PlannerFixture;
        using TestSupport::RequireError;

        TEST_CASE("Resumable spatial queries bound visited work and preserve exact publication fences",
                  "[world_streaming][source_planner]") {
            const auto partition = Partition();
            const WorldPartitionSpatialQuery query{{Point(0), Point(1'599, 1'599, 99)}, {}, {}};
            std::vector<WorldPartitionCellHandle> expected(256);
            REQUIRE(partition.Query(query, expected).Value().matches == expected.size());
            auto cursor = partition.BeginQuery(query).Value();
            std::vector<WorldPartitionCellHandle> actual;
            while (!cursor.Complete()) {
                std::array<WorldPartitionCellHandle, 3> batch;
                const auto result = partition.QueryNext(cursor, {2, 2}, batch).Value();
                REQUIRE(result.nodesVisited <= 2);
                REQUIRE(result.candidatesExamined <= 2);
                REQUIRE(result.matches <= 2);
                actual.insert(actual.end(), batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(result.matches));
            }
            std::ranges::sort(actual, {}, &WorldPartitionCellHandle::slot);
            REQUIRE(actual == expected);
            const auto replacement = Partition(16, 1, 2);
            std::array<WorldPartitionCellHandle, 1> sentinel{expected[0]};
            RequireError(replacement.QueryNext(cursor, {2, 2}, sentinel), WorldStreamingErrors::PartitionRegistryStale);
            REQUIRE(sentinel[0] == expected[0]);
        }

        TEST_CASE("Camera player gameplay network and preload RAII sources reduce independently of registration order",
                  "[world_streaming][source_planner]") {
            const auto partition = Partition();
            auto forward = Planner(partition);
            auto reverse = Planner(partition);
            std::array<StreamingSourceRequest, 5> requests{Source(1, StreamingSourceIntent::Camera), Source(2), Source(3),
                                                           Source(4, StreamingSourceIntent::NetworkRelevance),
                                                           Source(5, StreamingSourceIntent::Preload)};
            requests[2].residency = StreamingDesiredResidency::Activated;
            requests[4].retention = StreamingRetention::Pinned;
            std::vector<StreamingSourceRegistration> first, second;
            for (const auto &request : requests)
                first.push_back(forward->Register(request).Value());
            for (auto iterator = requests.rbegin(); iterator != requests.rend(); ++iterator)
                second.push_back(reverse->Register(*iterator).Value());
            const auto left = Finish(*forward);
            const auto right = Finish(*reverse);
            REQUIRE(left.cells.size() == 2);
            REQUIRE(left.contributions.size() == 10);
            REQUIRE(left.cells.size() == right.cells.size());
            for (std::size_t row = 0; row < left.cells.size(); ++row) {
                REQUIRE(left.cells[row].cell == right.cells[row].cell);
                REQUIRE(left.cells[row].residency == StreamingDesiredResidency::Activated);
                REQUIRE(left.cells[row].pinnedFloor == StreamingDesiredResidency::Loaded);
                REQUIRE(left.cells[row].priority == right.cells[row].priority);
                REQUIRE(left.cells[row].prioritySource->id == right.cells[row].prioritySource->id);
            }
        }

        TEST_CASE("Source replacement destruction and world swap revoke published intent before recomputation",
                  "[world_streaming][source_planner][lifecycle]") {
            auto planner = Planner(Partition());
            auto request = Source();
            auto owner = planner->Register(request).Value();
            const auto oldToken = owner.Owner();
            REQUIRE(Finish(*planner).cells.size() == 2);
            request.revision = Id<StreamingSourceRevision>(2);
            request.shape = StreamingSourceBox{Point(800), Point(899, 99, 99)};
            REQUIRE(owner.Replace(request).HasValue());
            RequireError(planner->Plan(), WorldStreamingErrors::PartitionRegistryUnavailable);
            REQUIRE(Finish(*planner).contributions[0].cell.cell.x == 8);
            REQUIRE(owner.Close().HasValue());
            RequireError(planner->Plan(), WorldStreamingErrors::PartitionRegistryUnavailable);
            auto next = planner->Register(Source()).Value();
            REQUIRE(next.Owner().generation > oldToken.generation);
            REQUIRE(planner->ReplacePartition(Partition(16, 1, 2), Environment()).HasValue());
            RequireError(next.Replace(request), WorldStreamingErrors::SourceOwnerStale);
            auto current = planner->Register(Source()).Value();
            REQUIRE(next.Close().HasValue());
            REQUIRE(Finish(*planner).contributions.size() == 2);
            REQUIRE(planner->Shutdown().HasValue());
            REQUIRE(current.Close().HasValue());
            RequireError(planner->Plan(), WorldStreamingErrors::SourceLifecycleUnavailable);
        }

        TEST_CASE("Runtime grid layer profile platform and gameplay tag filters fail closed without source substitution",
                  "[world_streaming][source_planner][filters]") {
            auto planner = Planner(Partition());
            auto request = Source();
            request.selection = {.grid = Id<WorldPartitionRegistryId>(1),
                                 .layer = Layer(),
                                 .lod = 0,
                                 .profile = StreamingSourceProfile{1},
                                 .platformMask = 1,
                                 .requiredGameplayTags = 2,
                                 .excludedGameplayTags = 4};
            auto owner = planner->Register(request).Value();
            REQUIRE(Finish(*planner).contributions.size() == 2);
            auto environment = Environment(2);
            environment.gameplayTags = 0;
            REQUIRE(planner->ReplaceEnvironment(environment).HasValue());
            RequireError(planner->Plan(), WorldStreamingErrors::PartitionRegistryUnavailable);
            REQUIRE(Finish(*planner).contributions.empty());
            environment = Environment(3);
            environment.profile.value = 2;
            REQUIRE(planner->ReplaceEnvironment(environment).HasValue());
            REQUIRE(Finish(*planner).contributions.empty());
            environment = Environment(4);
            environment.platformBit = 2;
            REQUIRE(planner->ReplaceEnvironment(environment).HasValue());
            REQUIRE(Finish(*planner).contributions.empty());
            environment = Environment(5);
            environment.enabledLayers = {};
            REQUIRE(planner->ReplaceEnvironment(environment).HasValue());
            const auto disabled = Finish(*planner);
            REQUIRE(disabled.contributions.empty());
            REQUIRE(disabled.cells.empty());
            RequireError(planner->ReplaceEnvironment(Environment(5)), WorldStreamingErrors::SourceRevisionStale);
            request.revision = Id<StreamingSourceRevision>(2);
            request.selection.grid = Id<WorldPartitionRegistryId>(2);
            REQUIRE(owner.Replace(request).HasValue());
            REQUIRE(planner->ReplaceEnvironment(Environment(6)).HasValue());
            REQUIRE(Finish(*planner).contributions.empty());
        }

        TEST_CASE("Travel readiness requires fresh actual destination residency independently of desired activation",
                  "[world_streaming][source_planner][travel]") {
            const auto partition = Partition();
            auto planner = Planner(partition);
            auto request = Source(1, StreamingSourceIntent::Preload, 0, 99);
            request.residency = StreamingDesiredResidency::Activated;
            auto owner = planner->Register(request).Value();
            StreamingTravelResidencyEvidence evidence{partition.Binding(), 1, 1'000, {}};
            REQUIRE(planner->PollTravel(owner, request.revision, evidence, 1'000, 10).Value() == StreamingTravelReadiness::Pending);
            const auto plan = Finish(*planner);
            REQUIRE(plan.cells[0].residency == StreamingDesiredResidency::Activated);
            REQUIRE(planner->PollTravel(owner, request.revision, evidence, 1'000, 10).Value() == StreamingTravelReadiness::Pending);
            const StreamingFence fence{partition.Binding().owner.partition, partition.Binding().owner.epoch, plan.cells[0].cell.cell,
                                       Id<StreamingGeneration>(1)};
            auto operation =
                StreamingCellOperation::Create({Id<StreamingCellOperationId>(1), fence}, StreamingCellOperationKind::Load).Value();
            auto ledger = StreamingCellStateLedger::Create({partition.Binding().owner, 4}).Value();
            operation = operation.Advance(operation.Handle(), StreamingCellOperationTransition::Admit).Value();
            REQUIRE(ledger.Apply(partition.Binding().owner, operation).HasValue());
            operation = operation.Advance(operation.Handle(), StreamingCellOperationTransition::BeginPreparation).Value();
            REQUIRE(ledger.Apply(partition.Binding().owner, operation).HasValue());
            operation = operation.Advance(operation.Handle(), StreamingCellOperationTransition::Complete).Value();
            const auto resident = ledger.Apply(partition.Binding().owner, operation).Value();
            std::array facts{StreamingTravelResidencyFact{resident.operation.fence, resident.state}};
            evidence.cells = facts;
            REQUIRE(planner->PollTravel(owner, request.revision, evidence, 1'000, 10).Value() == StreamingTravelReadiness::Pending);
            auto activate =
                StreamingCellOperation::Create({Id<StreamingCellOperationId>(2), fence}, StreamingCellOperationKind::Activate).Value();
            for (const auto transition : {StreamingCellOperationTransition::Admit, StreamingCellOperationTransition::BeginPreparation,
                                          StreamingCellOperationTransition::BeginActivation, StreamingCellOperationTransition::Complete}) {
                activate = activate.Advance(activate.Handle(), transition).Value();
                const auto state = ledger.Apply(partition.Binding().owner, activate).Value();
                facts[0] = {state.operation.fence, state.state};
            }
            REQUIRE(planner->PollTravel(owner, request.revision, evidence, 1'000, 10).Value() == StreamingTravelReadiness::Ready);
            RequireError(planner->PollTravel(owner, request.revision, evidence, 1'011, 10), WorldStreamingErrors::PartitionRegistryStale);
            request.revision = Id<StreamingSourceRevision>(2);
            REQUIRE(owner.Replace(request).HasValue());
            RequireError(planner->PollTravel(owner, Id<StreamingSourceRevision>(1), evidence, 1'000, 10),
                         WorldStreamingErrors::SourceRevisionStale);
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
