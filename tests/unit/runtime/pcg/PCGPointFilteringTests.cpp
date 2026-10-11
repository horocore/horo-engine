#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGPointFiltering.h"
#include "Horo/PCG/PCGPointGeneration.h"
#include "PCGCpuEvaluatorInternal.h"
#include "PCGPointFilteringInternal.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
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

        PCGNodeCatalog Owner(PCGCpuNodeKind kind) {
            auto created = PCGNodeCatalog::Create(ProjectPCGCapabilities(PCGHostProfile::Headless, Grant()).Value());
            REQUIRE(created.HasValue());
            auto owner = std::move(created).Value();
            REQUIRE(owner.Register(PCGCpuNodeKind::SnapshotGrid).HasValue());
            REQUIRE(owner.Register(kind).HasValue());
            return owner;
        }

        PCGGraphPin Pin(std::uint64_t id, PCGPinDirection direction) {
            return {Id<PinId>(id), direction, PCGPinType::PointSet,
                    direction == PCGPinDirection::Input ? PCGPinCardinality::Single : PCGPinCardinality::Multiple, std::nullopt};
        }

        PCGCookedPlan Plan(const PCGNodeCatalogSnapshot &catalog, PCGCpuNodeKind kind, const PCGPointFilterSettings &settings) {
            const auto encoded = EncodePCGPointFilterSettings(settings);
            REQUIRE(encoded.HasValue());
            PCGGraphSourceData source;
            source.generation = {Id<GraphId>(1), Id<GraphRevision>(1)};
            source.deterministicSeed = 41;
            source.nodes = {{Id<NodeId>(10),
                             PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value(),
                             {1, 0},
                             {Pin(101, PCGPinDirection::Output)},
                             EncodePCGPointGenerationSettings(PCGGridGenerationSettings{Id<SpatialElementId>(8)}).Value()},
                            {Id<NodeId>(20),
                             PCGCpuNodeType(kind).Value(),
                             {1, 0},
                             {Pin(201, PCGPinDirection::Input), Pin(202, PCGPinDirection::Output)},
                             encoded.Value()}};
            source.edges = {{Id<EdgeId>(1), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(20), Id<PinId>(201)}};
            return Compile(source, catalog);
        }

        PCGSpatialSnapshot Spatial(bool surfaces = true, std::uint64_t revision = 1, int terrain = 0) {
            PCGSpatialSnapshotCandidate source;
            source.snapshot = Id<SpatialSnapshotId>(revision);
            source.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(revision)};
            source.bounds = {{-10, -10, -10}, {10, 10, 10}};
            source.coverage = PCGSpatialCoverage::Complete;
            source.grids = {{Id<SpatialElementId>(8), {}, {1, 1, 1}, {4, 1, 1}}};
            if (surfaces)
                source.surfaces = {{Id<SpatialElementId>(9), {{{-5, 0, -5}, {0, 0, 5}, {5, 0, -5}}}, {0, 1, 0}}};
            if (terrain != 0) {
                const auto flatId = Id<SpatialElementId>(terrain == 1 ? 9 : 10);
                const auto slopeId = Id<SpatialElementId>(terrain == 1 ? 10 : 9);
                source.surfaces = {{flatId, {{{-5, 2, -5}, {0, 2, 5}, {5, 2, -5}}}, {0, 1, 0}},
                                   {slopeId, {{{-5, -5, -5}, {0, 0, 5}, {5, 5, -5}}}, {-0.70710677F, 0.70710677F, 0}}};
            }
            const auto captured = CapturePCGSpatialSnapshot(std::move(source));
            REQUIRE(captured.HasValue());
            return captured.Value();
        }

        std::shared_ptr<const PCGPointSchema> Schema(bool attributes = true) {
            std::vector<PCGAttributeDescriptor> fields;
            if (attributes) {
                constexpr std::array names{"test.boolean", "test.signed", "test.unsigned", "test.scalar",
                                           "test.vec2",    "test.vec3",   "test.vec4"};
                for (std::size_t i = 0; i < names.size(); ++i)
                    fields.push_back({PCGAttributeKey::Create(names[i]).Value(), static_cast<PCGAttributeType>(i)});
            }
            const auto schema = PCGPointSchema::Capture({PCGOperationalTier::Baseline, fields});
            REQUIRE(schema.HasValue());
            return std::make_shared<const PCGPointSchema>(schema.Value());
        }

        std::array<PCGPointOutputBound, 2> Bounds(std::shared_ptr<const PCGPointSchema> schema, std::size_t count = 4) {
            return {{{0, Id<PinId>(101), schema, count}, {1, Id<PinId>(202), schema, count}}};
        }

        PCGCpuEvaluationLimits Limits() {
            const auto tier = LimitsForTier(PCGOperationalTier::Baseline).Value();
            PCGCpuEvaluationLimits limits{tier.maximumScratchBytes, tier.maximumCandidateBytes};
            limits.grantedCapabilities = Grant();
            limits.world = Id<PCGWorldId>(1);
            limits.numericProfile.bytes[0] = 1;
            limits.providerContent.bytes[0] = 1;
            return limits;
        }

        struct Fixture final {
            PCGCookedPlan plan;
            PCGSpatialSnapshot spatial;
            std::array<PCGPointOutputBound, 2> bounds;
            std::unique_ptr<PCGPointCloudWorkspace> workspace;

            Fixture(PCGCpuNodeKind kind, PCGPointFilterSettings settings, std::size_t count = 4, float step = 1.0F)
                : plan([&] {
                      auto owner = Owner(kind);
                      return Plan(owner.Snapshot().Value(), kind, settings);
                  }()),
                  spatial(Spatial()), bounds(Bounds(Schema(), count)) {
                auto created = PCGPointCloudWorkspace::Create(plan, bounds, Limits().maximumScratchBytes);
                REQUIRE(created.HasValue());
                workspace = std::move(created).Value();
                const auto writer = workspace->BeginOutput(0, Id<PinId>(101), count);
                REQUIRE(writer.HasValue());
                for (std::size_t i = 0; i < count; ++i) {
                    Math::Transform transform;
                    transform.translation = {static_cast<float>(i) * step, 0, 0};
                    REQUIRE(writer.Value().SetPoint(i, transform, {{}, {1, 1, 1}}, static_cast<float>(i) / 3.0F, 100 + i).HasValue());
                    REQUIRE(writer.Value().SetColumnValue<PCGBoolColumn>("test.boolean", i, static_cast<std::uint8_t>(i % 2)).HasValue());
                    REQUIRE(
                        writer.Value()
                            .SetColumnValue<PCGSignedIntegerColumn>("test.signed", i,
                                                                    std::numeric_limits<std::int64_t>::min() + static_cast<std::int64_t>(i))
                            .HasValue());
                    REQUIRE(writer.Value()
                                .SetColumnValue<PCGUnsignedIntegerColumn>("test.unsigned", i, std::numeric_limits<std::uint64_t>::max() - i)
                                .HasValue());
                    REQUIRE(writer.Value().SetColumnValue<PCGScalarColumn>("test.scalar", i, static_cast<double>(i)).HasValue());
                    REQUIRE(writer.Value().SetColumnValue<PCGVector2Column>("test.vec2", i, {static_cast<float>(i), 2}).HasValue());
                    REQUIRE(writer.Value().SetColumnValue<PCGVector3Column>("test.vec3", i, {static_cast<float>(i), 2, 3}).HasValue());
                    REQUIRE(writer.Value().SetColumnValue<PCGVector4Column>("test.vec4", i, {static_cast<float>(i), 2, 3, 4}).HasValue());
                }
                REQUIRE(workspace->SealOutput(0, Id<PinId>(101)).HasValue());
                REQUIRE(workspace->FinishNode(0).HasValue());
            }

            Result<void> Execute(CancellationToken cancellation = {}) {
                const detail::NodeExecutionContext context{plan, spatial, *workspace, bounds, {}, 1, nullptr, cancellation, {}};
                return detail::ExecuteNode(context, 1);
            }

            PCGPointReadView Output() {
                const auto output = workspace->ReadFinal(1, Id<PinId>(202));
                REQUIRE(output.HasValue());
                return output.Value();
            }
        };

        void CheckRows(const PCGPointReadView &points, const std::vector<std::size_t> &rows) {
            REQUIRE(points.PointCount() == rows.size());
            const auto signedValues = points.FindColumn<PCGSignedIntegerColumn>("test.signed");
            const auto unsignedValues = points.FindColumn<PCGUnsignedIntegerColumn>("test.unsigned");
            const auto boolean = points.FindColumn<PCGBoolColumn>("test.boolean");
            const auto scalar = points.FindColumn<PCGScalarColumn>("test.scalar");
            const auto v2 = points.FindColumn<PCGVector2Column>("test.vec2");
            const auto v3 = points.FindColumn<PCGVector3Column>("test.vec3");
            const auto v4 = points.FindColumn<PCGVector4Column>("test.vec4");
            for (std::size_t i = 0; i < rows.size(); ++i) {
                CHECK(points.Seeds()[i] == 100 + rows[i]);
                CHECK(points.Transforms()[i].translation.x == static_cast<float>(rows[i]));
                CHECK(points.Bounds()[i].maximum.z == 1);
                CHECK(points.Densities()[i] == static_cast<float>(rows[i]) / 3.0F);
                CHECK(signedValues[i] == std::numeric_limits<std::int64_t>::min() + static_cast<std::int64_t>(rows[i]));
                CHECK(unsignedValues[i] == std::numeric_limits<std::uint64_t>::max() - rows[i]);
                CHECK(boolean[i] == rows[i] % 2);
                CHECK(scalar[i] == static_cast<double>(rows[i]));
                CHECK(v2[i].x == static_cast<float>(rows[i]));
                CHECK(v3[i].z == 3);
                CHECK(v4[i].w == 4);
            }
        }
    }  // namespace

    TEST_CASE("PCG density intervals and bounds compact complete rows in stable order", "[unit][pcg][filters]") {
        SECTION("inclusive density endpoints") {
            Fixture f(PCGCpuNodeKind::DensityRangeFilter, PCGDensityRangeFilterSettings{0, 1});
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {0, 1, 2, 3});
        }
        SECTION("density interior") {
            Fixture f(PCGCpuNodeKind::DensityRangeFilter, PCGDensityRangeFilterSettings{0.3, 0.7});
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {1, 2});
        }
        SECTION("inclusive position bounds") {
            Fixture f(PCGCpuNodeKind::BoundsFilter, PCGBoundsFilterSettings{{{1, 0, 0}, {2, 0, 0}}});
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {1, 2});
        }
        SECTION("empty result") {
            Fixture f(PCGCpuNodeKind::BoundsFilter, PCGBoundsFilterSettings{{{9, 0, 0}, {9, 0, 0}}});
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {});
        }
        SECTION("empty input") {
            Fixture f(PCGCpuNodeKind::DensityRangeFilter, PCGDensityRangeFilterSettings{}, 0);
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {});
        }
    }

    TEST_CASE("PCG separation greedily retains stable prior accepted points with exact boundary", "[unit][pcg][filters]") {
        SECTION("equality accepted") {
            Fixture f(PCGCpuNodeKind::DistanceFilter, PCGDistanceFilterSettings{1});
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {0, 1, 2, 3});
        }
        SECTION("rejected points do not constrain later ones") {
            Fixture f(PCGCpuNodeKind::DistanceFilter, PCGDistanceFilterSettings{1.1});
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {0, 2});
        }
        SECTION("zero threshold") {
            Fixture f(PCGCpuNodeKind::DistanceFilter, PCGDistanceFilterSettings{0});
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {0, 1, 2, 3});
        }
        SECTION("positive subnormal threshold rejects coincident positions") {
            Fixture f(PCGCpuNodeKind::DistanceFilter, PCGDistanceFilterSettings{std::numeric_limits<double>::denorm_min()}, 4, 0);
            REQUIRE(f.Execute().HasValue());
            CHECK(f.Output().PointCount() == 1);
        }
        SECTION("large finite threshold") {
            Fixture f(PCGCpuNodeKind::DistanceFilter, PCGDistanceFilterSettings{std::numeric_limits<double>::max()});
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {0});
        }
    }

    TEST_CASE("PCG exact attribute filters preserve all seven types without coercion", "[unit][pcg][filters]") {
        const std::array<PCGAttributeFilterSettings, 7> predicates{
            {{PCGAttributeKey::Create("test.boolean").Value(), PCGAttributeComparison::Equal, true},
             {PCGAttributeKey::Create("test.signed").Value(), PCGAttributeComparison::Equal, std::numeric_limits<std::int64_t>::min() + 1},
             {PCGAttributeKey::Create("test.unsigned").Value(), PCGAttributeComparison::Equal,
              std::numeric_limits<std::uint64_t>::max() - 1},
             {PCGAttributeKey::Create("test.scalar").Value(), PCGAttributeComparison::Equal, 1.0},
             {PCGAttributeKey::Create("test.vec2").Value(), PCGAttributeComparison::Equal, Math::Vec2{1, 2}},
             {PCGAttributeKey::Create("test.vec3").Value(), PCGAttributeComparison::Equal, Math::Vec3{1, 2, 3}},
             {PCGAttributeKey::Create("test.vec4").Value(), PCGAttributeComparison::Equal, Math::Vec4{1, 2, 3, 4}}}};
        for (std::size_t i = 0; i < predicates.size(); ++i) {
            Fixture f(PCGCpuNodeKind::AttributeFilter, predicates[i]);
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), i == 0 ? std::vector<std::size_t>{1, 3} : std::vector<std::size_t>{1});
        }
        for (const auto op : {PCGAttributeComparison::Less, PCGAttributeComparison::LessEqual, PCGAttributeComparison::Greater,
                              PCGAttributeComparison::GreaterEqual, PCGAttributeComparison::NotEqual}) {
            Fixture f(PCGCpuNodeKind::AttributeFilter, PCGAttributeFilterSettings{PCGAttributeKey::Create("test.scalar").Value(), op, 1.0});
            REQUIRE(f.Execute().HasValue());
            if (op == PCGAttributeComparison::Less)
                CheckRows(f.Output(), {0});
            if (op == PCGAttributeComparison::LessEqual)
                CheckRows(f.Output(), {0, 1});
            if (op == PCGAttributeComparison::Greater)
                CheckRows(f.Output(), {2, 3});
            if (op == PCGAttributeComparison::GreaterEqual)
                CheckRows(f.Output(), {1, 2, 3});
            if (op == PCGAttributeComparison::NotEqual)
                CheckRows(f.Output(), {0, 2, 3});
        }
    }

    TEST_CASE("PCG filters reject invalid ranges nonfinite values and noncanonical payloads", "[unit][pcg][filters]") {
        for (const auto settings :
             {PCGPointFilterSettings{PCGDensityRangeFilterSettings{0.8, 0.2}}, PCGPointFilterSettings{PCGDensityRangeFilterSettings{-1, 1}},
              PCGPointFilterSettings{PCGSlopeFilterSettings{0, 181}}, PCGPointFilterSettings{PCGDistanceFilterSettings{-1}},
              PCGPointFilterSettings{PCGBoundsFilterSettings{{{2, 0, 0}, {1, 1, 1}}}},
              PCGPointFilterSettings{PCGDistanceFilterSettings{std::numeric_limits<double>::infinity()}}})
            CheckError(EncodePCGPointFilterSettings(settings), PCGErrors::CpuEvaluationInvalid);
        CheckError(EncodePCGPointFilterSettings(
                       PCGAttributeFilterSettings{PCGAttributeKey::Create("test.boolean").Value(), PCGAttributeComparison::Less, true}),
                   PCGErrors::CpuEvaluationInvalid);
        Fixture f(PCGCpuNodeKind::DensityRangeFilter, PCGDensityRangeFilterSettings{});
        auto node = f.plan.Nodes()[1];
        const auto payload = node.payload;
        for (std::size_t size = 0; size < payload.size(); ++size) {
            node.payload.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(size));
            CHECK(detail::DecodePointFilter(node).HasError());
        }
        node.payload = payload;
        node.payload.push_back(0);
        CHECK(detail::DecodePointFilter(node).HasError());
        node.payload = payload;
        node.payload[4] = 0x80;
        CHECK(detail::DecodePointFilter(node).HasError());
    }

    TEST_CASE("PCG filter admission rejects missing and mismatched typed attributes before allocation", "[unit][pcg][filters]") {
        const auto key = PCGAttributeKey::Create("test.signed").Value();
        for (const auto value : {PCGAttributePredicateValue{std::int64_t{0}}, PCGAttributePredicateValue{std::uint64_t{0}}}) {
            auto owner = Owner(PCGCpuNodeKind::AttributeFilter);
            const auto plan = Plan(owner.Snapshot().Value(), PCGCpuNodeKind::AttributeFilter,
                                   PCGAttributeFilterSettings{key, PCGAttributeComparison::Equal, value});
            auto bounds = Bounds(Schema(value.index() == 2));
            CheckError(EvaluatePCGCpu(plan, Spatial(), bounds, {}, Limits()),
                       value.index() == 2 ? PCGErrors::PointAttributeTypeMismatch : PCGErrors::PointAttributeUnknown);
        }
    }

    TEST_CASE("PCG slope samples captured terrain and rejects uncovered input", "[unit][pcg][filters]") {
        SECTION("zero slope endpoints") {
            Fixture f(PCGCpuNodeKind::SlopeFilter, PCGSlopeFilterSettings{0, 0}, 3);
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {0, 1, 2});
        }
        SECTION("outside range") {
            Fixture f(PCGCpuNodeKind::SlopeFilter, PCGSlopeFilterSettings{1, 90}, 3);
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {});
        }
        SECTION("nearest terrain and stable identity tie") {
            Fixture f(PCGCpuNodeKind::SlopeFilter, PCGSlopeFilterSettings{44, 46}, 3);
            f.spatial = Spatial(true, 1, 1);
            REQUIRE(f.Execute().HasValue());
            CheckRows(f.Output(), {0, 1});
            Fixture reversed(PCGCpuNodeKind::SlopeFilter, PCGSlopeFilterSettings{44, 46}, 3);
            reversed.spatial = Spatial(true, 1, 2);
            REQUIRE(reversed.Execute().HasValue());
            CheckRows(reversed.Output(), {0, 1, 2});
        }
        SECTION("missing projected surface") {
            Fixture f(PCGCpuNodeKind::SlopeFilter, PCGSlopeFilterSettings{0, 180});
            CheckError(f.Execute(), PCGErrors::SpatialCoverageUnavailable);
            CHECK(f.workspace->ReadFinal(1, Id<PinId>(202)).HasError());
        }
        SECTION("missing snapshot surfaces") {
            auto owner = Owner(PCGCpuNodeKind::SlopeFilter);
            const auto plan = Plan(owner.Snapshot().Value(), PCGCpuNodeKind::SlopeFilter, PCGSlopeFilterSettings{});
            CheckError(EvaluatePCGCpu(plan, Spatial(false), Bounds(Schema()), {}, Limits()), PCGErrors::SpatialCoverageUnavailable);
        }
    }

    TEST_CASE("PCG filter checked work includes quadratic separation surface visits and bounded selection scratch",
              "[unit][pcg][filters]") {
        const auto distance = detail::ChargePointFilter(PCGCpuNodeKind::DistanceFilter, 4, 7);
        REQUIRE(distance.HasValue());
        CHECK(distance.Value().pointVisits == 24);
        CHECK(distance.Value().scratchBytes >= 4 * sizeof(std::size_t));
        const auto slope = detail::ChargePointFilter(PCGCpuNodeKind::SlopeFilter, 4, 7);
        REQUIRE(slope.HasValue());
        CHECK(slope.Value().snapshotElementVisits == 28);
        CHECK(detail::ChargePointFilter(PCGCpuNodeKind::DistanceFilter, std::numeric_limits<std::size_t>::max(), 1).HasError());
        CHECK(detail::ChargePointFilter(PCGCpuNodeKind::SlopeFilter, 2, std::numeric_limits<std::size_t>::max()).HasError());
        auto owner = Owner(PCGCpuNodeKind::DistanceFilter);
        const auto plan = Plan(owner.Snapshot().Value(), PCGCpuNodeKind::DistanceFilter, PCGDistanceFilterSettings{});
        auto limits = Limits();
        limits.maximumPointVisits = 1;
        CheckError(EvaluatePCGCpu(plan, Spatial(), Bounds(Schema()), {}, limits), PCGErrors::CpuEvaluationCapacityExceeded);
    }

    TEST_CASE("PCG filter plans retain exact historical catalog and spatial lifetime through replacement and close",
              "[unit][pcg][filters]") {
        auto owner = Owner(PCGCpuNodeKind::BoundsFilter);
        const auto root = owner.Snapshot().Value();
        const auto plan = Plan(root, PCGCpuNodeKind::BoundsFilter, PCGBoundsFilterSettings{{{1, 0, 0}, {2, 0, 0}}});
        REQUIRE(owner.Register(PCGCpuNodeKind::BoundsFilter, true).HasValue());
        REQUIRE(owner.Remove(PCGCpuNodeType(PCGCpuNodeKind::BoundsFilter).Value()).HasValue());
        REQUIRE(owner.Close().HasValue());
        const auto oldSpatial = Spatial();
        const auto newer = Spatial(true, 2);
        const auto first = EvaluatePCGCpu(plan, oldSpatial, Bounds(Schema()), {}, Limits());
        REQUIRE(first.HasValue());
        const auto second = EvaluatePCGCpu(plan, newer, Bounds(Schema()), {}, Limits());
        REQUIRE(second.HasValue());
        CHECK(first.Value().Catalog().Generation() == root.Generation());
        CHECK(first.Value().Outputs().back().points->PointCount() == 2);
        CHECK(first.Value().Outputs().back().points->Transforms()[0].translation.x == 1);
        CHECK(second.Value().Outputs().back().points->PointCount() == 2);
        CHECK(first.Value().Snapshot() != second.Value().Snapshot());
    }
}  // namespace Horo::PCG

