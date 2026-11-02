#pragma once
#include "Horo/Animation/AnimationGraphSource.h"

#include <array>
#include <nlohmann/json.hpp>
#include <set>

namespace Horo::Animation::GraphSourceDetail {
    // Parsing needs stable object nodes: ordered_map relocates populated JSON
    // values while growing, and their noexcept destruction may allocate. Canonical
    // field order belongs to SourceWriter.
    using Json = nlohmann::json;

    /** @brief Typed private parser rejection; candidate content never enters
     * diagnostics. */
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

    /** @brief Stable textual transport vocabulary, independent of C++ enum
     * representation. */
    inline constexpr std::array<std::string_view, 5> Types{"pose", "float", "boolean", "integer", "trigger"};
    inline constexpr std::array<std::string_view, 7> Roles{"value",     "firstPose", "secondPose",   "weight",
                                                           "interface", "result",    "referencePose"};
    inline constexpr std::array<std::string_view, 3> BlendModes{"normalized", "additiveLocal", "additiveParent"};
    inline constexpr std::array<std::string_view, 2> MissingJoints{"skeletonReference", "preserveBase"};
    inline constexpr std::array<std::string_view, 9> Kinds{"clip", "blend",        "parameter", "output",    "input",
                                                           "call", "stateMachine", "blendTree", "actionSlot"};
    inline constexpr std::array<std::string_view, 5> Predicates{"above", "below", "equal", "notEqual", "trigger"};
    inline constexpr std::array<std::string_view, 4> Interruptions{"none", "source", "destination", "either"};
    inline constexpr std::array<std::string_view, 4> BlendCurves{"linear", "smoothStep", "easeIn", "easeOut"};

    inline constexpr std::array<std::string_view, 2> Dimensions{"one", "two"};
    inline constexpr std::array<std::string_view, 2> Outside{"clampToDomain", "reject"};
    inline constexpr std::array<std::string_view, 3> SyncModes{"independent", "normalizedPhase", "markers"};
    inline constexpr std::array<std::string_view, 3> SyncEvents{"leaderOnly", "allActive", "suppress"};

    inline constexpr std::array<std::string_view, 1> SyncLeaders{"localSampleWeightWithStateContribution"};

    inline constexpr std::array<std::string_view, 2> ActionLoops{"finite", "untilReleased"};
    inline constexpr std::array<std::string_view, 3> ActionInterruptions{"reject", "atSectionBoundary", "immediate"};
    inline constexpr std::array<std::string_view, 2> ActionReverse{"reject", "allow"};

    /** @brief Owns source-envelope clip dependencies while visiting opcodes and
     * state references by their exact types. */
    struct DependencyProjection final {
        std::set<AnimationClipId> clips;

        /** @brief Retain the canonical clip identity; no source/provider state
         * escapes the projection. */
        void operator()(const AnimationClipId clip) {
            clips.insert(clip);
        }

        /** @brief Project an ordinary clip opcode through the same stable dependency
         * set. */
        void operator()(const GraphClipNode &clip) {
            (*this)(clip.clip);
        }

        /** @brief Retains every immutable blend-space dependency, including
         * zero-weight samples. */
        void operator()(const GraphBlendTreeNode &tree) {
            for (const auto &sample : tree.samples)
                (*this)(sample.clip);
        }

        /** @brief Include inactive state clips; definition references are traversed
         * by the outer complete definition walk. */
        void operator()(const GraphStateMachineNode &machine) {
            for (const auto &state : machine.states)
                std::visit(*this, state.source);
        }

        /** @brief Action sections retain their exact immutable clip dependencies even
         * when no request is currently active. */
        void operator()(const GraphActionDefinition &action) {
            for (const auto &section : action.sequence.sections)
                (*this)(section.clip);
        }

        /** @brief Non-clip references and scalar/composition opcodes add no direct
         * clip dependency. */
        template <typename Payload> void operator()(const Payload &) const noexcept {
            // Scalar/composition metadata has no direct clip. Definition references are
            // covered by the complete outer walk.
        }
    };

    /** @brief Canonical complete dependency projection shared by source admission and emission. */
    inline DependencyProjection ProjectDependencies(const AnimationGraphData &data) {
        DependencyProjection result;
        for (const auto &definition : data.definitions)
            for (const auto &node : definition.nodes)
                std::visit(result, node.payload);
        for (const auto &action : data.actions)
            result(action);
        return result;
    }

    /** @brief Source schemas retain one exact semantic contract through explicit
     * migration. */
    inline AnimationGraphContractVersion SourceContract(const std::uint32_t schema) noexcept {
        if (schema <= 2)
            return {1, 0, 0};
        if (schema == 3)
            return {1, 1, 0};
        if (schema == 4)
            return {1, 2, 0};
        if (schema == 5)
            return {1, 3, 0};
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

}  // namespace Horo::Animation::GraphSourceDetail
