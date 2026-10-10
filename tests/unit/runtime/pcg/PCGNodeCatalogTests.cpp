#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>

namespace Horo::PCG {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value) {
            const auto id = Identity::Create(value);
            REQUIRE(id.HasValue());
            return id.Value();
        }

        template <typename T> void CheckError(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        PCGCapabilitySet Grants(const bool evaluation = true) {
            const std::array values{PCGCapability::Validation, PCGCapability::OfflineBake};
            return PCGCapabilitySet::Create(std::span(values).first(evaluation ? 2 : 1)).Value();
        }

        PCGNodeCatalog Catalog(const std::size_t capacity = 4, const bool evaluation = true) {
            const auto projection = ProjectPCGCapabilities(PCGHostProfile::Headless, Grants(evaluation));
            REQUIRE(projection.HasValue());
            auto catalog = PCGNodeCatalog::Create(projection.Value(), capacity);
            REQUIRE(catalog.HasValue());
            return std::move(catalog).Value();
        }

        Result<PCGCookedPlan> Cook(const PCGNodeCatalogSnapshot &catalog, const PCGNodeTypeVersion version = {1, 0},
                                   const PCGGenerationMode mode = PCGGenerationMode::Offline, const bool badPins = false,
                                   const bool badSettings = false, const bool retainCatalog = true) {
            const auto type = PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value();
            PCGGraphSourceData source;
            source.generation = {Id<GraphId>(70), Id<GraphRevision>(1)};
            source.mode = mode;
            source.tier = mode == PCGGenerationMode::Offline ? PCGOperationalTier::Baseline : PCGOperationalTier::Standard;
            source.nodes = {{Id<NodeId>(14),
                             type,
                             version,
                             {{Id<PinId>(15), PCGPinDirection::Output, PCGPinType::PointSet, PCGPinCardinality::Multiple, std::nullopt}},
                             {0, 0, 0, 0, 0, 0, 0, 8}}};
            if (badPins)
                source.nodes[0].pins[0].type = PCGPinType::Scalar;
            if (badSettings)
                source.nodes[0].payload.clear();
            // Source capture deliberately permits future versions, so executable admission owns the exact-version diagnosis.
            const std::array supported{PCGNodeTypeSupport{type, {1, 0}, {9, 9}}};
            const auto graph = PCGGraphAsset::Create(source, {.tier = source.tier, .supportedNodeTypes = supported});
            REQUIRE(graph.HasValue());
            auto created = PCGRegistry::Create(Id<PCGRegistryInstanceId>(3), catalog.Capabilities());
            REQUIRE(created.HasValue());
            auto registry = std::move(created).Value();
            // Runtime availability alone must not bypass a withdrawn catalog entry or its settings/version checks.
            REQUIRE(registry.RegisterNodeRuntime({type, 1, PCGNodeDeterminism::ProfileDeterministic, Grants(false)}).HasValue());
            REQUIRE(registry.RegisterGraph({source.generation, {{Id<NodeId>(14), type}}, Grants(false)}).HasValue());
            const auto registryRoot = registry.Snapshot();
            REQUIRE(registryRoot.HasValue());
            const auto validated = ValidatePCGGraph(graph.Value(), registryRoot.Value(), Grants(false));
            REQUIRE(validated.HasValue());
            return CompilePCGGraph(graph.Value(), validated.Value(), registryRoot.Value(),
                                   retainCatalog ? catalog : PCGNodeCatalogSnapshot{}, Grants(false),
                                   PCGGraphSourceHardLimits::SourceBytes);
        }

        Result<PCGCpuCandidate> Evaluate(const PCGCookedPlan &plan, const std::size_t visits = 3, const std::size_t snapshotVisits = 1,
                                         const bool secondGrid = false) {
            PCGSpatialSnapshotCandidate source;
            source.snapshot = Id<SpatialSnapshotId>(1);
            source.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(1)};
            source.bounds = {{-1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, 1.0F}};
            source.coverage = PCGSpatialCoverage::Complete;
            source.grids = {{Id<SpatialElementId>(8), {}, {1.0F, 1.0F, 1.0F}, {1, 1, 1}}};
            if (secondGrid)
                source.grids.push_back({Id<SpatialElementId>(7), {}, {1.0F, 1.0F, 1.0F}, {1, 1, 1}});
            const auto spatial = CapturePCGSpatialSnapshot(std::move(source));
            REQUIRE(spatial.HasValue());
            auto schema = PCGPointSchema::Capture({PCGOperationalTier::Baseline, {}});
            REQUIRE(schema.HasValue());
            const std::array bounds{
                PCGPointOutputBound{0, Id<PinId>(15), std::make_shared<const PCGPointSchema>(std::move(schema).Value()), 1}};
            const auto tier = LimitsForTier(plan.Tier()).Value();
            PCGCpuEvaluationLimits limits{tier.maximumScratchBytes, tier.maximumCandidateBytes};
            limits.grantedCapabilities = Grants();
            limits.world = Id<PCGWorldId>(1);
            limits.numericProfile.bytes[0] = 1;
            limits.providerContent.bytes[0] = 1;
            limits.maximumPointVisits = visits;
            limits.maximumSnapshotElementVisits = snapshotVisits;
            return EvaluatePCGCpu(plan, spatial.Value(), bounds, {}, limits);
        }
    }  // namespace

    TEST_CASE("PCG built-in catalog composition is explicit bounded and typed", "[unit][pcg][catalog]") {
        auto catalog = Catalog();
        const auto empty = catalog.Snapshot();
        REQUIRE(empty.HasValue());
        CHECK(empty.Value().Nodes().empty());
        for (const auto kind :
             {PCGCpuNodeKind::Forward, PCGCpuNodeKind::Merge, PCGCpuNodeKind::SnapshotGrid, PCGCpuNodeKind::DensityFilter})
            REQUIRE(catalog.Register(kind).HasValue());
        const auto root = catalog.Snapshot();
        REQUIRE(root.HasValue());
        REQUIRE(root.Value().Nodes().size() == 4);
        for (const auto &entry : root.Value().Nodes()) {
            CHECK(entry.version == PCGNodeTypeVersion{1, 0});
            CHECK(entry.runtimeContractVersion == 1);
            CHECK(entry.migrationVersion == 1);
            CHECK(entry.requiredCapabilities == Grants(false));
            CHECK(entry.pinCount >= 1);
            CHECK(entry.pinCount <= 3);
            CHECK(entry.workPerPoint > 0);
        }
        CHECK(root.Value().Nodes().front().settings == PCGNodeSettingsSchema::SpatialGridIdentity);
        CHECK(root.Value().ResidentBytes() > sizeof(PCGBuiltInNodeDescriptor) * 4);
        CHECK(empty.Value().Nodes().empty());
        CheckError(catalog.Register(PCGCpuNodeKind::Forward), PCGErrors::RegistryDuplicate);
        CheckError(catalog.Register(static_cast<PCGCpuNodeKind>(99)), PCGErrors::CpuEvaluationUnsupported);
        CheckError(PCGNodeCatalog::Create(root.Value().Capabilities(), 0), PCGErrors::RegistryCapacityExceeded);
        CheckError(PCGNodeCatalog::Create(root.Value().Capabilities(), 5), PCGErrors::RegistryCapacityExceeded);
        auto small = Catalog(1);
        REQUIRE(small.Register(PCGCpuNodeKind::Forward).HasValue());
        CheckError(small.Register(PCGCpuNodeKind::Merge), PCGErrors::RegistryCapacityExceeded);
        CheckError(small.Register(PCGCpuNodeKind::Merge, true), PCGErrors::RuntimeUnavailable);
    }

    TEST_CASE("PCG catalog replacement withdrawal and shutdown preserve exact historical execution", "[unit][pcg][catalog]") {
        auto catalog = Catalog();
        REQUIRE(catalog.Register(PCGCpuNodeKind::SnapshotGrid).HasValue());
        const auto root = catalog.Snapshot().Value();
        const auto cooked = Cook(root);
        REQUIRE(cooked.HasValue());
        REQUIRE(catalog.Register(PCGCpuNodeKind::SnapshotGrid, true).HasValue());
        CHECK(catalog.Snapshot().Value().Generation() > root.Generation());
        REQUIRE(catalog.Remove(PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value()).HasValue());
        CheckError(Cook(catalog.Snapshot().Value()), PCGErrors::RuntimeUnavailable);
        REQUIRE(catalog.Close().HasValue());
        REQUIRE(catalog.Close().HasValue());
        CheckError(catalog.Snapshot(), PCGErrors::RegistryClosed);
        CheckError(catalog.Register(PCGCpuNodeKind::SnapshotGrid), PCGErrors::RegistryClosed);
        const auto candidate = Evaluate(cooked.Value());
        REQUIRE(candidate.HasValue());
        CHECK(candidate.Value().Catalog().Generation() == root.Generation());
        REQUIRE(candidate.Value().Outputs().size() == 1);
        CHECK(candidate.Value().Outputs()[0].points->PointCount() == 1);
        CHECK(candidate.Value().Catalog().Find(PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value()).HasValue());
    }

    TEST_CASE("PCG catalog rejects incompatible versions and product modes before evaluation", "[unit][pcg][catalog]") {
        auto catalog = Catalog();
        REQUIRE(catalog.Register(PCGCpuNodeKind::SnapshotGrid).HasValue());
        const auto root = catalog.Snapshot().Value();
        const auto incompatible = Cook(root, {2, 0});
        CheckError(incompatible, PCGErrors::GraphNodeVersionUnsupported);
        REQUIRE(incompatible.ErrorValue().diagnostics.size() == 1);
        CHECK(incompatible.ErrorValue().diagnostics[0].location.source == "pcg://node/14");
        CheckError(Cook(root, {1, 0}, PCGGenerationMode::Runtime), PCGErrors::UnsupportedCapability);
        auto validationOnly = Catalog(4, false);
        REQUIRE(validationOnly.Register(PCGCpuNodeKind::SnapshotGrid).HasValue());
        CheckError(Cook(validationOnly.Snapshot().Value()), PCGErrors::UnsupportedCapability);
        CheckError(Cook(root, {1, 0}, PCGGenerationMode::Offline, true), PCGErrors::RegistryDescriptorInvalid);
        CheckError(Cook(root, {1, 0}, PCGGenerationMode::Offline, false, true), PCGErrors::RegistryDescriptorInvalid);
        const auto metadata = Cook(root, {1, 0}, PCGGenerationMode::Offline, false, false, false);
        CheckError(metadata, PCGErrors::RuntimeUnavailable);
        const auto plan = Cook(root);
        REQUIRE(plan.HasValue());
        CHECK(plan.Value().RequiredCapabilities().Contains(PCGCapability::OfflineBake));
        auto invalid = plan.Value().Nodes()[0];
        invalid.payload[7] = 0;
        CheckError(root.Validate(invalid), PCGErrors::RegistryDescriptorInvalid);
        invalid = plan.Value().Nodes()[0];
        invalid.pins[0].cardinality = PCGPinCardinality::Single;
        CheckError(root.Validate(invalid), PCGErrors::RegistryDescriptorInvalid);
    }

    TEST_CASE("PCG catalog work cost is checked before executable allocation admission", "[unit][pcg][catalog]") {
        auto catalog = Catalog();
        REQUIRE(catalog.Register(PCGCpuNodeKind::SnapshotGrid).HasValue());
        REQUIRE(catalog.Register(PCGCpuNodeKind::DensityFilter).HasValue());
        const auto root = catalog.Snapshot().Value();
        const auto gridType = PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value();
        CHECK(root.Cost(gridType, 0, 1).Value().pointVisits == 1);
        CHECK(root.Cost(gridType, 1, 1).Value().pointVisits == 2);
        CHECK(root.Cost(PCGCpuNodeType(PCGCpuNodeKind::DensityFilter).Value(), std::numeric_limits<std::size_t>::max(), 0).HasError());
        const auto plan = Cook(root);
        REQUIRE(plan.HasValue());
        REQUIRE(Evaluate(plan.Value(), 2).HasValue());
        CHECK(root.Cost(gridType, 0, 2).Value().snapshotElementVisits == 2);
        CheckError(Evaluate(plan.Value(), 2, 1, true), PCGErrors::CpuEvaluationCapacityExceeded);
        REQUIRE(Evaluate(plan.Value(), 2, 2, true).HasValue());
        CheckError(Evaluate(plan.Value(), 2, 0), PCGErrors::CpuEvaluationInvalid);
        CheckError(Evaluate(plan.Value(), 1), PCGErrors::CpuEvaluationCapacityExceeded);
        CheckError(Evaluate(plan.Value(), 0), PCGErrors::CpuEvaluationInvalid);
    }

    TEST_CASE("PCG retained catalog storage is charged once in the complete candidate envelope", "[unit][pcg][catalog]") {
        auto catalog = Catalog();
        REQUIRE(catalog.Register(PCGCpuNodeKind::SnapshotGrid).HasValue());
        const auto firstRoot = catalog.Snapshot().Value();
        const auto firstPlan = Cook(firstRoot);
        REQUIRE(firstPlan.HasValue());
        const auto first = Evaluate(firstPlan.Value());
        REQUIRE(first.HasValue());
        REQUIRE(catalog.Register(PCGCpuNodeKind::DensityFilter).HasValue());
        const auto secondRoot = catalog.Snapshot().Value();
        const auto secondPlan = Cook(secondRoot);
        REQUIRE(secondPlan.HasValue());
        const auto second = Evaluate(secondPlan.Value());
        REQUIRE(second.HasValue());
        REQUIRE(secondRoot.ResidentBytes() > firstRoot.ResidentBytes());
        CHECK(second.Value().ReservedBytes() - first.Value().ReservedBytes() == secondRoot.ResidentBytes() - firstRoot.ResidentBytes());
        CHECK(first.Value().Catalog().Nodes().size() == 1);
        CHECK(second.Value().Catalog().Nodes().size() == 2);
    }

    TEST_CASE("PCG mutable catalog authority stays on its composition thread", "[unit][pcg][catalog]") {
        auto catalog = Catalog();
        REQUIRE(catalog.Register(PCGCpuNodeKind::Forward).HasValue());
        const auto root = catalog.Snapshot().Value();
        bool mutationRejected{};
        bool rootReadable{};
        std::jthread reader([&] {
            mutationRejected = catalog.Register(PCGCpuNodeKind::Merge).HasError();
            rootReadable = root.Find(PCGCpuNodeType(PCGCpuNodeKind::Forward).Value()).HasValue();
        });
        reader.join();
        CHECK(mutationRejected);
        CHECK(rootReadable);
        REQUIRE(catalog.Register(PCGCpuNodeKind::Merge).HasValue());
        CHECK(root.Nodes().size() == 1);
    }
}  // namespace Horo::PCG
