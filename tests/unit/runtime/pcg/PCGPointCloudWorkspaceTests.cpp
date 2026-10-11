#include "Horo/PCG/PCGPointCloudWorkspace.h"
#include "PCGCookedPlanFixture.h"
#include "PCGCookedPlanTestFixtures.h"

namespace Horo::PCG {
    using namespace CookedPlanFixtures;

    namespace {
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
                {Id<NodeId>(10), PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value(), {1, 0}, {output(101)}, {0, 0, 0, 0, 0, 0, 0, 8}},
                {Id<NodeId>(20), PCGCpuNodeType(PCGCpuNodeKind::Forward).Value(), {1, 0}, {input(200), output(201)}, {}},
                {Id<NodeId>(30),
                 PCGCpuNodeType(fanOut ? PCGCpuNodeKind::Merge : PCGCpuNodeKind::Forward).Value(),
                 {1, 0},
                 {input(300), output(301)},
                 {}},
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
                for (const auto id : {40U, 50U, 60U, 70U})
                    source.nodes.push_back({Id<NodeId>(id),
                                            PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value(),
                                            {1, 0},
                                            {output(id * 10 + 1)},
                                            {0, 0, 0, 0, 0, 0, 0, 8}});
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

    TEST_CASE("PCG point workspace preallocates typed columns and reuses only after the last reader", "[unit][pcg][point][workspace]") {
        const auto graph = PointGraph();
        auto registry = Registry(false, &graph.Data());
        const auto plan = Compile(graph, registry.Snapshot().Value()).Value();
        CHECK(plan.Tier() == PCGOperationalTier::Baseline);
        const auto schema = PointSchema();
        const auto bounds = PointBounds(schema);
        auto created = PCGPointCloudWorkspace::Create(plan, bounds, LimitsForTier(plan.Tier()).Value().maximumScratchBytes);
        REQUIRE(created.HasValue());
        auto &workspace = *created.Value();
        CHECK(workspace.CurrentNode() == 0);
        CHECK(workspace.PeakRecords() == 4);
        CHECK(workspace.ReadFinal(2, Id<PinId>(301)).HasError());
        WritePoints(workspace, 0, Id<PinId>(101), 10);
        const auto first = workspace.ReadInput(0, Id<PinId>(101));
        CHECK(first.HasError());
        REQUIRE(workspace.FinishNode(0).HasValue());
        auto input = workspace.ReadInput(0, Id<PinId>(101));
        REQUIRE(input.HasValue());
        CHECK(workspace.CurrentNode() == 1);
        CHECK(input.Value().PointCount() == 2);
        CHECK(input.Value().Transforms().size() == 2);
        CHECK(input.Value().Bounds().size() == 2);
        CHECK(input.Value().Densities()[0] == 0.5F);
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
        CHECK(workspace.CurrentNode() == 3);
        CHECK(final.Value().Seeds()[0] == 30);
        CHECK(final.Value().Seeds()[1] == 0);
        CHECK(final.Value().Seeds().data() == firstAddress);
        CHECK(final.Value().FindColumn<PCGScalarColumn>("pcg.weight")[0] == 0.0);
    }

    TEST_CASE("PCG point workspace protects fan-out readers and admission boundaries", "[unit][pcg][point][workspace]") {
        const auto graph = PointGraph(true);
        auto registry = Registry(false, &graph.Data());
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
        auto registry = Registry(false, &graph.Data());
        const auto plan = Compile(graph, registry.Snapshot().Value()).Value();
        const auto schema = PointSchema();
        auto bounds = PointBounds(schema);
        const auto scratch = LimitsForTier(plan.Tier()).Value().maximumScratchBytes;
        auto duplicate = bounds;
        duplicate[1] = duplicate[0];
        CheckError(PCGPointCloudWorkspace::Create(plan, duplicate, scratch), PCGErrors::PointDataInvalid);
        auto wrongPin = bounds;
        wrongPin[0].pin = Id<PinId>(999);
        CheckError(PCGPointCloudWorkspace::Create(plan, wrongPin, scratch), PCGErrors::PointDataInvalid);
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

        auto invalidAttribute = PCGPointCloudWorkspace::Create(plan, PointBounds(schema), scratch);
        REQUIRE(invalidAttribute.HasValue());
        auto attributeWriter = invalidAttribute.Value()->BeginOutput(0, Id<PinId>(101), 1);
        REQUIRE(attributeWriter.HasValue());
        REQUIRE(
            attributeWriter.Value().SetColumnValue<PCGScalarColumn>("pcg.weight", 0, std::numeric_limits<double>::quiet_NaN()).HasValue());
        CheckError(invalidAttribute.Value()->SealOutput(0, Id<PinId>(101)), PCGErrors::PointDataInvalid);

        const auto wideGraph = PointGraph(true, true);
        auto wideRegistry = Registry(false, &wideGraph.Data());
        const auto widePlan = Compile(wideGraph, wideRegistry.Snapshot().Value()).Value();
        auto wideBounds = PointBounds(schema, 16'384);
        for (std::uint32_t index = 3; index < 7; ++index)
            wideBounds.push_back({index, Id<PinId>((index + 1) * 100 + 1), schema, 16'384});
        CheckError(PCGPointCloudWorkspace::Create(widePlan, wideBounds, scratch), PCGErrors::PointCapacityExceeded);
    }

    TEST_CASE("PCG failed output seal revokes every open writer", "[unit][pcg][point][workspace]") {
        const auto graph = PointGraph();
        auto registry = Registry(false, &graph.Data());
        const auto schema = PointSchema();
        const auto scratch = LimitsForTier(PCGOperationalTier::Baseline).Value().maximumScratchBytes;
        const auto validPlan = Compile(graph, registry.Snapshot().Value());
        REQUIRE(validPlan.HasValue());
        const auto plan = detail::PCGCookedPlanFixture::WithAdditionalPointOutputs(validPlan.Value());
        auto bounds = PointBounds(schema, 1);
        bounds.push_back({0, Id<PinId>(102), schema, 1});
        bounds.push_back({0, Id<PinId>(103), schema, 1});
        auto multi = PCGPointCloudWorkspace::Create(plan, bounds, scratch);
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
        auto registry = Registry(false, &graph.Data());
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
