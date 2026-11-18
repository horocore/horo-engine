#pragma once

#include "Horo/AI/DecisionIdentity.h"
#include "editor/screens/workspace/DecisionStateMachineAuthoring.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Horo::AI {
    struct DecisionGraphSource;
}  // namespace Horo::AI

namespace Horo::Editor {

    /** @brief Neutral role labels used for authored nodes without a display name. */
    enum class GraphNodeRole : std::uint8_t {
        Root,
        Sequence,
        Selector,
        Composite,
        Decorator,
        Service,
        Task,
        Unknown,
        RunningState,
        SucceededState,
        FailedState,
        UtilityOption,
    };

    /** @brief Distinguishes authored transitions from projected hierarchy or score edges. */
    enum class GraphLinkRole : std::uint8_t {
        Child,
        Service,
        Transition,
        Hierarchy,
        UtilityOption
    };

    /** @brief Stable pin endpoint within a projected graph, independent of the widget identity. */
    struct GraphPinView final {
        AI::DecisionNodeId node;
        std::uint64_t authoredPin{}; /**< Original pin identity for Behavior Tree assets. */
        bool output{};
        auto operator<=>(const GraphPinView &) const = default;
    };

    /** @brief Presentation-only node and its bounded pin range. */
    struct GraphNodeView final {
        AI::DecisionNodeId id;
        GraphNodeRole role{GraphNodeRole::Unknown};
        std::string label;
        std::size_t firstPin{};
        std::size_t pinCount{};
        std::optional<std::size_t> incomingOrder;
        bool initial{};
        bool missing{};
    };

    /** @brief Stable link and the two projected endpoint indices it connects. */
    struct GraphLinkView final {
        GraphLinkRole role{GraphLinkRole::Child};
        std::uint64_t authoredId{};
        std::size_t from{};
        std::size_t to{};
    };

    /** @brief Bounded canonical graph presented by an ordinary authored Decision document revision. */
    struct GraphViewSnapshot final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
        AI::DecisionPlanKind family{AI::DecisionPlanKind::StateMachine};
        bool editable{};
        std::vector<GraphNodeView> nodes;
        std::vector<GraphPinView> pins;
        std::vector<GraphLinkView> links;
    };

    /** @brief Link creation between two stable source endpoints. */
    struct GraphConnectIntent final {
        GraphPinView first;
        GraphPinView second;
    };

    /** @brief Removal of a stable authored link. */
    struct GraphRemoveLinkIntent final {
        GraphLinkRole role;
        std::uint64_t id{};
    };

    /** @brief Widget-neutral graph edit intent correlated to the exact projected document revision. */
    struct GraphEditCommand final {
        DecisionGraphDocumentFence fence;
        std::variant<GraphConnectIntent, GraphRemoveLinkIntent> operation;
    };

    /** @brief Projects immutable family-specific sources without invoking a provider or compiler. */
    [[nodiscard]] GraphViewSnapshot ProjectDecisionCanvas(const AI::DecisionGraphSource &source, DecisionGraphDocumentFence fence,
                                                          bool editable);
    /** @brief Maps supported visual intents into the canonical typed Decision document command. */
    [[nodiscard]] std::optional<StateMachineEditRequest> ResolveDecisionCanvasEdit(const GraphViewSnapshot &snapshot,
                                                                                   const GraphEditCommand &intent);
}  // namespace Horo::Editor
