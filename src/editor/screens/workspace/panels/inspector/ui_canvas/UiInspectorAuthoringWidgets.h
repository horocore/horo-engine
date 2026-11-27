#pragma once
/** @file UiInspectorAuthoringWidgets.h
 * @brief Shared controls for canonical initial state, localized references and accessibility authoring.
 */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Runtime/Ui/UiElementCatalog.h"
#include "UiInspectorDraft.h"

namespace Horo::Editor {
    /** @brief Retained UI fields for one immutable canonical localized reference, including all typed argument alternatives. */
    class UiInspectorMessageDraft final {
    public:
        /** @brief Capture a canonical source on a source/selection event. @param source Immutable optional message. */
        void Capture(const std::optional<Runtime::Ui::UiLocalizedText> &source);
        /** @brief Stage a message edit through shared localized controls.
         * @param context Editor presentation context. @param key Localized section identity, never an authored property key.
         * @param destination Last valid detached message. @return Whether a draft field changed.
         */
        bool Draw(const EditorGuiContext &context, const char *key, std::optional<Runtime::Ui::UiLocalizedText> &destination);

        /** @brief Whether retained text and canonical message construction allow Apply. @return Cached validity. */
        [[nodiscard]] bool IsValid() const noexcept {
            return valid_;
        }

    private:
        /** @brief Capture exact text for one argument on a selection/type event. @param index Selected index or -1. */
        void SelectArgument(int index);
        /** @brief Edit bounded typed arguments. @param context Shared GUI context. @return Whether a field changed. */
        bool DrawArguments(const EditorGuiContext &context);
        /** @brief Draw the canonical argument alternative. @param context Shared context. @param argument Detached argument. @return Edit.
         */
        bool DrawArgumentValue(const EditorGuiContext &context, Runtime::Ui::UiLocalizedArgument &argument);
        /** @brief Check retained numeric validity. @return Whether selection can change without discarding invalid text. */
        bool ArgumentTextValid() const noexcept;
        /** @brief Construct an immutable canonical reference before replacement. @param destination Last valid reference. @return Validity.
         */
        bool Build(std::optional<Runtime::Ui::UiLocalizedText> &destination);
        bool enabled_{};
        bool valid_{true};
        std::array<char, Runtime::Ui::MaximumUiMessageKeyBytes + 1> namespace_{}, key_{};
        std::array<char, Runtime::Ui::MaximumUiLocalizedFallbackTextBytes + 1> fallback_{};
        Runtime::Ui::UiLocalizedTextFailurePolicy policy_{Runtime::Ui::UiLocalizedTextFailurePolicy::UseFallback};
        std::vector<Runtime::Ui::UiLocalizedArgument> arguments_;
        int selected_{-1};
        std::array<char, Runtime::Ui::MaximumUiLocalizedArgumentNameBytes + 1> argumentName_{};
        std::array<char, Runtime::Ui::MaximumUiLocalizedValueBytes + 1> argumentText_{};
        std::array<UiInspectorInteger, 2> integers_;
        UiInspectorDouble number_;
    };

    /** @brief Inspector-owned authoring buffers; canonical publication remains with the document command owner. */
    class UiInspectorAuthoringWidgets final {
    public:
        /** @brief Capture source text/numbers once. @param source Immutable authored presentation. */
        void Capture(const Runtime::Ui::UiElementPresentation &source);
        /** @brief Draw canonical state/localization/accessibility controls without mutating document storage.
         * @param context Shared context. @param kind Selected core kind. @param value Detached canonical record.
         * @return Whether fields changed, including invalid retained text.
         */
        bool Draw(const EditorGuiContext &context, Runtime::Ui::UiElementKind kind, Runtime::Ui::UiElementPresentation &value);
        /** @brief Checks all retained text without reparsing on unchanged frames. @return Whether Apply is permitted locally. */
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        /** @brief Capture initial buffers on source/type changes. @param state Canonical initial state. */
        void CaptureInitial(const Runtime::Ui::UiElementAuthoredState &state);
        /** @brief Stage initial state. @param context Shared context. @param kind Core kind. @param state Detached state. @return Edit. */
        bool DrawState(const EditorGuiContext &context, Runtime::Ui::UiElementKind kind, Runtime::Ui::UiElementAuthoredState &state);
        /** @brief Stage semantic metadata. @param context Shared context. @param kind Core kind. @param value Detached metadata. @return
         * Edit. */
        bool DrawAccessibility(const EditorGuiContext &context, Runtime::Ui::UiElementKind kind,
                               std::optional<Runtime::Ui::UiElementAccessibility> &value);
        UiInspectorMessageDraft content_, name_, description_;
        std::array<UiInspectorDouble, 4> scalar_;
        std::array<char, Runtime::Ui::MaximumUiActionTextBytes + 1> initialText_{};
        bool initialTextValid_{true};
        std::size_t initialKind_{};
    };
}  // namespace Horo::Editor
