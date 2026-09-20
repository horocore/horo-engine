#include "Horo/AI/BehaviorTree.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <new>
#include <ranges>
#include <tuple>
#include <utility>

namespace Horo::AI {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] constexpr bool IsKnown(const Enum value) noexcept {
            return value < Enum::Count;
        }

        [[nodiscard]] bool ValidName(const std::string_view value, const BehaviorTreeAssetLimits &limits) noexcept {
            return value.size() <= limits.maximumDisplayNameBytes && value.find('\0') == std::string_view::npos;
        }

        [[nodiscard]] bool ValidVersion(const BehaviorTreeSchemaVersion version) noexcept {
            return version.major == CurrentBehaviorTreeSchemaVersion.major && version.minor <= CurrentBehaviorTreeSchemaVersion.minor;
        }

        [[nodiscard]] constexpr bool ValidBound(const std::size_t value, const std::size_t maximum) noexcept {
            return value > 0 && value <= maximum;
        }

        [[nodiscard]] bool ValidLimits(const BehaviorTreeAssetLimits &limits) noexcept {
            return ValidBound(limits.maximumNodes, BehaviorTreeAssetHardLimits::Nodes) &&
                   ValidBound(limits.maximumEdges, BehaviorTreeAssetHardLimits::Edges) &&
                   ValidBound(limits.maximumPins, BehaviorTreeAssetHardLimits::Pins) &&
                   ValidBound(limits.maximumPinsPerNode, BehaviorTreeAssetHardLimits::PinsPerNode) &&
                   ValidBound(limits.maximumPropertiesPerNode, BehaviorTreeAssetHardLimits::PropertiesPerNode) &&
                   ValidBound(limits.maximumPropertyBytes, BehaviorTreeAssetHardLimits::PropertyBytes) &&
                   ValidBound(limits.maximumDisplayNameBytes, BehaviorTreeAssetHardLimits::DisplayNameBytes) &&
                   ValidBound(limits.maximumChildrenPerNode, BehaviorTreeAssetHardLimits::ChildrenPerNode);
        }

        [[nodiscard]] bool ValidPropertyValue(const BehaviorTreeProperty &property, const BehaviorTreeAssetLimits &limits) noexcept {
            using enum BehaviorTreePropertyKind;
            switch (property.kind) {
                case Boolean:
                    return std::holds_alternative<bool>(property.value);
                case SignedInteger:
                    return std::holds_alternative<std::int64_t>(property.value);
                case Scalar:
                    return std::holds_alternative<double>(property.value) && std::isfinite(std::get<double>(property.value));
                case String:
                    return std::holds_alternative<std::string>(property.value) && ValidName(std::get<std::string>(property.value), limits);
                case NodeReference:
                    return std::holds_alternative<BehaviorTreeNodeId>(property.value) &&
                           std::get<BehaviorTreeNodeId>(property.value).IsValid();
                case Opaque: {
                    if (!std::holds_alternative<BehaviorTreeOpaqueProperty>(property.value))
                        return false;
                    const auto &opaque = std::get<BehaviorTreeOpaqueProperty>(property.value);
                    return opaque.contractVersion != 0 && opaque.bytes.size() <= limits.maximumPropertyBytes;
                }
                case Count:
                    return false;
            }
            return false;
        }

        struct PinReference final {
            BehaviorTreePinId pin;
            BehaviorTreeNodeId node;
            BehaviorTreePinDirection direction{};
            BehaviorTreePinCardinality cardinality{};
        };

        [[nodiscard]] const PinReference *FindPin(const std::span<const PinReference> pins, const BehaviorTreePinId id) noexcept {
            const auto found = std::ranges::lower_bound(pins, id, {}, &PinReference::pin);
            return found == pins.end() || found->pin != id ? nullptr : std::to_address(found);
        }

        [[nodiscard]] std::size_t NodeIndex(const std::span<const BehaviorTreeNode> nodes, const BehaviorTreeNodeId id) noexcept {
            const auto found = std::ranges::lower_bound(nodes, id, {}, &BehaviorTreeNode::id);
            return found == nodes.end() || found->id != id ? nodes.size() : static_cast<std::size_t>(std::distance(nodes.begin(), found));
        }

        [[nodiscard]] Result<void> ValidateNodeShape(const BehaviorTreeNode &node, const BehaviorTreeGraphId graph,
                                                     const BehaviorTreeAssetLimits &limits) {
            using enum BehaviorTreeNodeKind;
            if (!node.id.IsValid() || !node.type.IsValid() || node.typeVersion.major == 0 || !IsKnown(node.kind) ||
                !ValidName(node.displayName, limits) || node.minimumChildren > node.maximumChildren ||
                node.maximumChildren > limits.maximumChildrenPerNode ||
                (node.nestedSubtree.has_value() &&
                 (!node.nestedSubtree->graph.IsValid() || !ValidVersion(node.nestedSubtree->requiredSchemaVersion) ||
                  node.nestedSubtree->graph == graph)))
                return Failure<void>(AIErrors::BehaviorTreeSchemaInvalid);
            if (node.kind == Root || node.kind == Decorator) {
                if (node.minimumChildren != 1 || node.maximumChildren != 1)
                    return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
            } else if ((node.kind == Task || node.kind == Service) && (node.minimumChildren != 0 || node.maximumChildren != 0))
                return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNodePins(BehaviorTreeNode &node, const BehaviorTreeAssetLimits &limits,
                                                    std::vector<PinReference> &pins) {
            if (node.pins.size() > limits.maximumPinsPerNode)
                return Failure<void>(AIErrors::BehaviorTreeLimitExceeded);
            std::ranges::sort(node.pins, {}, &BehaviorTreePin::id);
            if (std::ranges::adjacent_find(node.pins, {}, &BehaviorTreePin::id) != node.pins.end())
                return Failure<void>(AIErrors::BehaviorTreeIdentityConflict);
            for (const BehaviorTreePin &pin : node.pins) {
                if (!pin.id.IsValid() || !IsKnown(pin.direction) || !IsKnown(pin.cardinality))
                    return Failure<void>(AIErrors::BehaviorTreeSchemaInvalid);
                pins.emplace_back(pin.id, node.id, pin.direction, pin.cardinality);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNodeProperties(BehaviorTreeNode &node, const std::span<const BehaviorTreeNode> nodes,
                                                          const BehaviorTreeAssetLimits &limits, std::size_t &unknownProperties) {
            if (node.properties.size() > limits.maximumPropertiesPerNode)
                return Failure<void>(AIErrors::BehaviorTreeLimitExceeded);
            std::ranges::sort(node.properties, {}, &BehaviorTreeProperty::id);
            if (std::ranges::adjacent_find(node.properties, {}, &BehaviorTreeProperty::id) != node.properties.end())
                return Failure<void>(AIErrors::BehaviorTreeIdentityConflict);
            for (const BehaviorTreeProperty &property : node.properties) {
                if (!property.id.IsValid() || !ValidName(property.displayName, limits) || !IsKnown(property.kind) ||
                    !ValidPropertyValue(property, limits))
                    return Failure<void>(AIErrors::BehaviorTreeSchemaInvalid);
                if (property.kind == BehaviorTreePropertyKind::NodeReference &&
                    NodeIndex(nodes, std::get<BehaviorTreeNodeId>(property.value)) == nodes.size())
                    return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
                if (property.kind == BehaviorTreePropertyKind::Opaque)
                    ++unknownProperties;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNode(BehaviorTreeNode &node, const std::span<const BehaviorTreeNode> nodes,
                                                const BehaviorTreeGraphId graph, const BehaviorTreeAssetLimits &limits,
                                                std::vector<PinReference> &pins, std::size_t &unknownNodes,
                                                std::size_t &unknownProperties) {
            if (const auto valid = ValidateNodeShape(node, graph, limits); valid.HasError())
                return valid;
            if (node.kind == BehaviorTreeNodeKind::Unknown)
                ++unknownNodes;
            if (const auto valid = ValidateNodePins(node, limits, pins); valid.HasError())
                return valid;
            return ValidateNodeProperties(node, nodes, limits, unknownProperties);
        }

        [[nodiscard]] Result<void> ValidateNodes(BehaviorTreeAssetDescriptor &candidate, const BehaviorTreeAssetLimits &limits,
                                                 std::vector<PinReference> &pins, std::size_t &unknownNodes,
                                                 std::size_t &unknownProperties) {
            if (candidate.nodes.size() > limits.maximumNodes)
                return Failure<void>(AIErrors::BehaviorTreeLimitExceeded);
            std::ranges::sort(candidate.nodes, {}, &BehaviorTreeNode::id);
            if (std::ranges::adjacent_find(candidate.nodes, {}, &BehaviorTreeNode::id) != candidate.nodes.end())
                return Failure<void>(AIErrors::BehaviorTreeIdentityConflict);
            pins.reserve(std::min(limits.maximumPins, candidate.nodes.size() * std::size_t{4}));
            const auto nodes = std::span{candidate.nodes};
            for (BehaviorTreeNode &node : candidate.nodes) {
                if (const auto valid = ValidateNode(node, nodes, candidate.graph, limits, pins, unknownNodes, unknownProperties);
                    valid.HasError())
                    return valid;
            }
            if (pins.size() > limits.maximumPins)
                return Failure<void>(AIErrors::BehaviorTreeLimitExceeded);
            std::ranges::sort(pins, {}, &PinReference::pin);
            if (std::ranges::adjacent_find(pins, {}, &PinReference::pin) != pins.end())
                return Failure<void>(AIErrors::BehaviorTreeIdentityConflict);
            return Result<void>::Success();
        }

        struct OutgoingOrder final {
            BehaviorTreeNodeId source;
            BehaviorTreeEdgeKind kind{};
            std::size_t order{};

            [[nodiscard]] constexpr auto operator<=>(const OutgoingOrder &) const noexcept = default;
        };

        struct Endpoint final {
            BehaviorTreeNodeId sourceNode;
            BehaviorTreePinId sourcePin;
            BehaviorTreeNodeId targetNode;
            BehaviorTreePinId targetPin;

            [[nodiscard]] constexpr auto operator<=>(const Endpoint &) const noexcept = default;
        };

        struct EdgeValidationState final {
            std::vector<std::size_t> childCounts;
            std::vector<std::size_t> serviceCounts;
            std::vector<std::size_t> structuralParents;
            std::vector<BehaviorTreePinId> singleTargetPins;
            std::vector<OutgoingOrder> outgoingOrders;
            std::vector<Endpoint> endpoints;
        };

        [[nodiscard]] Result<void> ValidateEdge(const BehaviorTreeEdge &edge, const std::span<const BehaviorTreeNode> nodes,
                                                const std::span<const PinReference> pins, const BehaviorTreeAssetLimits &limits,
                                                EdgeValidationState &state) {
            if (!edge.id.IsValid() || !IsKnown(edge.kind) || !edge.sourceNode.IsValid() || !edge.sourcePin.IsValid() ||
                !edge.targetNode.IsValid() || !edge.targetPin.IsValid() || edge.order > limits.maximumChildrenPerNode)
                return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
            const std::size_t source = NodeIndex(nodes, edge.sourceNode);
            const std::size_t target = NodeIndex(nodes, edge.targetNode);
            const PinReference *sourcePin = FindPin(pins, edge.sourcePin);
            if (const PinReference *targetPin = FindPin(pins, edge.targetPin);
                source == nodes.size() || target == nodes.size() || sourcePin == nullptr || targetPin == nullptr ||
                sourcePin->node != edge.sourceNode || targetPin->node != edge.targetNode ||
                sourcePin->direction != BehaviorTreePinDirection::Output || targetPin->direction != BehaviorTreePinDirection::Input ||
                targetPin->cardinality != BehaviorTreePinCardinality::Single || source == target)
                return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);

            const BehaviorTreeNodeKind sourceKind = nodes[source].kind;
            const BehaviorTreeNodeKind targetKind = nodes[target].kind;
            if (sourceKind == BehaviorTreeNodeKind::Task || sourceKind == BehaviorTreeNodeKind::Service ||
                targetKind == BehaviorTreeNodeKind::Root)
                return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
            if (edge.kind == BehaviorTreeEdgeKind::Child) {
                if (targetKind == BehaviorTreeNodeKind::Service || state.childCounts[source] >= nodes[source].maximumChildren)
                    return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
                ++state.childCounts[source];
                ++state.structuralParents[target];
            } else if (edge.kind == BehaviorTreeEdgeKind::ServiceAttachment) {
                if (targetKind != BehaviorTreeNodeKind::Service || state.serviceCounts[target] != 0)
                    return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
                ++state.serviceCounts[target];
                ++state.structuralParents[target];
            } else {
                return Failure<void>(AIErrors::BehaviorTreeSchemaInvalid);
            }
            state.singleTargetPins.emplace_back(edge.targetPin);
            state.outgoingOrders.emplace_back(edge.sourceNode, edge.kind, edge.order);
            state.endpoints.emplace_back(edge.sourceNode, edge.sourcePin, edge.targetNode, edge.targetPin);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEdgeUniqueness(EdgeValidationState &state) {
            std::ranges::sort(state.singleTargetPins);
            if (std::ranges::adjacent_find(state.singleTargetPins) != state.singleTargetPins.end())
                return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
            std::ranges::sort(state.outgoingOrders);
            if (std::ranges::adjacent_find(state.outgoingOrders) != state.outgoingOrders.end())
                return Failure<void>(AIErrors::BehaviorTreeTopologyInvalid);
            std::ranges::sort(state.endpoints);
            if (std::ranges::adjacent_find(state.endpoints) != state.endpoints.end())
                return Failure<void>(AIErrors::BehaviorTreeIdentityConflict);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::size_t> ValidateRootAndParents(const BehaviorTreeAssetDescriptor &candidate,
                                                                 const std::span<const BehaviorTreeNode> nodes,
                                                                 const EdgeValidationState &state) {
            const std::size_t root = NodeIndex(nodes, candidate.root);
            if (!candidate.graph.IsValid() || !candidate.root.IsValid() || root == nodes.size() ||
                nodes[root].kind != BehaviorTreeNodeKind::Root || state.structuralParents[root] != 0)
                return Result<std::size_t>::Failure(MakeError(AIErrors::BehaviorTreeTopologyInvalid));
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                const BehaviorTreeNode &node = nodes[index];
                if (node.kind == BehaviorTreeNodeKind::Service) {
                    if (state.serviceCounts[index] != 1 || state.childCounts[index] != 0 || state.structuralParents[index] != 1)
                        return Result<std::size_t>::Failure(MakeError(AIErrors::BehaviorTreeTopologyInvalid));
                } else {
                    if ((index == root && state.childCounts[index] != 1) || (index != root && state.structuralParents[index] != 1) ||
                        state.childCounts[index] < node.minimumChildren || state.childCounts[index] > node.maximumChildren ||
                        state.serviceCounts[index] != 0)
                        return Result<std::size_t>::Failure(MakeError(AIErrors::BehaviorTreeTopologyInvalid));
                }
            }
            return Result<std::size_t>::Success(root);
        }

        [[nodiscard]] Result<void> ValidateAcyclic(const std::span<const BehaviorTreeNode> nodes,
                                                   const std::span<const BehaviorTreeEdge> edges,
                                                   const std::span<const std::size_t> structuralParents) {
            std::vector<std::size_t> indegree{structuralParents.begin(), structuralParents.end()};
            std::vector<std::vector<std::size_t>> outgoing(nodes.size());
            for (const BehaviorTreeEdge &edge : edges)
                outgoing[NodeIndex(nodes, edge.sourceNode)].push_back(NodeIndex(nodes, edge.targetNode));
            std::deque<std::size_t> ready;
            for (std::size_t index = 0; index < nodes.size(); ++index)
                if (indegree[index] == 0)
                    ready.push_back(index);
            std::ranges::sort(ready, {}, [&nodes](const std::size_t index) {
                return nodes[index].id;
            });
            std::size_t visited{};
            while (!ready.empty()) {
                const std::size_t current = ready.front();
                ready.pop_front();
                ++visited;
                for (const std::size_t target : outgoing[current]) {
                    if (--indegree[target] == 0)
                        ready.push_back(target);
                }
            }
            return visited == nodes.size() ? Result<void>::Success() : Failure<void>(AIErrors::BehaviorTreeCycle);
        }

        [[nodiscard]] Result<void> ValidateEdges(BehaviorTreeAssetDescriptor &candidate, const BehaviorTreeAssetLimits &limits,
                                                 const std::span<const PinReference> pins) {
            if (candidate.edges.size() > limits.maximumEdges)
                return Failure<void>(AIErrors::BehaviorTreeLimitExceeded);
            std::ranges::sort(candidate.edges, {}, &BehaviorTreeEdge::id);
            if (std::ranges::adjacent_find(candidate.edges, {}, &BehaviorTreeEdge::id) != candidate.edges.end())
                return Failure<void>(AIErrors::BehaviorTreeIdentityConflict);

            const auto nodes = std::span{candidate.nodes};
            EdgeValidationState state{
                .childCounts = std::vector<std::size_t>(nodes.size()),
                .serviceCounts = std::vector<std::size_t>(nodes.size()),
                .structuralParents = std::vector<std::size_t>(nodes.size()),
            };
            state.singleTargetPins.reserve(candidate.edges.size());
            state.outgoingOrders.reserve(candidate.edges.size());
            state.endpoints.reserve(candidate.edges.size());
            for (const BehaviorTreeEdge &edge : candidate.edges) {
                if (const auto valid = ValidateEdge(edge, nodes, pins, limits, state); valid.HasError())
                    return valid;
            }
            if (const auto valid = ValidateEdgeUniqueness(state); valid.HasError())
                return valid;
            if (const auto root = ValidateRootAndParents(candidate, nodes, state); root.HasError())
                return Result<void>::Failure(root.ErrorValue());
            if (const auto acyclic = ValidateAcyclic(nodes, candidate.edges, state.structuralParents); acyclic.HasError())
                return acyclic;

            std::ranges::sort(candidate.edges, [](const BehaviorTreeEdge &left, const BehaviorTreeEdge &right) {
                return std::tie(left.sourceNode, left.kind, left.order, left.id) <
                       std::tie(right.sourceNode, right.kind, right.order, right.id);
            });
            return Result<void>::Success();
        }
    }  // namespace

    BehaviorTreeAsset::BehaviorTreeAsset(BehaviorTreeAssetDescriptor data, const std::size_t unknownNodes,
                                         const std::size_t unknownProperties) noexcept
        : data_(std::move(data)), unknownNodeCount_(unknownNodes), unknownPropertyCount_(unknownProperties) {}

    /** @copydoc BehaviorTreeAsset::Capture */
    Result<BehaviorTreeAsset> BehaviorTreeAsset::Capture(BehaviorTreeAssetDescriptor candidate, const BehaviorTreeAssetLimits &limits) {
        try {
            if (!ValidLimits(limits) || !ValidVersion(candidate.version) || !candidate.graph.IsValid() || !candidate.root.IsValid() ||
                !ValidName(candidate.displayName, limits))
                return Failure<BehaviorTreeAsset>(AIErrors::BehaviorTreeSchemaInvalid);

            std::vector<PinReference> pins;
            std::size_t unknownNodes{};
            std::size_t unknownProperties{};
            if (const auto valid = ValidateNodes(candidate, limits, pins, unknownNodes, unknownProperties); valid.HasError())
                return Result<BehaviorTreeAsset>::Failure(valid.ErrorValue());
            if (const auto valid = ValidateEdges(candidate, limits, pins); valid.HasError())
                return Result<BehaviorTreeAsset>::Failure(valid.ErrorValue());
            return Result<BehaviorTreeAsset>::Success(BehaviorTreeAsset{std::move(candidate), unknownNodes, unknownProperties});
        } catch (const std::bad_alloc &) {
            return Failure<BehaviorTreeAsset>(AIErrors::BehaviorTreeStorageUnavailable);
        }
    }

    /** @copydoc BehaviorTreeAsset::Nodes */
    std::span<const BehaviorTreeNode> BehaviorTreeAsset::Nodes() const noexcept {
        return data_.nodes;
    }

    /** @copydoc BehaviorTreeAsset::Edges */
    std::span<const BehaviorTreeEdge> BehaviorTreeAsset::Edges() const noexcept {
        return data_.edges;
    }

    /** @copydoc BehaviorTreeAsset::FindNode */
    const BehaviorTreeNode *BehaviorTreeAsset::FindNode(const BehaviorTreeNodeId id) const noexcept {
        const auto found = std::ranges::lower_bound(data_.nodes, id, {}, &BehaviorTreeNode::id);
        return found == data_.nodes.end() || found->id != id ? nullptr : std::to_address(found);
    }
}  // namespace Horo::AI
