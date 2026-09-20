#include "AiTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/BehaviorTree.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        [[nodiscard]] BehaviorTreePin Pin(const std::uint64_t id, const BehaviorTreePinDirection direction,
                                          const BehaviorTreePinCardinality cardinality = BehaviorTreePinCardinality::Single) {
            return {MakeIdentity<BehaviorTreePinId>(id), direction, cardinality};
        }

        [[nodiscard]] BehaviorTreeNode Node(const std::uint64_t id, const BehaviorTreeNodeKind kind, const std::uint64_t type,
                                            const std::size_t minimumChildren, const std::size_t maximumChildren,
                                            std::vector<BehaviorTreePin> pins) {
            return {
                .id = MakeIdentity<BehaviorTreeNodeId>(id),
                .kind = kind,
                .type = MakeIdentity<BehaviorTreeNodeTypeId>(type),
                .typeVersion = {1, 0},
                .displayName = "node-" + std::to_string(id),
                .minimumChildren = minimumChildren,
                .maximumChildren = maximumChildren,
                .pins = std::move(pins),
            };
        }

        [[nodiscard]] BehaviorTreeEdge Edge(const std::uint64_t id, const BehaviorTreeEdgeKind kind, const std::uint64_t sourceNode,
                                            const std::uint64_t sourcePin, const std::uint64_t targetNode, const std::uint64_t targetPin,
                                            const std::size_t order) {
            return {
                .id = MakeIdentity<BehaviorTreeEdgeId>(id),
                .kind = kind,
                .sourceNode = MakeIdentity<BehaviorTreeNodeId>(sourceNode),
                .sourcePin = MakeIdentity<BehaviorTreePinId>(sourcePin),
                .targetNode = MakeIdentity<BehaviorTreeNodeId>(targetNode),
                .targetPin = MakeIdentity<BehaviorTreePinId>(targetPin),
                .order = order,
            };
        }

        [[nodiscard]] BehaviorTreeAssetDescriptor ValidAsset() {
            auto root = Node(1, BehaviorTreeNodeKind::Root, 101, 1, 1,
                             {Pin(1, BehaviorTreePinDirection::Output, BehaviorTreePinCardinality::Multiple)});
            auto composite = Node(2, BehaviorTreeNodeKind::Composite, 102, 1, 4,
                                  {Pin(2, BehaviorTreePinDirection::Input), Pin(3, BehaviorTreePinDirection::Output),
                                   Pin(6, BehaviorTreePinDirection::Output)});
            composite.nestedSubtree = BehaviorTreeSubtreeReference{MakeIdentity<BehaviorTreeGraphId>(11), {1, 0}};
            composite.properties.push_back(
                {MakeIdentity<BehaviorTreePropertyId>(20), "priority", BehaviorTreePropertyKind::SignedInteger, std::int64_t{3}});
            auto task = Node(3, BehaviorTreeNodeKind::Task, 103, 0, 0, {Pin(4, BehaviorTreePinDirection::Input)});
            auto service = Node(4, BehaviorTreeNodeKind::Service, 104, 0, 0, {Pin(5, BehaviorTreePinDirection::Input)});
            BehaviorTreeOpaqueProperty opaque;
            opaque.bytes.push_back(0xab);
            task.properties.push_back({MakeIdentity<BehaviorTreePropertyId>(21), "future", BehaviorTreePropertyKind::Opaque, opaque});

            return {
                .version = CurrentBehaviorTreeSchemaVersion,
                .graph = MakeIdentity<BehaviorTreeGraphId>(10),
                .displayName = "guard",
                .root = MakeIdentity<BehaviorTreeNodeId>(1),
                .nodes = {std::move(task), std::move(service), std::move(composite), std::move(root)},
                .edges =
                    {
                        Edge(32, BehaviorTreeEdgeKind::ServiceAttachment, 2, 6, 4, 5, 0),
                        Edge(31, BehaviorTreeEdgeKind::Child, 2, 3, 3, 4, 0),
                        Edge(30, BehaviorTreeEdgeKind::Child, 1, 1, 2, 2, 0),
                    },
            };
        }

        TEST_CASE("Behavior-tree capture canonicalizes identity and preserves unknown properties", "[unit][ai][behavior_tree]") {
            auto candidate = ValidAsset();
            const auto captured = BehaviorTreeAsset::Capture(std::move(candidate));
            REQUIRE(captured.HasValue());
            CHECK(captured.Value().Graph().Value() == 10);
            CHECK(captured.Value().Root().Value() == 1);
            REQUIRE(captured.Value().Nodes().size() == 4);
            CHECK(captured.Value().Nodes()[0].id.Value() == 1);
            CHECK(captured.Value().Nodes()[3].id.Value() == 4);
            REQUIRE(captured.Value().Edges().size() == 3);
            CHECK(captured.Value().Edges()[0].sourceNode.Value() == 1);
            CHECK(captured.Value().Edges()[1].sourceNode.Value() == 2);
            CHECK(captured.Value().Edges()[1].kind == BehaviorTreeEdgeKind::Child);
            CHECK(captured.Value().Edges()[2].kind == BehaviorTreeEdgeKind::ServiceAttachment);
            CHECK(captured.Value().UnknownNodeCount() == 0);
            CHECK(captured.Value().UnknownPropertyCount() == 1);
            CHECK(captured.Value().FindNode(MakeIdentity<BehaviorTreeNodeId>(3))->displayName == "node-3");
        }

        TEST_CASE("Behavior-tree identity is independent of display name and layout order", "[unit][ai][behavior_tree]") {
            auto candidate = ValidAsset();
            candidate.displayName = "renamed-asset";
            candidate.nodes[0].displayName = "renamed-node";
            const auto captured = BehaviorTreeAsset::Capture(std::move(candidate));
            REQUIRE(captured.HasValue());
            CHECK(captured.Value().FindNode(MakeIdentity<BehaviorTreeNodeId>(3))->id.Value() == 3);
            CHECK(captured.Value().FindNode(MakeIdentity<BehaviorTreeNodeId>(3))->displayName == "renamed-node");
            CHECK(captured.Value().Edges()[1].targetNode.Value() == 3);
        }

        TEST_CASE("Behavior-tree capture rejects duplicate identities and invalid topology", "[unit][ai][behavior_tree]") {
            auto duplicate = ValidAsset();
            duplicate.nodes.push_back(Node(3, BehaviorTreeNodeKind::Task, 105, 0, 0, {Pin(7, BehaviorTreePinDirection::Input)}));
            ExpectError(BehaviorTreeAsset::Capture(std::move(duplicate)), AIErrors::BehaviorTreeIdentityConflict);

            auto invalid = ValidAsset();
            invalid.edges[0].targetPin = MakeIdentity<BehaviorTreePinId>(3);
            ExpectError(BehaviorTreeAsset::Capture(std::move(invalid)), AIErrors::BehaviorTreeTopologyInvalid);

            auto badVersion = ValidAsset();
            badVersion.version = {2, 0};
            ExpectError(BehaviorTreeAsset::Capture(std::move(badVersion)), AIErrors::BehaviorTreeSchemaInvalid);
        }

        TEST_CASE("Behavior-tree capture enforces project-lowerable bounds", "[unit][ai][behavior_tree]") {
            auto candidate = ValidAsset();
            BehaviorTreeAssetLimits limits;
            limits.maximumPins = 2;
            ExpectError(BehaviorTreeAsset::Capture(std::move(candidate), limits), AIErrors::BehaviorTreeLimitExceeded);

            auto invalidReference = ValidAsset();
            invalidReference.nodes[2].properties.push_back({MakeIdentity<BehaviorTreePropertyId>(22), "target",
                                                            BehaviorTreePropertyKind::NodeReference,
                                                            MakeIdentity<BehaviorTreeNodeId>(999)});
            ExpectError(BehaviorTreeAsset::Capture(std::move(invalidReference)), AIErrors::BehaviorTreeTopologyInvalid);
        }

        TEST_CASE("Behavior-tree capture rejects disconnected cycles", "[unit][ai][behavior_tree]") {
            auto candidate = ValidAsset();
            candidate.nodes.push_back(Node(5, BehaviorTreeNodeKind::Unknown, 105, 0, 1,
                                           {Pin(7, BehaviorTreePinDirection::Input), Pin(8, BehaviorTreePinDirection::Output)}));
            candidate.nodes.push_back(Node(6, BehaviorTreeNodeKind::Unknown, 106, 0, 1,
                                           {Pin(9, BehaviorTreePinDirection::Input), Pin(10, BehaviorTreePinDirection::Output)}));
            candidate.edges.push_back(Edge(33, BehaviorTreeEdgeKind::Child, 5, 8, 6, 9, 0));
            candidate.edges.push_back(Edge(34, BehaviorTreeEdgeKind::Child, 6, 10, 5, 7, 0));
            ExpectError(BehaviorTreeAsset::Capture(std::move(candidate)), AIErrors::BehaviorTreeCycle);
        }

        TEST_CASE("Behavior-tree unknown nodes are retained for degraded inspection", "[unit][ai][behavior_tree]") {
            auto candidate = ValidAsset();
            candidate.nodes[2].kind = BehaviorTreeNodeKind::Unknown;
            const auto captured = BehaviorTreeAsset::Capture(std::move(candidate));
            REQUIRE(captured.HasValue());
            CHECK(captured.Value().UnknownNodeCount() == 1);
        }
    }  // namespace
}  // namespace Horo::AI
