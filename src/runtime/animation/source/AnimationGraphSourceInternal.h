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
    inline constexpr std::array<std::string_view, 6> Roles{"value", "firstPose", "secondPose", "weight", "interface", "result"};
    inline constexpr std::array<std::string_view, 6> Kinds{"clip", "blend", "parameter", "output", "input", "call"};

    /** @brief Produces the exact canonical typed dependency projection used by the host sink. */
    inline Json Dependencies(const AnimationGraphData &data) {
        std::set<AnimationClipId> clips;
        for (const auto &definition : data.definitions)
            for (const auto &node : definition.nodes)
                if (const auto *clip = std::get_if<GraphClipNode>(&node.payload))
                    clips.insert(clip->clip);
        Json values = Json::array();
        for (const auto clip : clips)
            values.push_back(Json{{"assetType", "core.animation.clip"}, {"assetId", clip.Asset().ToString()}});
        values.push_back(Json{{"assetType", "core.animation.skeleton"}, {"assetId", data.skeleton.Asset().ToString()}});
        return values;
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
