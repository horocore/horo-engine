#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string_view>
#include <utility>

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

        void CheckError(const Result<PCGCookedPlan> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
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
}  // namespace Horo::PCG
