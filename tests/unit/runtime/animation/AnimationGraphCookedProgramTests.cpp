#include "AnimationActionTestFixtures.h"
#include "AnimationGraphInstanceTestFixtures.h"
#include "AnimationStateGraphTestFixtures.h"

#include <limits>

namespace Horo::Animation {
    namespace {
        /** @brief Copies only deterministic compiled declarations from an actual compiler result. */
        AnimationGraphCookedData Cooked(const AnimationGraphProgram &program) {
            AnimationGraphCookedData data;
            data.id = program.Id();
            data.skeleton = program.Skeleton();
            data.entry = program.Definitions()[program.EntryIndex()].id;
            data.parameters.assign(program.Parameters().begin(), program.Parameters().end());
            data.actionSlots.assign(program.ActionSlots().begin(), program.ActionSlots().end());
            data.actions.assign(program.Actions().begin(), program.Actions().end());
            data.definitions.assign(program.Definitions().begin(), program.Definitions().end());
            return data;
        }

        /** @brief Supplies real dependencies and host-issued generations independently of the payload. */
        AnimationGraphCompileContext Context(const Test::Instance::Prepared &fixture) {
            AnimationGraphCompileContext context;
            context.skeleton = fixture.skeleton.get();
            context.skeletonGeneration = Test::Id<SkeletonAssetGeneration>(5);
            context.clips = fixture.compileClips;
            return context;
        }
    }  // namespace

    TEST_CASE("Cooked graph declarations reconstruct the actual immutable compiler program", "[animation][graph][cooked]") {
        for (auto graph : {Test::Graph::Simple(), Test::Graph::Blended(), Test::Graph::Subgraphs(), Test::Graph::RepeatedCalls(),
                           Test::StateGraph::Data()}) {
            Test::Instance::Prepared fixture(std::move(graph));
            const auto cooked = Cooked(*fixture.program);
            const auto loaded = AnimationGraphProgram::FromCooked(cooked, Context(fixture));
            REQUIRE(loaded.HasValue());
            CHECK(loaded.Value() == *fixture.program);
        }
    }

    TEST_CASE("Cooked graph retains real action catalog bindings and bounded runtime execution", "[animation][graph][cooked]") {
        Test::Actions::Prepared fixture;
        const auto data = Cooked(*fixture.program);
        auto loaded = AnimationGraphProgram::FromCooked(data, fixture.CompileContext());
        REQUIRE(loaded.HasValue());
        CHECK(loaded.Value() == *fixture.program);
        auto assets = fixture.Assets();
        assets.program = std::make_shared<const AnimationGraphProgram>(std::move(loaded).Value());
        auto created = AnimationGraphInstance::Create(assets, fixture.context);
        REQUIRE(created.HasValue());
        auto instance = std::move(created).Value();
        REQUIRE(instance.QueueAction(Test::Actions::Request()).HasValue());
        Test::Actions::Tick tick(instance, 1, 10);
        CHECK(tick.pose[0].translation.x == 130.0F);
    }

    TEST_CASE("Cooked graph rejects forged schedules operand indexes source identities and opcode metadata", "[animation][graph][cooked]") {
        Test::Instance::Prepared fixture;
        auto cooked = Cooked(*fixture.program);
        auto &definition = cooked.definitions.front();
        auto &output = definition.instructions.back();
        REQUIRE_FALSE(output.inputs.empty());
        SECTION("out of range register") {
            output.inputs.front().instruction = std::numeric_limits<std::uint32_t>::max();
        }
        SECTION("future register") {
            output.inputs.front().instruction = static_cast<std::uint32_t>(definition.instructions.size() - 1);
        }
        SECTION("foreign source definition") {
            output.inputs.front().source.definition = Test::Id<GraphDefinitionId>(99);
        }
        SECTION("foreign source node") {
            output.inputs.front().source.node = Test::Id<GraphNodeId>(99);
        }
        SECTION("foreign source pin") {
            output.inputs.front().source.pin = Test::Id<GraphPinId>(99);
        }
        SECTION("instruction source definition") {
            output.source.definition = Test::Id<GraphDefinitionId>(99);
        }
        SECTION("extra instruction source pin") {
            output.source.pin = Test::Id<GraphPinId>(99);
        }
        SECTION("wrong operand type") {
            output.inputs.front().type = GraphValueType::Float;
        }
        SECTION("missing operand") {
            output.inputs.clear();
        }
        SECTION("wrong final output") {
            definition.outputInstruction = 0;
        }
        SECTION("reordered schedule") {
            std::swap(definition.instructions.front(), definition.instructions.back());
        }
        SECTION("unused forged parameter index") {
            output.parameterIndex = 0;
        }
        SECTION("unused forged call index") {
            output.definitionIndex = std::numeric_limits<std::uint32_t>::max();
        }
        SECTION("unused forged state call") {
            output.stateDefinitions.push_back(std::numeric_limits<std::uint32_t>::max());
        }
        SECTION("unused forged blend index") {
            output.blendParameters[0] = 1;
        }
        CHECK(AnimationGraphProgram::FromCooked(cooked, Context(fixture)).HasError());
    }

