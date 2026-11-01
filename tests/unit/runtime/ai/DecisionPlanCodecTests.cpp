#include "DecisionGraphCompilerTestSupport.h"

#include <algorithm>

namespace Horo::AI {
    namespace {
        using namespace DecisionGraphTestSupport;

        TEST_CASE("Decision source and cooked codecs canonicalize actual graph order and reproduce executable identity",
                  "[ai][decision-codec]") {
            Fixture fixture;
            std::array sources{CallingMachine(1, 2, DecisionPlanKind::BehaviorTree), TaskTree(2)};
            auto first = DecisionPlanCodec::EncodeSource(sources, Graph(1));
            REQUIRE(first.HasValue());
            std::ranges::reverse(sources);
            auto second = DecisionPlanCodec::EncodeSource(sources, Graph(1));
            REQUIRE(second.HasValue());
            CHECK(first.Value() == second.Value());
            DecisionGraphAssetId root;
            auto decoded = DecisionPlanCodec::DecodeSource(first.Value(), root);
            REQUIRE(decoded.HasValue());
            CHECK(root == Graph(1));
            auto plan = fixture.Compile(decoded.Value(), 1);
            auto cooked = DecisionPlanCodec::EncodeCooked(*plan);
            REQUIRE(cooked.HasValue());
            CHECK(plan->Digest() == ComputeSha256(std::as_bytes(std::span{cooked.Value()})));
            auto loaded = DecisionPlanCodec::DecodeCooked(cooked.Value(), fixture.Environment());
            REQUIRE(loaded.HasValue());
            REQUIRE(loaded.Value().plan);
            CHECK(loaded.Value().plan->Digest() == plan->Digest());
            CHECK(loaded.Value().plan->Storage().coreInstanceBytes == plan->Storage().coreInstanceBytes);
            auto instance = fixture.Instance(loaded.Value().plan);
            CHECK(fixture.Step(*instance, 1) == AiTaskState::Running);
            CHECK(fixture.Step(*instance, 2) == AiTaskState::Succeeded);
        }

        TEST_CASE("Unknown authored nodes and opaque values remain inspectable but cannot become an executable plan",
                  "[ai][decision-codec]") {
            Fixture fixture;
            auto unknown = TreeNode(2, DecisionControlKind::Task);
            unknown.kind = BehaviorTreeNodeKind::Unknown;
            unknown.control.kind = DecisionControlKind::Unspecified;
            unknown.type = MakeIdentity<BehaviorTreeNodeTypeId>(777);
            unknown.task = {};
            unknown.properties.push_back({MakeIdentity<BehaviorTreePropertyId>(9), "unknown", BehaviorTreePropertyKind::Opaque,
                                          BehaviorTreeOpaqueProperty{1, {1, 2, 3}}});
            const auto graph = Tree(1, {TreeNode(1, DecisionControlKind::Root), unknown}, {Edge(1, 1, 2)});
            auto encoded = DecisionPlanCodec::EncodeSource(std::span{&graph, 1}, Graph(1));
            REQUIRE(encoded.HasValue());
            DecisionGraphAssetId root;
            auto decoded = DecisionPlanCodec::DecodeSource(encoded.Value(), root);
            REQUIRE(decoded.HasValue());
            const auto &inspected = *std::get<std::shared_ptr<const BehaviorTreeAsset>>(decoded.Value()[0].body);
            CHECK(inspected.UnknownNodeCount() == 1);
            CHECK(inspected.UnknownPropertyCount() == 1);
            CHECK(inspected.Nodes().size() == 2);
            auto candidate = DecisionGraphCompiler::Compile(root, decoded.Value(), fixture.Environment());
            REQUIRE(candidate.HasValue());
            CHECK_FALSE(candidate.Value().plan);
            CHECK(candidate.Value().validation.HasErrors());
            CHECK(fixture.probe.factories == 0);
            auto preserved = DecisionPlanCodec::EncodeSource(decoded.Value(), root);
            REQUIRE(preserved.HasValue());
            CHECK(preserved.Value() == encoded.Value());
        }

