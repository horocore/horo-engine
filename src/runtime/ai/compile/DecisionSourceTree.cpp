#include "DecisionSourceWire.h"

namespace Horo::AI::Detail {
    namespace {
        void WritePin(DecisionWriter &writer, const BehaviorTreePin &pin) {
            writer.IdentityValue(pin.id);
            writer.Enumeration(pin.direction);
            writer.Enumeration(pin.cardinality);
        }

        BehaviorTreePin ReadPin(DecisionReader &reader) {
            return {reader.IdentityValue<BehaviorTreePinId>(), reader.Enumeration(BehaviorTreePinDirection::Count),
                    reader.Enumeration(BehaviorTreePinCardinality::Count)};
        }

        void WriteControl(DecisionWriter &writer, const DecisionControlParameters &control) {
            writer.Enumeration(control.kind);
            writer.Enumeration(control.parallelPolicy);
            writer.Enumeration(control.abortMode);
            writer.Enumeration(control.serviceMode);
            writer.Unsigned(control.durationTicks);
            writer.Unsigned(control.intervalTicks);
            writer.Unsigned(control.iterations, 4);
        }

        DecisionControlParameters ReadControl(DecisionReader &reader) {
            DecisionControlParameters control;
            control.kind = reader.Enumeration(DecisionControlKind::Count);
            control.parallelPolicy = reader.Enumeration(BehaviorTreeParallelPolicy::Count);
            control.abortMode = reader.Enumeration(BehaviorTreeAbortMode::Count);
            control.serviceMode = reader.Enumeration(BehaviorTreeServiceMode::Count);
            control.durationTicks = reader.Unsigned();
            control.intervalTicks = reader.Unsigned();
            control.iterations = static_cast<std::uint32_t>(reader.Unsigned(4));
            return control;
        }

        void WriteLegacySubtree(DecisionWriter &writer, const std::optional<BehaviorTreeSubtreeReference> &call) {
            writer.Unsigned(call.has_value(), 1);
            if (!call)
                return;
            writer.IdentityValue(call->graph);
            writer.Unsigned(call->requiredSchemaVersion.major, 2);
            writer.Unsigned(call->requiredSchemaVersion.minor, 2);
        }

        std::optional<BehaviorTreeSubtreeReference> ReadLegacySubtree(DecisionReader &reader) {
            if (!reader.Boolean())
                return {};
            return BehaviorTreeSubtreeReference{reader.IdentityValue<BehaviorTreeGraphId>(),
                                                {static_cast<std::uint16_t>(reader.Unsigned(2)),
                                                 static_cast<std::uint16_t>(reader.Unsigned(2))}};
        }

        void WriteNode(DecisionWriter &writer, const BehaviorTreeNode &node) {
            writer.IdentityValue(node.id);
            writer.Enumeration(node.kind);
            writer.IdentityValue(node.type, true);
            writer.Unsigned(node.typeVersion.major, 2);
            writer.Unsigned(node.typeVersion.minor, 2);
            writer.String(node.displayName, BehaviorTreeAssetHardLimits::DisplayNameBytes);
            writer.Unsigned(node.minimumChildren, 4);
            writer.Unsigned(node.maximumChildren, 4);
            WriteSequence<BehaviorTreePin>(writer, node.pins, BehaviorTreeAssetHardLimits::PinsPerNode, WritePin);
            WriteSequence<BehaviorTreeProperty>(writer, node.properties, BehaviorTreeAssetHardLimits::PropertiesPerNode, WriteProperty);
            WriteLegacySubtree(writer, node.nestedSubtree);
            WriteControl(writer, node.control);
            WriteCall(writer, node.call);
            writer.IdentityValue(node.task, true);
            WriteRequirements(writer, node.requirements);
            WriteLocation(writer, node.source);
        }

