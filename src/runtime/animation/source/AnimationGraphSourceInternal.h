#pragma once
#include "Horo/Animation/AnimationGraphSource.h"

#include <array>
#include <nlohmann/json.hpp>
#include <set>

namespace Horo::Animation::GraphSourceDetail {
    using Json = nlohmann::ordered_json;

    /** @brief Typed private parser rejection; candidate content never enters diagnostics. */
    struct Rejection final {
        const ErrorCodeDescriptor *code;
    };

    /** @brief Rejects one source operation with a stable Animation-owned cause. */
    [[noreturn]] inline void Reject(const ErrorCodeDescriptor &code) {
        throw Rejection{&code};
    }

    /** @brief Checks synchronous codec admission without registering callbacks. */
    inline void CheckAdmission(const AnimationGraphCompileContext &context) {
        if (!context.accepting)
            Reject(AnimationErrors::GraphAdmissionRejected);
        if (context.cancellation.IsCancellationRequested())
            Reject(AnimationErrors::GraphOperationCancelled);
    }

    /** @brief Rejects caller policies that would raise implementation ceilings. */
    inline void CheckLimits(const AnimationGraphSourceLimits &limits) {
        if (limits.bytes == 0 || limits.bytes > AnimationGraphSourceHardLimits::Bytes || limits.jsonValues == 0 ||
            limits.jsonValues > AnimationGraphSourceHardLimits::JsonValues || limits.depth == 0 ||
            limits.depth > AnimationGraphSourceHardLimits::Depth)
            Reject(AnimationErrors::GraphLimitExceeded);
    }

    /** @brief Stable textual transport vocabulary, independent of C++ enum representation. */
    inline constexpr std::array<std::string_view, 5> Types{"pose", "float", "boolean", "integer", "trigger"};
    inline constexpr std::array<std::string_view, 7> Roles{"value",     "firstPose", "secondPose",   "weight",
                                                           "interface", "result",    "referencePose"};
    inline constexpr std::array<std::string_view, 3> BlendModes{"normalized", "additiveLocal", "additiveParent"};
    inline constexpr std::array<std::string_view, 2> MissingJoints{"skeletonReference", "preserveBase"};
    inline constexpr std::array<std::string_view, 7> Kinds{"clip", "blend", "parameter", "output", "input", "call", "stateMachine"};
    inline constexpr std::array<std::string_view, 5> Predicates{"above", "below", "equal", "notEqual", "trigger"};
    inline constexpr std::array<std::string_view, 4> Interruptions{"none", "source", "destination", "either"};
    inline constexpr std::array<std::string_view, 4> BlendCurves{"linear", "smoothStep", "easeIn", "easeOut"};

    /** @brief Owns source-envelope clip dependencies while visiting opcodes and state references by their exact types. */
    struct DependencyProjection final {
        std::set<AnimationClipId> clips;

        /** @brief Retain the canonical clip identity; no source/provider state escapes the projection. */
        void operator()(const AnimationClipId clip) {
            clips.insert(clip);
        }

        /** @brief Project an ordinary clip opcode through the same stable dependency set. */
        void operator()(const GraphClipNode &clip) {
            (*this)(clip.clip);
        }

        /** @brief Include inactive state clips; definition references are traversed by the outer complete definition walk. */
        void operator()(const GraphStateMachineNode &machine) {
            for (const auto &state : machine.states)
                std::visit(*this, state.source);
        }

        /** @brief Non-clip references and scalar/composition opcodes add no direct clip dependency. */
        template <typename Payload> void operator()(const Payload &) const noexcept {
            // Scalar/composition metadata has no direct clip. Definition references are covered by the complete outer walk.
        }
    };

    /** @brief Produces the exact canonical typed dependency projection used by the host sink. */
    inline Json Dependencies(const AnimationGraphData &data) {
        DependencyProjection projection;
        for (const auto &definition : data.definitions)
            for (const auto &node : definition.nodes)
                std::visit(projection, node.payload);
        Json values = Json::array();
        for (const auto clip : projection.clips)
            values.push_back(Json{{"assetType", "core.animation.clip"}, {"assetId", clip.Asset().ToString()}});
        values.push_back(Json{{"assetType", "core.animation.skeleton"}, {"assetId", data.skeleton.Asset().ToString()}});
        return values;
    }

    /** @brief Source schemas retain one exact semantic contract through explicit migration. */
    inline AnimationGraphContractVersion SourceContract(const std::uint32_t schema) noexcept {
        if (schema <= 2)
            return {1, 0, 0};
        if (schema == 3)
            return {1, 1, 0};
        return CurrentAnimationGraphContractVersion;
    }

    /** @brief Rejects unknown or missing object fields before typed decoding. */
    inline void Keys(const Json &value, std::initializer_list<std::string_view> keys) {
        if (!value.is_object() || value.size() != keys.size())
            Reject(AnimationErrors::GraphMalformed);
        for (const auto key : keys)
            if (!value.contains(std::string(key)))
                Reject(AnimationErrors::GraphMalformed);
    }

    /** @brief Applies symmetric JSON token/nesting policy to the detached serialization tree. */
    inline void CheckJsonBudget(const Json &value, const AnimationGraphCompileContext &context, const AnimationGraphSourceLimits &limits,
                                std::size_t depth, std::size_t &values) {
        CheckAdmission(context);
        if (depth > limits.depth)
            Reject(AnimationErrors::GraphLimitExceeded);
        const auto events = value.is_structured() ? 2U : 1U;
        if (values > limits.jsonValues || events > limits.jsonValues - values)
            Reject(AnimationErrors::GraphLimitExceeded);
        values += events;
        if (value.is_object()) {
            for (auto field = value.begin(); field != value.end(); ++field) {
                if (depth + 1 > limits.depth || values >= limits.jsonValues)
                    Reject(AnimationErrors::GraphLimitExceeded);
                ++values;
                CheckJsonBudget(field.value(), context, limits, depth + 1, values);
            }
        } else if (value.is_array()) {
            for (const auto &item : value)
                CheckJsonBudget(item, context, limits, depth + 1, values);
        }
    }
}  // namespace Horo::Animation::GraphSourceDetail
