#include "AnimationGraphTestFixtures.h"

#include <algorithm>
#include <limits>
#include <type_traits>
using namespace Horo;
using namespace Horo::Animation;
using namespace Horo::Animation::Test;
using namespace Horo::Animation::Test::Graph;

TEST_CASE("Graph compilation schedules stable typed operands and source locations", "[animation][graph]") {
    const auto data = Blended();
    const auto compiled = CompileGraph(data);
    REQUIRE(compiled.HasValue());
    const auto &program = compiled.Value();
    CHECK(program.Id() == data.id);
    CHECK(program.Skeleton() == data.skeleton);
    REQUIRE(program.Clips().size() == 1);
    const auto &definition = program.Definitions().front();
    REQUIRE(definition.instructions.size() == 5);
    CHECK(definition.instructions[0].source.node == Id<GraphNodeId>(3));
    CHECK(definition.instructions[0].parameterIndex == 0);
    CHECK(definition.outputInstruction == 4);
    const auto &blend = definition.instructions[3];
    REQUIRE(blend.inputs.size() == 3);
    CHECK(blend.inputs[0].source.pin == Id<GraphPinId>(1));
    CHECK(blend.inputs[0].source.node == Id<GraphNodeId>(4));
    CHECK(blend.inputs[0].instruction == 1);
    CHECK(blend.inputs[2].type == GraphValueType::Float);
    CHECK(blend.inputs[2].instruction == 0);
    CHECK(definition.callDepth == 1);
    CHECK(definition.evaluationInstructions == 5);
    for (std::size_t index = 0; index < definition.instructions.size(); ++index)
        for (const auto &input : definition.instructions[index].inputs)
            CHECK(input.instruction < index);
}

TEST_CASE("Graph canonical programs ignore authoring collection order", "[animation][graph]") {
    auto data = Subgraphs();
    const auto first = CompileGraph(data);
    REQUIRE(first.HasValue());
    std::reverse(data.definitions.begin(), data.definitions.end());
    for (auto &definition : data.definitions) {
        std::reverse(definition.nodes.begin(), definition.nodes.end());
        std::reverse(definition.inputs.begin(), definition.inputs.end());
        std::reverse(definition.connections.begin(), definition.connections.end());
        for (auto &node : definition.nodes)
            std::reverse(node.pins.begin(), node.pins.end());
    }
    const auto second = CompileGraph(data);
    REQUIRE(second.HasValue());
    CHECK(first.Value() == second.Value());
    const auto &root = first.Value().Definitions()[first.Value().EntryIndex()];
    REQUIRE(root.instructions[1].definitionIndex == 1);
    CHECK(root.callDepth == 2);
    CHECK(root.evaluationInstructions == 5);
}

TEST_CASE("Graph malformed identities and endpoint structures fail without publication", "[animation][graph]") {
    auto data = Simple();
    SECTION("invalid graph") {
        data.id = {};
    }
    SECTION("invalid skeleton") {
        data.skeleton = {};
    }
    SECTION("missing entry") {
        data.entry = Id<GraphDefinitionId>(9);
    }
    SECTION("duplicate definition") {
        data.definitions.push_back(data.definitions.front());
    }
    SECTION("duplicate node") {
        data.definitions.front().nodes.push_back(Clip(4));
    }
    SECTION("duplicate pin") {
        data.definitions.front().nodes.front().pins.push_back(data.definitions.front().nodes.front().pins.front());
    }
    SECTION("missing output") {
        data.definitions.front().nodes.pop_back();
    }
    SECTION("unknown source") {
        data.definitions.front().connections.front().source.node = Id<GraphNodeId>(99);
    }
    SECTION("unknown pin") {
        data.definitions.front().connections.front().destination.pin = Id<GraphPinId>(99);
    }
    SECTION("missing edge") {
        data.definitions.front().connections.clear();
    }
    SECTION("duplicate destination") {
        data.definitions.front().connections.push_back(data.definitions.front().connections.front());
    }
    SECTION("source is input") {
        data.definitions.front().connections.front().source = data.definitions.front().connections.front().destination;
    }
    SECTION("unreachable node") {
        data.definitions.front().nodes.push_back(Clip(6));
    }
    SECTION("wrong semantic role") {
        data.definitions.front().nodes.front().pins.front().role = GraphPinRole::Weight;
    }
    SECTION("unexpected interface id") {
        data.definitions.front().nodes.front().pins.front().interfaceId = Id<GraphInterfaceId>(1);
    }
    Fails(data, AnimationErrors::GraphMalformed);
}

