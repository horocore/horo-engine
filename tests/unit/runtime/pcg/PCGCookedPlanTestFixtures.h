#pragma once

#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::PCG::CookedPlanFixtures {
    template <typename Identity> inline Identity Id(const std::uint64_t value) {
        auto result = Identity::Create(value);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    inline PCGCapabilitySet Capabilities(const std::initializer_list<PCGCapability> values) {
        auto result = PCGCapabilitySet::Create(values);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    inline PCGNodeCatalogSnapshot ExecutableCatalog() {
        const auto projection =
            ProjectPCGCapabilities(PCGHostProfile::Interactive, Capabilities({PCGCapability::Validation, PCGCapability::OfflineBake}));
        REQUIRE(projection.HasValue());
        auto created = PCGNodeCatalog::Create(projection.Value());
        REQUIRE(created.HasValue());
        auto owner = std::move(created).Value();
        for (const auto kind :
             {PCGCpuNodeKind::SnapshotGrid, PCGCpuNodeKind::DensityFilter, PCGCpuNodeKind::Merge, PCGCpuNodeKind::Forward})
            REQUIRE(owner.Register(kind).HasValue());
        return owner.Snapshot().Value();
    }

    inline std::vector<PCGNodeTypeSupport> Catalog() {
        std::vector<PCGNodeTypeSupport> types;
        const auto root = ExecutableCatalog();
        for (const auto &entry : root.Nodes())
            types.push_back({entry.type, entry.version, entry.version});
        return types;
    }

    inline PCGGraphPin Pin(const std::uint64_t id, const PCGPinDirection direction, const bool defaultValue = false) {
        return {Id<PinId>(id), direction, PCGPinType::Scalar,
                direction == PCGPinDirection::Input ? PCGPinCardinality::Single : PCGPinCardinality::Multiple,
                defaultValue ? std::optional<PCGGraphValue>{PCGGraphValue{2.5}} : std::nullopt};
    }

    inline PCGGraphSourceData Source(const std::uint64_t revision = 1) {
        PCGGraphSourceData source;
        source.generation = {Id<GraphId>(501), Id<GraphRevision>(revision)};
        source.tier = PCGOperationalTier::Baseline;
        source.mode = PCGGenerationMode::Offline;
        source.deterministicSeed = 42;
        const auto point = [](const std::uint64_t id, const PCGPinDirection direction) {
            return PCGGraphPin{Id<PinId>(id), direction, PCGPinType::PointSet,
                               direction == PCGPinDirection::Input ? PCGPinCardinality::Single : PCGPinCardinality::Multiple, std::nullopt};
        };
        source.nodes = {
            {Id<NodeId>(40),
             PCGCpuNodeType(PCGCpuNodeKind::DensityFilter).Value(),
             {1, 0},
             {Pin(403, PCGPinDirection::Input, true), point(401, PCGPinDirection::Input), point(404, PCGPinDirection::Output)},
             {}},
            {Id<NodeId>(30),
             PCGCpuNodeType(PCGCpuNodeKind::Merge).Value(),
             {1, 0},
             {point(301, PCGPinDirection::Input), point(302, PCGPinDirection::Input), point(303, PCGPinDirection::Output)},
             {}},
            {Id<NodeId>(20),
             PCGCpuNodeType(PCGCpuNodeKind::DensityFilter).Value(),
             {1, 0},
             {point(200, PCGPinDirection::Input), Pin(202, PCGPinDirection::Input, true), point(201, PCGPinDirection::Output)},
             {}},
            {Id<NodeId>(10),
             PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value(),
             {1, 0},
             {point(101, PCGPinDirection::Output)},
             {0, 0, 0, 0, 0, 0, 0, 8}},
        };
        source.edges = {
            {Id<EdgeId>(1), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(30), Id<PinId>(301)},
            {Id<EdgeId>(2), Id<NodeId>(20), Id<PinId>(201), Id<NodeId>(30), Id<PinId>(302)},
            {Id<EdgeId>(3), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(20), Id<PinId>(200)},
            {Id<EdgeId>(4), Id<NodeId>(30), Id<PinId>(303), Id<NodeId>(40), Id<PinId>(401)},
        };
        source.exposedInputs = {{Id<ExposedInputId>(70), "world.density", Id<NodeId>(20), Id<PinId>(202), 3.5}};
        return source;
    }

    inline PCGGraphAsset Asset(PCGGraphSourceData source = Source()) {
        const auto types = Catalog();
        auto graph = PCGGraphAsset::Create(std::move(source), {.tier = PCGOperationalTier::Baseline, .supportedNodeTypes = types});
        REQUIRE(graph.HasValue());
        return std::move(graph).Value();
    }

    inline PCGGraphDescriptor Descriptor(const std::uint64_t revision = 1, const PCGGraphSourceData *supplied = nullptr) {
        const auto source = supplied ? *supplied : Source(revision);
        std::vector<PCGGraphNodeDescriptor> nodes;
        for (const auto &node : source.nodes)
            nodes.push_back({node.id, node.type});
        return {source.generation, std::move(nodes), Capabilities({PCGCapability::OfflineBake})};
    }

    inline PCGRegistry Registry(const bool reverseRuntimeRegistration = false, const PCGGraphSourceData *source = nullptr) {
        const auto root = ExecutableCatalog();
        auto registry = PCGRegistry::Create(Id<PCGRegistryInstanceId>(reverseRuntimeRegistration ? 8 : 7), root.Capabilities()).Value();
        std::vector<PCGNodeRuntimeDescriptor> runtimes;
        for (const auto &entry : root.Nodes())
            runtimes.push_back({entry.type, entry.runtimeContractVersion, entry.determinism, entry.requiredCapabilities});
        if (reverseRuntimeRegistration)
            std::ranges::reverse(runtimes);
        for (const auto &runtime : runtimes)
            REQUIRE(registry.RegisterNodeRuntime(runtime).HasValue());
        REQUIRE(registry.RegisterGraph(Descriptor(1, source)).HasValue());
        return registry;
    }

    inline Result<PCGCookedPlan> Compile(const PCGGraphAsset &graph, const PCGRegistrySnapshot &snapshot,
                                         const std::size_t maximumBytes = PCGGraphSourceHardLimits::SourceBytes) {
        auto validated = ValidatePCGGraph(graph, snapshot, Capabilities({PCGCapability::OfflineBake}));
        REQUIRE(validated.HasValue());
        return CompilePCGGraph(graph, validated.Value(), snapshot, ExecutableCatalog(), Capabilities({PCGCapability::OfflineBake}),
                               maximumBytes);
    }

    inline void CheckError(const auto &result, const ErrorCodeDescriptor &descriptor) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
    }

}  // namespace Horo::PCG::CookedPlanFixtures
