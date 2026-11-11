#include "DecisionSourceWire.h"

#include <algorithm>

namespace Horo::AI::Detail {
    /** @copydoc WriteProviderNode */
    void WriteProviderNode(DecisionWriter &writer, const DecisionAssetNode &node, const std::span<const BehaviorTreeProperty> source) {
        if (source.size() > BehaviorTreeAssetHardLimits::PropertiesPerNode) {
            writer.Reject();
            return;
        }
        writer.IdentityValue(node.id);
        writer.IdentityValue(node.type);
        writer.Unsigned(node.descriptorVersion.minimum, 4);
        writer.Unsigned(node.descriptorVersion.maximum, 4);
        writer.Enumeration(node.intrinsic);
        WriteRequirements(writer, node.requirements);
        WriteLocation(writer, node.source);
        std::vector<const BehaviorTreeProperty *> properties;
        for (const auto &property : source)
            properties.push_back(&property);
        std::ranges::sort(properties, {}, [](const auto *property) {
            return property->id;
        });
        writer.Count(properties.size(), BehaviorTreeAssetHardLimits::PropertiesPerNode);
        for (const auto *property : properties)
            WriteProperty(writer, *property);
    }

    /** @copydoc ReadProviderNode */
    void ReadProviderNode(DecisionReader &reader, DecisionAssetNode &node, std::vector<BehaviorTreeProperty> &properties) {
        node.id = reader.IdentityValue<DecisionNodeId>();
        node.type = reader.IdentityValue<DecisionNodeTypeId>();
        node.descriptorVersion = {static_cast<std::uint32_t>(reader.Unsigned(4)), static_cast<std::uint32_t>(reader.Unsigned(4))};
        node.intrinsic = reader.Enumeration(DecisionControlKind::Count);
        node.requirements =
            ReadSequence<DecisionBlackboardRequirement>(reader, DecisionAssetValidationHardLimits::RequirementsPerNode, ReadRequirement);
        node.source = ReadLocation(reader);
        properties = ReadSequence<BehaviorTreeProperty>(reader, BehaviorTreeAssetHardLimits::PropertiesPerNode, ReadProperty);
    }
}  // namespace Horo::AI::Detail
