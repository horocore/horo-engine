#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::PCG {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value) {
            auto result = Identity::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        template <typename T> void CheckError(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        PCGCapabilitySet ValidationCapability() {
            const std::array capabilities{PCGCapability::Validation};
            auto set = PCGCapabilitySet::Create(capabilities);
            REQUIRE(set.HasValue());
            return set.Value();
        }

        PCGGraphPin PointPin(const std::uint64_t value, const PCGPinDirection direction) {
            return {Id<PinId>(value), direction, PCGPinType::PointSet,
                    direction == PCGPinDirection::Input ? PCGPinCardinality::Single : PCGPinCardinality::Multiple, std::nullopt};
        }

        PCGGraphSourceData Source(const std::uint64_t revision = 1, const bool merge = false) {
            PCGGraphSourceData source;
            source.generation = {Id<GraphId>(77), Id<GraphRevision>(revision)};
            source.deterministicSeed = 41;
            const auto gridType = PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value();
            const auto filterType = PCGCpuNodeType(PCGCpuNodeKind::DensityFilter).Value();
            const auto finalType = PCGCpuNodeType(merge ? PCGCpuNodeKind::Merge : PCGCpuNodeKind::Forward).Value();
            source.nodes = {
                {Id<NodeId>(10), gridType, {1, 0}, {PointPin(101, PCGPinDirection::Output)}, {0, 0, 0, 0, 0, 0, 0, 8}},
                {Id<NodeId>(20),
                 filterType,
                 {1, 0},
                 {PointPin(201, PCGPinDirection::Input),
                  {Id<PinId>(202), PCGPinDirection::Input, PCGPinType::Scalar, PCGPinCardinality::Single, PCGGraphValue{0.5}},
                  PointPin(203, PCGPinDirection::Output)},
                 {}},
                {Id<NodeId>(30),
                 finalType,
                 {1, 0},
                 merge ? std::vector{PointPin(301, PCGPinDirection::Input), PointPin(303, PCGPinDirection::Input),
                                     PointPin(302, PCGPinDirection::Output)}
                       : std::vector{PointPin(301, PCGPinDirection::Input), PointPin(302, PCGPinDirection::Output)},
                 {}},
            };
            source.edges = {{Id<EdgeId>(1), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(20), Id<PinId>(201)},
                            {Id<EdgeId>(2), Id<NodeId>(20), Id<PinId>(203), Id<NodeId>(30), Id<PinId>(301)}};
            if (merge)
                source.edges.push_back({Id<EdgeId>(3), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(30), Id<PinId>(303)});
            source.exposedInputs = {{Id<ExposedInputId>(99), "pcg.threshold", Id<NodeId>(20), Id<PinId>(202), 0.5}};
            return source;
        }

        PCGCookedPlan Plan(const bool merge = false, const bool malformedPayload = false, const std::uint32_t runtimeVersion = 1) {
            auto source = Source(1, merge);
            if (malformedPayload)
                source.nodes[0].payload.clear();
            const std::array<PCGNodeTypeSupport, 4> types{
                PCGNodeTypeSupport{PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value(), {1, 0}, {1, 0}},
                PCGNodeTypeSupport{PCGCpuNodeType(PCGCpuNodeKind::DensityFilter).Value(), {1, 0}, {1, 0}},
                PCGNodeTypeSupport{PCGCpuNodeType(PCGCpuNodeKind::Merge).Value(), {1, 0}, {1, 0}},
                PCGNodeTypeSupport{PCGCpuNodeType(PCGCpuNodeKind::Forward).Value(), {1, 0}, {1, 0}},
            };
            auto graph = PCGGraphAsset::Create(source, {.tier = PCGOperationalTier::Baseline, .supportedNodeTypes = types});
            REQUIRE(graph.HasValue());
            auto projection = ProjectPCGCapabilities(PCGHostProfile::Interactive, ValidationCapability());
            REQUIRE(projection.HasValue());
            auto created = PCGRegistry::Create(Id<PCGRegistryInstanceId>(1), projection.Value());
            REQUIRE(created.HasValue());
            auto registry = std::move(created).Value();
            for (const auto &type : types) {
                const auto determinism =
                    type.type == types[0].type ? PCGNodeDeterminism::ProfileDeterministic : PCGNodeDeterminism::PortableDeterministic;
                REQUIRE(registry.RegisterNodeRuntime({type.type, runtimeVersion, determinism, {}}).HasValue());
            }
            REQUIRE(registry
                        .RegisterGraph({source.generation,
                                        {{Id<NodeId>(10), types[0].type},
                                         {Id<NodeId>(20), types[1].type},
                                         {Id<NodeId>(30), merge ? types[2].type : types[3].type}},
                                        ValidationCapability()})
                        .HasValue());
            auto snapshot = registry.Snapshot();
            REQUIRE(snapshot.HasValue());
            auto validated = ValidatePCGGraph(graph.Value(), snapshot.Value(), ValidationCapability());
            REQUIRE(validated.HasValue());
            auto plan = CompilePCGGraph(graph.Value(), validated.Value(), snapshot.Value(), ValidationCapability());
            REQUIRE(plan.HasValue());
            return std::move(plan).Value();
        }

        PCGSpatialSnapshot Spatial(const std::uint64_t revision = 1, const float origin = 0.0F, const std::uint64_t gridIdentity = 8) {
            PCGSpatialSnapshotCandidate candidate;
            candidate.snapshot = Id<SpatialSnapshotId>(revision);
            candidate.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(revision)};
            candidate.bounds = {{-10.0F, -10.0F, -10.0F}, {10.0F, 10.0F, 10.0F}};
            candidate.coverage = PCGSpatialCoverage::Complete;
            candidate.grids = {PCGGrid{Id<SpatialElementId>(gridIdentity), {origin, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {2, 2, 2}}};
            auto captured = CapturePCGSpatialSnapshot(std::move(candidate));
            REQUIRE(captured.HasValue());
            return captured.Value();
        }

        std::shared_ptr<const PCGPointSchema> Schema() {
            auto key = PCGAttributeKey::Create("pcg.weight");
            REQUIRE(key.HasValue());
            const std::array fields{PCGAttributeDescriptor{std::move(key).Value(), PCGAttributeType::Scalar}};
            auto schema = PCGPointSchema::Capture({PCGOperationalTier::Baseline, fields});
            REQUIRE(schema.HasValue());
            return std::make_shared<const PCGPointSchema>(std::move(schema).Value());
        }

        std::vector<PCGPointOutputBound> Bounds(const bool merge = false, const std::size_t finalCount = 8) {
            const auto schema = Schema();
            return {{0, Id<PinId>(101), schema, 8}, {1, Id<PinId>(203), schema, 8}, {2, Id<PinId>(302), schema, merge ? 16 : finalCount}};
        }

        PCGCpuEvaluationLimits Limits(const PCGCookedPlan &plan, const std::uint32_t workers = 1, JobSystem *jobs = nullptr) {
            const auto tier = LimitsForTier(plan.Tier()).Value();
            PCGCpuEvaluationLimits limits{tier.maximumScratchBytes, tier.maximumCandidateBytes, 0, workers};
            limits.jobs = jobs;
            limits.grantedCapabilities = ValidationCapability();
            limits.numericProfile.bytes[0] = 1;
            return limits;
        }
    }  // namespace

    TEST_CASE("PCG CPU dispatch produces identical immutable point candidates across worker partitions", "[unit][pcg][cpu]") {
        const auto plan = Plan(true);
        const auto spatial = Spatial();
        const auto bounds = Bounds(true);
        JobSystem jobs{JobSystemConfig{.workerCount = 8, .maxQueuedJobs = 16}};
        const auto single = EvaluatePCGCpu(plan, spatial, bounds, {}, Limits(plan, 1));
        const auto many = EvaluatePCGCpu(plan, spatial, bounds, {}, Limits(plan, 8, &jobs));
        REQUIRE(single.HasValue());
        REQUIRE(many.HasValue());
        CHECK(plan.Seed() == 41);
        CHECK(single.Value().Seed() == plan.Seed());
        CHECK(single.Value().SourceDigest() == plan.SourceDigest());
        CHECK(single.Value().Generation() == plan.Generation());
        CHECK(single.Value().NumericProfile() == Limits(plan).numericProfile);
        REQUIRE(single.Value().Outputs().size() == 1);
        REQUIRE(many.Value().Outputs().size() == 1);
        const auto &left = *single.Value().Outputs()[0].points;
        const auto &right = *many.Value().Outputs()[0].points;
        REQUIRE(left.PointCount() == 16);
        CHECK(left.Transforms()[0].translation.x == 0.0F);
        CHECK(left.Transforms()[8].translation.x == 0.0F);
        CHECK(left.Transforms().size() == right.Transforms().size());
        for (std::size_t index = 0; index < left.PointCount(); ++index) {
            CHECK(left.Transforms()[index] == right.Transforms()[index]);
            CHECK(left.Seeds()[index] == right.Seeds()[index]);
        }
        REQUIRE(left.FindColumn<PCGScalarColumn>("pcg.weight") != nullptr);
        CHECK(left.FindColumn<PCGScalarColumn>("pcg.weight")->size() == 16);
    }

    TEST_CASE("PCG CPU evaluation enforces typed filters, empty boundaries and output bounds", "[unit][pcg][cpu]") {
        const auto plan = Plan();
        const auto spatial = Spatial();
        const auto bounds = Bounds();
        const std::array empty{PCGCpuInput{Id<ExposedInputId>(99), PCGGraphValue{2.0}}};
        const auto filtered = EvaluatePCGCpu(plan, spatial, bounds, empty, Limits(plan));
        REQUIRE(filtered.HasValue());
        REQUIRE(filtered.Value().Outputs().size() == 1);
        CHECK(filtered.Value().Outputs()[0].points->PointCount() == 0);
        JobSystem jobs{JobSystemConfig{.workerCount = 4, .maxQueuedJobs = 8}};
        const auto filteredParallel = EvaluatePCGCpu(plan, spatial, bounds, empty, Limits(plan, 4, &jobs));
        REQUIRE(filteredParallel.HasValue());
        REQUIRE(filteredParallel.Value().Outputs().size() == 1);
        CHECK(filteredParallel.Value().Outputs()[0].points->PointCount() == 0);

        auto narrow = Bounds(false, 7);
        CheckError(EvaluatePCGCpu(plan, spatial, narrow, {}, Limits(plan)), PCGErrors::PointCapacityExceeded);
        const std::array wrongType{PCGCpuInput{Id<ExposedInputId>(99), PCGGraphValue{true}}};
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, wrongType, Limits(plan)), PCGErrors::CpuEvaluationInvalid);
        const std::array duplicate{empty[0], empty[0]};
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, duplicate, Limits(plan)), PCGErrors::CpuEvaluationInvalid);
    }

    TEST_CASE("PCG CPU replacement retains prior candidate and closes cancelled admission", "[unit][pcg][cpu]") {
        const auto plan = Plan();
        const auto bounds = Bounds();
        const auto old = Spatial();
        auto first = EvaluatePCGCpu(plan, old, bounds, {}, Limits(plan));
        REQUIRE(first.HasValue());
        const auto next = Spatial(2, 3.0F);
        auto replacementLimits = Limits(plan);
        replacementLimits.retainedBytes = first.Value().ReservedBytes();
        auto replacement = EvaluatePCGCpu(plan, next, bounds, {}, replacementLimits);
        REQUIRE(replacement.HasValue());
        CHECK(first.Value().Outputs()[0].points->Transforms()[0].translation.x == 0.0F);
        CHECK(replacement.Value().Outputs()[0].points->Transforms()[0].translation.x == 3.0F);
        CHECK(first.Value().Snapshot() == old.Id());
        CHECK(replacement.Value().Snapshot() == next.Id());

        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CheckError(EvaluatePCGCpu(plan, next, bounds, {}, Limits(plan), PCGCpuAdmission::Accepting, cancelled.Token()),
                   PCGErrors::CpuEvaluationClosed);
        CheckError(EvaluatePCGCpu(plan, next, bounds, {}, Limits(plan), PCGCpuAdmission::ShuttingDown), PCGErrors::CpuEvaluationClosed);
        auto noCapacity = Limits(plan);
        noCapacity.maximumCandidateBytes = 1;
        CheckError(EvaluatePCGCpu(plan, next, bounds, {}, noCapacity), PCGErrors::CpuEvaluationCapacityExceeded);
    }

    TEST_CASE("PCG CPU dispatch rejects unsupported descriptors and missing immutable input evidence", "[unit][pcg][cpu]") {
        CheckError(PCGCpuNodeType(static_cast<PCGCpuNodeKind>(99)), PCGErrors::CpuEvaluationUnsupported);
        const auto spatial = Spatial();
        const auto bounds = Bounds();
        const auto plan = Plan();
        auto limits = Limits(plan);
        limits.numericProfile = {};
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::CpuEvaluationUnsupported);
        limits = Limits(plan);
        limits.grantedCapabilities = {};
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::UnsupportedCapability);
        limits = Limits(plan);
        limits.workers = 0;
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::CpuEvaluationInvalid);
        limits.workers = 9;
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::CpuEvaluationInvalid);
        limits = Limits(plan, 2);
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::CpuEvaluationInvalid);

        const auto malformed = Plan(false, true);
        CheckError(EvaluatePCGCpu(malformed, spatial, bounds, {}, Limits(malformed)), PCGErrors::CpuEvaluationInvalid);
        const auto unsupported = Plan(false, false, 2);
        CheckError(EvaluatePCGCpu(unsupported, spatial, bounds, {}, Limits(unsupported)), PCGErrors::CpuEvaluationUnsupported);
        const auto differentGrid = Spatial(2, 0.0F, 9);
        CheckError(EvaluatePCGCpu(plan, differentGrid, bounds, {}, Limits(plan)), PCGErrors::CpuEvaluationInvalid);
    }

    TEST_CASE("PCG CPU routed schema and aggregate budget fail without a partial candidate", "[unit][pcg][cpu]") {
        const auto plan = Plan();
        const auto spatial = Spatial();
        auto bounds = Bounds();
        auto emptySchema = PCGPointSchema::Capture({PCGOperationalTier::Baseline, {}});
        REQUIRE(emptySchema.HasValue());
        bounds[2].schema = std::make_shared<const PCGPointSchema>(std::move(emptySchema).Value());
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, Limits(plan)), PCGErrors::CpuEvaluationInvalid);

        bounds = Bounds();
        auto limits = Limits(plan);
        limits.retainedBytes = LimitsForTier(plan.Tier()).Value().maximumReplacementOverlapBytes;
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::PointCapacityExceeded);
    }
}  // namespace Horo::PCG
