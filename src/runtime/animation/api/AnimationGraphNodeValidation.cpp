#include "AnimationGraphCompilerInternal.h"

#include <cmath>
#include <set>

namespace Horo::Animation::GraphCompileDetail {
    namespace {
        using namespace AnimationErrors;
        using Shape = Result<std::vector<GraphPin>>;
        using Validation = Horo::Result<void>;
        using enum GraphPinRole;

        /** @brief Captures only the authoring references needed to prove node shape. */
        struct ShapeContext final {
            const GraphDefinition &definition;
            const AnimationGraphData &data;
            GraphSourceLocation location;
        };

        /** @brief Constructs one semantic pin shape without assigning authored identity. */
        GraphPin Pin(const GraphPinRole role, const GraphValueType type, const bool output, const GraphInterfaceId input = {}) {
            return {{}, role, type, output, input};
        }

        /** @brief Validates an absolute or explicitly reference-bound additive clip's exact shape. */
        Shape NodePins(const GraphClipNode &clip, const ShapeContext &context) {
            using enum GraphValueType;
            if (!clip.clip.IsValid() || clip.clip.Asset() == context.data.id.Asset() || clip.clip.Asset() == context.data.skeleton.Asset())
                return Shape::Failure(Failure(GraphMalformed, context.location));
            std::vector pins{Pin(Result, Pose, true)};
            if (clip.referencePose) {
                if (!clip.referencePose->IsValid())
                    return Shape::Failure(Failure(GraphMalformed, context.location));
                pins.push_back(Pin(FirstPose, Pose, false));
                pins.push_back(Pin(ReferencePose, Pose, false));
            }
            return Shape::Success(std::move(pins));
        }

        /** @brief Checks finite sparse mask values and unique canonical stable joint identities. */
        bool ValidMask(const GraphBoneMask &mask) {
            const auto validWeight = [](const float weight) {
                return std::isfinite(weight) && weight >= 0.0F && weight <= 1.0F;
            };
            if (!validWeight(mask.defaultWeight))
                return false;
            JointId previous{};
            for (const auto &entry : mask.joints) {
                if (!entry.joint.IsValid() || entry.joint == previous || !validWeight(entry.weight))
                    return false;
                previous = entry.joint;
            }
            return true;
        }

        /** @brief Validates a typed layer policy and its required reference operand. */
        Shape NodePins(const GraphBlendNode &blend, const ShapeContext &context) {
            using enum GraphValueType;
            if (blend.mode >= GraphBlendMode::Count || blend.missingJoint >= GraphMissingJointPolicy::Count)
                return Shape::Failure(Failure(GraphTypeMismatch, context.location));
            if (!ValidMask(blend.mask))
                return Shape::Failure(Failure(GraphMalformed, context.location));
            std::vector pins{Pin(FirstPose, Pose, false), Pin(SecondPose, Pose, false), Pin(Weight, Float, false), Pin(Result, Pose, true)};
            if (blend.mode != GraphBlendMode::Normalized)
                pins.push_back(Pin(ReferencePose, Pose, false));
            return Shape::Success(std::move(pins));
        }

        /** @brief Requires an exact declared parameter and its transport type. */
        Shape NodePins(const GraphParameterNode &parameter, const ShapeContext &context) {
            const auto *value = Find(context.data.parameters, parameter.parameter);
            if (!value)
                return Shape::Failure(Failure(GraphMalformed, context.location));
            return Shape::Success({Pin(Result, value->type, true)});
        }

        /** @brief Defines the sole pose input of a final output sink. */
        Shape NodePins(const GraphOutputNode &, const ShapeContext &) {
            return Shape::Success({Pin(Value, GraphValueType::Pose, false)});
        }

        /** @brief Requires an exact definition interface and exposes its transport type. */
        Shape NodePins(const GraphInputNode &input, const ShapeContext &context) {
            const auto *value = Find(context.definition.inputs, input.input);
            if (!value)
                return Shape::Failure(Failure(GraphMalformed, context.location));
            return Shape::Success({Pin(Result, value->type, true)});
        }

        /** @brief Requires the exact asset-local callee interface shape. */
        Shape NodePins(const GraphCallNode &call, const ShapeContext &context) {
            const auto *target = Find(context.data.definitions, call.definition);
            if (!target)
                return Shape::Failure(Failure(GraphMalformed, context.location));
            std::vector<GraphPin> pins;
            for (const auto &input : target->inputs)
                pins.push_back(Pin(Interface, input.type, false, input.id));
            pins.push_back(Pin(Result, GraphValueType::Pose, true));
            return Shape::Success(std::move(pins));
        }
    }  // namespace

    /** @copydoc ValidatePins */
    Validation ValidatePins(const GraphNode &node, const GraphDefinition &definition, const AnimationGraphData &data) {
        const ShapeContext context{definition, data, {definition.id, node.id, {}}};
        auto expected = std::visit([&context](const auto &payload) {
            return NodePins(payload, context);
        }, node.payload);
        if (expected.HasError())
            return Validation::Failure(expected.ErrorValue());
        if (!UniqueIds(node.pins) || node.pins.size() != expected.Value().size())
            return Validation::Failure(Failure(GraphMalformed, context.location));
        std::set<std::pair<GraphPinRole, GraphInterfaceId>> seen;
        for (const auto &pin : node.pins) {
            const auto found = std::ranges::find_if(expected.Value(), [&pin](const auto &value) {
                return value.role == pin.role && value.interfaceId == pin.interfaceId;
            });
            if (found == expected.Value().end() || !seen.emplace(pin.role, pin.interfaceId).second || found->output != pin.output)
                return Validation::Failure(Failure(GraphMalformed, {definition.id, node.id, pin.id}));
            if (found->type != pin.type)
                return Validation::Failure(Failure(GraphTypeMismatch, {definition.id, node.id, pin.id}));
        }
        return Validation::Success();
    }
}  // namespace Horo::Animation::GraphCompileDetail
