#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGErrors.h"
#include "PCGCpuEvaluatorInternal.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <string>
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

        void ReplaceWithFilterChain(PCGGraphSourceData &source, const std::uint32_t inputCount) {
            source.nodes.resize(1);
            source.edges.clear();
            source.exposedInputs.clear();
            NodeId previous = Id<NodeId>(10);
            PinId previousPin = Id<PinId>(101);
            for (std::uint32_t index = 0; index < inputCount; ++index) {
                const auto node = Id<NodeId>(1000 + index);
                const auto inputPin = Id<PinId>(10000 + index * 3);
                const auto scalarPin = Id<PinId>(10001 + index * 3);
                const auto outputPin = Id<PinId>(10002 + index * 3);
                source.nodes.push_back(
                    {node,
                     PCGCpuNodeType(PCGCpuNodeKind::DensityFilter).Value(),
                     {1, 0},
                     {PointPin(inputPin.Value(), PCGPinDirection::Input),
                      {scalarPin, PCGPinDirection::Input, PCGPinType::Scalar, PCGPinCardinality::Single, PCGGraphValue{0.5}},
                      PointPin(outputPin.Value(), PCGPinDirection::Output)},
                     {}});
                source.edges.push_back({Id<EdgeId>(index + 1), previous, previousPin, node, inputPin});
                source.exposedInputs.push_back(
                    {Id<ExposedInputId>(index + 1), "pcg.threshold_" + std::to_string(index), node, scalarPin, 0.5});
                previous = node;
                previousPin = outputPin;
            }
        }

        PCGCookedPlan Plan(const bool merge = false, const bool malformedPayload = false, const std::uint32_t runtimeVersion = 1,
                           const bool reordered = false, const std::uint32_t inputCount = 1,
                           const PCGOperationalTier tier = PCGOperationalTier::Baseline) {
            auto source = Source(1, merge);
            source.tier = tier;
            if (inputCount > 1)
                ReplaceWithFilterChain(source, inputCount);
            if (malformedPayload)
                source.nodes[0].payload.clear();
            if (reordered) {
                std::ranges::reverse(source.nodes);
                std::ranges::reverse(source.edges);
                for (auto &node : source.nodes)
                    std::ranges::reverse(node.pins);
            }
            const std::array<PCGNodeTypeSupport, 4> types{
                PCGNodeTypeSupport{PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value(), {1, 0}, {1, 0}},
                PCGNodeTypeSupport{PCGCpuNodeType(PCGCpuNodeKind::DensityFilter).Value(), {1, 0}, {1, 0}},
                PCGNodeTypeSupport{PCGCpuNodeType(PCGCpuNodeKind::Merge).Value(), {1, 0}, {1, 0}},
                PCGNodeTypeSupport{PCGCpuNodeType(PCGCpuNodeKind::Forward).Value(), {1, 0}, {1, 0}},
            };
            auto graph = PCGGraphAsset::Create(source, {.tier = tier, .supportedNodeTypes = types});
            INFO("graph capture error: " << (graph.HasError() ? graph.ErrorValue().code.Value() : std::string{"none"}));
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
            std::vector<PCGGraphNodeDescriptor> registered;
            for (const auto &node : source.nodes)
                registered.push_back({node.id, node.type});
            REQUIRE(registry.RegisterGraph({source.generation, std::move(registered), ValidationCapability()}).HasValue());
            auto snapshot = registry.Snapshot();
            REQUIRE(snapshot.HasValue());
            auto validated = ValidatePCGGraph(graph.Value(), snapshot.Value(), ValidationCapability());
            REQUIRE(validated.HasValue());
            auto plan = CompilePCGGraph(graph.Value(), validated.Value(), snapshot.Value(), ValidationCapability());
            REQUIRE(plan.HasValue());
            return std::move(plan).Value();
        }

        PCGSpatialSnapshot Spatial(const std::uint64_t revision = 1, const float origin = 0.0F, const std::uint64_t gridIdentity = 8,
                                   const std::uint32_t side = 2) {
            PCGSpatialSnapshotCandidate candidate;
            candidate.snapshot = Id<SpatialSnapshotId>(revision);
            candidate.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(revision)};
            candidate.bounds = {{-10.0F, -10.0F, -10.0F}, {10.0F, 10.0F, 10.0F}};
            candidate.coverage = PCGSpatialCoverage::Complete;
            candidate.grids = {PCGGrid{Id<SpatialElementId>(gridIdentity), {origin, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {side, side, side}}};
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

        std::vector<PCGPointOutputBound> Bounds(const bool merge = false, const std::size_t finalCount = 8,
                                                const std::size_t sourceCount = 8) {
            const auto schema = Schema();
            return {{0, Id<PinId>(101), schema, sourceCount},
                    {1, Id<PinId>(203), schema, sourceCount},
                    {2, Id<PinId>(302), schema, merge ? sourceCount * 2 : finalCount}};
        }

        PCGCpuEvaluationLimits Limits(const PCGCookedPlan &plan, const std::uint32_t workers = 1, JobSystem *jobs = nullptr) {
            const auto tier = LimitsForTier(plan.Tier()).Value();
            PCGCpuEvaluationLimits limits{tier.maximumScratchBytes, tier.maximumCandidateBytes, 0, workers};
            limits.jobs = jobs;
            const std::array capabilities{PCGCapability::Validation, PCGCapability::OfflineBake};
            limits.grantedCapabilities = PCGCapabilitySet::Create(capabilities).Value();
            limits.numericProfile.bytes[0] = 1;
            limits.world = Id<PCGWorldId>(1);
            limits.providerContent.bytes[0] = 1;
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
        const std::array nonfinite{PCGCpuInput{Id<ExposedInputId>(99), PCGGraphValue{std::numeric_limits<double>::infinity()}}};
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, nonfinite, Limits(plan)), PCGErrors::CpuEvaluationInvalid);
        const std::array unknown{PCGCpuInput{Id<ExposedInputId>(100), PCGGraphValue{0.5}}};
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, unknown, Limits(plan)), PCGErrors::CpuEvaluationInvalid);
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
        auto missingOutput = plan.Nodes()[0];
        missingOutput.pins.clear();
        CheckError(detail::ValidateNode(missingOutput), PCGErrors::CpuEvaluationInvalid);
        auto missingPointInput = plan.Nodes()[1];
        missingPointInput.pins.erase(missingPointInput.pins.begin());
        CheckError(detail::ValidateNode(missingPointInput), PCGErrors::CpuEvaluationInvalid);
        auto excessivePins = plan.Nodes()[0];
        excessivePins.pins.insert(excessivePins.pins.end(), 3, excessivePins.pins.front());
        CheckError(detail::ValidateNode(excessivePins), PCGErrors::CpuEvaluationInvalid);
        auto limits = Limits(plan);
        limits.numericProfile = {};
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::CpuEvaluationUnsupported);
        limits = Limits(plan);
        limits.grantedCapabilities = ValidationCapability();
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::UnsupportedCapability);
        limits.grantedCapabilities = {};
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::UnsupportedCapability);
        limits = Limits(plan);
        limits.workers = 0;
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::CpuEvaluationInvalid);
        limits.workers = 9;
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::CpuEvaluationInvalid);
        limits = Limits(plan, 2);
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::CpuEvaluationInvalid);
        JobSystem closedJobs{JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 4}};
        closedJobs.Shutdown(ShutdownPolicy::Cancel);
        CHECK(EvaluatePCGCpu(plan, spatial, bounds, {}, Limits(plan, 2, &closedJobs)).HasError());

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
        CheckError(EvaluatePCGCpu(plan, spatial, bounds, {}, limits), PCGErrors::CpuEvaluationCapacityExceeded);
    }

    TEST_CASE("PCG CPU uneven partitions and reordered authoring preserve every output column", "[unit][pcg][cpu]") {
        const auto plan = Plan(true);
        const auto reordered = Plan(true, false, 1, true);
        const auto spatial = Spatial(1, 0.0F, 8, 3);
        const auto bounds = Bounds(true, 27, 27);
        const auto reference = EvaluatePCGCpu(plan, spatial, bounds, {}, Limits(plan));
        REQUIRE(reference.HasValue());
        const auto &expected = *reference.Value().Outputs()[0].points;
        REQUIRE(expected.PointCount() == 54);
        JobSystem jobs{JobSystemConfig{.workerCount = 8, .maxQueuedJobs = 16}};
        for (std::uint32_t workers = 1; workers <= 8; ++workers) {
            CAPTURE(workers);
            const auto result = EvaluatePCGCpu(reordered, spatial, bounds, {}, Limits(reordered, workers, &jobs));
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Outputs().size() == 1);
            CHECK(result.Value().SourceDigest() == reference.Value().SourceDigest());
            CHECK(result.Value().Outputs()[0].node == reference.Value().Outputs()[0].node);
            CHECK(result.Value().Outputs()[0].pin == reference.Value().Outputs()[0].pin);
            const auto &actual = *result.Value().Outputs()[0].points;
            CHECK(std::ranges::equal(actual.Transforms(), expected.Transforms()));
            CHECK(std::ranges::equal(actual.Bounds(), expected.Bounds(), [](const auto &left, const auto &right) {
                return left.minimum == right.minimum && left.maximum == right.maximum;
            }));
            CHECK(std::ranges::equal(actual.Densities(), expected.Densities()));
            CHECK(std::ranges::equal(actual.Seeds(), expected.Seeds()));
            REQUIRE(actual.FindColumn<PCGScalarColumn>("pcg.weight") != nullptr);
            CHECK(*actual.FindColumn<PCGScalarColumn>("pcg.weight") == *expected.FindColumn<PCGScalarColumn>("pcg.weight"));
        }
    }

    TEST_CASE("PCG CPU binds all Standard and High exposed inputs in canonical roots", "[unit][pcg][cpu]") {
        for (const auto tier : {PCGOperationalTier::Standard, PCGOperationalTier::High}) {
            const auto count = static_cast<std::uint32_t>(LimitsForTier(tier).Value().maximumExposedInputs);
            CAPTURE(count);
            const auto plan = Plan(false, false, 1, false, count, tier);
            const auto schema = Schema();
            // The schema tier must match the request's actual supported operational tier.
            auto matching = PCGPointSchema::Capture({tier, schema->Attributes()});
            REQUIRE(matching.HasValue());
            const auto shared = std::make_shared<const PCGPointSchema>(std::move(matching).Value());
            std::vector<PCGPointOutputBound> bounds;
            for (std::uint32_t node = 0; node < plan.Nodes().size(); ++node)
                for (const auto &pin : plan.Nodes()[node].pins)
                    if (pin.direction == PCGPinDirection::Output)
                        bounds.push_back({node, pin.id, shared, 8});
            const auto first = EvaluatePCGCpu(plan, Spatial(), bounds, {}, Limits(plan));
            REQUIRE(first.HasValue());
            REQUIRE(first.Value().Provenance()[0].Data().inputs.size() == count);
            const std::array lastOverride{PCGCpuInput{Id<ExposedInputId>(count), 0.75, 2}};
            const auto changed = EvaluatePCGCpu(plan, Spatial(), bounds, lastOverride, Limits(plan));
            REQUIRE(changed.HasValue());
            CHECK(changed.Value().Provenance()[0].Key() != first.Value().Provenance()[0].Key());
            CHECK(changed.Value().Outputs()[0].points->Seeds()[0] != first.Value().Outputs()[0].points->Seeds()[0]);
            CHECK(changed.Value().Provenance()[0].Data().inputs.back().revision == 2);
        }
    }

    TEST_CASE("PCG CPU grid coordinates preserve accepted finite cancellation endpoints", "[unit][pcg][cpu]") {
        const auto plan = Plan();
        PCGSpatialSnapshotCandidate candidate;
        candidate.snapshot = Id<SpatialSnapshotId>(1);
        candidate.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(1)};
        const float maximum = std::numeric_limits<float>::max();
        candidate.bounds = {{-maximum, -1.0F, -1.0F}, {maximum, 1.0F, 1.0F}};
        candidate.coverage = PCGSpatialCoverage::Complete;
        candidate.grids = {{Id<SpatialElementId>(8), {-maximum, 0.0F, 0.0F}, {maximum, 1.0F, 1.0F}, {3, 1, 1}}};
        const auto spatial = CapturePCGSpatialSnapshot(std::move(candidate));
        INFO("spatial capture error: " << (spatial.HasError() ? spatial.ErrorValue().code.Value() : std::string{"none"}));
        REQUIRE(spatial.HasValue());
        const auto result = EvaluatePCGCpu(plan, spatial.Value(), Bounds(false, 3, 3), {}, Limits(plan));
        REQUIRE(result.HasValue());
        const auto points = result.Value().Outputs()[0].points;
        REQUIRE(points->PointCount() == 3);
        CHECK(points->Transforms()[0].translation.x == -maximum);
        CHECK(points->Transforms()[1].translation.x == 0.0F);
        CHECK(points->Transforms()[2].translation.x == maximum);
    }

    TEST_CASE("PCG CPU provenance binds effective inputs and world provider numeric truth", "[unit][pcg][cpu]") {
        const auto plan = Plan();
        const auto spatial = Spatial();
        const auto first = EvaluatePCGCpu(plan, spatial, Bounds(), {}, Limits(plan));
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().Provenance().size() == plan.Nodes().size());
        const auto &root = first.Value().Provenance()[0];
        CHECK(root.Data().graphContent == plan.SourceDigest());
        REQUIRE(root.Data().inputs.size() == 1);
        REQUIRE(root.Data().providers.size() == 1);
        CHECK(root.Data().providers[0].originEpoch == spatial.Coordinates().originEpoch);
        CHECK(first.Value().Outputs()[0].points->Seeds()[0] == root.Seed(Id<SourceSampleId>(1)).Value());
        for (int variation = 0; variation < 5; ++variation) {
            auto limits = Limits(plan);
            if (variation == 0)
                limits.world = Id<PCGWorldId>(2);
            if (variation == 1)
                limits.cell[0] = 1;
            if (variation == 2)
                limits.numericPolicyVersion = 2;
            if (variation == 3)
                limits.numericProfile.bytes[0] = 2;
            if (variation == 4)
                limits.providerContent.bytes[0] = 2;
            const auto changed = EvaluatePCGCpu(plan, spatial, Bounds(), {}, limits);
            REQUIRE(changed.HasValue());
            CHECK(changed.Value().Provenance()[0].Key() != root.Key());
            CHECK(changed.Value().Outputs()[0].points->Seeds()[0] != first.Value().Outputs()[0].points->Seeds()[0]);
            CHECK(ValidatePCGProvenanceReuse(root, changed.Value().Provenance()[0]).HasError());
        }
        const std::array overrideValue{PCGCpuInput{Id<ExposedInputId>(99), 0.75, 2}};
        const auto changed = EvaluatePCGCpu(plan, spatial, Bounds(), overrideValue, Limits(plan));
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().Provenance()[0].Data().inputs[0].content != root.Data().inputs[0].content);
        CHECK(changed.Value().Provenance()[0].Data().inputs[0].revision == 2);
        CHECK(changed.Value().Provenance()[0].Key() != root.Key());
        auto missing = Limits(plan);
        missing.providerContent = {};
        CheckError(EvaluatePCGCpu(plan, spatial, Bounds(), {}, missing), PCGErrors::CpuEvaluationInvalid);
        auto pointOnly = Limits(plan);
        const auto pointBytes = detail::AdmitCandidate(plan, Bounds(), pointOnly);
        REQUIRE(pointBytes.HasValue());
        pointOnly.maximumCandidateBytes = pointBytes.Value();
        CheckError(EvaluatePCGCpu(plan, spatial, Bounds(), {}, pointOnly), PCGErrors::CpuEvaluationCapacityExceeded);
        auto capacity = Limits(plan);
        const auto reservation = PCGPointCloudWorkspace::RequiredBytes(plan, Bounds());
        REQUIRE(reservation.HasValue());
        capacity.maximumScratchBytes = reservation.Value();
        CheckError(EvaluatePCGCpu(plan, spatial, Bounds(), {}, capacity), PCGErrors::CpuEvaluationCapacityExceeded);
    }

    TEST_CASE("PCG CPU candidates outlive evaluation inputs and failed replacement", "[unit][pcg][cpu]") {
        const auto retained = [] {
            const auto plan = Plan();
            const auto spatial = Spatial();
            auto result = EvaluatePCGCpu(plan, spatial, Bounds(), {}, Limits(plan));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }();
        REQUIRE(retained.Outputs().size() == 1);
        CHECK(retained.Outputs()[0].points->PointCount() == 8);
        CHECK(retained.Outputs()[0].points->Transforms()[7].translation == Math::Vec3{1.0F, 1.0F, 1.0F});
        const auto plan = Plan();
        auto bounds = Bounds();
        bounds[0].schema.reset();
        CheckError(EvaluatePCGCpu(plan, Spatial(), bounds, {}, Limits(plan)), PCGErrors::CpuEvaluationInvalid);
        CHECK(retained.Outputs()[0].points->Seeds().size() == 8);
        CHECK(retained.Seed() == 41);
    }
}  // namespace Horo::PCG
