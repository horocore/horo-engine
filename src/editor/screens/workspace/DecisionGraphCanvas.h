#pragma once

#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "editor/screens/workspace/DecisionGraphLayout.h"

#include <memory>
#include <optional>
#include <span>

namespace Horo::Editor {
    struct EditorGuiContext;
    struct DecisionGraphWorkspaceDocument;

    /** @brief Canvas selection and at most one validated edit intent for the ordinary Decision route. */
    struct DecisionCanvasInteraction final {
        std::optional<AI::DecisionNodeId> node;
        std::optional<AI::DecisionNodeId> transition;
        std::optional<StateMachineEditRequest> edit;
        std::optional<DecisionGraphLayoutRequest> layout;
    };

    /** @brief Workspace-owned presentation adapter. Vendor types stay in the implementation. */
    class DecisionGraphCanvas final {
    public:
        DecisionGraphCanvas();
        ~DecisionGraphCanvas();
        DecisionGraphCanvas(const DecisionGraphCanvas &) = delete;
        DecisionGraphCanvas &operator=(const DecisionGraphCanvas &) = delete;

        /** @brief Renders one immutable graph using its revision-correlated native presentation session. */
        [[nodiscard]] DecisionCanvasInteraction Draw(const DecisionGraphWorkspaceDocument &document, const EditorGuiContext &context);
        /** @brief Retires presentation sessions for closed document instances. */
        void Retain(std::span<const DecisionGraphWorkspaceDocument> documents);

    private:
        struct Impl;
        std::unique_ptr<Impl> state_;
    };
}  // namespace Horo::Editor
