#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiControlsInternal.h"

#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Compares bounded typed arguments without comparing unused text tails. */
        [[nodiscard]] bool SameArgument(const UiActionValue &left, const UiActionValue &right) noexcept {
            return std::visit([&right]<typename T>(const T &value) noexcept {
                const auto *other = std::get_if<T>(&right);
                if (!other)
                    return false;
                if constexpr (std::is_same_v<T, UiActionText>)
                    return value.View() == other->View();
                else
                    return value == *other;
            }, left);
        }

        /** @brief Requires the same semantic owner and action contract, excluding replaced tree/input revisions. */
        [[nodiscard]] bool SameContract(const UiControlDescriptor &left, const UiControlDescriptor &right) noexcept {
            const auto &a = UiControlDetail::BaseOf(left);
            const auto &b = UiControlDetail::BaseOf(right);
            if (left.index() != right.index() || a.owner.instance != b.owner.instance || a.owner.canvas != b.owner.canvas ||
                a.owner.document != b.owner.document || a.action != b.action || a.payload.Size() != b.payload.Size())
                return false;
            for (std::size_t i = 0; i < a.payload.Size(); ++i)
                if (!SameArgument(a.payload.Values()[i], b.payload.Values()[i]))
                    return false;
            return true;
        }

        /** @brief Compares exact presentation identity and common focus/repeat/availability constraints. */
        [[nodiscard]] bool SamePresentation(const UiControlDescriptorBase &left, const UiControlDescriptorBase &right) noexcept {
            return left.owner == right.owner && left.element == right.element && left.initiallyEnabled == right.initiallyEnabled &&
                   left.focusable == right.focusable && left.repeat.enabled == right.repeat.enabled &&
                   left.repeat.initialDelayTicks == right.repeat.initialDelayTicks &&
                   left.repeat.intervalTicks == right.repeat.intervalTicks;
        }

        /** @brief Buttons carry no value-specific constraints beyond the common contract. */
        [[nodiscard]] bool SameConstraints(const UiButtonControlDescriptor &, const UiButtonControlDescriptor &) noexcept {
            return true;
        }

        /** @brief Compares the toggle's authored initial value. */
        [[nodiscard]] bool SameConstraints(const UiToggleControlDescriptor &left, const UiToggleControlDescriptor &right) noexcept {
            return left.initiallyChecked == right.initiallyChecked;
        }

        /** @brief Compares the slider's complete numeric domain and initial value. */
        [[nodiscard]] bool SameConstraints(const UiSliderControlDescriptor &left, const UiSliderControlDescriptor &right) noexcept {
            return left.minimum == right.minimum && left.maximum == right.maximum && left.step == right.step &&
                   left.initialValue == right.initialValue;
        }

        /** @brief Compares the text control's authored draft, submit behavior and complete edit policy. */
        [[nodiscard]] bool SameConstraints(const UiTextInputControlDescriptor &left, const UiTextInputControlDescriptor &right) noexcept {
            return left.initialText.View() == right.initialText.View() && left.maximumTextBytes == right.maximumTextBytes &&
                   left.submitEndsEditing == right.submitEndsEditing && left.editing == right.editing;
        }

        /** @brief Compares copied real immutable control constraints in addition to its exact owner/action contract. */
        [[nodiscard]] bool SameDescriptor(const UiControlDescriptor &a, const UiControlDescriptor &b) noexcept {
            if (!SameContract(a, b) || !SamePresentation(UiControlDetail::BaseOf(a), UiControlDetail::BaseOf(b)))
                return false;
            return std::visit([&b]<typename T>(const T &value) noexcept {
                return SameConstraints(value, std::get<T>(b));
            }, a);
        }

        /** @brief Checks both a draft and its cancel baseline against the replacement constraints before mutation. */
        [[nodiscard]] bool CompatibleValue(const UiControlDescriptor &descriptor, const UiControlState &state,
                                           const UiActionText &baseline) noexcept {
            if (const auto *slider = std::get_if<UiSliderControlDescriptor>(&descriptor)) {
                const double value = std::get<UiSliderControlState>(state).value;
                return std::isfinite(value) && value >= slider->minimum && value <= slider->maximum;
            }
            if (const auto *text = std::get_if<UiTextInputControlDescriptor>(&descriptor)) {
                const auto &draft = std::get<UiTextInputControlState>(state).text;
                return UiControlDetail::IsValidControlText(draft) && UiControlDetail::IsValidControlText(baseline) &&
                       draft.size <= text->maximumTextBytes && baseline.size <= text->maximumTextBytes &&
                       ValidateUiTextEditValue(draft, text->EditPolicy()).HasValue() &&
                       ValidateUiTextEditValue(baseline, text->EditPolicy()).HasValue();
            }
            return true;
        }
    }  // namespace

    /** @copydoc UiControlStateMachine::CaptureReloadStamp */
    Result<UiControlReloadStamp> UiControlStateMachine::CaptureReloadStamp() const {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Result<UiControlReloadStamp>::Failure(MakeError(UiErrors::ControlLifecycleUnavailable));
        if (storage_->textResetRevision == std::numeric_limits<std::uint64_t>::max())
            return Result<UiControlReloadStamp>::Failure(MakeError(UiErrors::ControlSequenceInvalid));
        UiControlReloadStamp stamp;
        stamp.state_ = storage_->state;
        stamp.descriptor_ = storage_->descriptor;
        stamp.editStartText_ = storage_->editStartText;
        stamp.sequence_ = storage_->lastSequence;
        stamp.tick_ = storage_->lastTick;
        stamp.textResetRevision_ = storage_->textResetRevision;
        stamp.pending_ = storage_->pending;
        return Result<UiControlReloadStamp>::Success(std::move(stamp));
    }

    /** @copydoc UiControlStateMachine::MatchesReloadStamp */
    bool UiControlStateMachine::MatchesReloadStamp(const UiControlReloadStamp &stamp) const noexcept {
        return storage_ && storage_->lifecycle == UiControlLifecycleState::Active && storage_->state == stamp.state_ &&
               SameDescriptor(storage_->descriptor, stamp.descriptor_) && storage_->editStartText.View() == stamp.editStartText_.View() &&
               storage_->lastSequence == stamp.sequence_ && storage_->lastTick == stamp.tick_ && storage_->pending == stamp.pending_ &&
               storage_->textResetRevision == stamp.textResetRevision_;
    }

    /** @copydoc UiControlStateMachine::ReconcileReload */
    Result<bool> UiControlStateMachine::ReconcileReload(const UiControlStateMachine &source, const bool preserveFocus) {
        if (!storage_ || !source.storage_ || storage_->lifecycle != UiControlLifecycleState::Active ||
            source.storage_->lifecycle != UiControlLifecycleState::Active)
            return Result<bool>::Failure(MakeError(UiErrors::ControlLifecycleUnavailable));
        if (!SameContract(storage_->descriptor, source.storage_->descriptor) ||
            !CompatibleValue(storage_->descriptor, source.storage_->state, source.storage_->editStartText))
            return Result<bool>::Success(false);
        const auto availability = storage_->configuredAvailability;
        storage_->state = source.storage_->state;
        storage_->editStartText = source.storage_->editStartText;
        storage_->ClearTransient(true);
        storage_->SetAvailabilityProjection(availability);
        const bool focused =
            preserveFocus && availability == UiControlAvailability::Enabled && UiControlDetail::BaseOf(storage_->descriptor).focusable;
        UiControlDetail::SetFocused(storage_->state, focused);
        if (auto *text = std::get_if<UiTextInputControlState>(&storage_->state))
            text->editing = focused && std::get<UiTextInputControlState>(source.storage_->state).editing;
        storage_->asyncAction.reset();
        storage_->lastSequence = 0;
        storage_->lastTick = 0;
        return Result<bool>::Success(true);
    }
}  // namespace Horo::Runtime::Ui