namespace Horo::PCG {
    TEST_CASE("PCG filter cancellation discards candidates and closes workspace admission", "[unit][pcg][filters]") {
        Fixture f(PCGCpuNodeKind::DistanceFilter, PCGDistanceFilterSettings{1.1});
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto candidate = EvaluatePCGCpu(f.plan, f.spatial, f.bounds, {}, Limits(), PCGCpuAdmission::Accepting, cancellation.Token());
        CheckError(candidate, PCGErrors::CpuEvaluationClosed);
        CheckError(f.Execute(cancellation.Token()), PCGErrors::CpuEvaluationClosed);
        f.workspace->Cancel();
        CHECK(f.workspace->ReadInput(0, Id<PinId>(101)).HasError());
        CHECK(f.workspace->ReadFinal(1, Id<PinId>(202)).HasError());
        Fixture replacement(PCGCpuNodeKind::DistanceFilter, PCGDistanceFilterSettings{1.1});
        REQUIRE(replacement.Execute().HasValue());
        CheckRows(replacement.Output(), {0, 2});
    }
}  // namespace Horo::PCG

namespace Horo::PCG {
    TEST_CASE("PCG filters produce identical stable candidates across declared worker counts", "[unit][pcg][filters]") {
        auto owner = Owner(PCGCpuNodeKind::DistanceFilter);
        const auto plan = Plan(owner.Snapshot().Value(), PCGCpuNodeKind::DistanceFilter, PCGDistanceFilterSettings{1.1});
        const auto bounds = Bounds(Schema());
        const auto spatial = Spatial();
        const auto first = EvaluatePCGCpu(plan, spatial, bounds, {}, Limits());
        REQUIRE(first.HasValue());
        JobSystem jobs{JobSystemConfig{.workerCount = 2, .maxQueuedJobs = 8}};
        auto limits = Limits();
        limits.workers = 2;
        limits.jobs = &jobs;
        const auto second = EvaluatePCGCpu(plan, spatial, bounds, {}, limits);
        REQUIRE(second.HasValue());
        const auto &one = *first.Value().Outputs().back().points;
        const auto &two = *second.Value().Outputs().back().points;
        REQUIRE(one.PointCount() == 2);
        REQUIRE(two.PointCount() == one.PointCount());
        CHECK(std::ranges::equal(one.Seeds(), two.Seeds()));
        for (std::size_t i = 0; i < one.PointCount(); ++i)
            CHECK(one.Transforms()[i].translation.x == two.Transforms()[i].translation.x);
        auto retainedLimits = Limits();
        retainedLimits.retainedBytes = first.Value().ReservedBytes();
        const auto replacement = EvaluatePCGCpu(plan, Spatial(true, 2), bounds, {}, retainedLimits);
        REQUIRE(replacement.HasValue());
        CHECK(first.Value().Outputs().back().points->PointCount() == 2);
    }
}  // namespace Horo::PCG

namespace Horo::PCG {
    TEST_CASE("PCG terrain admission does not charge unrelated surfaces to legacy grid lookup", "[unit][pcg][filters]") {
        auto owner = Owner(PCGCpuNodeKind::DensityRangeFilter);
        const auto plan = Plan(owner.Snapshot().Value(), PCGCpuNodeKind::DensityRangeFilter, PCGDensityRangeFilterSettings{});
        auto limits = Limits();
        limits.maximumSnapshotElementVisits = 2;
        REQUIRE(EvaluatePCGCpu(plan, Spatial(), Bounds(Schema()), {}, limits).HasValue());
        auto slopeOwner = Owner(PCGCpuNodeKind::SlopeFilter);
        const auto slopePlan = Plan(slopeOwner.Snapshot().Value(), PCGCpuNodeKind::SlopeFilter, PCGSlopeFilterSettings{});
        limits.maximumSnapshotElementVisits = 9;
        CheckError(EvaluatePCGCpu(slopePlan, Spatial(), Bounds(Schema()), {}, limits), PCGErrors::CpuEvaluationCapacityExceeded);
    }
}  // namespace Horo::PCG
