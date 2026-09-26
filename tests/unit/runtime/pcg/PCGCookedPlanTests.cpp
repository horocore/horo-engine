#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGPointCloudWorkspace.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::PCG {
    namespace {
        template <typename Identity> Identity Id(const std::uint64_t value) {
            auto result = Identity::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        PCGCapabilitySet Capabilities(const std::initializer_list<PCGCapability> values) {
            auto result = PCGCapabilitySet::Create(values);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        const std::array<PCGNodeTypeSupport, 3> &Catalog() {
            static const std::array catalog{
                PCGNodeTypeSupport{Id<NodeTypeId>(11), {1, 0}, {1, 0}},
                PCGNodeTypeSupport{Id<NodeTypeId>(22), {1, 0}, {1, 0}},
                PCGNodeTypeSupport{Id<NodeTypeId>(33), {1, 0}, {1, 0}},
            };
            return catalog;
        }

        PCGGraphPin Pin(const std::uint64_t id, const PCGPinDirection direction, const bool defaultValue = false) {
            return {Id<PinId>(id), direction, PCGPinType::Scalar,
                    direction == PCGPinDirection::Input ? PCGPinCardinality::Single : PCGPinCardinality::Multiple,
                    defaultValue ? std::optional<PCGGraphValue>{PCGGraphValue{2.5}} : std::nullopt};
        }

        PCGGraphSourceData Source(const std::uint64_t revision = 1) {
            PCGGraphSourceData source;
            source.generation = {Id<GraphId>(501), Id<GraphRevision>(revision)};
            source.tier = PCGOperationalTier::Baseline;
            source.mode = PCGGenerationMode::Offline;
            source.deterministicSeed = 42;
            source.nodes = {
                {Id<NodeId>(30),
                 Id<NodeTypeId>(33),
                 {1, 0},
                 {Pin(303, PCGPinDirection::Input, true), Pin(302, PCGPinDirection::Input), Pin(301, PCGPinDirection::Input)},
                 {3}},
                {Id<NodeId>(20), Id<NodeTypeId>(22), {1, 0}, {Pin(201, PCGPinDirection::Output)}, {2}},
                {Id<NodeId>(10),
                 Id<NodeTypeId>(11),
                 {1, 0},
                 {Pin(101, PCGPinDirection::Output), Pin(100, PCGPinDirection::Input, true)},
                 {1}},
            };
            source.edges = {
                {Id<EdgeId>(2), Id<NodeId>(20), Id<PinId>(201), Id<NodeId>(30), Id<PinId>(302)},
                {Id<EdgeId>(1), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(30), Id<PinId>(301)},
            };
            source.exposedInputs = {{Id<ExposedInputId>(70), "world.density", Id<NodeId>(10), Id<PinId>(100), 3.5}};
            return source;
        }

        PCGGraphAsset Asset(PCGGraphSourceData source = Source()) {
            auto graph = PCGGraphAsset::Create(std::move(source), {.tier = PCGOperationalTier::Baseline, .supportedNodeTypes = Catalog()});
            REQUIRE(graph.HasValue());
            return std::move(graph).Value();
        }

        PCGGraphDescriptor Descriptor(const std::uint64_t revision = 1) {
            return {{Id<GraphId>(501), Id<GraphRevision>(revision)},
                    {{Id<NodeId>(10), Id<NodeTypeId>(11)}, {Id<NodeId>(20), Id<NodeTypeId>(22)}, {Id<NodeId>(30), Id<NodeTypeId>(33)}},
                    Capabilities({PCGCapability::OfflineBake})};
        }

        PCGRegistry Registry(const bool reverseRuntimeRegistration = false) {
            auto projection =
                ProjectPCGCapabilities(PCGHostProfile::Interactive, Capabilities({PCGCapability::Validation, PCGCapability::OfflineBake}));
            REQUIRE(projection.HasValue());
            auto registry = PCGRegistry::Create(Id<PCGRegistryInstanceId>(reverseRuntimeRegistration ? 8 : 7), projection.Value()).Value();
            const std::array<std::uint64_t, 3> types =
                reverseRuntimeRegistration ? std::array<std::uint64_t, 3>{33, 22, 11} : std::array<std::uint64_t, 3>{11, 22, 33};
            for (const std::uint64_t type : types) {
                const PCGNodeRuntimeDescriptor runtime{Id<NodeTypeId>(type), 1, PCGNodeDeterminism::PortableDeterministic,
                                                       type == 33 ? Capabilities({PCGCapability::OfflineBake}) : PCGCapabilitySet::Empty()};
                REQUIRE(registry.RegisterNodeRuntime(runtime).HasValue());
            }
            REQUIRE(registry.RegisterGraph(Descriptor()).HasValue());
            return registry;
        }

        Result<PCGCookedPlan> Compile(const PCGGraphAsset &graph, const PCGRegistrySnapshot &snapshot,
                                      const std::size_t maximumBytes = PCGGraphSourceHardLimits::SourceBytes) {
            auto validated = ValidatePCGGraph(graph, snapshot, Capabilities({PCGCapability::OfflineBake}));
            REQUIRE(validated.HasValue());
            return CompilePCGGraph(graph, validated.Value(), snapshot, Capabilities({PCGCapability::OfflineBake}), maximumBytes);
        }

        void CheckError(const auto &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        PCGGraphAsset PointGraph(const bool fanOut = false, const bool extraFinalOutputs = false) {
            PCGGraphSourceData source;
            source.generation = {Id<GraphId>(501), Id<GraphRevision>(1)};
            source.tier = PCGOperationalTier::Baseline;
            source.mode = PCGGenerationMode::Offline;
            source.deterministicSeed = 42;
            const auto output = [](const std::uint64_t id) {
                return PCGGraphPin{Id<PinId>(id), PCGPinDirection::Output, PCGPinType::PointSet, PCGPinCardinality::Multiple, std::nullopt};
            };
            const auto input = [](const std::uint64_t id) {
                return PCGGraphPin{Id<PinId>(id), PCGPinDirection::Input, PCGPinType::PointSet, PCGPinCardinality::Single, std::nullopt};
            };
            source.nodes = {
                {Id<NodeId>(10), Id<NodeTypeId>(11), {1, 0}, {output(101)}, {}},
                {Id<NodeId>(20), Id<NodeTypeId>(22), {1, 0}, {input(200), output(201)}, {}},
                {Id<NodeId>(30), Id<NodeTypeId>(33), {1, 0}, {input(300), output(301)}, {}},
            };
            source.edges = {
                {Id<EdgeId>(1), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(20), Id<PinId>(200)},
                {Id<EdgeId>(2), Id<NodeId>(20), Id<PinId>(201), Id<NodeId>(30), Id<PinId>(300)},
            };
            if (fanOut) {
                source.nodes[2].pins.push_back(input(302));
                source.edges.push_back({Id<EdgeId>(3), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(30), Id<PinId>(302)});
            }
            if (extraFinalOutputs) {
                source.nodes[0].pins.push_back(output(102));
                source.nodes[0].pins.push_back(output(103));
            }
            return Asset(std::move(source));
        }

        std::shared_ptr<const PCGPointSchema> PointSchema() {
            auto key = PCGAttributeKey::Create("pcg.weight");
            REQUIRE(key.HasValue());
            const std::array fields{PCGAttributeDescriptor{std::move(key).Value(), PCGAttributeType::Scalar}};
            auto captured = PCGPointSchema::Capture({PCGOperationalTier::Baseline, fields});
            REQUIRE(captured.HasValue());
            return std::make_shared<const PCGPointSchema>(std::move(captured).Value());
        }

        std::vector<PCGPointOutputBound> PointBounds(const std::shared_ptr<const PCGPointSchema> &schema, const std::size_t count = 2) {
            return {{0, Id<PinId>(101), schema, count}, {1, Id<PinId>(201), schema, count}, {2, Id<PinId>(301), schema, count}};
        }

        void WritePoints(PCGPointCloudWorkspace &workspace, const std::uint32_t node, const PinId pin, const std::uint64_t seed,
                         const std::size_t count = 2) {
            auto write = workspace.BeginOutput(node, pin, count);
            REQUIRE(write.HasValue());
            const auto view = write.Value();
            for (std::size_t index = 0; index < count; ++index) {
                REQUIRE(view.SetPoint(index, {}, {{-1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, 1.0F}}, 0.5F, seed + index).HasValue());
                REQUIRE(view.SetColumnValue<PCGScalarColumn>("pcg.weight", index, 0.25).HasValue());
            }
            REQUIRE(workspace.SealOutput(node, pin).HasValue());
            CheckError(view.SetPoint(0, {}, {}, 0.0F, 999), PCGErrors::PointDataInvalid);
        }
    }  // namespace

    TEST_CASE("PCG cooked plan owns canonical ordered nodes, routing and constants", "[unit][pcg][cook]") {
        const auto graph = Asset();
        auto registry = Registry();
        const auto snapshot = registry.Snapshot().Value();
        const auto compiled = Compile(graph, snapshot);
        REQUIRE(compiled.HasValue());
        const auto &plan = compiled.Value();
        const auto validated = ValidatePCGGraph(graph, snapshot, PCGCapabilitySet::Empty());
        REQUIRE(validated.HasValue());
        CHECK(plan.Generation() == graph.Data().generation);
        CHECK(plan.SourceSchema() == CurrentPCGGraphSchemaVersion);
        CHECK(plan.SourceDigest() == validated.Value().SourceDigest());
        CHECK(plan.Version() == CurrentPCGCookedPlanVersion);
        CHECK(plan.CompilerVersion() == CurrentPCGCompilerVersion);
        REQUIRE(plan.Nodes().size() == 3);
        CHECK(plan.Nodes()[0].id == Id<NodeId>(10));
        CHECK(plan.Nodes()[1].id == Id<NodeId>(20));
        CHECK(plan.Nodes()[2].id == Id<NodeId>(30));
        CHECK(plan.Nodes()[2].requiredCapabilities.Contains(PCGCapability::OfflineBake));
        REQUIRE(plan.Routes().size() == 2);
        CHECK(plan.Routes()[0].sourceNode == 0);
        CHECK(plan.Routes()[0].targetNode == 2);
        CHECK(plan.Routes()[1].sourceNode == 1);
        CHECK(plan.Routes()[1].targetNode == 2);
        REQUIRE(plan.Constants().size() == 1);
        CHECK(plan.Constants()[0].pin == Id<PinId>(303));
        REQUIRE(plan.ExposedInputs().size() == 1);
        CHECK(plan.ExposedInputs()[0].key == "world.density");
        CHECK(std::get<double>(plan.ExposedInputs()[0].defaultValue) == 3.5);
        CHECK(plan.RequiredCapabilities().Contains(PCGCapability::OfflineBake));
        REQUIRE(plan.CanonicalBytes().size() > 64);
        CHECK(plan.CanonicalBytes()[0] == 'H');
        CHECK(plan.CanonicalBytes()[3] == 'P');
    }

    TEST_CASE("Equivalent PCG sources and provider registration orders emit byte-equivalent plans", "[unit][pcg][cook][canonical]") {
        auto source = Source();
        std::ranges::reverse(source.nodes);
        std::ranges::reverse(source.edges);
        std::ranges::reverse(source.nodes.front().pins);
        const auto firstGraph = Asset();
        const auto secondGraph = Asset(std::move(source));
        auto firstRegistry = Registry();
        auto secondRegistry = Registry(true);
        const auto first = Compile(firstGraph, firstRegistry.Snapshot().Value());
        const auto second = Compile(secondGraph, secondRegistry.Snapshot().Value());
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(std::ranges::equal(first.Value().CanonicalBytes(), second.Value().CanonicalBytes()));

        const auto nextRevision = Asset(Source(2));
        auto nextRegistry = Registry();
        REQUIRE(nextRegistry.ReplaceGraph(Descriptor(2)).HasValue());
        const auto changed = Compile(nextRevision, nextRegistry.Snapshot().Value());
        REQUIRE(changed.HasValue());
        CHECK_FALSE(std::ranges::equal(first.Value().CanonicalBytes(), changed.Value().CanonicalBytes()));

        auto providerReplacement = Registry();
        REQUIRE(providerReplacement
                    .ReplaceNodeRuntime({Id<NodeTypeId>(11), 2, PCGNodeDeterminism::PortableDeterministic, PCGCapabilitySet::Empty()})
                    .HasValue());
        const auto providerChanged = Compile(firstGraph, providerReplacement.Snapshot().Value());
        REQUIRE(providerChanged.HasValue());
        CHECK(providerChanged.Value().Nodes()[0].runtimeContractVersion == 2);
        CHECK_FALSE(std::ranges::equal(first.Value().CanonicalBytes(), providerChanged.Value().CanonicalBytes()));
    }

    TEST_CASE("PCG cooked constants encode every typed value in network byte order", "[unit][pcg][cook][canonical]") {
        struct ValueCase final {
            PCGPinType type;
            PCGGraphValue value;
            std::vector<std::uint8_t> encoded;
        };

        const std::array cases{
            ValueCase{PCGPinType::Boolean, true, {1, 1}},
            ValueCase{PCGPinType::SignedInteger, std::int64_t{-2}, {2, 255, 255, 255, 255, 255, 255, 255, 254}},
            ValueCase{PCGPinType::UnsignedInteger, std::uint64_t{0x0102030405060708}, {3, 1, 2, 3, 4, 5, 6, 7, 8}},
            ValueCase{PCGPinType::Scalar, 2.5, {4, 0x40, 0x04, 0, 0, 0, 0, 0, 0}},
            ValueCase{PCGPinType::Vector2, Math::Vec2{1.0f, -2.0f}, {5, 0x3f, 0x80, 0, 0, 0xc0, 0, 0, 0}},
            ValueCase{PCGPinType::Vector3, Math::Vec3{1.0f, -2.0f, 0.5f}, {6, 0x3f, 0x80, 0, 0, 0xc0, 0, 0, 0, 0x3f, 0, 0, 0}},
            ValueCase{PCGPinType::Vector4,
                      Math::Vec4{1.0f, -2.0f, 0.5f, 3.0f},
                      {7, 0x3f, 0x80, 0, 0, 0xc0, 0, 0, 0, 0x3f, 0, 0, 0, 0x40, 0x40, 0, 0}},
        };
        auto registry = Registry();
        const auto snapshot = registry.Snapshot().Value();
        for (const ValueCase &valueCase : cases) {
            auto source = Source();
            source.nodes.front().pins.front().type = valueCase.type;
            source.nodes.front().pins.front().defaultValue = valueCase.value;
            const auto graph = Asset(std::move(source));
            const auto compiled = Compile(graph, snapshot);
            REQUIRE(compiled.HasValue());
            REQUIRE(compiled.Value().Constants().size() == 1);
            CHECK(compiled.Value().Constants().front().value == valueCase.value);
            const auto bytes = compiled.Value().CanonicalBytes();
            CHECK(std::search(bytes.begin(), bytes.end(), valueCase.encoded.begin(), valueCase.encoded.end()) != bytes.end());
        }
    }

    TEST_CASE("PCG cooked plan rejects stale snapshot and finite byte overflow", "[unit][pcg][cook][failure]") {
        const auto graph = Asset();
        auto registry = Registry();
        const auto retained = registry.Snapshot().Value();
        auto validated = ValidatePCGGraph(graph, retained, Capabilities({PCGCapability::OfflineBake}));
        REQUIRE(validated.HasValue());
        REQUIRE(registry.ReplaceGraph(Descriptor(2)).HasValue());
        CheckError(CompilePCGGraph(graph, validated.Value(), registry.Snapshot().Value()), PCGErrors::CookedPlanStale);
        auto alteredSource = Source();
        alteredSource.nodes.front().payload = {9};
        CheckError(CompilePCGGraph(Asset(std::move(alteredSource)), validated.Value(), retained), PCGErrors::CookedPlanStale);
        CheckError(CompilePCGGraph(graph, validated.Value(), retained, PCGCapabilitySet::Empty(), 1),
                   PCGErrors::CookedPlanCapacityExceeded);
        CheckError(CompilePCGGraph(graph, validated.Value(), retained, PCGCapabilitySet::Empty(), 0),
                   PCGErrors::CookedPlanCapacityExceeded);

        const auto full = CompilePCGGraph(graph, validated.Value(), retained);
        REQUIRE(full.HasValue());
        const auto exactBytes = full.Value().CanonicalBytes().size();
        CHECK(CompilePCGGraph(graph, validated.Value(), retained, PCGCapabilitySet::Empty(), exactBytes).HasValue());
        CheckError(CompilePCGGraph(graph, validated.Value(), retained, PCGCapabilitySet::Empty(), exactBytes - 1),
                   PCGErrors::CookedPlanCapacityExceeded);
    }

    TEST_CASE("PCG cooked plan rejects an exposed input competing with an incoming route", "[unit][pcg][cook][routing]") {
        auto source = Source();
        source.exposedInputs.front().node = Id<NodeId>(30);
        source.exposedInputs.front().pin = Id<PinId>(301);
        const auto graph = Asset(std::move(source));
        auto registry = Registry();
        const auto snapshot = registry.Snapshot().Value();
        auto validated = ValidatePCGGraph(graph, snapshot, PCGCapabilitySet::Empty());
        REQUIRE(validated.HasValue());
        CheckError(CompilePCGGraph(graph, validated.Value(), snapshot), PCGErrors::CookedPlanInvalid);
    }

    TEST_CASE("PCG cooked plan survives source and registry retirement without borrowed state", "[unit][pcg][cook][lifecycle]") {
        auto plan = [] {
            const auto graph = Asset();
            auto registry = Registry();
            const auto snapshot = registry.Snapshot().Value();
            auto compiled = Compile(graph, snapshot);
            REQUIRE(compiled.HasValue());
            registry.Close();
            return std::move(compiled).Value();
        }();
        const std::vector<std::uint8_t> original(plan.CanonicalBytes().begin(), plan.CanonicalBytes().end());
        CHECK(plan.Generation().revision == Id<GraphRevision>(1));
        CHECK(plan.Nodes()[0].payload == std::vector<std::uint8_t>{1});
        CHECK(std::ranges::equal(plan.CanonicalBytes(), original));
    }

    TEST_CASE("PCG cooked-plan errors are stable unique public descriptors", "[unit][pcg][cook][errors]") {
        const std::array descriptors{&PCGErrors::CookedPlanInvalid, &PCGErrors::CookedPlanStale, &PCGErrors::CookedPlanCapacityExceeded};
        std::set<std::string_view> codes;
        for (const ErrorCodeDescriptor *descriptor : descriptors) {
            CHECK(descriptor->domain.Value() == "horo.pcg");
            CHECK_FALSE(descriptor->summary.empty());
            CHECK_FALSE(descriptor->remediationHint.empty());
            CHECK(codes.insert(descriptor->code.Value()).second);
        }
    }

    TEST_CASE("PCG point workspace preallocates typed columns and reuses only after the last reader", "[unit][pcg][point][workspace]") {
        const auto graph = PointGraph();
        auto registry = Registry();
        const auto plan = Compile(graph, registry.Snapshot().Value()).Value();
        CHECK(plan.Tier() == PCGOperationalTier::Baseline);
        const auto schema = PointSchema();
        const auto bounds = PointBounds(schema);
        auto created = PCGPointCloudWorkspace::Create(plan, bounds, LimitsForTier(plan.Tier()).Value().maximumScratchBytes);
        REQUIRE(created.HasValue());
        auto &workspace = *created.Value();
        CHECK(workspace.PeakRecords() == 4);
        WritePoints(workspace, 0, Id<PinId>(101), 10);
        const auto first = workspace.ReadInput(0, Id<PinId>(101));
        CHECK(first.HasError());
        REQUIRE(workspace.FinishNode(0).HasValue());
        auto input = workspace.ReadInput(0, Id<PinId>(101));
        REQUIRE(input.HasValue());
        CHECK(input.Value().Seeds()[0] == 10);
        const auto *firstAddress = input.Value().Seeds().data();
        WritePoints(workspace, 1, Id<PinId>(201), 20);
        CHECK(input.Value().Seeds()[0] == 10);
        REQUIRE(workspace.FinishNode(1).HasValue());
        auto second = workspace.ReadInput(1, Id<PinId>(201));
        REQUIRE(second.HasValue());
        CHECK(second.Value().Seeds()[0] == 20);
        auto recycled = workspace.BeginOutput(2, Id<PinId>(301), 2);
        REQUIRE(recycled.HasValue());
        REQUIRE(recycled.Value().SetPoint(0, {}, {}, 0.0F, 30).HasValue());
        REQUIRE(workspace.SealOutput(2, Id<PinId>(301)).HasValue());
        CheckError(recycled.Value().SetColumnValue<PCGScalarColumn>("pcg.weight", 0, 2.0), PCGErrors::PointDataInvalid);
        auto finalWrite = workspace.ReadInput(1, Id<PinId>(201));
        REQUIRE(finalWrite.HasValue());
        CHECK(finalWrite.Value().Seeds()[0] == 20);
        REQUIRE(workspace.FinishNode(2).HasValue());
        const auto final = workspace.ReadFinal(2, Id<PinId>(301));
        REQUIRE(final.HasValue());
        CHECK(final.Value().Seeds()[0] == 30);
        CHECK(final.Value().Seeds()[1] == 0);
        CHECK(final.Value().Seeds().data() == firstAddress);
        CHECK(final.Value().FindColumn<PCGScalarColumn>("pcg.weight")[0] == 0.0);
    }

    TEST_CASE("PCG point workspace protects fan-out readers and admission boundaries", "[unit][pcg][point][workspace]") {
        const auto graph = PointGraph(true);
        auto registry = Registry();
        const auto plan = Compile(graph, registry.Snapshot().Value()).Value();
        const auto schema = PointSchema();
        const auto bounds = PointBounds(schema);
        auto created = PCGPointCloudWorkspace::Create(plan, bounds, LimitsForTier(plan.Tier()).Value().maximumScratchBytes);
        REQUIRE(created.HasValue());
        auto &workspace = *created.Value();
        CHECK(workspace.PeakRecords() == 6);
        const auto charged = workspace.ReservedBytes();
        CHECK(PCGPointCloudWorkspace::Create(plan, bounds, charged).HasValue());
        CheckError(PCGPointCloudWorkspace::Create(plan, bounds, charged - 1), PCGErrors::PointCapacityExceeded);
        WritePoints(workspace, 0, Id<PinId>(101), 10);
        REQUIRE(workspace.FinishNode(0).HasValue());
        const auto first = workspace.ReadInput(0, Id<PinId>(101));
        REQUIRE(first.HasValue());
        const auto *firstAddress = first.Value().Seeds().data();
        WritePoints(workspace, 1, Id<PinId>(201), 20);
        REQUIRE(workspace.FinishNode(1).HasValue());
        const auto retained = workspace.ReadInput(0, Id<PinId>(101));
        REQUIRE(retained.HasValue());
        CHECK(retained.Value().Seeds()[0] == 10);
        const auto second = workspace.ReadInput(1, Id<PinId>(201));
        REQUIRE(second.HasValue());
        WritePoints(workspace, 2, Id<PinId>(301), 30);
        REQUIRE(workspace.FinishNode(2).HasValue());
        const auto final = workspace.ReadFinal(2, Id<PinId>(301));
        REQUIRE(final.HasValue());
        CHECK(final.Value().Seeds().data() != firstAddress);
        CHECK(final.Value().Seeds()[0] == 30);
    }

    TEST_CASE("PCG point workspace rejects malformed bounds, invalid writes, and stale lifecycle", "[unit][pcg][point][workspace]") {
        const auto graph = PointGraph();
        auto registry = Registry();
        const auto plan = Compile(graph, registry.Snapshot().Value()).Value();
        const auto schema = PointSchema();
        auto bounds = PointBounds(schema);
        const auto scratch = LimitsForTier(plan.Tier()).Value().maximumScratchBytes;
        auto duplicate = bounds;
        duplicate[1] = duplicate[0];
        CheckError(PCGPointCloudWorkspace::Create(plan, duplicate, scratch), PCGErrors::PointDataInvalid);
        auto missing = bounds;
        missing.pop_back();
        CheckError(PCGPointCloudWorkspace::Create(plan, missing, scratch), PCGErrors::PointDataInvalid);
        bounds[0].maximumPoints = 16'385;
        CheckError(PCGPointCloudWorkspace::Create(plan, bounds, scratch), PCGErrors::PointCapacityExceeded);
        bounds[0].maximumPoints = 16'384;
        auto boundary = PCGPointCloudWorkspace::Create(plan, bounds, scratch);
        REQUIRE(boundary.HasValue());
        CHECK(boundary.Value()->PeakRecords() == 16'386);
        CheckError(boundary.Value()->BeginOutput(0, Id<PinId>(101), 16'385), PCGErrors::PointCapacityExceeded);
        auto exact = PCGPointCloudWorkspace::Create(plan, bounds, scratch);
        REQUIRE(exact.HasValue());
        REQUIRE(exact.Value()->BeginOutput(0, Id<PinId>(101), 16'384).HasValue());
        REQUIRE(exact.Value()->SealOutput(0, Id<PinId>(101)).HasValue());
        auto write = boundary.Value()->BeginOutput(0, Id<PinId>(101), 1);
        REQUIRE(write.HasValue());
        REQUIRE(write.Value().SetPoint(0, {}, {}, 2.0F, 1).HasValue());
        CheckError(boundary.Value()->SealOutput(0, Id<PinId>(101)), PCGErrors::PointDataInvalid);
        CheckError(write.Value().SetPoint(0, {}, {}, 0.5F, 1), PCGErrors::PointDataInvalid);
        CheckError(boundary.Value()->FinishNode(0), PCGErrors::PointDataInvalid);
        boundary.Value()->Cancel();
        CheckError(boundary.Value()->BeginOutput(0, Id<PinId>(101), 1), PCGErrors::PointDataInvalid);

        const auto wideGraph = PointGraph(true, true);
        const auto widePlan = Compile(wideGraph, registry.Snapshot().Value()).Value();
        auto wideBounds = PointBounds(schema, 16'384);
        wideBounds.push_back({0, Id<PinId>(102), schema, 16'384});
        wideBounds.push_back({0, Id<PinId>(103), schema, 16'384});
        CheckError(PCGPointCloudWorkspace::Create(widePlan, wideBounds, scratch), PCGErrors::PointCapacityExceeded);
    }

    TEST_CASE("PCG failed output seal revokes every open writer", "[unit][pcg][point][workspace]") {
        auto registry = Registry();
        const auto schema = PointSchema();
        const auto scratch = LimitsForTier(PCGOperationalTier::Baseline).Value().maximumScratchBytes;
        const auto multiGraph = PointGraph(false, true);
        const auto multiPlan = Compile(multiGraph, registry.Snapshot().Value()).Value();
        auto multiBounds = PointBounds(schema, 1);
        multiBounds.push_back({0, Id<PinId>(102), schema, 1});
        multiBounds.push_back({0, Id<PinId>(103), schema, 1});
        auto multi = PCGPointCloudWorkspace::Create(multiPlan, multiBounds, scratch);
        REQUIRE(multi.HasValue());
        auto invalidWriter = multi.Value()->BeginOutput(0, Id<PinId>(101), 1);
        auto otherWriter = multi.Value()->BeginOutput(0, Id<PinId>(102), 1);
        REQUIRE(invalidWriter.HasValue());
        REQUIRE(otherWriter.HasValue());
        REQUIRE(invalidWriter.Value().SetPoint(0, {}, {}, 2.0F, 1).HasValue());
        CheckError(multi.Value()->SealOutput(0, Id<PinId>(101)), PCGErrors::PointDataInvalid);
        CheckError(otherWriter.Value().SetPoint(0, {}, {}, 0.5F, 2), PCGErrors::PointDataInvalid);
    }

    TEST_CASE("PCG point workspace replacement reserves overlap and leaves old results intact", "[unit][pcg][point][workspace]") {
        const auto graph = PointGraph();
        auto registry = Registry();
        const auto plan = Compile(graph, registry.Snapshot().Value()).Value();
        const auto schema = PointSchema();
        const auto bounds = PointBounds(schema);
        const auto limits = LimitsForTier(plan.Tier()).Value();
        auto old = PCGPointCloudWorkspace::Create(plan, bounds, limits.maximumScratchBytes);
        REQUIRE(old.HasValue());
        WritePoints(*old.Value(), 0, Id<PinId>(101), 10);
        REQUIRE(old.Value()->FinishNode(0).HasValue());
        auto replacement = PCGPointCloudWorkspace::Create(plan, bounds, limits.maximumScratchBytes, old.Value()->ReservedBytes());
        REQUIRE(replacement.HasValue());
        CheckError(PCGPointCloudWorkspace::Create(plan, bounds, limits.maximumScratchBytes, limits.maximumReplacementOverlapBytes),
                   PCGErrors::PointCapacityExceeded);
        WritePoints(*replacement.Value(), 0, Id<PinId>(101), 100);
        REQUIRE(replacement.Value()->FinishNode(0).HasValue());
        CHECK(old.Value()->ReadInput(0, Id<PinId>(101)).Value().Seeds()[0] == 10);
        CHECK(replacement.Value()->ReadInput(0, Id<PinId>(101)).Value().Seeds()[0] == 100);
        old.Value()->Cancel();
        CHECK(old.Value()->ReadInput(0, Id<PinId>(101)).HasError());
        CHECK(replacement.Value()->ReadInput(0, Id<PinId>(101)).HasValue());
        auto cancelled = PCGPointCloudWorkspace::Create(plan, bounds, limits.maximumScratchBytes);
        REQUIRE(cancelled.HasValue());
        auto openWriter = cancelled.Value()->BeginOutput(0, Id<PinId>(101), 1);
        REQUIRE(openWriter.HasValue());
        cancelled.Value()->Cancel();
        CheckError(openWriter.Value().SetPoint(0, {}, {}, 0.5F, 77), PCGErrors::PointDataInvalid);
        CheckError(openWriter.Value().SetColumnValue<PCGScalarColumn>("pcg.weight", 0, 0.25), PCGErrors::PointDataInvalid);
    }
}  // namespace Horo::PCG
