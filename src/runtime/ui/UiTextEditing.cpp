#include "Horo/Runtime/Ui/UiTextEditing.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <utf8proc.h>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Complete extended-grapheme boundaries with the terminal byte offset. */
        struct Boundaries final {
            std::array<std::uint16_t, MaximumUiActionTextBytes + 1> offsets{};
            std::uint16_t count{};
        };

        /** @brief Decodes complete UTF-8 and segments with the same pinned Unicode library as shaping. */
        Result<Boundaries> Segment(const UiActionText &text) {
            if (!text.IsValid())
                return Result<Boundaries>::Failure(MakeError(UiErrors::TextInputInvalid));
            Boundaries result;
            utf8proc_int32_t previous{}, state{};
            std::uint16_t offset{};
            while (offset < text.size) {
                utf8proc_int32_t scalar{};
                const auto length =
                    utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.bytes.data() + offset), text.size - offset, &scalar);
                if (length <= 0)
                    return Result<Boundaries>::Failure(MakeError(UiErrors::TextInputInvalid));
                if (offset == 0 || utf8proc_grapheme_break_stateful(previous, scalar, &state))
                    result.offsets[result.count++] = offset;
                previous = scalar;
                offset = static_cast<std::uint16_t>(offset + length);
            }
            result.offsets[result.count] = text.size;
            return Result<Boundaries>::Success(result);
        }

        /** @brief Tests one decoded scalar against the closed complete-draft validation policy. */
        bool AllowsScalar(const UiTextValidation validation, const utf8proc_int32_t scalar) noexcept {
            switch (validation) {
                case UiTextValidation::Any:
                    return true;
                case UiTextValidation::SingleLine:
                    return scalar != '\r' && scalar != '\n' && scalar != 0x85 && scalar != 0x2028 && scalar != 0x2029;
                case UiTextValidation::AsciiDigits:
                    return scalar >= '0' && scalar <= '9';
                case UiTextValidation::Count:
                    return false;
            }
            return false;
        }

        /** @brief Validates the whole resulting draft, including line separators and capacity. */
        Result<Boundaries> Validate(const UiActionText &text, const UiTextEditPolicy &policy) {
            auto segmented = Segment(text);
            if (segmented.HasError())
                return segmented;
            if (text.size > policy.maximumBytes || segmented.Value().count > policy.maximumGraphemes)
                return Result<Boundaries>::Failure(MakeError(UiErrors::ControlCapacityExceeded));
            for (std::size_t offset = 0; offset < text.size;) {
                utf8proc_int32_t scalar{};
                const auto length = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.bytes.data() + offset),
                                                     static_cast<utf8proc_ssize_t>(text.size - offset), &scalar);
                if (!AllowsScalar(policy.validation, scalar))
                    return Result<Boundaries>::Failure(MakeError(UiErrors::TextInputInvalid));
                offset += static_cast<std::size_t>(length);
            }
            return segmented;
        }

        /** @brief Identifies commands carrying normalized text rather than selection or navigation payloads. */
        bool CarriesText(const UiTextEditKind kind) noexcept {
            return kind == UiTextEditKind::Insert || kind == UiTextEditKind::Paste;
        }

        /** @brief Identifies commands allowed to extend the logical caret selection. */
        bool IsNavigation(const UiTextEditKind kind) noexcept {
            using enum UiTextEditKind;
            return kind == Previous || kind == Next || kind == Home || kind == End;
        }

        /** @brief Identifies commands that atomically replace a selected or adjacent range. */
        bool ReplacesText(const UiTextEditKind kind) noexcept {
            return CarriesText(kind) || kind == UiTextEditKind::Backspace || kind == UiTextEditKind::DeleteForward;
        }

        /** @brief Rejects unrecognized or conflicting command fields before mutation. */
        bool IsValidCommand(const UiTextEditCommand &command) noexcept {
            using enum UiTextEditKind;
            return command.kind < Count && command.text.IsValid() && (CarriesText(command.kind) || command.text.size == 0) &&
                   (command.kind == Select || command.selection == UiTextSelection{}) &&
                   (IsNavigation(command.kind) || !command.extendSelection);
        }
    }  // namespace

    /** @copydoc UiTextEditPolicy::IsValid */
    bool UiTextEditPolicy::IsValid() const noexcept {
        return maximumBytes > 0 && maximumBytes <= MaximumUiActionTextBytes && maximumGraphemes > 0 &&
               maximumGraphemes <= MaximumUiActionTextBytes && undoDepth <= MaximumUiTextUndoDepth && validation < UiTextValidation::Count;
    }

    /** @copydoc ValidateUiTextEditValue */
    Result<void> ValidateUiTextEditValue(const UiActionText &text, const UiTextEditPolicy &policy) {
        if (!policy.IsValid())
            return Result<void>::Failure(MakeError(UiErrors::ControlDescriptorInvalid));
        const auto valid = Validate(text, policy);
        return valid.HasError() ? Result<void>::Failure(valid.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc UiTextEditBuffer::Create */
    Result<UiTextEditBuffer> UiTextEditBuffer::Create(const UiTextEditPolicy &policy, const UiActionText &initial) {
        if (!policy.IsValid())
            return Result<UiTextEditBuffer>::Failure(MakeError(UiErrors::ControlDescriptorInvalid));
        UiTextEditBuffer result;
        result.policy_ = policy;
        if (const auto reset = result.Reset(initial); reset.HasError())
            return Result<UiTextEditBuffer>::Failure(reset.ErrorValue());
        return Result<UiTextEditBuffer>::Success(std::move(result));
    }

    /** @copydoc UiTextEditBuffer::Reset */
    Result<void> UiTextEditBuffer::Reset(const UiActionText &text) {
        if (closed_)
            return Result<void>::Failure(MakeError(UiErrors::TextLifecycleUnavailable));
        const auto boundaries = Validate(text, policy_);
        if (boundaries.HasError())
            return Result<void>::Failure(boundaries.ErrorValue());
        const auto count = boundaries.Value().count;
        state_ = {text, {count, count}, count};
        undo_ = {};
        redo_ = {};
        undoCount_ = redoCount_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc UiTextEditBuffer::PushUndo */
    void UiTextEditBuffer::PushUndo(const UiTextEditSnapshot &previous) noexcept {
        if (policy_.undoDepth != 0) {
            if (undoCount_ == policy_.undoDepth) {
                std::move(undo_.begin() + 1, undo_.begin() + undoCount_, undo_.begin());
                --undoCount_;
            }
            undo_[undoCount_++] = previous;
        }
        redo_ = {};
        redoCount_ = 0;
    }

    /** @copydoc UiTextEditBuffer::Replace */
    Result<UiTextEditResult> UiTextEditBuffer::Replace(const UiActionText &text, const UiTextSelection range) {
        const auto source = Segment(state_.text).Value();
        const auto first = source.offsets[std::min(range.anchor, range.caret)];
        const auto last = source.offsets[std::max(range.anchor, range.caret)];
        const auto size = static_cast<std::size_t>(state_.text.size - (last - first)) + text.size;
        if (size > policy_.maximumBytes)
            return Result<UiTextEditResult>::Failure(MakeError(UiErrors::ControlCapacityExceeded));
        UiActionText candidate;
        candidate.size = static_cast<std::uint16_t>(size);
        std::copy_n(state_.text.bytes.begin(), first, candidate.bytes.begin());
        std::copy_n(text.bytes.begin(), text.size, candidate.bytes.begin() + first);
        std::copy(state_.text.bytes.begin() + last, state_.text.bytes.begin() + state_.text.size,
                  candidate.bytes.begin() + first + text.size);
        const auto boundaries = Validate(candidate, policy_);
        if (boundaries.HasError())
            return Result<UiTextEditResult>::Failure(boundaries.ErrorValue());
        // Inserted combining marks/ZWJ can join adjacent clusters. Place the caret at the next complete boundary.
        const auto end = first + text.size;
        const auto &offsets = boundaries.Value().offsets;
        const auto caret = static_cast<std::uint16_t>(
            std::lower_bound(offsets.begin(), offsets.begin() + boundaries.Value().count + 1, end) - offsets.begin());
        const bool changed = candidate.View() != state_.text.View();
        if (changed)
            PushUndo(state_);
        state_ = {candidate, {caret, caret}, boundaries.Value().count};
        return Result<UiTextEditResult>::Success({changed, {}});
    }

    /** @copydoc UiTextEditBuffer::Apply */
    Result<UiTextEditResult> UiTextEditBuffer::Apply(const UiTextEditCommand &command) {
        using enum UiTextEditKind;
        if (closed_)
            return Result<UiTextEditResult>::Failure(MakeError(UiErrors::TextLifecycleUnavailable));
        if (!IsValidCommand(command))
            return Result<UiTextEditResult>::Failure(MakeError(UiErrors::TextInputInvalid));
        if (command.kind == Select || command.kind == SelectAll)
            return SelectRange(command);
        if (ReplacesText(command.kind))
            return EditRange(command);
        if (command.kind == Copy || command.kind == Cut) {
            return Clipboard(command.kind);
        } else if (command.kind == Undo || command.kind == Redo) {
            return Result<UiTextEditResult>::Success(RestoreHistory(command.kind));
        } else {
            Navigate(command);
        }
        return Result<UiTextEditResult>::Success({});
    }

    /** @copydoc UiTextEditBuffer::SelectRange */
    Result<UiTextEditResult> UiTextEditBuffer::SelectRange(const UiTextEditCommand &command) {
        if (command.kind == UiTextEditKind::SelectAll) {
            state_.selection = {0, state_.graphemeCount};
        } else {
            if (command.selection.anchor > state_.graphemeCount || command.selection.caret > state_.graphemeCount)
                return Result<UiTextEditResult>::Failure(MakeError(UiErrors::TextInputInvalid));
            state_.selection = command.selection;
        }
        return Result<UiTextEditResult>::Success({});
    }

    /** @copydoc UiTextEditBuffer::EditRange */
    Result<UiTextEditResult> UiTextEditBuffer::EditRange(const UiTextEditCommand &command) {
        auto range = state_.selection;
        if (CarriesText(command.kind)) {
            if (const auto valid = Segment(command.text); valid.HasError())
                return Result<UiTextEditResult>::Failure(valid.ErrorValue());
            return Replace(command.text, range);
        }
        if (range.anchor == range.caret) {
            if (command.kind == UiTextEditKind::Backspace && range.anchor > 0)
                --range.anchor;
            if (command.kind == UiTextEditKind::DeleteForward && range.caret < state_.graphemeCount)
                ++range.caret;
        }
        return Replace({}, range);
    }

    /** @copydoc UiTextEditBuffer::Clipboard */
    Result<UiTextEditResult> UiTextEditBuffer::Clipboard(const UiTextEditKind kind) {
        if (policy_.password)
            return Result<UiTextEditResult>::Failure(MakeError(UiErrors::TextInputInvalid));
        const auto range = state_.selection;
        const auto boundaries = Segment(state_.text).Value();
        const auto first = boundaries.offsets[std::min(range.anchor, range.caret)];
        const auto last = boundaries.offsets[std::max(range.anchor, range.caret)];
        const auto copied = UiActionText::Create(state_.text.View().substr(first, last - first)).Value();
        UiTextEditResult result;
        if (kind == UiTextEditKind::Cut) {
            auto replaced = Replace({}, range);
            if (replaced.HasError())
                return replaced;
            result = replaced.Value();
        }
        result.clipboard = copied;
        return Result<UiTextEditResult>::Success(result);
    }

    /** @copydoc UiTextEditBuffer::RestoreHistory */
    UiTextEditResult UiTextEditBuffer::RestoreHistory(const UiTextEditKind kind) noexcept {
        const bool undo = kind == UiTextEditKind::Undo;
        auto &from = undo ? undo_ : redo_;
        auto &to = undo ? redo_ : undo_;
        auto &fromCount = undo ? undoCount_ : redoCount_;
        auto &toCount = undo ? redoCount_ : undoCount_;
        if (fromCount != 0) {
            to[toCount++] = state_;
            state_ = from[--fromCount];
            from[fromCount] = {};
            return {true, {}};
        }
        return {};
    }

    /** @copydoc UiTextEditBuffer::Navigate */
    void UiTextEditBuffer::Navigate(const UiTextEditCommand &command) noexcept {
        using enum UiTextEditKind;
        const auto range = state_.selection;
        auto caret = range.caret;
        const bool selected = range.anchor != range.caret && !command.extendSelection;
        if (command.kind == Home)
            caret = 0;
        else if (command.kind == End)
            caret = state_.graphemeCount;
        else if (command.kind == Previous)
            caret = selected ? std::min(range.anchor, range.caret) : static_cast<std::uint16_t>(caret == 0 ? 0 : caret - 1);
        else if (command.kind == Next)
            caret =
                selected ? std::max(range.anchor, range.caret) : static_cast<std::uint16_t>(std::min<int>(caret + 1, state_.graphemeCount));
        state_.selection = {command.extendSelection ? range.anchor : caret, caret};
    }

    /** @copydoc UiTextEditBuffer::Snapshot */
    Result<UiTextEditSnapshot> UiTextEditBuffer::Snapshot() const {
        if (closed_)
            return Result<UiTextEditSnapshot>::Failure(MakeError(UiErrors::TextLifecycleUnavailable));
        return Result<UiTextEditSnapshot>::Success(state_);
    }

    /** @copydoc UiTextEditBuffer::Display */
    Result<UiTextEditDisplay> UiTextEditBuffer::Display() const {
        if (closed_)
            return Result<UiTextEditDisplay>::Failure(MakeError(UiErrors::TextLifecycleUnavailable));
        UiTextEditDisplay result;
        if (policy_.password) {
            for (std::uint16_t i = 0; i < state_.graphemeCount; ++i) {
                for (const char byte : std::string_view{"\xe2\x80\xa2"})
                    result.bytes[result.size++] = byte;
            }
        } else {
            std::copy_n(state_.text.bytes.begin(), state_.text.size, result.bytes.begin());
            result.size = state_.text.size;
        }
        return Result<UiTextEditDisplay>::Success(result);
    }

    /** @copydoc UiTextEditBuffer::Shutdown */
    void UiTextEditBuffer::Shutdown() noexcept {
        closed_ = true;
        state_ = {};
        undo_ = {};
        redo_ = {};
        undoCount_ = redoCount_ = 0;
    }
}  // namespace Horo::Runtime::Ui
