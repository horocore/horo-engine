#include "PCGCookedPlanTestFixtures.h"
#include "PCGCookedPlanWriter.h"

namespace Horo::PCG {
    using namespace CookedPlanFixtures;

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
        REQUIRE(plan.Nodes().size() == 4);
        CHECK(plan.Nodes()[0].id == Id<NodeId>(10));
        CHECK(plan.Nodes()[1].id == Id<NodeId>(20));
        CHECK(plan.Nodes()[2].id == Id<NodeId>(30));
        CHECK(plan.Nodes()[2].requiredCapabilities.Contains(PCGCapability::Validation));
        CHECK(plan.Nodes()[3].id == Id<NodeId>(40));
        REQUIRE(plan.Routes().size() == 4);
        const std::array expectedRoutes{
            PCGCookedRoute{Id<EdgeId>(3), 0, Id<PinId>(101), 1, Id<PinId>(200)},
            PCGCookedRoute{Id<EdgeId>(1), 0, Id<PinId>(101), 2, Id<PinId>(301)},
            PCGCookedRoute{Id<EdgeId>(2), 1, Id<PinId>(201), 2, Id<PinId>(302)},
            PCGCookedRoute{Id<EdgeId>(4), 2, Id<PinId>(303), 3, Id<PinId>(401)},
        };
        CHECK(std::ranges::equal(plan.Routes(), expectedRoutes));
        REQUIRE(plan.Constants().size() == 1);
        CHECK(plan.Constants()[0].pin == Id<PinId>(403));
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
                    .ReplaceNodeRuntime({PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value(), 2, PCGNodeDeterminism::ProfileDeterministic,
                                         Capabilities({PCGCapability::Validation})})
                    .HasValue());
        const auto providerChanged = Compile(firstGraph, providerReplacement.Snapshot().Value());
        CheckError(providerChanged, PCGErrors::GraphNodeVersionUnsupported);
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
            detail::BoundedPlanWriter writer{valueCase.encoded.size()};
            REQUIRE(writer.Value(valueCase.value));
            CHECK(std::move(writer).Take() == valueCase.encoded);
            detail::BoundedPlanWriter narrow{valueCase.encoded.size() - 1};
            CHECK_FALSE(narrow.Value(valueCase.value));
            if (valueCase.type == PCGPinType::Scalar) {
                REQUIRE(compiled.HasValue());
                REQUIRE(compiled.Value().Constants().size() == 1);
                CHECK(compiled.Value().Constants().front().value == valueCase.value);
                const auto bytes = compiled.Value().CanonicalBytes();
                CHECK(std::search(bytes.begin(), bytes.end(), valueCase.encoded.begin(), valueCase.encoded.end()) != bytes.end());
            } else
                CheckError(compiled, PCGErrors::RegistryDescriptorInvalid);
        }
    }

    TEST_CASE("PCG cooked plan rejects stale snapshot and finite byte overflow", "[unit][pcg][cook][failure]") {
        const auto graph = Asset();
        auto registry = Registry();
        const auto retained = registry.Snapshot().Value();
        auto validated = ValidatePCGGraph(graph, retained, Capabilities({PCGCapability::OfflineBake}));
        REQUIRE(validated.HasValue());
        REQUIRE(registry.ReplaceGraph(Descriptor(2)).HasValue());
        CheckError(CompilePCGGraph(graph, validated.Value(), registry.Snapshot().Value(), ExecutableCatalog()), PCGErrors::CookedPlanStale);
        auto alteredSource = Source();
        alteredSource.nodes.front().payload = {9};
        CheckError(CompilePCGGraph(Asset(std::move(alteredSource)), validated.Value(), retained, ExecutableCatalog()),
                   PCGErrors::CookedPlanStale);
        CheckError(CompilePCGGraph(graph, validated.Value(), retained, ExecutableCatalog(), PCGCapabilitySet::Empty(), 1),
                   PCGErrors::CookedPlanCapacityExceeded);
        CheckError(CompilePCGGraph(graph, validated.Value(), retained, ExecutableCatalog(), PCGCapabilitySet::Empty(), 0),
                   PCGErrors::CookedPlanCapacityExceeded);

        const auto full = CompilePCGGraph(graph, validated.Value(), retained, ExecutableCatalog());
        REQUIRE(full.HasValue());
        const auto exactBytes = full.Value().CanonicalBytes().size();
        CHECK(CompilePCGGraph(graph, validated.Value(), retained, ExecutableCatalog(), PCGCapabilitySet::Empty(), exactBytes).HasValue());
        CheckError(CompilePCGGraph(graph, validated.Value(), retained, ExecutableCatalog(), PCGCapabilitySet::Empty(), exactBytes - 1),
                   PCGErrors::CookedPlanCapacityExceeded);
    }

    TEST_CASE("PCG cooked plan rejects an exposed input competing with an incoming route", "[unit][pcg][cook][routing]") {
        auto source = Source();
        source.nodes.back().pins.front().type = PCGPinType::Scalar;
        source.edges.erase(source.edges.begin());
        source.edges[1].targetPin = Id<PinId>(202);
        const auto graph = Asset(std::move(source));
        auto registry = Registry();
        const auto snapshot = registry.Snapshot().Value();
        auto validated = ValidatePCGGraph(graph, snapshot, PCGCapabilitySet::Empty());
        REQUIRE(validated.HasValue());
        CheckError(CompilePCGGraph(graph, validated.Value(), snapshot, ExecutableCatalog()), PCGErrors::CookedPlanInvalid);
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
        CHECK(plan.Nodes()[0].payload == std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 8});
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
