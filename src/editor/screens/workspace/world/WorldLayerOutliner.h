#pragma once

/** @file WorldLayerOutliner.h
 * @brief Read-only world-layer presentation emitting revision-fenced authoring intent.
 */

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/PartitionedWorldWorkspace.h"

namespace Horo::Editor {
    struct EditorWorkspaceViewCommandData;
    /** @brief One host-routed action; creation opens the host-owned transient modal. */
    enum class WorldLayerOutlinerAction : std::uint8_t {
        None,
        Execute,
        Undo,
        Redo,
        Create
    };

    /** @brief Intent from a captured immutable publication; the controller owns execution and error presentation. */
    struct WorldLayerOutlinerIntent final {
        WorldLayerOutlinerAction action{};
        WorldDocumentFence expected;
        std::optional<WorldLayerCommand> command;
    };

    /** @brief Routes a presentation intent into the existing controller/screen command boundary.
     * @param intent At most one captured outliner action. @param command Existing host output; unchanged for None.
     */
    void RouteWorldLayerOutlinerIntent(WorldLayerOutlinerIntent intent, EditorWorkspaceViewCommandData &command);

    /** @brief Draws shared themed controls for exact authored order, filtering and current context.
     * @param snapshot Current immutable owner publication. @param context Localized GUI services.
     * @return At most one typed intent; no document or history mutation occurs in drawing.
     */
    [[nodiscard]] WorldLayerOutlinerIntent DrawWorldLayerOutliner(const PartitionedWorldSnapshot &snapshot,
                                                                  const EditorGuiContext &context);
}  // namespace Horo::Editor