        BehaviorTreeNode ReadNode(DecisionReader &reader) {
            BehaviorTreeNode node;
            node.id = reader.IdentityValue<BehaviorTreeNodeId>();
            node.kind = reader.Enumeration(BehaviorTreeNodeKind::Count);
            node.type = reader.IdentityValue<BehaviorTreeNodeTypeId>(true);
            node.typeVersion = {static_cast<std::uint16_t>(reader.Unsigned(2)), static_cast<std::uint16_t>(reader.Unsigned(2))};
            node.displayName = reader.String(BehaviorTreeAssetHardLimits::DisplayNameBytes);
            node.minimumChildren = reader.Count(BehaviorTreeAssetHardLimits::ChildrenPerNode);
            node.maximumChildren = reader.Count(BehaviorTreeAssetHardLimits::ChildrenPerNode);
            node.pins = ReadSequence<BehaviorTreePin>(reader, BehaviorTreeAssetHardLimits::PinsPerNode, ReadPin);
            node.properties = ReadSequence<BehaviorTreeProperty>(reader, BehaviorTreeAssetHardLimits::PropertiesPerNode, ReadProperty);
            node.nestedSubtree = ReadLegacySubtree(reader);
            node.control = ReadControl(reader);
            node.call = ReadCall(reader);
            node.task = reader.IdentityValue<TaskId>(true);
            node.requirements = ReadSequence<DecisionBlackboardRequirement>(reader, DecisionAssetValidationHardLimits::RequirementsPerNode,
                                                                            ReadRequirement);
            node.source = ReadLocation(reader);
            return node;
        }

        void WriteEdge(DecisionWriter &writer, const BehaviorTreeEdge &edge) {
            writer.IdentityValue(edge.id);
            writer.Enumeration(edge.kind);
            writer.IdentityValue(edge.sourceNode);
            writer.IdentityValue(edge.sourcePin);
            writer.IdentityValue(edge.targetNode);
            writer.IdentityValue(edge.targetPin);
            writer.Unsigned(edge.order, 4);
        }

        BehaviorTreeEdge ReadEdge(DecisionReader &reader) {
            BehaviorTreeEdge edge;
            edge.id = reader.IdentityValue<BehaviorTreeEdgeId>();
            edge.kind = reader.Enumeration(BehaviorTreeEdgeKind::Count);
            edge.sourceNode = reader.IdentityValue<BehaviorTreeNodeId>();
            edge.sourcePin = reader.IdentityValue<BehaviorTreePinId>();
            edge.targetNode = reader.IdentityValue<BehaviorTreeNodeId>();
            edge.targetPin = reader.IdentityValue<BehaviorTreePinId>();
            edge.order = reader.Count(BehaviorTreeAssetHardLimits::ChildrenPerNode);
            return edge;
        }
    }  // namespace

    void WriteTree(DecisionWriter &writer, const BehaviorTreeAsset &tree) {
        writer.IdentityValue(tree.Graph());
        writer.Unsigned(tree.Version().major, 2);
        writer.Unsigned(tree.Version().minor, 2);
        writer.String(tree.Data().displayName, BehaviorTreeAssetHardLimits::DisplayNameBytes);
        writer.IdentityValue(tree.Root());
        WriteSequence<BehaviorTreeNode>(writer, tree.Nodes(), BehaviorTreeAssetHardLimits::Nodes, WriteNode);
        WriteSequence<BehaviorTreeEdge>(writer, tree.Edges(), BehaviorTreeAssetHardLimits::Edges, WriteEdge);
    }

    Result<std::shared_ptr<const BehaviorTreeAsset>> ReadTree(DecisionReader &reader) {
        BehaviorTreeAssetDescriptor tree;
        tree.graph = reader.IdentityValue<BehaviorTreeGraphId>();
        tree.version = {static_cast<std::uint16_t>(reader.Unsigned(2)), static_cast<std::uint16_t>(reader.Unsigned(2))};
        tree.displayName = reader.String(BehaviorTreeAssetHardLimits::DisplayNameBytes);
        tree.root = reader.IdentityValue<BehaviorTreeNodeId>();
        tree.nodes = ReadSequence<BehaviorTreeNode>(reader, BehaviorTreeAssetHardLimits::Nodes, ReadNode);
        tree.edges = ReadSequence<BehaviorTreeEdge>(reader, BehaviorTreeAssetHardLimits::Edges, ReadEdge);
        if (!reader.Valid())
            return Result<std::shared_ptr<const BehaviorTreeAsset>>::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        auto captured = BehaviorTreeAsset::Capture(std::move(tree));
        if (captured.HasError())
            return Result<std::shared_ptr<const BehaviorTreeAsset>>::Failure(captured.ErrorValue());
        return Result<std::shared_ptr<const BehaviorTreeAsset>>::Success(
            std::make_shared<const BehaviorTreeAsset>(std::move(captured).Value()));
    }
}  // namespace Horo::AI::Detail
