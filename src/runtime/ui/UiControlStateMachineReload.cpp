#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiControlsInternal.h"

#include <cmath>
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

        /** @brief Compares copied real immutable control constraints in addition to its exact owner/action contract. */
        [[nodiscard]] bool SameDescriptor(const UiControlDescriptor &a, const UiControlDescriptor &b) noexcept {
            const auto &left = UiControlDetail::BaseOf(a);
            if (const auto &right = UiControlDetail::BaseOf(b);
                !SameContract(a, b) || left.owner != right.owner || left.element != right.element ||
                left.initiallyEnabled != right.initiallyEnabled || left.focusable != right.focusable ||
                left.repeat.enabled != right.repeat.enabled || left.repeat.initialDelayTicks != right.repeat.initialDelayTicks ||
                left.repeat.intervalTicks != right.repeat.intervalTicks)
                return false;
            return std::visit([&b]<typename T>(const T &value) noexcept {
                const auto &other = std::get<T>(b);
                if constexpr (std::is_same_v<T, UiSliderControlDescriptor>)
                    return value.minimum == other.minimum && value.maximum == other.maximum && value.step == other.step &&
                           value.initialValue == other.initialValue;
                else if constexpr (std::is_same_v<T, UiTextInputControlDescriptor>)
                    return value.initialText.View() == other.initialText.View() && value.maximumTextBytes == other.maximumTextBytes &&
                           value.submitEndsEditing == other.submitEndsEditing;
                else if constexpr (std::is_same_v<T, UiToggleControlDescriptor>)
                    return value.initiallyChecked == other.initiallyChecked;
                else
                    return true;
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
                       draft.size <= text->maximumTextBytes && baseline.size <= text->maximumTextBytes;
            }
            return true;
        }
    }  // namespace

    /** @copydoc UiControlStateMachine::CaptureReloadStamp */
    Result<UiControlReloadStamp> UiControlStateMachine::CaptureReloadStamp() const {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Result<UiControlReloadStamp>::Failure(MakeError(UiErrors::ControlLifecycleUnavailable));
        UiControlReloadStamp stamp;
        stamp.state_ = storage_->state;
        stamp.descriptor_ = storage_->descriptor;
        stamp.editStartText_ = storage_->editStartText;
        stamp.sequence_ = storage_->lastSequence;
        stamp.tick_ = storage_->lastTick;
        stamp.pending_ = storage_->pending;
        return Result<UiControlReloadStamp>::Success(std::move(stamp));
    }

    /** @copydoc UiControlStateMachine::MatchesReloadStamp */
    bool UiControlStateMachine::MatchesReloadStamp(const UiControlReloadStamp &stamp) const noexcept {
        return storage_ && storage_->lifecycle == UiControlLifecycleState::Active && storage_->state == stamp.state_ &&
               SameDescriptor(storage_->descriptor, stamp.descriptor_) && storage_->editStartText.View() == stamp.editStartText_.View() &&
               storage_->lastSequence == stamp.sequence_ && storage_->lastTick == stamp.tick_ && storage_->pending == stamp.pending_;
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
