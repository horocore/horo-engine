#pragma once

/** @file UiCanvasInspectorPanel.h
 * @brief Transient Runtime UI inspector surface; canonical source/history stay in UiCanvasDocument.
 */
#include "UiInspectorAuthoringWidgets.h"
#include "UiInspectorWidgets.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

namespace Horo::Editor {
    /** @brief Detached ID text; no string is used as an authored property key. */
    struct UiInspectorIdText final {
        std::array<char, 37> bytes{};
        bool valid{true};
    };

    /** @brief Owner-thread inspector draft and bounded shared controls; retains no source pointers. */
    class UiCanvasInspectorPanel final {
    public:
        /** @brief Releases draft admission and all source leases on document detach/close. */
        void Clear() noexcept;
        /** @brief Draws a selected canonical element and emits one fenced command on Apply.
         * @param source Immutable cached host projection. @param context Shared design/localization context.
         * @param output Typed gesture receiver; drawing never mutates the document owner.
         */
        void Draw(const UiCanvasWorkspaceDocument &source, const EditorGuiContext &context, EditorWorkspaceViewCommandData &output);

    private:
        /** @brief Select a real prepared namespace without requiring an active theme or selected element.
         * @param source Cached actual project themes/fences. @param context Shared controls. @param output Host command.
         * @return Whether a theme preparation command was emitted; no I/O or registry construction occurs here.
         */
        bool DrawTheme(const UiCanvasWorkspaceDocument &source, const EditorGuiContext &context, EditorWorkspaceViewCommandData &output);
        /** @brief Capture the exact host source and reset transient values. @param source Cached source. @param context Exact fences. */
        void Capture(const UiCanvasWorkspaceDocument &source, UiInspectorContext context);
        /** @brief Restore the captured baseline and reset text without mutating authored state. */
        void Reset();
        /** @brief Select one bounded override while preserving staged layout. @param index Index, or -1 for none. */
        void SelectProperty(int index);
        /** @brief Copy the current typed property into the detached canonical-shaped draft only. */
        void StoreProperty();
        /** @brief Capture class identity text on selection changes. @param index Index, or -1 for none. */
        void SelectClass(int index);
        /** @brief Cache local typed and retained-text validity on input events. */
        void Validate();
        /** @brief Draw detached fields with actual capability availability. @param context Shared context.
         * @param stylesAvailable Whether the real style producer is available. @return Whether any retained field changed.
         */
        bool DrawFields(const EditorGuiContext &context, bool stylesAvailable);
        /** @brief Draw registry namespace IDs. @param context Shared controls. @return Whether values changed. */
        bool DrawResources(const EditorGuiContext &context);
        /** @brief Draw bounded class references. @param context Shared controls. @return Whether values changed. */
        bool DrawClasses(const EditorGuiContext &context);
        /** @brief Draw typed literal/token overrides. @param context Shared controls. @return Whether values changed. */
        bool DrawStyle(const EditorGuiContext &context);
        /** @brief Edit the selected literal/token value. @param context Shared controls. @return Whether values changed. */
        bool DrawStyleValue(const EditorGuiContext &context);
        /** @brief Draw intrinsic sizing policy. @param context Shared controls. @return Whether values changed. */
        bool DrawIntrinsic(const EditorGuiContext &context);
        /** @brief Emit an owned fenced request. @param command Canonical command. @param output Host receiver. */
        void Emit(const UiCanvasSetPresentation &command, EditorWorkspaceViewCommandData &output) const;

        std::optional<UiInspectorContext> context_;
        Runtime::Ui::UiElementPresentation baseline_;
        Runtime::Ui::UiElementPresentation presentation_;
        UiInspectorDraft draft_;
        UiInspectorAuthoringWidgets authoring_;
        Runtime::Ui::UiDocumentElement sourceElement_;
        UiInspectorWidgets::Buffers layoutText_;
        UiInspectorWidgets::Buffers styleText_;
        std::array<UiInspectorInteger, 2> intrinsicText_;
        UiInspectorIdText styleAsset_, typeAsset_, typeClass_, classAsset_, classId_, tokenAsset_, tokenId_, newProperty_;
        Runtime::Ui::UiStylePropertyId pendingProperty_;
        std::vector<UiInspectorIdText> propertyLabels_, classLabels_;
        int propertyIndex_{-1};
        int classIndex_{-1};
        bool dirty_{};
        bool valid_{};
        bool hadPresentation_{};
    };
}  // namespace Horo::Editor
