#pragma once

#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "editor/screens/workspace/DecisionGraphLayout.h"

#include <memory>
#include <optional>
#include <span>

namespace Horo::AI {
    class DecisionBlackboardProviderMetadata;
}  // namespace Horo::AI

namespace Horo::Editor {
    struct EditorGuiContext;
    struct DecisionGraphWorkspaceDocument;

    /** @brief Canvas selection and at most one validated edit intent for the ordinary Decision route. */
    struct DecisionCanvasInteraction final {
        std::optional<AI::DecisionNodeId> node;
        std::optional<AI::DecisionNodeId> transition;
        std::optional<AI::DecisionNodeId> revealed; /**< Focus destination confirmed in the visible native canvas. */
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

        /** @brief Renders one immutable graph using its revision-correlated native presentation session.
         * @param document Exact authored document.
         * @param context Editor presentation dependencies.
         * @param providers Current detached Project provider catalog, when available.
         * @return Typed selection and edit intents owned by this frame.
         */
        [[nodiscard]] DecisionCanvasInteraction Draw(const DecisionGraphWorkspaceDocument &document, const EditorGuiContext &context,
                                                     const std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> &providers = {});
        /** @brief Reveals one stable selected node in the exact current document revision. */
        void RequestFocus(DecisionGraphDocumentFence fence, AI::DecisionNodeId node);
        /** @brief Retires presentation sessions for closed document instances. */
        void Retain(std::span<const DecisionGraphWorkspaceDocument> documents);

    private:
        struct Impl;
        std::unique_ptr<Impl> state_;
    };
}  // namespace Horo::Editor
