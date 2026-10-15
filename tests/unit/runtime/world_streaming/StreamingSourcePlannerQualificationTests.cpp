#include "Horo/WorldStreaming/StreamingSourcePlanner.h"
#include "StreamingPlannerFixture.h"
#include "WorldStreamingTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::WorldStreaming {
    namespace {
        using namespace PlannerFixture;
        using TestSupport::RequireError;

        TEST_CASE("Continuous moving source samples make useful bounded progress alongside fixed travel intent",
                  "[world_streaming][source_planner][qualification]") {
            const auto partition = Partition();
            auto config = Config();
            config.limits = {8, 256, 2'048, 1, 2, 2, 2, 10'000};
            auto planner = StreamingSourcePlanner::Create(config, partition, Environment()).Value();
            auto cameraRequest = Source(1, StreamingSourceIntent::Camera, 0, 499);
            cameraRequest.prediction = StreamingSourceMotionSample{Point(0), {{{1'000, 0, 0}}}, 1'000};
            auto camera = planner->Register(cameraRequest).Value();
            auto player = planner->Register(Source(2)).Value();
            auto travel = planner->Register(Source(3, StreamingSourceIntent::Preload, 1'500, 99)).Value();
            const StreamingCellId destination{15, 0, 0, 0, Layer()};
            const std::array facts{StreamingTravelResidencyFact{{partition.Binding().owner.partition, partition.Binding().owner.epoch,
                                                                 destination, Id<StreamingGeneration>(1)},
                                                                StreamingCellState::Resident}};
            unsigned publications = 0;
            bool ready = false;
            for (unsigned frame = 0; frame < 500; ++frame) {
                const auto time = 1'001 + frame * 10;
                REQUIRE(camera.SampleMotion({Point(static_cast<std::int64_t>(frame % 8) * 100), {{{1'000, 0, 0}}}, time}).HasValue());
                REQUIRE(player.SampleMotion({Point(static_cast<std::int64_t>(frame % 4) * 100), {}, time}).HasValue());
                const auto progress = planner->Service(time).Value();
                REQUIRE(progress.sourceVisits <= 1);
                REQUIRE(progress.nodesVisited <= 2);
                REQUIRE(progress.candidatesExamined <= 2);
                REQUIRE(progress.stabilityRows <= 2);
                if (progress.published) {
                    ++publications;
                    const auto plan = planner->Plan().Value();
                    REQUIRE(time - plan.sampledAtServiceMilliseconds <= config.limits.maximumPlanAgeMilliseconds);
                    REQUIRE_FALSE(plan.contributions.empty());
                }
                const StreamingTravelResidencyEvidence evidence{partition.Binding(), frame + 1, time, facts};
                ready = ready || planner->PollTravel(travel, Id<StreamingSourceRevision>(1), evidence, time, 20).Value() ==
                                     StreamingTravelReadiness::Ready;
            }
            REQUIRE(publications >= 2);
            REQUIRE(ready);
            RequireError(travel.SampleMotion({Point(0), {}, 6'100}), WorldStreamingErrors::SourceIntentUnsupported);
        }

        TEST_CASE("High speed prefetch cannot pin or activate predicted cells and queue age grows across stable cycles",
                  "[world_streaming][source_planner][prediction][fairness]") {
            auto planner = Planner(Partition());
            auto request = Source(1, StreamingSourceIntent::Gameplay, 0, 99);
            request.residency = StreamingDesiredResidency::Activated;
            request.retention = StreamingRetention::Pinned;
            request.prediction = StreamingSourceMotionSample{Point(0), {{{10'000, 0, 0}}}, 1'000};
            auto owner = planner->Register(request).Value();
            auto first = Finish(*planner);
            bool predicted = false;
            for (const auto &contribution : first.contributions)
                if (contribution.predicted) {
                    predicted = true;
                    REQUIRE(contribution.residency == StreamingDesiredResidency::Loaded);
                    REQUIRE(contribution.retention == StreamingRetention::Releasable);
                }
            REQUIRE(predicted);
            const auto queuedAt = first.cells[0].queuedAtServiceMilliseconds;
            const double initialPriority = first.cells[0].priority;
            REQUIRE(owner.SampleMotion({Point(0), {{{10'000, 0, 0}}}, 2'000}).HasValue());
            const auto later = Finish(*planner, 2'000);
            REQUIRE(later.cells[0].queuedAtServiceMilliseconds == queuedAt);
            REQUIRE(later.cells[0].priority > initialPriority);
        }

        TEST_CASE("Boundary oscillation retains existing demand while hard revision regeneration revokes old publication",
                  "[world_streaming][source_planner][stability]") {
            auto config = Config();
            StreamingCellStabilityPolicyRequest stability;
            stability.id = Id<StreamingCellStabilityPolicyId>(1);
            stability.revision = Id<StreamingCellStabilityPolicyRevision>(1);
            stability.enterMarginMillimeters = 10;
            stability.exitMarginMillimeters = 20;
            stability.lingerMilliseconds = 100;
            stability.pressureLingerMilliseconds = 20;
            stability.maximumTrackedCells = 256;
            config.stability = StreamingCellStabilityPolicy::Create(stability).Value();
            auto planner = StreamingSourcePlanner::Create(config, Partition(), Environment()).Value();
            auto request = Source(1, StreamingSourceIntent::Gameplay, 10, 79);
            auto owner = planner->Register(request).Value();
            REQUIRE(Finish(*planner).cells.size() == 1);
            REQUIRE(owner.SampleMotion({Point(105), {}, 1'010}).HasValue());
            REQUIRE(Finish(*planner, 1'010).cells[0].cell.cell.x == 0);
            REQUIRE(owner.SampleMotion({Point(10), {}, 1'020}).HasValue());
            REQUIRE(Finish(*planner, 1'020).cells[0].cell.cell.x == 0);
            request.revision = Id<StreamingSourceRevision>(2);
            request.shape = StreamingSourceBox{Point(1'500), Point(1'599, 99, 99)};
            REQUIRE(owner.Replace(request).HasValue());
            RequireError(planner->Plan(), WorldStreamingErrors::PartitionRegistryUnavailable);
            const auto regenerated = Finish(*planner, 1'030);
            for (const auto &contribution : regenerated.contributions) {
                REQUIRE(contribution.source.revision.Value() == 2);
                REQUIRE(contribution.cell.cell.x == 15);
            }
        }

        TEST_CASE("Planner capacity failures never publish a partial cycle or skip the failing candidate on retry",
                  "[world_streaming][source_planner][failure]") {
            auto planner = Planner(Partition(), 1);
            auto owner = planner->Register(Source()).Value();
            bool failed = false;
            for (unsigned frame = 0; frame < 20 && !failed; ++frame) {
                const auto result = planner->Service(1'000);
                failed = result.HasError();
                if (failed)
                    RequireError(result, WorldStreamingErrors::SourceCapacityExceeded);
            }
            REQUIRE(failed);
            RequireError(planner->Plan(), WorldStreamingErrors::PartitionRegistryUnavailable);
            for (unsigned attempt = 0; attempt < 3; ++attempt) {
                bool retryFailed = false;
                for (unsigned frame = 0; frame < 20 && !retryFailed; ++frame)
                    retryFailed = planner->Service(1'000).HasError();
                REQUIRE(retryFailed);
                RequireError(planner->Plan(), WorldStreamingErrors::PartitionRegistryUnavailable);
            }
        }

        TEST_CASE("Shape margin evidence and canonical priority distance remain explicit for every supported source shape",
                  "[world_streaming][source_planner][geometry]") {
            const auto partition = Partition();
            StreamingSourceFrustum
                frustum{Point(50, 50, 50),
                        {{{{1, 0, 0}, 49}, {{-1, 0, 0}, 49}, {{0, 1, 0}, 49}, {{0, -1, 0}, 49}, {{0, 0, 1}, 49}, {{0, 0, -1}, 49}}},
                        49};
            StreamingSourcePathVolume path;
            path.points[0] = Point(25, 50, 50);
            path.points[1] = Point(75, 50, 50);
            path.pointCount = 2;
            path.halfExtentMillimeters = 20;
            const std::array<StreamingSourceShape, 4> shapes{StreamingSourceSphere{Point(50, 50, 50), 49},
                                                             StreamingSourceBox{Point(0), Point(99, 99, 99)}, frustum, path};
            for (const auto &shape : shapes) {
                REQUIRE(StreamingSourceQueryBounds(shape, 20).HasValue());
                REQUIRE(EvaluateStreamingSourceCellBoundary(shape, *partition.Grid(), {0, 0, 0, 0, Layer()}, 10, 20).Value() == 10);
                REQUIRE(EvaluateStreamingSourceCellBoundary(shape, *partition.Grid(), {15, 0, 0, 0, Layer()}, 10, 20).Value() == -21);
                REQUIRE(StreamingSourceCellDistance(shape, *partition.Grid(), {0, 0, 0, 0, Layer()}).Value() == 0);
                REQUIRE(StreamingSourceCellDistance(shape, *partition.Grid(), {1, 0, 0, 0, Layer()}).Value() > 0);
            }
            RequireError(StreamingSourceQueryBounds(shapes[0], -1), WorldStreamingErrors::SourceShapeInvalid);
        }

        TEST_CASE("Travel rejects a different issuer even when copied token fields collide and reports terminal residency failure",
                  "[world_streaming][source_planner][travel][lifecycle]") {
            const auto partition = Partition();
            auto planner = Planner(partition);
            auto other = Planner(partition);
            auto request = Source(1, StreamingSourceIntent::Preload, 0, 99);
            auto owner = planner->Register(request).Value();
            auto foreign = other->Register(request).Value();
            REQUIRE(owner.Owner() == foreign.Owner());
            const StreamingTravelResidencyEvidence empty{partition.Binding(), 1, 1'000, {}};
            RequireError(planner->PollTravel(foreign, request.revision, empty, 1'000, 10), WorldStreamingErrors::SourceOwnerStale);
            const auto plan = Finish(*planner);
            const std::array failed{StreamingTravelResidencyFact{{partition.Binding().owner.partition, partition.Binding().owner.epoch,
                                                                  plan.contributions[0].cell.cell, Id<StreamingGeneration>(1)},
                                                                 StreamingCellState::Failed}};
            REQUIRE(planner->PollTravel(owner, request.revision, {partition.Binding(), 2, 1'000, failed}, 1'000, 10).Value() ==
                    StreamingTravelReadiness::Failed);
            REQUIRE(owner.Close().HasValue());
            RequireError(planner->PollTravel(owner, request.revision, empty, 1'000, 10), WorldStreamingErrors::SourceOwnerStale);
        }

        TEST_CASE("Invalid replacement and exhausted source capacity preserve admitted state and publication",
                  "[world_streaming][source_planner][admission]") {
            auto config = Config();
            config.limits.sources = 1;
            config.limits.sourceVisitsPerFrame = 1;
            config.shapeSupport.sphere = false;
            auto planner = StreamingSourcePlanner::Create(config, Partition(), Environment()).Value();
            auto request = Source();
            auto owner = planner->Register(request).Value();
            const auto sequence = Finish(*planner).revision;
            RequireError(planner->Register(Source(2)), WorldStreamingErrors::SourceCapacityExceeded);
            RequireError(owner.Replace(request), WorldStreamingErrors::SourceRevisionStale);
            REQUIRE(planner->Plan().Value().revision == sequence);
            request.revision = Id<StreamingSourceRevision>(2);
            request.shape = StreamingSourceSphere{Point(50), 10};
            RequireError(owner.Replace(request), WorldStreamingErrors::SourceShapeUnsupported);
            REQUIRE(planner->Plan().Value().revision == sequence);
            auto environment = Environment(2);
            environment.platformBit = 3;
            RequireError(planner->ReplaceEnvironment(environment), WorldStreamingErrors::SourceDescriptorInvalid);
            REQUIRE(planner->Plan().Value().revision == sequence);
        }

        TEST_CASE("Foreign lane calls reject before source storage access and retained receipts survive planner shutdown",
                  "[world_streaming][source_planner][lifecycle]") {
            auto planner = Planner(Partition());
            auto owner = planner->Register(Source()).Value();
            bool rejected = false;
            std::thread foreign([&] {
                rejected = planner->Service(1'000).HasError() && owner.Close().HasError();
            });
            foreign.join();
            REQUIRE(rejected);
            REQUIRE(Finish(*planner).cells.size() == 2);
            planner.reset();
            REQUIRE(owner.Close().HasValue());
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
