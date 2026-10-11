#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGNodeCatalog.h"
#include "Horo/PCG/PCGPointGeneration.h"
#include "PCGPointIdentityInternal.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace Horo::PCG {
    namespace {
        template <typename T> T Id(const std::uint64_t value) {
            const auto id = T::Create(value);
            REQUIRE(id.HasValue());
            return id.Value();
        }

        template <typename T> void CheckError(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        PCGCapabilitySet Grant() {
            const std::array capabilities{PCGCapability::Validation, PCGCapability::OfflineBake};
            return PCGCapabilitySet::Create(capabilities).Value();
        }

        PCGCookedPlan Compile(const PCGGraphSourceData &source, const PCGNodeCatalogSnapshot &catalog) {
            std::vector<PCGNodeTypeSupport> types;
            for (const auto &entry : catalog.Nodes())
                types.push_back({entry.type, entry.version, entry.version});
            const auto graph = PCGGraphAsset::Create(source, {.supportedNodeTypes = types});
            REQUIRE(graph.HasValue());
            auto created = PCGRegistry::Create(Id<PCGRegistryInstanceId>(1), catalog.Capabilities());
            REQUIRE(created.HasValue());
            auto registry = std::move(created).Value();
            for (const auto &entry : catalog.Nodes())
                REQUIRE(
                    registry.RegisterNodeRuntime({entry.type, entry.runtimeContractVersion, entry.determinism, entry.requiredCapabilities})
                        .HasValue());
            PCGGraphDescriptor descriptor;
            descriptor.generation = source.generation;
            descriptor.requiredCapabilities = Grant();
            for (const auto &node : source.nodes)
                descriptor.nodes.push_back({node.id, node.type});
            REQUIRE(registry.RegisterGraph(descriptor).HasValue());
            const auto snapshot = registry.Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto validated = ValidatePCGGraph(graph.Value(), snapshot.Value(), Grant());
            REQUIRE(validated.HasValue());
            const auto plan = CompilePCGGraph(graph.Value(), validated.Value(), snapshot.Value(), catalog, Grant(),
                                              std::min(LimitsForTier(source.tier).Value().maximumResidentPlanBytes,
                                                       PCGGraphSourceHardLimits::SourceBytes));
            REQUIRE(plan.HasValue());
            return plan.Value();
        }

        PCGCookedPlan Cook(const PCGCpuNodeKind kind, std::vector<std::uint8_t> payload, const std::uint64_t seed = 41) {
            const auto projection = ProjectPCGCapabilities(PCGHostProfile::Interactive, Grant());
            REQUIRE(projection.HasValue());
            auto created = PCGNodeCatalog::Create(projection.Value());
            REQUIRE(created.HasValue());
            auto owner = std::move(created).Value();
            REQUIRE(owner.Register(kind).HasValue());
            const auto catalog = owner.Snapshot();
            REQUIRE(catalog.HasValue());
            const auto &entry = catalog.Value().Nodes().front();
            PCGGraphSourceData source;
            source.generation = {Id<GraphId>(77), Id<GraphRevision>(1)};
            source.deterministicSeed = seed;
            source.nodes = {{Id<NodeId>(10),
                             entry.type,
                             entry.version,
                             {{Id<PinId>(101), PCGPinDirection::Output, PCGPinType::PointSet, PCGPinCardinality::Multiple, std::nullopt}},
                             std::move(payload)}};
            auto plan = Compile(source, catalog.Value());
            REQUIRE(owner.Close().HasValue());
            return plan;
        }

        template <typename T> std::vector<std::uint8_t> Payload(const T &settings) {
            const auto encoded = EncodePCGPointGenerationSettings(settings);
            REQUIRE(encoded.HasValue());
            return encoded.Value();
        }

        PCGGraphPin PointPin(const std::uint64_t value, const PCGPinDirection direction) {
            return {Id<PinId>(value), direction, PCGPinType::PointSet,
                    direction == PCGPinDirection::Input ? PCGPinCardinality::Single : PCGPinCardinality::Multiple, std::nullopt};
        }

        PCGCookedPlan RoutedPlan(const bool duplicate, const double threshold = 0.5) {
            auto created = PCGNodeCatalog::Create(ProjectPCGCapabilities(PCGHostProfile::Interactive, Grant()).Value());
            REQUIRE(created.HasValue());
            auto owner = std::move(created).Value();
            for (const auto kind :
                 {PCGCpuNodeKind::GridJitter, PCGCpuNodeKind::DensityFilter, PCGCpuNodeKind::Forward, PCGCpuNodeKind::Merge})
                REQUIRE(owner.Register(kind).HasValue());
            const auto root = owner.Snapshot().Value();
            const auto type = [](const PCGCpuNodeKind kind) {
                return PCGCpuNodeType(kind).Value();
            };
            PCGGraphSourceData source;
            source.generation = {Id<GraphId>(77), Id<GraphRevision>(1)};
            source.deterministicSeed = 41;
            const auto jitter = Payload(PCGJitterGenerationSettings{Id<SpatialElementId>(8), 1.0});
            source.nodes =
                {{Id<NodeId>(10), type(PCGCpuNodeKind::GridJitter), {1, 0}, {PointPin(101, PCGPinDirection::Output)}, jitter},
                 {Id<NodeId>(30),
                  type(PCGCpuNodeKind::DensityFilter),
                  {1, 0},
                  {PointPin(301, PCGPinDirection::Input),
                   {Id<PinId>(302), PCGPinDirection::Input, PCGPinType::Scalar, PCGPinCardinality::Single, PCGGraphValue{threshold}},
                   PointPin(303, PCGPinDirection::Output)},
                  {}},
                 {Id<NodeId>(40),
                  type(PCGCpuNodeKind::Forward),
                  {1, 0},
                  {PointPin(401, PCGPinDirection::Input), PointPin(402, PCGPinDirection::Output)},
                  {}},
                 {Id<NodeId>(50),
                  type(PCGCpuNodeKind::Merge),
                  {1, 0},
                  {PointPin(501, PCGPinDirection::Input), PointPin(502, PCGPinDirection::Input), PointPin(503, PCGPinDirection::Output)},
                  {}}};
            if (!duplicate)
                source.nodes.push_back(
                    {Id<NodeId>(20), type(PCGCpuNodeKind::GridJitter), {1, 0}, {PointPin(201, PCGPinDirection::Output)}, jitter});
            source.edges = {{Id<EdgeId>(1), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(30), Id<PinId>(301)},
                            {Id<EdgeId>(2), Id<NodeId>(30), Id<PinId>(303), Id<NodeId>(40), Id<PinId>(401)},
                            {Id<EdgeId>(3), Id<NodeId>(40), Id<PinId>(402), Id<NodeId>(50), Id<PinId>(501)},
                            {Id<EdgeId>(4), Id<NodeId>(duplicate ? 10 : 20), Id<PinId>(duplicate ? 101 : 201), Id<NodeId>(50),
                             Id<PinId>(502)}};
            return Compile(source, root);
        }

        PCGSpatialSnapshot Spatial(const std::uint64_t revision = 1) {
            PCGSpatialSnapshotCandidate source;
            source.snapshot = Id<SpatialSnapshotId>(revision);
            source.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(revision)};
            source.bounds = {{-2.0F, -2.0F, -2.0F}, {2.0F, 2.0F, 2.0F}};
            source.coverage = PCGSpatialCoverage::Complete;
            source.grids = {{Id<SpatialElementId>(8), {}, {1.0F, 1.0F, 1.0F}, {2, 2, 2}}};
            source.volumes = {PCGBoxVolume{Id<SpatialElementId>(9), {{-1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, 1.0F}}}};
            const auto captured = CapturePCGSpatialSnapshot(std::move(source));
            REQUIRE(captured.HasValue());
            return captured.Value();
        }

        PCGPointOutputBound Bound(const std::size_t count) {
            std::vector<PCGAttributeDescriptor> fields;
            for (const auto name : PCGSourceRootAttributes)
                fields.push_back({PCGAttributeKey::Create(name).Value(), PCGAttributeType::UnsignedInteger});
            fields.push_back({PCGAttributeKey::Create(PCGSourceSampleAttribute).Value(), PCGAttributeType::UnsignedInteger});
            const auto schema = PCGPointSchema::Capture({PCGOperationalTier::Baseline, fields});
            REQUIRE(schema.HasValue());
            return {0, Id<PinId>(101), std::make_shared<const PCGPointSchema>(schema.Value()), count};
        }

        PCGCpuEvaluationLimits Limits(const PCGCookedPlan &plan, const std::uint8_t profile = 1) {
            const auto tier = LimitsForTier(plan.Tier()).Value();
            PCGCpuEvaluationLimits limits{tier.maximumScratchBytes, tier.maximumCandidateBytes};
            limits.grantedCapabilities = Grant();
            limits.numericProfile.bytes[0] = profile;
            limits.world = Id<PCGWorldId>(1);
            limits.providerContent.bytes[0] = 1;
            return limits;
        }

        PCGCpuCandidate Evaluate(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial, const std::size_t count,
                                 const std::uint8_t profile = 1, const std::uint32_t workers = 1, JobSystem *jobs = nullptr) {
            const std::array bounds{Bound(count)};
            auto limits = Limits(plan, profile);
            limits.workers = workers;
            limits.jobs = jobs;
            const auto candidate = EvaluatePCGCpu(plan, spatial, bounds, {}, limits);
            REQUIRE(candidate.HasValue());
            return candidate.Value();
        }

        const PCGPointStorage &Points(const PCGCpuCandidate &candidate) {
            REQUIRE(candidate.Outputs().size() == 1);
            return *candidate.Outputs()[0].points;
        }

        Result<void> RetainedIdentities(const std::span<const std::array<std::uint64_t, 5>> identities, const bool mismatch = false) {
            const auto plan = Cook(PCGCpuNodeKind::SnapshotGrid, Payload(PCGGridGenerationSettings{Id<SpatialElementId>(8)}));
            const std::array bounds{Bound(identities.size())};
            const auto tier = LimitsForTier(plan.Tier()).Value();
            const auto workspace = PCGPointCloudWorkspace::Create(plan, bounds, tier.maximumScratchBytes);
            REQUIRE(workspace.HasValue());
            const auto writer = workspace.Value()->BeginOutput(0, Id<PinId>(101), identities.size());
            REQUIRE(writer.HasValue());
            for (std::size_t index = 0; index < identities.size(); ++index) {
                Sha256Digest key;
                for (std::size_t word = 0; word < 4; ++word) {
                    REQUIRE(writer.Value()
                                .SetColumnValue<PCGUnsignedIntegerColumn>(PCGSourceRootAttributes[word], index, identities[index][word])
                                .HasValue());
                    for (std::size_t byte = 0; byte < 8; ++byte)
                        key.bytes[word * 8 + byte] = static_cast<std::uint8_t>(identities[index][word] >> ((7 - byte) * 8));
                }
                REQUIRE(writer.Value()
                            .SetColumnValue<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute, index, identities[index][4])
                            .HasValue());
                // Zero sample is deliberately malformed evidence, but core point storage accepts arbitrary seed values.
                const auto sample = SourceSampleId::Create(identities[index][4]);
                const auto seed = sample.HasValue() ? DerivePCGSampleSeed(key, sample.Value()).Value() : 0;
                REQUIRE(writer.Value().SetPoint(index, {}, {{}, {}}, 1.0F, mismatch ? seed ^ 1U : seed).HasValue());
            }
            REQUIRE(workspace.Value()->SealOutput(0, Id<PinId>(101)).HasValue());
            REQUIRE(workspace.Value()->FinishNode(0).HasValue());
            const auto final = workspace.Value()->ReadFinal(0, Id<PinId>(101));
            REQUIRE(final.HasValue());
            const std::array inputs{final.Value()};
            return detail::ValidatePointIdentities(inputs, {});
        }

        void CheckIdentities(const PCGCpuCandidate &candidate) {
            const auto &points = Points(candidate);
            const auto *samples = points.FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute);
            REQUIRE(samples != nullptr);
            REQUIRE(samples->size() == points.PointCount());
            for (std::size_t index = 0; index < samples->size(); ++index) {
                CHECK((*samples)[index] != 0);
                if (index != 0)
                    CHECK((*samples)[index] > (*samples)[index - 1]);
                Sha256Digest key;
                for (std::size_t word = 0; word < 4; ++word) {
                    const auto *values = points.FindColumn<PCGUnsignedIntegerColumn>(PCGSourceRootAttributes[word]);
                    REQUIRE(values != nullptr);
                    for (std::size_t byte = 0; byte < 8; ++byte)
                        key.bytes[word * 8 + byte] = static_cast<std::uint8_t>((*values)[index] >> ((7 - byte) * 8));
                }
                CHECK(DerivePCGSampleSeed(key, Id<SourceSampleId>((*samples)[index])).Value() == points.Seeds()[index]);
            }
        }
    }  // namespace

    TEST_CASE("PCG grid retains canonical version-one coordinates and adds semantic identity evidence", "[unit][pcg][generation]") {
        const auto plan = Cook(PCGCpuNodeKind::SnapshotGrid, Payload(PCGGridGenerationSettings{Id<SpatialElementId>(8)}));
        const auto candidate = Evaluate(plan, Spatial(), 8);
        const auto &points = Points(candidate);
        REQUIRE(points.PointCount() == 8);
        for (std::size_t index = 0; index < 8; ++index) {
            const auto &position = points.Transforms()[index].translation;
            CHECK(position.x == static_cast<float>(index % 2));
            CHECK(position.y == static_cast<float>((index / 2) % 2));
            CHECK(position.z == static_cast<float>(index / 4));
        }
        CheckIdentities(candidate);
    }

    TEST_CASE("PCG jitter is reproducible and zero jitter retains the exact lattice", "[unit][pcg][generation]") {
        const auto settings = PCGJitterGenerationSettings{Id<SpatialElementId>(8), 1.0};
        const auto plan = Cook(PCGCpuNodeKind::GridJitter, Payload(settings));
        const auto first = Evaluate(plan, Spatial(), 8);
        const auto second = Evaluate(plan, Spatial(), 8);
        const auto &points = Points(first);
        CHECK(std::ranges::equal(points.Seeds(), Points(second).Seeds()));
        for (std::size_t index = 0; index < 8; ++index) {
            const auto position = points.Transforms()[index].translation;
            CHECK(position == Points(second).Transforms()[index].translation);
            CHECK(std::abs(position.x - static_cast<float>(index % 2)) <= 0.5F);
            CHECK(std::abs(position.y - static_cast<float>((index / 2) % 2)) <= 0.5F);
            CHECK(std::abs(position.z - static_cast<float>(index / 4)) <= 0.5F);
        }
        const auto zero = Cook(PCGCpuNodeKind::GridJitter, Payload(PCGJitterGenerationSettings{Id<SpatialElementId>(8), 0.0}));
        const auto lattice = Evaluate(zero, Spatial(), 8);
        CHECK(Points(lattice).Transforms()[7].translation == Math::Vec3{1.0F, 1.0F, 1.0F});
        CHECK(Points(lattice).Transforms()[0].translation == Math::Vec3{});
        CheckIdentities(first);
        CheckIdentities(lattice);
    }

    TEST_CASE("PCG stratified samples retain cell identities through profile and settings variation", "[unit][pcg][generation]") {
        const auto settings = PCGStratifiedGenerationSettings{Id<SpatialElementId>(9), {2, 2, 2}};
        const auto plan = Cook(PCGCpuNodeKind::Stratified, Payload(settings));
        const auto first = Evaluate(plan, Spatial(), 8);
        const auto second = Evaluate(plan, Spatial(), 8);
        for (std::size_t index = 0; index < 8; ++index) {
            const auto position = Points(first).Transforms()[index].translation;
            CHECK(position == Points(second).Transforms()[index].translation);
            const std::array<float, 3> coordinates{position.x, position.y, position.z};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto cell = static_cast<float>((index >> axis) & 1);
                CHECK(coordinates[axis] >= cell - 1.0F);
                CHECK(coordinates[axis] <= cell);
            }
        }
        const auto profile = Evaluate(plan, Spatial(), 8, 2);
        CHECK(!std::ranges::equal(Points(first).Seeds(), Points(profile).Seeds()));
        const auto changed = Cook(PCGCpuNodeKind::Stratified, Payload(PCGStratifiedGenerationSettings{Id<SpatialElementId>(9), {1, 2, 4}}));
        const auto changedCandidate = Evaluate(changed, Spatial(), 8);
        CHECK(!std::ranges::equal(Points(first).Seeds(), Points(changedCandidate).Seeds()));
        CHECK(*Points(first).FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute) ==
              *Points(profile).FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute));
        CHECK(*Points(first).FindColumn<PCGUnsignedIntegerColumn>(PCGSourceRootAttributes[0]) !=
              *Points(profile).FindColumn<PCGUnsignedIntegerColumn>(PCGSourceRootAttributes[0]));
        CheckIdentities(first);
        CheckIdentities(profile);
    }

    TEST_CASE("PCG Poisson scatter returns exact count with bounded stored-coordinate separation", "[unit][pcg][generation]") {
        const auto plan = Cook(PCGCpuNodeKind::PoissonScatter, Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 8, 256, 0.5}));
        const auto first = Evaluate(plan, Spatial(), 8);
        const auto second = Evaluate(plan, Spatial(), 8);
        const auto &points = Points(first);
        REQUIRE(points.PointCount() == 8);
        CHECK(std::ranges::equal(points.Seeds(), Points(second).Seeds()));
        for (std::size_t index = 0; index < 8; ++index) {
            const auto position = points.Transforms()[index].translation;
            CHECK(position == Points(second).Transforms()[index].translation);
            CHECK(position.x >= -1.0F);
            CHECK(position.x <= 1.0F);
            CHECK(position.y >= -1.0F);
            CHECK(position.y <= 1.0F);
            CHECK(position.z >= -1.0F);
            CHECK(position.z <= 1.0F);
            for (std::size_t previous = 0; previous < index; ++previous) {
                const auto other = points.Transforms()[previous].translation;
                const double x = static_cast<double>(position.x) - other.x;
                const double y = static_cast<double>(position.y) - other.y;
                const double z = static_cast<double>(position.z) - other.z;
                CHECK(x * x + y * y + z * z >= 0.25);
            }
        }
        CheckIdentities(first);
    }

    TEST_CASE("PCG scatter failure discards partial output and preserves previous candidate across replacement",
              "[unit][pcg][generation]") {
        const auto spatial = Spatial();
        const auto good = Cook(PCGCpuNodeKind::PoissonScatter, Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 2, 64, 0.25}));
        const auto prior = Evaluate(good, spatial, 2);
        const auto before = Points(prior).Transforms()[0].translation;
        const std::array bounds{Bound(2)};
        // Separation exceeds the box diagonal, but the conservative cell bound allows it: attempts must terminate explicitly.
        const auto exhausted =
            Cook(PCGCpuNodeKind::PoissonScatter, Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 2, 2, 3.5}));
        auto limits = Limits(exhausted);
        limits.retainedBytes = prior.ReservedBytes();
        CheckError(EvaluatePCGCpu(exhausted, Spatial(2), bounds, {}, limits), PCGErrors::GenerationAttemptsExhausted);
        CHECK(Points(prior).Transforms()[0].translation == before);
        CHECK(prior.Snapshot() == spatial.Id());
        const auto impossible =
            Cook(PCGCpuNodeKind::PoissonScatter, Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 2, 64, 10.0}));
        CheckError(EvaluatePCGCpu(impossible, spatial, bounds, {}, Limits(impossible)), PCGErrors::GenerationDensityImpossible);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CheckError(EvaluatePCGCpu(good, spatial, bounds, {}, Limits(good), PCGCpuAdmission::Accepting, cancellation.Token()),
                   PCGErrors::CpuEvaluationClosed);
        CheckError(EvaluatePCGCpu(good, spatial, bounds, {}, Limits(good), PCGCpuAdmission::ShuttingDown), PCGErrors::CpuEvaluationClosed);
        const auto replacement = Evaluate(good, Spatial(2), 2);
        CHECK(replacement.Snapshot() != prior.Snapshot());
        CHECK(!std::ranges::equal(Points(prior).Seeds(), Points(replacement).Seeds()));
        CHECK(Points(prior).Transforms()[0].translation == before);
    }

    TEST_CASE("PCG generated composite identity survives filter and forward and distinguishes merged roots", "[unit][pcg][generation]") {
        const auto evaluate = [](const PCGCookedPlan &plan) {
            std::vector<PCGPointOutputBound> bounds;
            for (std::uint32_t index = 0; index < plan.Nodes().size(); ++index) {
                const auto &node = plan.Nodes()[index];
                auto bound = Bound(node.type == PCGCpuNodeType(PCGCpuNodeKind::Merge).Value() ? 16 : 8);
                bound.node = index;
                bound.pin = std::ranges::find(node.pins, PCGPinDirection::Output, &PCGCookedPin::direction)->id;
                bounds.push_back(std::move(bound));
            }
            return EvaluatePCGCpu(plan, Spatial(), bounds, {}, Limits(plan));
        };
        const auto distinct = evaluate(RoutedPlan(false));
        REQUIRE(distinct.HasValue());
        const auto &points = Points(distinct.Value());
        REQUIRE(points.PointCount() == 16);
        const auto samples = points.FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute);
        for (std::size_t index = 0; index < 16; ++index)
            CHECK((*samples)[index] == index % 8 + 1);
        CHECK(points.Seeds()[0] != points.Seeds()[8]);
        bool distinctRoots{};
        for (const auto name : PCGSourceRootAttributes) {
            const auto roots = points.FindColumn<PCGUnsignedIntegerColumn>(name);
            distinctRoots |= (*roots)[0] != (*roots)[8];
        }
        CHECK(distinctRoots);
        CheckError(evaluate(RoutedPlan(true)), PCGErrors::GenerationIdentityInvalid);
        // The empty filter prefix is forwarded as empty; merging the original root no longer duplicates a record.
        const auto filtered = evaluate(RoutedPlan(true, 2.0));
        REQUIRE(filtered.HasValue());
        CHECK(Points(filtered.Value()).PointCount() == 8);
        CHECK(std::ranges::equal(*Points(filtered.Value()).FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute),
                                 std::array<std::uint64_t, 8>{1, 2, 3, 4, 5, 6, 7, 8}));
    }

    TEST_CASE("PCG Poisson admission charges all attempts comparisons and transient scratch before writes", "[unit][pcg][generation]") {
        const auto plan =
            Cook(PCGCpuNodeKind::PoissonScatter, Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 8, 256, 0.25}));
        const std::array bounds{Bound(8)};
        const auto cost = plan.Catalog().Cost(plan.Nodes()[0], 8, 2);
        REQUIRE(cost.HasValue());
        CHECK(cost.Value().pointVisits == 256 * 8 + 256 + 8 + 1);
        CHECK(cost.Value().scratchBytes == 8 * sizeof(Math::Vec3));
        auto limits = Limits(plan);
        limits.maximumPointVisits = cost.Value().pointVisits - 1;
        CheckError(EvaluatePCGCpu(plan, Spatial(), bounds, {}, limits), PCGErrors::CpuEvaluationCapacityExceeded);
        limits.maximumPointVisits = cost.Value().pointVisits;
        limits.maximumSnapshotElementVisits = cost.Value().snapshotElementVisits - 1;
        CheckError(EvaluatePCGCpu(plan, Spatial(), bounds, {}, limits), PCGErrors::CpuEvaluationCapacityExceeded);
        limits.maximumSnapshotElementVisits = cost.Value().snapshotElementVisits;
        REQUIRE(EvaluatePCGCpu(plan, Spatial(), bounds, {}, limits).HasValue());
        limits.maximumScratchBytes = cost.Value().scratchBytes;
        CheckError(EvaluatePCGCpu(plan, Spatial(), bounds, {}, limits), PCGErrors::CpuEvaluationCapacityExceeded);
    }

    TEST_CASE("PCG generation rejects huge bounded attempts and lower output ceilings without entering sampling",
              "[unit][pcg][generation]") {
        const auto huge =
            Cook(PCGCpuNodeKind::PoissonScatter,
                 Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 8, std::numeric_limits<std::uint32_t>::max(), 0.25}));
        const std::array bounds{Bound(8)};
        CheckError(EvaluatePCGCpu(huge, Spatial(), bounds, {}, Limits(huge)), PCGErrors::CpuEvaluationCapacityExceeded);
        const auto normal = Cook(PCGCpuNodeKind::Stratified, Payload(PCGStratifiedGenerationSettings{Id<SpatialElementId>(9), {2, 2, 2}}));
        const std::array lowered{Bound(7)};
        CheckError(EvaluatePCGCpu(normal, Spatial(), lowered, {}, Limits(normal)), PCGErrors::CpuEvaluationCapacityExceeded);
    }

    TEST_CASE("PCG generation streams and semantic IDs do not depend on admitted worker count", "[unit][pcg][generation]") {
        const std::array kinds{PCGCpuNodeKind::SnapshotGrid, PCGCpuNodeKind::GridJitter, PCGCpuNodeKind::Stratified,
                               PCGCpuNodeKind::PoissonScatter};
        const std::array payloads{Payload(PCGGridGenerationSettings{Id<SpatialElementId>(8)}),
                                  Payload(PCGJitterGenerationSettings{Id<SpatialElementId>(8), 1.0}),
                                  Payload(PCGStratifiedGenerationSettings{Id<SpatialElementId>(9), {2, 2, 2}}),
                                  Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 8, 128, 0.25})};
        JobSystem jobs{JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 8}};
        for (std::size_t kind = 0; kind < kinds.size(); ++kind) {
            const auto plan = Cook(kinds[kind], payloads[kind]);
            const auto spatial = Spatial();
            const auto serial = Evaluate(plan, spatial, 8);
            const auto multiple = Evaluate(plan, spatial, 8, 1, 2, &jobs);
            CHECK(std::ranges::equal(Points(serial).Transforms(), Points(multiple).Transforms()));
            CHECK(std::ranges::equal(Points(serial).Seeds(), Points(multiple).Seeds()));
            for (const auto key : PCGSourceRootAttributes)
                CHECK(*Points(serial).FindColumn<PCGUnsignedIntegerColumn>(key) ==
                      *Points(multiple).FindColumn<PCGUnsignedIntegerColumn>(key));
            CHECK(*Points(serial).FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute) ==
                  *Points(multiple).FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute));
            CheckIdentities(multiple);
        }
    }

    TEST_CASE("PCG retained composite identity permits distinct roots and rejects duplicate or mismatched evidence",
              "[unit][pcg][generation]") {
        const std::array distinct{std::array<std::uint64_t, 5>{1, 2, 3, 4, 7}, std::array<std::uint64_t, 5>{5, 2, 3, 4, 7}};
        CHECK(RetainedIdentities(distinct).HasValue());
        const std::array repeated{distinct[0], distinct[1], distinct[0]};
        CheckError(RetainedIdentities(repeated), PCGErrors::GenerationIdentityInvalid);
        CheckError(RetainedIdentities(distinct, true), PCGErrors::GenerationIdentityInvalid);
        const std::array<std::array<std::uint64_t, 5>, 1> missingSample{{{1, 2, 3, 4, 0}}};
        CheckError(RetainedIdentities(missingSample), PCGErrors::GenerationIdentityInvalid);
        // Digest bytes are not reserved stable-ID words: even a zero SHA key must retain the existing encoder semantics.
        const std::array<std::array<std::uint64_t, 5>, 1> zeroDigest{{{0, 0, 0, 0, 1}}};
        CHECK(RetainedIdentities(zeroDigest).HasValue());
        const auto charge = detail::ChargePointIdentity(3);
        REQUIRE(charge.HasValue());
        CHECK(charge.Value().scratchBytes == 3 * 2 * sizeof(std::array<std::uint64_t, 5>));
        CHECK(charge.Value().pointVisits >= 3 * 2 * 5);
        CheckError(detail::ChargePointIdentity(std::numeric_limits<std::size_t>::max()), PCGErrors::PointSizeOverflow);
    }

    TEST_CASE("PCG generation preserves finite extreme grid cancellation and explicit empty scatter", "[unit][pcg][generation]") {
        const auto grid = Cook(PCGCpuNodeKind::SnapshotGrid, Payload(PCGGridGenerationSettings{Id<SpatialElementId>(8)}));
        const float maximum = std::numeric_limits<float>::max();
        PCGSpatialSnapshotCandidate source;
        source.snapshot = Id<SpatialSnapshotId>(1);
        source.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(1)};
        source.bounds = {{-maximum, -1.0F, -1.0F}, {maximum, 1.0F, 1.0F}};
        source.coverage = PCGSpatialCoverage::Complete;
        source.grids = {{Id<SpatialElementId>(8), {-maximum, 0.0F, 0.0F}, {maximum, 1.0F, 1.0F}, {3, 1, 1}}};
        const auto spatial = CapturePCGSpatialSnapshot(std::move(source));
        REQUIRE(spatial.HasValue());
        const auto finite = Evaluate(grid, spatial.Value(), 3);
        CHECK(Points(finite).Transforms()[0].translation.x == -maximum);
        CHECK(Points(finite).Transforms()[1].translation.x == 0.0F);
        CHECK(Points(finite).Transforms()[2].translation.x == maximum);
        CheckIdentities(finite);
        const auto empty = Cook(PCGCpuNodeKind::PoissonScatter,
                                Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 0, 0, std::numeric_limits<double>::max()}));
        const auto candidate = Evaluate(empty, Spatial(), 0);
        CHECK(Points(candidate).PointCount() == 0);
        CheckIdentities(candidate);
        const auto one = Cook(PCGCpuNodeKind::PoissonScatter,
                              Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 1, 1, std::numeric_limits<double>::max()}));
        const auto single = Evaluate(one, Spatial(), 1);
        CHECK(Points(single).PointCount() == 1);
        CheckIdentities(single);
    }

    TEST_CASE("PCG rejected attempts remain visible in stable semantic sample IDs", "[unit][pcg][generation]") {
        bool exercisedRejection{};
        const auto spatial = Spatial();
        const std::array bounds{Bound(8)};
        for (std::uint64_t seed = 1; seed <= 16 && !exercisedRejection; ++seed) {
            const auto plan =
                Cook(PCGCpuNodeKind::PoissonScatter, Payload(PCGPoissonGenerationSettings{Id<SpatialElementId>(9), 8, 512, 1.0}), seed);
            const auto candidate = EvaluatePCGCpu(plan, spatial, bounds, {}, Limits(plan));
            if (candidate.HasError()) {
                // A finite rejection sampler may jam; exhausted cases remain failures, never partial success.
                CHECK(candidate.ErrorValue().code.Value() == PCGErrors::GenerationAttemptsExhausted.code.Value());
                continue;
            }
            const auto *samples = Points(candidate.Value()).FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute);
            REQUIRE(samples != nullptr);
            REQUIRE(samples->size() == 8);
            exercisedRejection = samples->back() > samples->size();
            CheckIdentities(candidate.Value());
            if (exercisedRejection) {
                const auto repeated = Evaluate(plan, spatial, 8);
                CHECK(*samples == *Points(repeated).FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute));
                CHECK(std::ranges::equal(Points(candidate.Value()).Seeds(), Points(repeated).Seeds()));
            }
        }
        REQUIRE(exercisedRejection);
    }
}  // namespace Horo::PCG
