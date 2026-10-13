#include "AnimationGraphCompilerInternal.h"

namespace Horo::Animation::GraphCompileDetail {
    namespace {
        using namespace AnimationErrors;

        /** @brief Checks caller structural ceilings without traversing candidate storage. */
        bool ValidStructureLimits(const AnimationGraphLimits &limits) {
            const AnimationGraphLimits hard{};
            return limits.definitions > 0 && limits.definitions <= hard.definitions && limits.nodes > 0 && limits.nodes <= hard.nodes &&
                   limits.pins > 0 && limits.pins <= hard.pins && limits.connections <= hard.connections &&
                   limits.parameters <= hard.parameters;
        }

        /** @brief Checks finite expansion policy independently from allocation categories. */
        bool ValidExpansionLimits(const AnimationGraphLimits &limits) {
            const AnimationGraphLimits hard{};
            return limits.callDepth > 0 && limits.callDepth <= hard.callDepth && limits.evaluationInstructions > 0 &&
                   limits.evaluationInstructions <= hard.evaluationInstructions && limits.sourceMapCallEntries <= hard.sourceMapCallEntries;
        }

        /** @brief Checks caller-lowerable prepared storage categories. */
        bool ValidStorageLimits(const AnimationGraphLimits &limits) {
            const AnimationGraphLimits hard{};
            return limits.poseSlots <= hard.poseSlots && limits.scalarSlots <= hard.scalarSlots && limits.clipPlayers <= hard.clipPlayers &&
                   limits.poseTransforms <= hard.poseTransforms;
        }

        /** @brief Whole-asset storage admission counters before any detached copy. */
        struct CandidateCounts final {
            std::uint64_t nodes{};
            std::uint64_t pins{};
            std::uint64_t connections{};
            std::uint64_t interfaces{};
            std::uint64_t maskEntries{};
            std::uint64_t machines{}, states{}, transitions{}, conditions{};
        };

        /** @brief Charges the complete inert state table independently of node pins and pose masks. */
        Result<void> CountState(const GraphStateMachineNode &machine, CandidateCounts &counts, const AnimationGraphLimits &limits) {
            ++counts.machines;
            counts.states += machine.states.size();
            counts.transitions += machine.transitions.size();
            for (const auto &transition : machine.transitions)
                counts.conditions += transition.conditions.size();
            if (counts.machines > limits.stateMachines || counts.states > limits.states || counts.transitions > limits.transitions ||
                counts.conditions > limits.conditions)
                return Result<void>::Failure(Failure(GraphLimitExceeded));
            return Result<void>::Success();
        }

        /** @brief Charges one node's bounded pins and sparse mask storage. */
        Result<void> CountNode(const GraphNode &node, CandidateCounts &counts, const AnimationGraphLimits &limits) {
            counts.pins += node.pins.size();
            if (counts.pins > limits.pins)
                return Result<void>::Failure(Failure(GraphLimitExceeded));
            if (const auto *blend = std::get_if<GraphBlendNode>(&node.payload)) {
                counts.maskEntries += blend->mask.joints.size();
                if (blend->mask.joints.size() > SkeletonAssetHardLimits::Joints || counts.maskEntries > limits.poseTransforms)
                    return Result<void>::Failure(Failure(GraphLimitExceeded));
            }
            if (const auto *machine = std::get_if<GraphStateMachineNode>(&node.payload))
                return CountState(*machine, counts, limits);
            return Result<void>::Success();
        }

        /** @brief Charges one definition with cancellation checkpoints between nodes. */
        Result<void> CountDefinition(const GraphDefinition &definition, CandidateCounts &counts,
                                     const AnimationGraphCompileContext &context) {
            counts.nodes += definition.nodes.size();
            counts.connections += definition.connections.size();
            counts.interfaces += definition.inputs.size();
            if (counts.nodes > context.limits.nodes || counts.connections > context.limits.connections ||
                counts.interfaces > context.limits.pins)
                return Result<void>::Failure(Failure(GraphLimitExceeded));
            for (const auto &node : definition.nodes) {
                if (auto state = Admission(context); state.HasError())
                    return state;
                if (auto counted = CountNode(node, counts, context.limits); counted.HasError())
                    return counted;
            }
            return Result<void>::Success();
        }

        /** @brief Keeps state-policy hard ceilings separate from pose and expansion policies. */
        bool ValidStateLimits(const AnimationGraphLimits &limits) {
            return limits.stateMachines <= AnimationGraphHardLimits::StateMachines && limits.states <= AnimationGraphHardLimits::States &&
                   limits.transitions <= AnimationGraphHardLimits::Transitions &&
                   limits.conditions <= AnimationGraphHardLimits::Conditions && limits.stateEvents <= AnimationGraphHardLimits::StateEvents;
        }

        /** @brief Validates persistent asset-role identity before inspecting any nested containers. */
        bool ValidIdentity(const AnimationGraphData &data) {
            return data.id.IsValid() && data.skeleton.IsValid() && data.entry.IsValid() && data.id.Asset() != data.skeleton.Asset();
        }

        /** @brief Requires bounded nonempty top-level storage before charging its nested nodes. */
        bool ValidCollections(const AnimationGraphData &data, const AnimationGraphLimits &limits) {
            return !data.definitions.empty() && data.definitions.size() <= limits.definitions &&
                   data.parameters.size() <= limits.parameters;
        }

        /** @brief Rejects malformed parameter names before the canonical copy can allocate them. */
        Result<void> CheckNames(const AnimationGraphData &data) {
            for (const auto &parameter : data.parameters)
                if (parameter.name.empty() || parameter.name.size() > AnimationGraphHardLimits::ParameterNameBytes)
                    return Result<void>::Failure(Failure(GraphMalformed));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PreflightGraph */
    Result<void> PreflightGraph(const AnimationGraphData &data, const AnimationGraphCompileContext &context) {
        if (auto state = Admission(context); state.HasError())
            return state;
        if (!ValidIdentity(data))
            return Result<void>::Failure(Failure(GraphMalformed));
        if (context.replacing && *context.replacing != data.id)
            return Result<void>::Failure(Failure(GraphReloadMismatch));
        const auto &limits = context.limits;
        if (!ValidStateLimits(limits))
            return Result<void>::Failure(Failure(GraphLimitExceeded));
        if (!ValidStructureLimits(limits) || !ValidExpansionLimits(limits) || !ValidStorageLimits(limits))
            return Result<void>::Failure(Failure(GraphLimitExceeded));
        if (!ValidCollections(data, limits))
            return Result<void>::Failure(Failure(GraphLimitExceeded));
        CandidateCounts counts;
        for (const auto &definition : data.definitions)
            if (auto counted = CountDefinition(definition, counts, context); counted.HasError())
                return counted;
        return CheckNames(data);
    }

}  // namespace Horo::Animation::GraphCompileDetail
