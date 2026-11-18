#pragma once

#include "Horo/AI/DecisionControl.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <optional>

namespace Horo::Editor {
    /** @brief One stable-ID edit or lifecycle action on an intrinsic State Machine draft. */
    enum class StateMachineEditOperation : std::uint8_t {
        AddState,
        SetTerminal,
        Connect,
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
        MoveEarlier,
        MoveLater,
        DeleteTask,
        Undo,
        Redo,
        Save,
    };

    /** @brief Exact installed descriptor selected by the workspace Task picker. */
    struct DecisionTaskSelection final {
        AI::DecisionNodeTypeId type;
        AI::DecisionProviderId provider;
        std::uint32_t version{};
        std::uint64_t catalogRevision{};
    };

    /** @brief Revision-fenced tree edit using an authored node and an optional explicit Task contract. */
    struct BehaviorTreeEditRequest final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
        BehaviorTreeEditOperation operation{BehaviorTreeEditOperation::WrapSequence};
        AI::DecisionNodeId node;
        std::optional<DecisionTaskSelection> task;
    };

    /** @brief Exact revision held by an outstanding Decision document close prompt. */
    struct DecisionGraphDocumentFence final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
    };

    /** @brief Revision-correlated authoring intent. Operands are semantic identities.
     * @details AddState takes an existing running source in node and atomically adds a state plus its incoming transition.
     * RemoveState erases a non-initial leaf and its incident transitions, subject to compiler admission of the remaining graph. */
    struct StateMachineEditRequest final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
        StateMachineEditOperation operation{StateMachineEditOperation::AddState};
        AI::DecisionNodeId node;   /**< Source state, edited state, or removed transition depending on operation. */
        AI::DecisionNodeId target; /**< Target of a connection. */
        AI::DecisionStateTerminal terminal{AI::DecisionStateTerminal::None};
    };
}  // namespace Horo::Editor
