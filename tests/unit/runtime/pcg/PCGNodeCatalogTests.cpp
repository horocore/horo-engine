#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <limits>
#include <thread>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace Horo::PCG {
    namespace {
#if defined(_MSC_VER) && defined(_DEBUG)
        /** @brief Routes this owner-thread case's CRT assertion text to CTest without changing fail-fast behavior. */
        class ScopedAssertionReport final {
        public:
            ScopedAssertionReport()
                : previousMode_(_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE)),
                  previousFile_(_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR)) {}

            ScopedAssertionReport(const ScopedAssertionReport &) = delete;
            ScopedAssertionReport &operator=(const ScopedAssertionReport &) = delete;

            ~ScopedAssertionReport() {
                _CrtSetReportFile(_CRT_ASSERT, previousFile_);
                _CrtSetReportMode(_CRT_ASSERT, previousMode_);
            }

        private:
            int previousMode_;
            _HFILE previousFile_;
        };
#endif

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

        Result<PCGCpuCandidate> Evaluate(const PCGCookedPlan &plan, const std::size_t visits = 3, const std::size_t snapshotVisits = 2,
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
        using enum PCGCpuNodeKind;
        auto catalog = Catalog();
        const auto empty = catalog.Snapshot();
        REQUIRE(empty.HasValue());
        CHECK(empty.Value().Nodes().empty());
        for (const auto kind : {Forward, Merge, SnapshotGrid, DensityFilter})
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
        CheckError(catalog.Register(Forward), PCGErrors::RegistryDuplicate);
        CheckError(catalog.Register(static_cast<PCGCpuNodeKind>(99)), PCGErrors::CpuEvaluationUnsupported);
        CheckError(PCGNodeCatalog::Create(root.Value().Capabilities(), 0), PCGErrors::RegistryCapacityExceeded);
        CheckError(PCGNodeCatalog::Create(root.Value().Capabilities(), 13), PCGErrors::RegistryCapacityExceeded);
        auto small = Catalog(1);
        REQUIRE(small.Register(Forward).HasValue());
        CheckError(small.Register(Merge), PCGErrors::RegistryCapacityExceeded);
        CheckError(small.Register(Merge, true), PCGErrors::RuntimeUnavailable);
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

    TEST_CASE("PCG generation catalog explicitly registers seven stable builtins with exact new schemas", "[unit][pcg][catalog]") {
        using enum PCGCpuNodeKind;
        auto created = PCGNodeCatalog::Create(ProjectPCGCapabilities(PCGHostProfile::Headless, Grants()).Value());
        REQUIRE(created.HasValue());
        auto owner = std::move(created).Value();
        for (std::uint8_t index = 0; index < 7; ++index)
            REQUIRE(owner.Register(static_cast<PCGCpuNodeKind>(index)).HasValue());
        const auto root = owner.Snapshot().Value();
        REQUIRE(root.Nodes().size() == 7);
        for (std::size_t index = 0; index < root.Nodes().size(); ++index) {
            const auto &entry = root.Nodes()[index];
            CHECK(entry.type.Value() == 0x5043470206000001ULL + index);
            CHECK(static_cast<std::size_t>(entry.kind) == index);
            if (index >= 4) {
                CHECK(entry.determinism == PCGNodeDeterminism::ProfileDeterministic);
                CHECK(entry.pinCount == 1);
                CHECK(entry.pins[0].direction == PCGPinDirection::Output);
                CHECK(entry.settings != PCGNodeSettingsSchema::None);
            }
        }
        CHECK(PCGCpuNodeType(static_cast<PCGCpuNodeKind>(7)).HasError());
        auto lowered = Catalog(4);
        for (const auto kind : {SnapshotGrid, GridJitter, Stratified, PoissonScatter})
            REQUIRE(lowered.Register(kind).HasValue());
        CheckError(lowered.Register(Forward), PCGErrors::RegistryCapacityExceeded);
    }

    TEST_CASE("PCG catalog work cost is checked before executable allocation admission", "[unit][pcg][catalog]") {
#if defined(_MSC_VER) && defined(_DEBUG)
        const ScopedAssertionReport assertionReport;
#endif
        auto catalog = Catalog();
        REQUIRE(catalog.Register(PCGCpuNodeKind::SnapshotGrid).HasValue());
        REQUIRE(catalog.Register(PCGCpuNodeKind::DensityFilter).HasValue());
        const auto root = catalog.Snapshot().Value();
        const auto plan = Cook(root);
        REQUIRE(plan.HasValue());
        const auto &grid = plan.Value().Nodes()[0];
        CHECK(root.Cost(grid, 0, 1).Value().pointVisits == 1);
        CHECK(root.Cost(grid, 1, 1).Value().pointVisits == 2);
        CHECK(root.Cost(grid, std::numeric_limits<std::size_t>::max(), 0).HasError());
        auto malformed = grid;
        malformed.payload.clear();
        CheckError(root.Cost(malformed, 1, 1), PCGErrors::RegistryDescriptorInvalid);
#if defined(_MSC_VER) && defined(_DEBUG)
        std::fputs("PCG one-point evaluation: entering original successful evaluator path\n", stderr);
        std::fflush(stderr);
#endif
        const auto first = Evaluate(plan.Value(), 2);
#if defined(_MSC_VER) && defined(_DEBUG)
        std::fputs("PCG one-point evaluation: returned from evaluator\n", stderr);
        std::fflush(stderr);
#endif
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().Provenance().size() == 1);
        Sha256Digest expectedProfile;
        expectedProfile.bytes[0] = 1;
        CHECK(first.Value().NumericProfile() == expectedProfile);
        CHECK(first.Value().Provenance()[0].Data().profile == expectedProfile);
        CHECK(first.Value().Provenance()[0].Data().node == grid.id);
        CHECK(root.Cost(grid, 0, 2).Value().snapshotElementVisits == 4);
        CheckError(Evaluate(plan.Value(), 2, 3, true), PCGErrors::CpuEvaluationCapacityExceeded);
        REQUIRE(Evaluate(plan.Value(), 2, 4, true).HasValue());
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
        std::jthread reader([&catalog, &mutationRejected, &root, &rootReadable] {
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