    TEST_CASE("Cooked graph verifies expansion costs instead of trusting allocation declarations", "[animation][graph][cooked]") {
        Test::Instance::Prepared fixture(Test::Graph::Subgraphs());
        auto cooked = Cooked(*fixture.program);
        SECTION("call depth") {
            ++cooked.definitions.front().callDepth;
        }
        SECTION("instruction work") {
            ++cooked.definitions.front().evaluationInstructions;
        }
        SECTION("source map work") {
            ++cooked.definitions.front().sourceMapCallEntries;
        }
        CHECK(AnimationGraphProgram::FromCooked(cooked, Context(fixture)).HasError());
    }

    TEST_CASE("Cooked graph declaration limits and cancellation fail before publication", "[animation][graph][cooked]") {
        Test::Instance::Prepared fixture;
        auto cooked = Cooked(*fixture.program);
        auto context = Context(fixture);
        CancellationSource cancellation;
        SECTION("unknown semantic version") {
            ++cooked.contractVersion.minor;
        }
        SECTION("unknown entry") {
            cooked.entry = Test::Id<GraphDefinitionId>(99);
        }
        SECTION("zero node policy") {
            context.limits.nodes = 0;
        }
        SECTION("node budget") {
            context.limits.nodes = 1;
        }
        SECTION("operand budget") {
            context.limits.connections = 0;
        }
        SECTION("cancelled") {
            cancellation.RequestCancellation();
            context.cancellation = cancellation.Token();
        }
        SECTION("closed admission") {
            context.accepting = false;
        }
        SECTION("replacement identity") {
            context.replacing = Test::Asset<AnimationGraphId>(99);
        }
        CHECK(AnimationGraphProgram::FromCooked(cooked, context).HasError());
    }

    TEST_CASE("Cooked graph binds current real dependency generations without retaining original runtime identity",
              "[animation][graph][cooked]") {
        Test::Instance::Prepared original;
        const auto cooked = Cooked(*original.program);
        Test::Instance::Prepared current(Test::Graph::Simple(), 7);
        auto loaded = AnimationGraphProgram::FromCooked(cooked, Context(current));
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().ClipBindings().size() == 1);
        CHECK(loaded.Value().ClipBindings().front().generation == Test::Id<AnimationClipGeneration>(7));
        auto assets = current.Assets();
        assets.program = std::make_shared<const AnimationGraphProgram>(std::move(loaded).Value());
        auto created = AnimationGraphInstance::Create(assets, current.context);
        REQUIRE(created.HasValue());
        auto instance = std::move(created).Value();
        const auto tick = Test::Id<AnimationTickId>(1);
        REQUIRE(instance.Evaluate({tick, {10}}).HasValue());
        const auto published = instance.Commit(tick);
        REQUIRE(published.HasValue());
        std::array<Math::Transform, 1> pose;
        REQUIRE(instance.CopyPose(published.publication.currentPose, pose).HasValue());
        CHECK(pose[0].translation.x == 10.0F);
    }

    TEST_CASE("Cooked graph requires actual complete dependency snapshots", "[animation][graph][cooked]") {
        Test::Instance::Prepared fixture;
        const auto cooked = Cooked(*fixture.program);
        auto context = Context(fixture);
        SECTION("skeleton missing") {
            context.skeleton = nullptr;
        }
        SECTION("generation missing") {
            context.skeletonGeneration = {};
        }
        SECTION("clips missing") {
            context.clips = {};
        }
        CHECK(AnimationGraphProgram::FromCooked(cooked, context).HasError());
    }
}  // namespace Horo::Animation
