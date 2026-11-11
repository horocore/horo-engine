#pragma once
#include "DecisionBinary.h"
#include "Horo/AI/DecisionGraphCompiler.h"

namespace Horo::AI::Detail {
    template <typename T, typename Write>
    void WriteSequence(DecisionWriter &writer, std::span<const T> values, std::size_t maximum, Write write) {
        if (!writer.Count(values.size(), maximum))
            return;
        for (const auto &value : values)
            write(writer, value);
    }

    template <typename T, typename Read> std::vector<T> ReadSequence(DecisionReader &reader, std::size_t maximum, Read read) {
        const auto count = reader.Count(maximum);
        std::vector<T> values;
        values.reserve(count);
        for (std::size_t index = 0; index < count && reader.Valid(); ++index)
            values.push_back(read(reader));
        return values;
    }

    void WriteLocation(DecisionWriter &writer, const SourceLocation &value);
    [[nodiscard]] SourceLocation ReadLocation(DecisionReader &reader);
    void WriteRequirement(DecisionWriter &writer, const DecisionBlackboardRequirement &value);
    void WriteRequirements(DecisionWriter &writer, std::span<const DecisionBlackboardRequirement> values);
    [[nodiscard]] DecisionBlackboardRequirement ReadRequirement(DecisionReader &reader);
    void WriteProperty(DecisionWriter &writer, const BehaviorTreeProperty &value);
    [[nodiscard]] BehaviorTreeProperty ReadProperty(DecisionReader &reader);
    void WriteScalar(DecisionWriter &writer, const BlackboardScalarValue &value);
    [[nodiscard]] BlackboardScalarValue ReadScalar(DecisionReader &reader);
    void WriteCall(DecisionWriter &writer, const std::optional<DecisionSubplanCall> &value);
    [[nodiscard]] std::optional<DecisionSubplanCall> ReadCall(DecisionReader &reader);
    void WriteTree(DecisionWriter &writer, const BehaviorTreeAsset &tree);
    [[nodiscard]] Result<std::shared_ptr<const BehaviorTreeAsset>> ReadTree(DecisionReader &reader);
    /** @brief Writes canonical bounded Utility source with stable option ordering. */
    void WriteUtility(DecisionWriter &writer, const UtilityDecisionSource &source);
    /** @brief Reads a bounded Utility family payload, retaining sticky transport errors. */
    [[nodiscard]] UtilityDecisionSource ReadUtility(DecisionReader &reader);
    /** @brief Writes bounded provider metadata and properties with canonical field ordering. */
    void WriteProviderNode(DecisionWriter &writer, const DecisionAssetNode &node, std::span<const BehaviorTreeProperty> properties);
    /** @brief Reads the shared bounded task-provider representation. */
    void ReadProviderNode(DecisionReader &reader, DecisionAssetNode &node, std::vector<BehaviorTreeProperty> &properties);
    void WriteMachine(DecisionWriter &writer, const StateMachineAssetDescriptor &machine);
    [[nodiscard]] StateMachineAssetDescriptor ReadMachine(DecisionReader &reader);
}  // namespace Horo::AI::Detail