TEST_CASE("Graph typed connections reject implicit coercions and invalid enums", "[animation][graph]") {
    auto data = Simple();
    SECTION("wrong edge") {
        data.definitions.front().connections.front().type = GraphValueType::Float;
    }
    SECTION("untyped current edge") {
        data.definitions.front().connections.front().type = GraphValueType::Unspecified;
    }
    SECTION("wrong pin") {
        data.definitions.front().nodes.front().pins.front().type = GraphValueType::Float;
    }
    SECTION("unknown type") {
        data.definitions.front().nodes.front().pins.front().type = static_cast<GraphValueType>(255);
    }
    Fails(data, AnimationErrors::GraphTypeMismatch);
}

TEST_CASE("Graph parameters have stable ids and finite exactly typed defaults", "[animation][graph]") {
    auto data = Blended();
    SECTION("duplicate id") {
        data.parameters.push_back(data.parameters.front());
        Fails(data, AnimationErrors::GraphMalformed);
        return;
    }
    SECTION("unknown reference") {
        std::get<GraphParameterNode>(data.definitions.front().nodes[2].payload).parameter = Id<GraphParameterId>(4);
        Fails(data, AnimationErrors::GraphMalformed);
        return;
    }
    SECTION("duplicate name") {
        auto other = data.parameters.front();
        other.id = Id<GraphParameterId>(2);
        data.parameters.push_back(other);
    }
    SECTION("nonfinite") {
        data.parameters.front().defaultValue = std::numeric_limits<float>::infinity();
    }
    SECTION("nan") {
        data.parameters.front().defaultValue = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("wrong variant") {
        data.parameters.front().defaultValue = true;
    }
    SECTION("pose parameter") {
        data.parameters.front().type = GraphValueType::Pose;
    }
    SECTION("invalid name") {
        data.parameters.front().name = "speed dot";
    }
    SECTION("numeric prefix") {
        data.parameters.front().name = "0speed";
    }
    SECTION("trigger starts set") {
        data.parameters.front().type = GraphValueType::Trigger;
        data.parameters.front().defaultValue = true;
    }
    Fails(data, AnimationErrors::GraphTypeMismatch);
}

TEST_CASE("Graph unused scalar declarations retain their exact types", "[animation][graph]") {
    auto data = Simple();
    data.parameters = {{Id<GraphParameterId>(1), "enabled", GraphValueType::Boolean, true},
                       {Id<GraphParameterId>(2), "mode", GraphValueType::Integer, std::int32_t{3}},
                       {Id<GraphParameterId>(3), "fire", GraphValueType::Trigger, false}};
    const auto program = CompileGraph(data);
    REQUIRE(program.HasValue());
    REQUIRE(program.Value().Parameters().size() == 3);
    CHECK(std::get<std::int32_t>(program.Value().Parameters()[1].defaultValue) == 3);
}

TEST_CASE("Graph cycle diagnostics are independent of vector ordering", "[animation][graph]") {
    auto data = Blended();
    auto &definition = data.definitions.front();
    definition.nodes[1] = Blend(5);
    definition.connections = {Edge(4, 1, 6, 1), Edge(5, 4, 6, 2), Edge(3, 1, 6, 3, GraphValueType::Float), Edge(6, 4, 2, 1),
                              Edge(6, 4, 5, 1), Edge(4, 1, 5, 2), Edge(3, 1, 5, 3, GraphValueType::Float)};
    const auto first = CompileGraph(data);
    REQUIRE(first.HasError());
    CHECK(first.ErrorValue().code.Value() == AnimationErrors::GraphCycle.code.Value());
    std::reverse(definition.nodes.begin(), definition.nodes.end());
    std::reverse(definition.connections.begin(), definition.connections.end());
    const auto second = CompileGraph(data);
    REQUIRE(second.HasError());
    CHECK(second.ErrorValue().code.Value() == first.ErrorValue().code.Value());
    CHECK(second.ErrorValue().diagnostics.front().path == first.ErrorValue().diagnostics.front().path);
}

TEST_CASE("Graph subgraph recursion missing references and interface drift fail", "[animation][graph]") {
    auto data = Subgraphs();
    SECTION("recursion") {
        data.definitions.front().nodes = {Call(5, 1), Output(2)};
        data.definitions.front().connections = {Edge(5, 1, 2, 1)};
        Fails(data, AnimationErrors::GraphCycle);
    }
    SECTION("unknown call") {
        std::get<GraphCallNode>(data.definitions.front().nodes[1].payload).definition = Id<GraphDefinitionId>(99);
        Fails(data, AnimationErrors::GraphMalformed);
    }
    SECTION("wrong interface type") {
        data.definitions.back().inputs.front().type = GraphValueType::Float;
        Fails(data, AnimationErrors::GraphTypeMismatch);
    }
    SECTION("missing interface") {
        data.definitions.back().inputs.front().id = Id<GraphInterfaceId>(99);
        Fails(data, AnimationErrors::GraphMalformed);
    }
    SECTION("root requires input") {
        data.entry = Id<GraphDefinitionId>(2);
        Fails(data, AnimationErrors::GraphMalformed);
    }
}

TEST_CASE("Graph policies preflight counts names and call depth", "[animation][graph]") {
    auto data = Simple();
    AnimationGraphCompileContext context;
    SECTION("exact limits") {
        context.limits.nodes = 2;
        context.limits.pins = 2;
        context.limits.connections = 1;
        CHECK(CompileGraph(data, context).HasValue());
        context.limits.nodes = 1;
    }
    SECTION("hard policy overflow") {
        context.limits.nodes = 4097;
    }
    SECTION("instruction budget") {
        context.limits.evaluationInstructions = 1;
    }
    SECTION("zero depth") {
        context.limits.callDepth = 0;
    }
    SECTION("zero definition policy") {
        context.limits.definitions = 0;
    }
    SECTION("too many edges") {
        context.limits.connections = 0;
    }
    SECTION("too many pins") {
        context.limits.pins = 1;
    }
    SECTION("call depth") {
        data = Subgraphs();
        context.limits.callDepth = 1;
    }
    SECTION("long name") {
        data.parameters.push_back({Id<GraphParameterId>(1), std::string(65, 'a'), GraphValueType::Float, 0.0F});
        Fails(data, AnimationErrors::GraphMalformed);
        return;
    }
    Fails(data, AnimationErrors::GraphLimitExceeded, context);
}

TEST_CASE("Graph repeated calls are budgeted without exponential program expansion", "[animation][graph]") {
    auto data = Simple();
    data.parameters.push_back({Id<GraphParameterId>(1), "weight", GraphValueType::Float, 0.5F});
    data.entry = Id<GraphDefinitionId>(16);
    for (std::uint64_t definition = 2; definition <= 16; ++definition) {
        data.definitions.push_back({Id<GraphDefinitionId>(definition),
                                    {},
                                    {Call(1, definition - 1), Call(2, definition - 1), Parameter(3), Blend(4), Output(5)},
                                    {Edge(1, 1, 4, 1), Edge(2, 1, 4, 2), Edge(3, 1, 4, 3, GraphValueType::Float), Edge(4, 4, 5, 1)}});
    }
    Fails(data, AnimationErrors::GraphLimitExceeded);
    data.definitions.pop_back();
    data.entry = Id<GraphDefinitionId>(15);
    // Fifteen levels also exceed the cap; smaller bounded graphs remain compact.
    Fails(data, AnimationErrors::GraphLimitExceeded);
    data.definitions.resize(10);
    data.entry = Id<GraphDefinitionId>(10);
    const auto compiled = CompileGraph(data);
    REQUIRE(compiled.HasValue());
    CHECK(compiled.Value().Definitions().size() == 10);
    CHECK(compiled.Value().Definitions().back().instructions.size() == 5);
    CHECK(compiled.Value().Definitions().back().evaluationInstructions == 3579);
}

TEST_CASE("Graph explicit migration infers edge types atomically and preserves identity", "[animation][graph]") {
    auto data = Blended();
    const auto current = CompileGraph(data);
    REQUIRE(current.HasValue());
    data.schemaVersion = 1;
    for (auto &edge : data.definitions.front().connections)
        edge.type = GraphValueType::Unspecified;
    const auto original = data;
    Fails(data, AnimationErrors::GraphVersionUnsupported);
    const auto migrated = MigrateAnimationGraph(data);
    REQUIRE(migrated.HasValue());
    CHECK(migrated.Value().schemaVersion == 2);
    CHECK(migrated.Value().id == original.id);
    CHECK(data == original);
    const auto compiled = CompileGraph(migrated.Value());
    REQUIRE(compiled.HasValue());
    CHECK(compiled.Value() == current.Value());
    const auto repeated = MigrateAnimationGraph(migrated.Value());
    REQUIRE(repeated.HasValue());
    CHECK(repeated.Value() == migrated.Value());
    data.definitions.front().connections.front().source.pin = Id<GraphPinId>(99);
    CHECK(MigrateAnimationGraph(data).HasError());
    data = original;
    data.schemaVersion = 99;
    const auto future = MigrateAnimationGraph(data);
    REQUIRE(future.HasError());
    CHECK(future.ErrorValue().code.Value() == AnimationErrors::GraphVersionUnsupported.code.Value());
}

TEST_CASE("Graph cancellation shutdown and failed reload leave the last good program untouched", "[animation][graph]") {
    auto data = Simple();
    const auto good = CompileGraph(data);
    REQUIRE(good.HasValue());
    const auto saved = good.Value();
    AnimationGraphCompileContext context;
    context.replacing = data.id;
    CHECK(CompileGraph(data, context).HasValue());
    context.replacing = Asset<AnimationGraphId>(9);
    Fails(data, AnimationErrors::GraphReloadMismatch, context);
    context.replacing = data.id;
    CancellationSource source;
    context.cancellation = source.Token();
    source.RequestCancellation();
    Fails(data, AnimationErrors::GraphOperationCancelled, context);
    CHECK(MigrateAnimationGraph(data, context).HasError());
    context.accepting = false;
    Fails(data, AnimationErrors::GraphAdmissionRejected, context);
    data.definitions.front().connections.clear();
    CHECK(good.Value() == saved);
    CHECK(good.Value().Definitions().front().instructions.size() == 2);
    const auto before = HoroAnimationTestAllocationCount();
    for (int iteration = 0; iteration < 10; ++iteration) {
        const auto view = good.Value().Definitions();
        const auto parameters = good.Value().Parameters();
        const auto occurrences = good.Value().Occurrences();
        const auto &memory = good.Value().Memory();
        static_cast<void>(occurrences.front().output);
        static_cast<void>(memory.poseTransforms);
        static_cast<void>(good.Value().OutputSlot());
        static_cast<void>(view.front().instructions.front().source);
        static_cast<void>(parameters);
    }
    CHECK(HoroAnimationTestAllocationCount() == before);
}

TEST_CASE("Graph indirect and unused subgraph call cycles are rejected", "[animation][graph]") {
    auto data = Simple();
    data.definitions.push_back({Id<GraphDefinitionId>(2), {}, {Call(4, 3), Output(2)}, {Edge(4, 1, 2, 1)}});
    data.definitions.push_back({Id<GraphDefinitionId>(3), {}, {Call(4, 2), Output(2)}, {Edge(4, 1, 2, 1)}});
    SECTION("entry traverses cycle") {
        data.definitions.front().nodes.front() = Call(4, 2);
    }
    SECTION("unused definitions contain cycle") {}
    Fails(data, AnimationErrors::GraphCycle);
}

TEST_CASE("Graph semantic contract skew requires an explicit unavailable migration", "[animation][graph]") {
    auto data = Simple();
    data.contractVersion.major = 2;
    Fails(data, AnimationErrors::GraphVersionUnsupported);
    const auto migrated = MigrateAnimationGraph(data);
    REQUIRE(migrated.HasError());
    CHECK(migrated.ErrorValue().code.Value() == AnimationErrors::GraphVersionUnsupported.code.Value());
}
