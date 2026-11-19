#pragma once

#include "Horo/AI/BehaviorTree.h"
#include "Horo/AI/DecisionControl.h"
#include "Horo/AI/StateMachine.h"
#include "Horo/AI/UtilitySelector.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Foundation/Sha256.h"

#include <cstdint>
#include <optional>

namespace Horo::Editor {
    /** @brief One stable-ID edit or lifecycle action on an intrinsic State Machine draft. */
    enum class StateMachineEditOperation : std::uint8_t {
        AddState,
        SetTerminal,
        Connect,
        SetTransitionPriority,
        SetTaskProperty,
        RemoveTransition,
        RemoveState,
        Undo,
        Redo,
        Save,
    };

    /** @brief Shared menu and keyboard history action for a focused Decision document. */
    enum class DecisionHistoryAction : std::uint8_t {
        Save,
        Undo,
        Redo,
    };

    /** @brief One reachable Behavior Tree source edit or lifecycle action. */
    enum class BehaviorTreeEditOperation : std::uint8_t {
        WrapSequence,
        WrapSelector,
        UnwrapComposite,
        AppendTask,
        ReviewSubtree,
        AppendSubtree,
        RetargetSubtree,
        ReviewQueryOutput,
        SetQueryOutput,
        SetTaskProperty,
        MoveEarlier,
        MoveLater,
        DeleteTask,
        Undo,
        Redo,
        Save,
    };

    /** @brief Selected saved query and the writable Blackboard target chosen after review. */
    struct BehaviorTreeQueryOutputEdit final {
        Assets::AssetId asset;
        AI::BlackboardKeyId key;
        std::uint32_t timeoutTicks{};
    };

    /** @brief Sets a scalar override using a stable property declared by the installed provider. */
    struct DecisionTaskPropertyEdit final {
        AI::BehaviorTreePropertyId property;
        AI::BehaviorTreePropertyValue value;
    };

    /** @brief Identifies properties whose scalar values can use the shared Decision inspector edit path. */
    [[nodiscard]] constexpr bool CanEditDecisionTaskProperty(AI::BehaviorTreePropertyKind kind) noexcept {
        using enum AI::BehaviorTreePropertyKind;
        return kind == Boolean || kind == SignedInteger || kind == Scalar || kind == String;
    }

    /** @brief Exact installed descriptor selected by the workspace Task picker. */
    struct DecisionTaskSelection final {
        AI::DecisionNodeTypeId type;
        AI::DecisionProviderId provider;
        std::uint32_t version{};
        std::uint64_t catalogRevision{};
        bool operator==(const DecisionTaskSelection &) const = default;
    };

    /** @brief Detached exact saved source evidence from the active tree's explicit target review. */
    struct BehaviorTreeSubtreeReview final {
        Assets::AssetId asset;
        AI::DecisionGraphAssetId graph;
        std::uint32_t graphVersion{};
        Sha256Digest digest;
        Sha256Digest schemaDigest;
        std::uint64_t registryRevision{};
        std::uint64_t catalogRevision{};
        std::uint64_t documentRevision{};
        auto operator<=>(const BehaviorTreeSubtreeReview &) const = default;
    };

    /** @brief Revision-fenced tree edit using an authored node and optional explicit source evidence. */
    struct BehaviorTreeEditRequest final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
        BehaviorTreeEditOperation operation{BehaviorTreeEditOperation::WrapSequence};
        AI::DecisionNodeId node;
        std::optional<DecisionTaskSelection> task;
        std::optional<Assets::AssetId> subtreeTarget;
        std::optional<BehaviorTreeSubtreeReview> subtree;
        std::optional<BehaviorTreeQueryOutputEdit> queryOutput;
        std::optional<DecisionTaskPropertyEdit> property;
    };

    /** @brief Typed operations for the authored option and its complete scoring policy. */
    enum class UtilityGraphEditOperation : std::uint8_t {
        AddOption,
        SetOptionPolicy,
        SetTaskProperty,
        DeleteOption,
        AddConsideration,
        SetConsideration,
        DeleteConsideration,
        Undo,
        Redo,
        Save,
    };

    /** @brief An explicit scalar score selected from the exact saved Blackboard bytes shown in the editor. */
    struct UtilityScoreEdit final {
        AI::UtilityConsideration score;
        Sha256Digest schemaSource;
    };

    /** @brief Consideration rows follow semantic evaluation order; revision and prior key fence their identity. */
    struct UtilityConsiderationSlot final {
        std::size_t index{};
        AI::BlackboardKeyId priorKey;
    };

    /** @brief Revision-fenced Utility authoring intent. */
    struct UtilityGraphEditRequest final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
        UtilityGraphEditOperation operation{UtilityGraphEditOperation::AddOption};
        AI::DecisionNodeId option;
        std::optional<DecisionTaskSelection> task;
        std::optional<UtilityScoreEdit> score;
        std::optional<UtilityConsiderationSlot> slot;
        double priority{1};
        double compensation{};
        std::optional<DecisionTaskPropertyEdit> property;
    };

    /** @brief Exact revision held by an outstanding Decision document close prompt. */
    struct DecisionGraphDocumentFence final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
    };

    /** @brief Stable owner of a provider-bound action within an authored state. */
    enum class StateMachineTaskPhase : std::uint8_t {
        Entry,
        Update,
        Exit,
        Count,
    };

    /** @brief Locates a typed provider action without depending on a widget position. */
    [[nodiscard]] constexpr auto StateMachineTaskSlot(StateMachineTaskPhase phase) noexcept
        -> std::optional<AI::StateMachineProviderAction> AI::StateMachineState::* {
        using enum StateMachineTaskPhase;
        switch (phase) {
            case Entry:
                return &AI::StateMachineState::entryProvider;
            case Update:
                return &AI::StateMachineState::updateProvider;
            case Exit:
                return &AI::StateMachineState::exitProvider;
            default:
                return nullptr;
        }
    }

    /** @brief Reports whether ordinary topology commands support every state in this machine. */
    [[nodiscard]] bool CanEditStateMachineTopology(const AI::StateMachineAssetDescriptor &machine);

    /** @brief Revision-correlated authoring intent. Operands are semantic identities.
     * @details AddState takes an existing running source in node and atomically adds a state plus its incoming transition.
     * SetTransitionPriority takes an existing transition in node and an explicit signed priority.
     * RemoveState erases a non-initial leaf and its incident transitions, subject to compiler admission of the remaining graph. */
    struct StateMachineEditRequest final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
        StateMachineEditOperation operation{StateMachineEditOperation::AddState};
        AI::DecisionNodeId node;   /**< Source state, edited state, or edited transition depending on operation. */
        AI::DecisionNodeId target; /**< Target of a connection. */
        AI::DecisionStateTerminal terminal{AI::DecisionStateTerminal::None};
        std::optional<std::int32_t> priority{}; /**< Required signed authored value for SetTransitionPriority. */
        StateMachineTaskPhase taskPhase{StateMachineTaskPhase::Count};
        std::optional<DecisionTaskSelection> task;
        std::optional<DecisionTaskPropertyEdit> property;
    };
}  // namespace Horo::Editor
