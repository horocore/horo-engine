#include "Horo/Runtime/InputAccessibility.h"

namespace Horo::Input {
    /** @copydoc DigitalAffordancePolicy::IsValid */
    bool DigitalAffordancePolicy::IsValid() const noexcept {
        constexpr std::uint64_t maximum = 60000000;
        return holdMicroseconds <= maximum && repeatDelayMicroseconds <= maximum && repeatIntervalMicroseconds > 0 &&
               repeatIntervalMicroseconds <= maximum;
    }

    /** @copydoc DigitalAffordance::Create */
    Result<DigitalAffordance> DigitalAffordance::Create(const DigitalAffordancePolicy policy) {
        if (!policy.IsValid())
            return Result<DigitalAffordance>::Failure(MakeError(InputAccessibilityErrors::InvalidPolicy));
        return Result<DigitalAffordance>::Success(DigitalAffordance{policy});
    }

    /** @copydoc DigitalAffordance::DigitalAffordance */
    DigitalAffordance::DigitalAffordance(const DigitalAffordancePolicy policy) noexcept : policy_(policy) {}

    /** @copydoc DigitalAffordance::Cancel */
    DigitalAffordanceValue DigitalAffordance::Cancel() noexcept {
        const DigitalAffordanceValue result{{false, false, active_}, false};
        active_ = false;
        waiting_ = false;
        repeating_ = false;
        clockStarted_ = false;
        return result;
    }

    /** @copydoc DigitalAffordance::Update */
    DigitalAffordanceValue DigitalAffordance::Update(const ButtonState physical, const std::uint64_t nowMicroseconds) noexcept {
        if (clockStarted_ && nowMicroseconds < lastTime_)
            return Cancel();
        clockStarted_ = true;
        lastTime_ = nowMicroseconds;
        const bool wasActive = active_;
        bool activated = false;
        if (physical.pressed) {
            waiting_ = true;
            pressTime_ = nowMicroseconds;
        }
        if (waiting_ && (physical.down || physical.pressed) && nowMicroseconds - pressTime_ >= policy_.holdMicroseconds) {
            active_ = policy_.toggle ? !active_ : true;
            activated = active_;
            waiting_ = false;
            repeating_ = false;
            repeatTime_ = nowMicroseconds;
        }
        if (!physical.down) {
            waiting_ = false;
            if (!policy_.toggle)
                active_ = false;
        }
        DigitalAffordanceValue result{{active_, activated && !wasActive, !active_ && (wasActive || activated)}, false};
        const auto delay = repeating_ ? policy_.repeatIntervalMicroseconds : policy_.repeatDelayMicroseconds;
        if (active_ && policy_.repeat && !result.button.pressed && nowMicroseconds - repeatTime_ >= delay) {
            result.repeated = true;
            repeating_ = true;
            repeatTime_ = nowMicroseconds;
        }
        return result;
    }

    /** @copydoc StickyModifierLatch::Observe */
    void StickyModifierLatch::Observe(const ModifierState physical) noexcept {
        if (awaitingNeutral_) {
            physical_ = physical;
            if (physical == ModifierState{})
                awaitingNeutral_ = false;
            return;
        }
        latched_.control |= physical.control && !physical_.control;
        latched_.shift |= physical.shift && !physical_.shift;
        latched_.alt |= physical.alt && !physical_.alt;
        latched_.command |= physical.command && !physical_.command;
        physical_ = physical;
    }

    /** @copydoc StickyModifierLatch::Effective */
    ModifierState StickyModifierLatch::Effective() const noexcept {
        if (awaitingNeutral_)
            return {};
        return {physical_.control || latched_.control, physical_.shift || latched_.shift, physical_.alt || latched_.alt,
                physical_.command || latched_.command};
    }

    /** @copydoc StickyModifierLatch::Consume */
    void StickyModifierLatch::Consume() noexcept {
        latched_ = {};
    }

    /** @copydoc StickyModifierLatch::Reset */
    void StickyModifierLatch::Reset() noexcept {
        physical_ = {};
        latched_ = {};
        awaitingNeutral_ = true;
    }

    namespace InputAccessibilityErrors {
        /** @copydoc InvalidPolicy */
        const ErrorCodeDescriptor InvalidPolicy{ErrorDomainId{"horo.input"},
                                                ErrorCode{"input.accessibility.policy_invalid"},
                                                ErrorSeverity::Error,
                                                "Digital input affordance policy is invalid.",
                                                "Use bounded hold/repeat durations and a positive repeat interval.",
                                                false,
                                                false};
    }  // namespace InputAccessibilityErrors
}  // namespace Horo::Input