        TEST_CASE("Decision source transport rejects malformed typed property values without altering authored input",
                  "[ai][decision-codec]") {
            Fixture fixture;
            auto source = TerminalMachine(1);
            auto &state = std::get<StateMachineAssetDescriptor>(source.body).states[0];
            state.entry = MakeIdentity<TaskId>(6001);
            state.entryProvider = StateMachineProviderAction{{.id = Node(10), .type = Type(501), .source = Location()},
                                                             {{MakeIdentity<BehaviorTreePropertyId>(4), "invalid",
                                                               BehaviorTreePropertyKind::Boolean, std::int64_t{1}}}};
            auto encoded = DecisionPlanCodec::EncodeSource(std::span{&source, 1}, Graph(1));
            CHECK(encoded.HasError());
            CHECK(std::get<std::int64_t>(state.entryProvider->properties[0].value) == 1);
            CHECK(fixture.probe.factories == 0);
        }

        TEST_CASE("Every truncated decision source or cooked prefix rejects without returning partial authority", "[ai][decision-codec]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            auto source = DecisionPlanCodec::EncodeSource(sources, Graph(1));
            REQUIRE(source.HasValue());
            auto plan = fixture.Compile(sources, 1);
            auto cooked = DecisionPlanCodec::EncodeCooked(*plan);
            REQUIRE(cooked.HasValue());
            for (std::size_t count = 0; count < source.Value().size(); ++count) {
                auto root = Graph(999);
                auto result = DecisionPlanCodec::DecodeSource(std::span{source.Value()}.first(count), root);
                CHECK(result.HasError());
                CHECK(root == Graph(999));
            }
            for (std::size_t count = 0; count < cooked.Value().size(); ++count)
                CHECK(DecisionPlanCodec::DecodeCooked(std::span{cooked.Value()}.first(count), fixture.Environment()).HasError());
            CHECK(fixture.probe.factories == 0);
        }

        TEST_CASE("Decision cooked admission rejects forged layout even after its transport digest was recomputed",
                  "[ai][decision-codec]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            auto plan = fixture.Compile(sources, 1);
            auto encoded = DecisionPlanCodec::EncodeCooked(*plan);
            REQUIRE(encoded.HasValue());
            auto forged = encoded.Value();
            REQUIRE(forged.size() > 48);
            forged.back() ^= 1;  // The final canonical layout field is the task-slot declaration.
            const auto digest = ComputeSha256(std::as_bytes(std::span{forged}.subspan(48)));
            std::ranges::copy(digest.bytes, forged.begin() + 12);
            CHECK(DecisionPlanCodec::DecodeCooked(forged, fixture.Environment()).HasError());
            CHECK(fixture.probe.factories == 0);
        }

        TEST_CASE("Decision codec rejects future versions trailing bytes and mismatched actual runtime environment",
                  "[ai][decision-codec]") {
            Fixture fixture;
            const std::array sources{TaskTree(1)};
            auto source = DecisionPlanCodec::EncodeSource(sources, Graph(1));
            REQUIRE(source.HasValue());
            auto plan = fixture.Compile(sources, 1);
            auto cooked = DecisionPlanCodec::EncodeCooked(*plan);
            REQUIRE(cooked.HasValue());
            SECTION("future source version") {
                auto future = source.Value();
                future[8] = 2;
                auto root = Graph(999);
                CHECK(DecisionPlanCodec::DecodeSource(future, root).HasError());
                CHECK(root == Graph(999));
            }
            SECTION("future cooked version") {
                auto future = cooked.Value();
                future[8] = 2;
                CHECK(DecisionPlanCodec::DecodeCooked(future, fixture.Environment()).HasError());
            }
            SECTION("trailing bytes") {
                auto trailing = cooked.Value();
                trailing.push_back(0);
                CHECK(DecisionPlanCodec::DecodeCooked(trailing, fixture.Environment()).HasError());
            }
            SECTION("incompatible runtime layout") {
                auto environment = fixture.Environment();
                ++environment.runtimeLayoutVersion;
                CHECK(DecisionPlanCodec::DecodeCooked(cooked.Value(), environment).HasError());
            }
            SECTION("no provider authority recovered from bytes") {
                auto environment = fixture.Environment();
                environment.nodes.reset();
                CHECK(DecisionPlanCodec::DecodeCooked(cooked.Value(), environment).HasError());
            }
        }
    }  // namespace
}  // namespace Horo::AI
