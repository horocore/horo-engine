#pragma once

/** @file UiCanvasPreview.h
 * @brief Session-owned device preview and deterministic visual layout gestures.
 */
#include "Horo/Editor/UiCanvasDocument.h"
#include "Horo/Editor/UiInspectorResources.h"
#include "Horo/Runtime/Ui/UiLayout.h"
#include "Horo/Runtime/Ui/UiPresentationProfiles.h"

#include <array>

namespace Horo::Editor {
    enum class UiCanvasPreviewDevice : std::uint8_t {
        Desktop,
        Phone,
        Tablet,
        Count
    };
    enum class UiCanvasLayoutTool : std::uint8_t {
        Select,
        Move,
        Resize,
        Anchors,
        Count
    };

    /** @brief Workspace values only; none enters a source document or undo history. */
    struct UiCanvasPreviewState final {
        UiCanvasPreviewDevice device{UiCanvasPreviewDevice::Desktop};
        bool rotated{};
        std::uint32_t zoomPercent{100};
        Runtime::Ui::UiLogicalPoint pan;
        Runtime::Ui::UiScalar grid{8 * 64};
        bool snap{true};
        UiCanvasLayoutTool tool{UiCanvasLayoutTool::Select};
        std::array<Runtime::Ui::UiScalar, 8> horizontalGuides{};
        std::array<Runtime::Ui::UiScalar, 8> verticalGuides{};
        std::uint8_t horizontalGuideCount{};
        std::uint8_t verticalGuideCount{};
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const UiCanvasPreviewState &) const noexcept = default;
    };

    struct UiCanvasPreviewBox final {
        Runtime::Ui::UiElementId element;
        Runtime::Ui::UiLogicalRect bounds;
    };

    /** @brief Copied actual layout output; a failed refresh discards stale interaction geometry. */
    struct UiCanvasPreview final {
        Runtime::Ui::UiCanvasViewportEvidence viewport;
        Runtime::Ui::UiResolvedScreenCanvas canvas;
        std::optional<Runtime::Ui::UiPresentationProfileId> profile;
        std::vector<UiCanvasPreviewBox> boxes;
    };

    /** @brief Resolve device evidence and run the Runtime UI layout authority outside draw work.
     * @param source Canonical immutable authored document. @param state Session preview values.
     * @param ownership Exact document workspace incarnation, isolated from game runtime ownership.
     * @return Evaluated boxes or the original typed resolution/layout failure.
     */
    [[nodiscard]] Result<UiCanvasPreview> BuildUiCanvasPreview(const Runtime::Ui::UiDocument &source, const UiCanvasPreviewState &state,
                                                               DocumentInstanceId ownership,
                                                               const UiInspectorResourceSnapshot *resources = nullptr);

    /** @brief Convert a completed logical drag into one canonical presentation transaction.
     * @param element Current authored element. @param box Actual preview box at gesture start.
     * @param delta Signed logical drag, independent of DPI and zoom. @param state Current tool and snap policy.
     * @return Owned command preserving unrelated typed presentation, or an invalid/overflow failure.
     */
    [[nodiscard]] Result<UiCanvasSetPresentation> PrepareUiCanvasLayoutGesture(const Runtime::Ui::UiDocumentElement &element,
                                                                               Runtime::Ui::UiLogicalRect box,
                                                                               Runtime::Ui::UiLogicalPoint delta,
                                                                               Runtime::Ui::UiLogicalExtent parentExtent,
                                                                               const UiCanvasPreviewState &state);
    /** @brief Execute a completed visual gesture under source and selection-publication fences.
     * @param owner Canonical session. @param state Exact gesture source state.
     * @param expectedSelection Captured selection publication. @param currentSelection Current workspace publication.
     * @param command Complete canonical presentation edit.
     * @return Typed stale/closed failure or the single canonical transaction result.
     */
    [[nodiscard]] Result<void> ExecuteUiCanvasLayoutGesture(UiCanvasDocument &owner, UiCanvasDocumentStateId state,
                                                            std::uint64_t expectedSelection, std::uint64_t currentSelection,
                                                            const UiCanvasSetPresentation &command);
}  // namespace Horo::Editor
