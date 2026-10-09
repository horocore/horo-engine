#pragma once

/** @file UiTextEditing.h
 * @brief Bounded owner-thread Unicode editing, selection, clipboard values and history.
 */

#include "Horo/Runtime/Ui/UiActions.h"

#include <array>
#include <cstdint>

namespace Horo::Runtime::Ui {
    /** @brief Absolute number of undoable edits retained by one text buffer. */
    inline constexpr std::uint16_t MaximumUiTextUndoDepth = 16;

    /** @brief Closed validation policies; empty drafts remain valid for every policy. */
    enum class UiTextValidation : std::uint8_t {
        Any,
        SingleLine,
        AsciiDigits,
        Count
    };

    /** @brief Immutable edit bounds, validation and password presentation policy. */
    struct UiTextEditPolicy final {
        std::uint16_t maximumBytes{MaximumUiActionTextBytes};
        std::uint16_t maximumGraphemes{MaximumUiActionTextBytes};
        std::uint16_t undoDepth{MaximumUiTextUndoDepth}; /**< Zero explicitly disables history. */
        UiTextValidation validation{UiTextValidation::Any};
        bool password{}; /**< Masks display and rejects copying/cutting secrets to clipboard. */

        /** @brief Checks finite bounds and known validation policy. @return Whether usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const UiTextEditPolicy &) const noexcept = default;
    };

    /** @brief Validates a complete value under explicit edit bounds and Unicode policy.
     * @param text Owned candidate UTF-8 value.
     * @param policy Immutable validation and capacity policy.
     * @return Success or typed malformed, validation or capacity failure; no mutation occurs.
     */
    [[nodiscard]] Result<void> ValidateUiTextEditValue(const UiActionText &text, const UiTextEditPolicy &policy);

    /** @brief Logical extended-grapheme positions, independent of glyph/ligature and byte offsets. */
    struct UiTextSelection final {
        std::uint16_t anchor{};
        std::uint16_t caret{};
        [[nodiscard]] bool operator==(const UiTextSelection &) const noexcept = default;
    };

    /** @brief Owned semantic text and selection; contains no native input/composition state. */
    struct UiTextEditSnapshot final {
        UiActionText text;
        UiTextSelection selection;
        std::uint16_t graphemeCount{};
    };

    /** @brief Typed editing operation; Previous/Next are logical, not visual bidi navigation. */
    enum class UiTextEditKind : std::uint8_t {
        Insert,
        Backspace,
        DeleteForward,
        Previous,
        Next,
        Home,
        End,
        Select,
        SelectAll,
        Copy,
        Cut,
        Paste,
        Undo,
        Redo,
        Count
    };

    /** @brief Synchronously borrowed command; irrelevant payload fields must be empty. */
    struct UiTextEditCommand final {
        UiTextEditKind kind{UiTextEditKind::Insert};
        UiActionText text;         /**< Insert/Paste bytes copied from normalized input or host clipboard. */
        UiTextSelection selection; /**< Select target, expressed in complete grapheme positions. */
        bool extendSelection{};    /**< Previous/Next/Home/End retain anchor when true. */
    };

    /** @brief Clipboard output is a value only; the host performs optional platform I/O outside frame work. */
    struct UiTextEditResult final {
        bool textChanged{};
        std::optional<UiActionText> clipboard; /**< Present for Copy/Cut, including an empty selection. */
    };

    /** @brief Owned display text; password mode emits one U+2022 per grapheme, never secret bytes. */
    struct UiTextEditDisplay final {
        std::array<char, MaximumUiActionTextBytes * 3> bytes{};
        std::uint16_t size{};

        /** @brief Borrows display bytes until this value is destroyed/modified. @return UTF-8 display text. */
        [[nodiscard]] std::string_view View() const noexcept {
            return {bytes.data(), size};
        }
    };

    /**
     * @brief Runtime UI-owned fixed-storage edit model, serialized on its owner thread.
     * @details Successful operations allocate nothing, perform no I/O and invoke no callback.
     * Copies are independent semantic values, never live native input sessions. Owners fence their
     * generation before Apply; UiControlStateMachine supplies that boundary for runtime controls.
     * Closed buffers reject reads/edits. Structural reload resets history; an interaction-only
     * projection may copy the model while fencing the previous source identity.
     */
    class UiTextEditBuffer final {
    public:
        /** @brief Validates initial text and reserves inline history.
         * @param policy Immutable bounds, validation and password policy.
         * @param initial Complete UTF-8 initial value.
         * @return Buffer or typed text/policy/capacity failure.
         */
        [[nodiscard]] static Result<UiTextEditBuffer> Create(const UiTextEditPolicy &policy, const UiActionText &initial);
        /** @brief Validates and atomically applies one bounded command.
         * @param command Typed edit, normalized text or host clipboard copy.
         * @return Changed/clipboard outcome or typed failure; rejection preserves all state/history.
         */
        [[nodiscard]] Result<UiTextEditResult> Apply(const UiTextEditCommand &command);
        /** @brief Copies current semantic state. @return Owned state or lifecycle failure. */
        [[nodiscard]] Result<UiTextEditSnapshot> Snapshot() const;
        /** @brief Copies display-safe text. @return Plain/masked text or lifecycle failure. */
        [[nodiscard]] Result<UiTextEditDisplay> Display() const;
        /** @brief Reconciles an owner value and clears selection/history at an explicit safe point.
         * @param text Validated provider/reload/cancel value.
         * @return Success or typed failure, preserving state on rejection.
         */
        [[nodiscard]] Result<void> Reset(const UiActionText &text);
        /** @brief Closes admission and erases retained semantic and history bytes; idempotent. */
        void Shutdown() noexcept;

    private:
        UiTextEditBuffer() = default;
        /** @brief Atomically replaces a validated complete grapheme range. */
        [[nodiscard]] Result<UiTextEditResult> Replace(const UiActionText &text, UiTextSelection range);
        /** @brief Validates and applies an explicit complete-grapheme selection. */
        [[nodiscard]] Result<UiTextEditResult> SelectRange(const UiTextEditCommand &command);
        /** @brief Replaces selected text or deletes one adjacent complete grapheme. */
        [[nodiscard]] Result<UiTextEditResult> EditRange(const UiTextEditCommand &command);
        /** @brief Retains the bounded undo horizon and discards the redo branch. */
        void PushUndo(const UiTextEditSnapshot &previous) noexcept;
        /** @brief Produces owned clipboard output, removing the selection only for Cut. */
        [[nodiscard]] Result<UiTextEditResult> Clipboard(UiTextEditKind kind);
        /** @brief Transfers one complete snapshot between bounded history stacks. */
        [[nodiscard]] UiTextEditResult RestoreHistory(UiTextEditKind kind) noexcept;
        /** @brief Moves/collapses/extends a selection on complete logical grapheme boundaries. */
        void Navigate(const UiTextEditCommand &command) noexcept;
        UiTextEditPolicy policy_;
        UiTextEditSnapshot state_;
        std::array<UiTextEditSnapshot, MaximumUiTextUndoDepth> undo_{};
        std::array<UiTextEditSnapshot, MaximumUiTextUndoDepth> redo_{};
        std::uint16_t undoCount_{};
        std::uint16_t redoCount_{};
        bool closed_{};
    };
}  // namespace Horo::Runtime::Ui
