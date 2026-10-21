#pragma once
/** @file PCGAuthoringInspector.h @brief Transient shared-control inspector presentation. */
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/PCGGraphDocument.h"

#include <map>
#include <set>

namespace Horo::Editor {
    class ILocalizationService;

    /** @brief Bounded numeric composition buffers and one cancellable gesture owned by the workspace view. */
    struct PCGAuthoringInspectorState final {
        std::optional<PCGGraphAuthoringDraft> draft;
        std::map<std::string, std::string, std::less<>> text;
        std::set<std::string, std::less<>> invalid;
        std::optional<Error> error;
    };

    /** @brief Draws graph policy, exposed defaults and selected exact-schema properties.
     * @param state Workspace-only gesture. @param current Exact source. @param catalog Current executable schemas.
     * @param selection Stable selected identities. @param canEdit Writable route grant.
     * @param localization Shared localized copy. @param fonts Shared typography.
     * @return One coalesced command only on explicit Apply; cancel submits nothing. */
    [[nodiscard]] std::optional<PCGDocumentCommand> DrawPCGAuthoringInspector(
        PCGAuthoringInspectorState &state, const PCGDocumentSnapshot &current, const PCG::PCGNodeCatalogSnapshot &catalog,
        std::span<const PCG::NodeId> selection, bool canEdit, const ILocalizationService &localization, const Theme::Fonts &fonts);
}  // namespace Horo::Editor
