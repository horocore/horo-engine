#include "AnimationGraphDocumentBounds.h"

#include <algorithm>

namespace Horo::Editor::AnimationDocumentInternal {
    namespace {
        using namespace Animation;

        /** @brief Saturation-free counters reject before inspecting or copying an excessive child collection. */
        struct Counts final {
            std::size_t definitions{}, nodes{}, pins{}, connections{}, inputs{}, machines{}, states{}, transitions{}, conditions{}, masks{};
        };

        /** @brief Adds only within the admitted bound; no size arithmetic can wrap. */
        bool Reserve(std::size_t &count, const std::size_t amount, const std::size_t bound) noexcept {
            if (amount > bound - count)
                return false;
            count += amount;
            return true;
        }

        /** @brief Counts nested state payloads before any source copy, bounded by the existing graph profile. */
        bool CountPayload(const GraphNodePayload &payload, Counts &counts, const AnimationGraphLimits &limits) {
            if (const auto *blend = std::get_if<GraphBlendNode>(&payload))
                return blend->mask.joints.size() <= SkeletonAssetHardLimits::Joints &&
                       Reserve(counts.masks, blend->mask.joints.size(), limits.poseTransforms);
            const auto *machine = std::get_if<GraphStateMachineNode>(&payload);
            if (!machine)
                return true;
            if (!Reserve(counts.machines, 1, limits.stateMachines) || !Reserve(counts.states, machine->states.size(), limits.states) ||
                !Reserve(counts.transitions, machine->transitions.size(), limits.transitions))
                return false;
            for (const auto &transition : machine->transitions)
                if (!Reserve(counts.conditions, transition.conditions.size(), limits.conditions))
                    return false;
            return true;
        }

        /** @brief Counts only bounded structural data; topology/type validation remains owned by Animation. */
        bool CountDefinition(const GraphDefinition &definition, Counts &counts, const AnimationGraphLimits &limits) {
            if (!Reserve(counts.definitions, 1, limits.definitions) || !Reserve(counts.nodes, definition.nodes.size(), limits.nodes) ||
                !Reserve(counts.connections, definition.connections.size(), limits.connections) ||
                !Reserve(counts.inputs, definition.inputs.size(), limits.pins))
                return false;
            for (const auto &node : definition.nodes)
                if (!Reserve(counts.pins, node.pins.size(), limits.pins) || !CountPayload(node.payload, counts, limits))
                    return false;
            return true;
        }

        /** @brief A replacement is counted in place of its exact stable definition. */
        bool ValidDefinitionBudget(const AnimationGraphData &data, const GraphDefinition &replacement, const AnimationGraphLimits &limits) {
            Counts counts;
            if (!CountDefinition(replacement, counts, limits))
                return false;
            for (const auto &definition : data.definitions)
                if (definition.id != replacement.id && !CountDefinition(definition, counts, limits))
                    return false;
            return true;
        }
    }  // namespace

    /** @copydoc CheckCommandBounds */
    Result<void> CheckCommandBounds(const AnimationGraphData &candidate, const AnimationGraphDocumentCommand &command,
                                    const AnimationGraphDocumentLimits &limits) {
        bool valid = true;
        if (const auto *put = std::get_if<PutAnimationGraphDefinition>(&command))
            valid = ValidDefinitionBudget(candidate, put->value, limits.graph);
        if (const auto *put = std::get_if<PutAnimationGraphParameter>(&command)) {
            const bool inserts = std::ranges::find(candidate.parameters, put->value.id, &GraphParameter::id) == candidate.parameters.end();
            valid = put->value.name.size() <= AnimationGraphHardLimits::ParameterNameBytes &&
                    (!inserts || candidate.parameters.size() < limits.graph.parameters);
        }
        return valid ? Result<void>::Success() : Result<void>::Failure(MakeError(AnimationErrors::GraphLimitExceeded));
    }
}  // namespace Horo::Editor::AnimationDocumentInternal
