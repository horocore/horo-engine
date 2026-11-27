#pragma once

/** @file EditorControlMetadata.h
 * @brief Non-installed observation seam for actual editor controls; no runtime tree, focus authority or native dispatch.
 */
#include "Horo/Runtime/Ui/UiAccessibility.h"

#include <array>
#include <memory>
#include <span>
#include <string_view>

struct ImGuiContext;
struct ImGuiContextHook;

namespace Horo::Editor::Detail {
    /** @brief Complete-frame capture outcome, separate from native accessibility capability. */
    enum class EditorControlMetadataStatus : std::uint8_t {
        NoFrame,
        Complete,
        CapacityExceeded,
        InvalidInput,
        Retired
    };

    /** @brief Whether the actual control interprets the supplied text using framework label/identity syntax. */
    enum class EditorControlLabelKind : std::uint8_t {
        FrameworkLabel,
        ResolvedText
    };

    /** @brief Owned-frame observation of one actual shared widget; source IDs never authorize native actions. */
    struct EditorControlMetadataRecord final {
        std::uint32_t window{}, item{};
        Runtime::Ui::UiAccessibilityRole role{};
        Runtime::Ui::UiAccessibilityState state;
        Runtime::Ui::UiAccessibilityTextRef name;
        Runtime::Ui::UiAccessibilityValue value;
        std::array<float, 4> bounds{};
        bool clipped{};
        bool valueRedacted{};
    };

    /** @brief Call-duration immutable observations; valid until the next completed frame or capture retirement.
     * @details These facts are not UiAccessibilitySnapshot or presentation evidence. Focus is observed from ImGui,
     * never independently stored or changed. Runtime UI still owns semantic identity, audience and native action admission.
     */
    struct EditorControlMetadataFrame final {
        std::uint32_t schemaVersion{Runtime::Ui::CurrentUiAccessibilitySchemaVersion};
        std::uint64_t sequence{};
        EditorControlMetadataStatus status{EditorControlMetadataStatus::NoFrame};
        std::span<const EditorControlMetadataRecord> controls;
        std::span<const char> text;
        std::uint32_t missingNames{};
        /** @brief Resolves only an in-bounds reference from this frame. @param reference Owned text range.
         * @return Call-duration text or empty for an absent/invalid range. */
        [[nodiscard]] std::string_view Text(Runtime::Ui::UiAccessibilityTextRef reference) const noexcept;
    };

    /** @brief GUI-owner-thread capture with fixed setup budgets and no registration-time allocations.
     * @details Explicitly binds one ImGui context. Destruction removes its hooks; context shutdown revokes the borrow first.
     * Two frame buffers retain copied labels through synchronous QA observation. Overflow exposes no partial control inventory.
     * No settings, widget values, focus, callbacks, native nodes or asynchronous work are owned by this capture.
     */
    class EditorControlMetadataCapture final {
    public:
        static constexpr std::size_t MaximumControls = 2'048;
        static constexpr std::size_t MaximumTextBytes = 128U * 1'024U;
        static constexpr std::size_t MaximumFieldBytes = 4'096;
        /** @brief Reserves both bounded frame stores at cold host setup. @throws std::bad_alloc If setup storage is unavailable. */
        EditorControlMetadataCapture();
        /** @brief Retires hooks before releasing copied frame storage. */
        ~EditorControlMetadataCapture();
        EditorControlMetadataCapture(const EditorControlMetadataCapture &) = delete;
        EditorControlMetadataCapture &operator=(const EditorControlMetadataCapture &) = delete;
        /** @brief Binds an actual context without replacing another capture or foreign user data.
         * @param context Actual framework context, or null for a headless host.
         * @return True only when this capture is bound. Rebinding revokes the previous context's observation hooks.
         */
        [[nodiscard]] bool Bind(ImGuiContext *context);
        /** @brief Removes only this capture's hooks; later framework callbacks cannot access this owner. */
        void Unbind() noexcept;
        /** @brief Borrows the last complete attempt, including explicit capacity/validation failure.
         * @return Frame facts, or NoFrame/Retired with no readable controls.
         */
        [[nodiscard]] EditorControlMetadataFrame Frame() const noexcept;

    private:
        struct Storage;
        friend void EditorControlMetadataHook(ImGuiContext *, ImGuiContextHook *);
        friend void ObserveEditorControl(Runtime::Ui::UiAccessibilityRole, const char *, Runtime::Ui::UiAccessibilityState,
                                         Runtime::Ui::UiAccessibilityValueInput, bool, EditorControlLabelKind) noexcept;
        std::unique_ptr<Storage> storage_;
    };

    /** @brief Context-local lifecycle adapter; its host address is shared with built-in GUI live reload. */
    void EditorControlMetadataHook(ImGuiContext *context, ImGuiContextHook *hook);
    /** @brief Copies actual last-item facts without changing widget behavior; absence of a bound host is a no-op.
     * @param role Control-produced semantic role. @param label Visible resolved label; ImGui ID suffixes are excluded.
     * @param extra Additional control-produced state. @param value Actual typed value; text is copied only when supplied.
     * @param valueRedacted Whether editable content was deliberately omitted for privacy.
     * @param labelKind Actual label protocol; resolved tooltip/content text retains literal hash characters.
     * @details Call immediately after the actual item, before submitting another widget. Input placeholders are never names.
     */
    void ObserveEditorControl(Runtime::Ui::UiAccessibilityRole role, const char *label, Runtime::Ui::UiAccessibilityState extra = {},
                              Runtime::Ui::UiAccessibilityValueInput value = {}, bool valueRedacted = false,
                              EditorControlLabelKind labelKind = EditorControlLabelKind::FrameworkLabel) noexcept;
    /** @brief Borrows only the capture explicitly bound to the current GUI context.
     * @return Last frame facts or NoFrame when this context has no capture. Never reads foreign ImGui user data.
     */
    [[nodiscard]] EditorControlMetadataFrame CurrentEditorControlMetadata() noexcept;
}  // namespace Horo::Editor::Detail
