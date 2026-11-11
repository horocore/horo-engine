#pragma once

/** @file InputAccessibility.h
 * @brief Allocation-free digital affordance state consumed by semantic mapping.
 */
#include "Horo/Runtime/Input.h"

namespace Horo::Input {
    /** @brief One semantic mapping result; repeat is separate from the activation press edge. */
    struct DigitalAffordanceValue final {
        ButtonState button;
        bool repeated{};
    };

    /**
     * @brief Value-owned state for one exact digital binding/context/device incarnation.
     * @details Mapping calls Update once per sample after raw collection; no raw snapshot is
     * modified. The owner must Cancel on focus loss, context removal/preemption, disconnect,
     * binding replacement and shutdown, deliver the returned release to the old action owner,
     * then discard/recreate the processor. No allocation, native polling or bus calls occur.
     * This type neither validates binding conflicts nor introduces a persistence format.
     */
    class DigitalAffordance final {
    public:
        /** @brief Creates an independent mapping state with validated immutable policy.
         * @param policy Explicit per-binding affordance policy.
         * @return State or InputAccessibilityErrors::InvalidPolicy.
         */
        [[nodiscard]] static Result<DigitalAffordance> Create(DigitalAffordancePolicy policy);
        /** @brief Advances synthetic state from one eligible physical sample.
         * @param physical Immutable post-routing button facts. A fresh press edge arms activation.
         * @param nowMicroseconds Nondecreasing owner clock; backwards time cancels and disarms.
         * @return Semantic edges and at most one repeat; missed repeats never catch up in a loop.
         */
        [[nodiscard]] DigitalAffordanceValue Update(ButtonState physical, std::uint64_t nowMicroseconds) noexcept;
        /** @brief Clears held/toggled/waiting state and emits one release when active.
         * @return Old owner's release, then neutral on subsequent calls.
         */
        [[nodiscard]] DigitalAffordanceValue Cancel() noexcept;

    private:
        /** @brief Stores the already validated immutable per-binding policy. @param policy Validated digital policy. */
        explicit DigitalAffordance(DigitalAffordancePolicy policy) noexcept;
        DigitalAffordancePolicy policy_;
        std::uint64_t lastTime_{};
        std::uint64_t pressTime_{};
        std::uint64_t repeatTime_{};
        bool clockStarted_{};
        bool waiting_{};
        bool active_{};
        bool repeating_{};
    };

    /**
     * @brief Sequential modifier latch owned by one semantic mapping context, never the raw collector.
     * @details Observe once per raw snapshot before matching chords. Consume after an action
     * uses the latched chord. Reset at every context/focus/device/binding lifetime boundary.
     * New and reset latches require an observed neutral baseline before accepting modifiers.
     * Physically held modifiers remain facts even after the latch is consumed.
     */
    class StickyModifierLatch final {
    public:
        /** @brief Latches modifier press edges from immutable facts. @param physical Current physical modifiers. */
        void Observe(ModifierState physical) noexcept;
        /** @brief Returns physical-or-latched modifiers for chord matching. @return Copied effective mapping state. */
        [[nodiscard]] ModifierState Effective() const noexcept;
        /** @brief Clears sequential latches after one action consumes its chord. */
        void Consume() noexcept;
        /** @brief Clears all state at an owner boundary; held modifiers require a fresh neutral-to-press edge. */
        void Reset() noexcept;

    private:
        ModifierState physical_;
        ModifierState latched_;
        bool awaitingNeutral_{true};
    };

    namespace InputAccessibilityErrors {
        /** @brief Hold/repeat policy exceeds deterministic mapping bounds. */
        extern const ErrorCodeDescriptor InvalidPolicy;
        /** @brief Fixed semantic mapping inventory or setup identity storage cannot admit this request. */
        extern const ErrorCodeDescriptor CapacityExceeded;
    }  // namespace InputAccessibilityErrors
}  // namespace Horo::Input
