#pragma once
/** @file PCGWorkspace.h @brief Workspace presentation and typed intents for the canonical PCG document. */
#include "Horo/Editor/PCGGraphDocument.h"
#include "Horo/Editor/PCGPreviewSession.h"
#include "Horo/PCG/PCGNodeCatalog.h"

#include <array>

namespace Horo::Editor {
    class IWorkspacePanel;
    inline constexpr std::string_view PCGGraphPanelId = "horo.pcg_graph";
    /** @brief Creates the registered PCG document presenter; attach/detach owns no document lifetime.
     * @return Inert panel ready for workspace registration. */
    [[nodiscard]] std::shared_ptr<IWorkspacePanel> CreatePCGGraphWorkspacePanel();

    /** @brief Inert host-provided creation recipe; identities are allocated by the command adapter. */
    struct PCGPaletteEntry final {
        PCG::PCGCpuNodeKind kind{};
        PCG::PCGGraphNode prototype;
        std::string labelKey;
    };

    /** @brief Explicit editor composition; source support and creation recipes share one catalog generation. */
    struct PCGEditorCatalog final {
        PCGDocumentCatalog document;
        std::vector<PCGPaletteEntry> palette;
        PCG::PCGNodeCatalogSnapshot executable;
    };

    /** @brief Composes repository-owned schemas without activating evaluators or selecting a backend.
     * @return Catalog and canonical payload recipes, or a typed composition failure. */
    [[nodiscard]] Result<PCGEditorCatalog> CreatePCGEditorCatalog();

    /** @brief Presentation position in unscaled graph coordinates; never serialized in source. */
    struct PCGNodePlacement final {
        PCG::NodeId node;
        Math::Vec2 position;
    };

    /** @brief Bounded per-session workspace state, separate from source and semantic history. */
    struct PCGCanvasState final {
        std::vector<PCGNodePlacement> placements;
        std::vector<PCG::NodeId> selection;
        Math::Vec2 pan;
        float zoom{1.0F};
        std::string filter;
        std::optional<PCG::NodeId> connectingNode;
        std::optional<PCG::PinId> connectingPin;
        std::optional<PCG::NodeId> draggingNode;
        Math::Vec2 dragOrigin;
    };

    /** @brief Immutable source lease plus workspace-only presentation and actionable failure. */
    struct PCGWorkspaceDocument final {
        PCGDocumentSnapshot snapshot;
        PCGCanvasState canvas;
        std::optional<Error> error;
        PCGPreviewSnapshot preview;
        Math::Aabb previewRegion{{-8, -8, -8}, {8, 8, 8}};
        bool previewAvailable{};
    };
    /** @brief Closed canvas actions; semantic actions always use the canonical executor. */
    enum class PCGEditorAction : std::uint8_t {
        Present,
        CreateNode,
        RemoveSelection,
        Connect,
        Disconnect,
        Undo,
        Redo,
        Save,
        PreviewRefresh,
        PreviewCancel
    };

    /** @brief One exact-frame intent. Connection gestures submit no document operations until completion. */
    struct PCGEditorRequest final {
        PCGEditorAction action{};
        PCGDocumentCommand fence;
        PCGCanvasState presentation;
        PCG::NodeTypeId type;
        PCG::NodeId sourceNode;
        PCG::NodeId targetNode;
        PCG::PinId sourcePin;
        PCG::PinId targetPin;
        PCG::EdgeId edge;
    };

    /** @brief Synchronizes presentation after an immutable publication, dropping stale semantic references.
     * @param document Existing projection. @param snapshot Current canonical lease. */
    void RefreshPCGCanvas(PCGWorkspaceDocument &document, const PCGDocumentSnapshot &snapshot);
    /** @brief Cancels workspace-only gestures, restoring the drag origin without source/history changes.
     * @param state Workspace-owned gesture state. */
    void CancelPCGCanvasGesture(PCGCanvasState &state);
    class ILocalizationService;

    namespace Theme {
        struct Fonts;
    }

    /** @brief Draws the real graph surface from immutable source using shared controls and localized labels.
     * @param document Exact source projection. @param palette Host creation recipes.
     * @param localization Shared catalog. @param fonts Shared typography.
     * @return Fenced input intent, or no change for an idle frame. */
    [[nodiscard]] std::optional<PCGEditorRequest> DrawPCGGraphEditor(const PCGWorkspaceDocument &document,
                                                                     std::span<const PCGPaletteEntry> palette,
                                                                     const ILocalizationService &localization, const Theme::Fonts &fonts);
    /** @brief Checks finite, bounded presentation against one source without mutating either.
     * @param state Detached presentation intent. @param source Exact immutable source.
     * @return Zero-mutation validation result. */
    [[nodiscard]] Result<void> ValidatePCGCanvas(const PCGCanvasState &state, const PCG::PCGGraphAsset &source);
    /** @brief Expands one edit into a bounded canonical transaction with stable fresh identities.
     * @param request Fenced typed intent. @param source Current immutable source. @param palette Host recipes.
     * @return Complete operations or rejection; validation/publication remains document-owned. */
    [[nodiscard]] Result<PCGDocumentCommand> PreparePCGEdit(const PCGEditorRequest &request, const PCG::PCGGraphAsset &source,
                                                            std::span<const PCGPaletteEntry> palette);
}  // namespace Horo::Editor
