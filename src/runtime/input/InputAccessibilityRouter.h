#pragma once

/** @file InputAccessibilityRouter.h @brief Target-private bounded semantic mapping state and transactional admission. */
#include "Horo/Runtime/InputAccessibility.h"

namespace Horo::Input::Detail {
    using AccessibilityActionIdentity = std::shared_ptr<const ActionId>;

    /** @brief Call-duration exact ownership; the action lease is borrowed from immutable setup metadata. */
    struct AccessibilityMappingKey final {
        std::uint64_t context;
        const AccessibilityActionIdentity &action;
        std::size_t binding;
        std::optional<PlayerId> player;
    };

    /** @brief Physical facts and admission evidence for one candidate semantic mapping step. */
    struct AccessibilityMappingSample final {
        std::optional<GamepadDeviceId> device;
        ButtonState physical;
        std::optional<InputSampleTime> time;
        bool eligible{}, chordEligible{}, deviceHadNeutral{};
    };

    /** @brief Reads the canonical physical binding interpretation without consuming any transition. */
    [[nodiscard]] ButtonState PeekAccessibilityPhysical(const InputBinding &binding, const RawInputSnapshot &raw,
                                                        const RawInputSnapshot *previous, std::optional<GamepadDeviceId> device) noexcept;

    /** @brief Reserves immutable typed action-name leases at setup, never during physical event/action processing.
     * @param actions Candidate action map. @param profile Validated effective profile.
     * @return One aligned optional identity per action; setup allocation failure remains typed.
     */
    [[nodiscard]] Result<std::vector<AccessibilityActionIdentity>> PrepareAccessibilityIdentities(std::span<const ActionDescriptor> actions,
                                                                                                  const InputBindingProfile &profile);

    /** @brief Old-owner cancellation value and its original cause, independent of the current action map. */
    struct AccessibilityRelease final {
        bool released{};
        ActionReadStatus status{ActionReadStatus::Resolved};
    };

    /** @brief Router-owned fixed mapping inventory; every call is serialized on the existing Input owner thread. */
    class InputAccessibilityRouter final {
        static constexpr std::size_t Capacity = MaximumAccessibilityMappings;

        struct Cell final {
            std::uint64_t context{}, serial{};
            AccessibilityActionIdentity action;
            std::size_t binding{};
            std::optional<PlayerId> player;
            std::optional<GamepadDeviceId> device;
            BindingAccessibilityPolicy policy;
            InputBinding source;
            std::optional<DigitalAffordance> processor;
            DigitalAffordanceValue cached;
            bool awaitingNeutral{true}, chordHeld{}, physicalHeld{}, retired{}, pendingRelease{}, ownerRemoved{};
            ActionReadStatus releaseStatus{ActionReadStatus::Resolved};
        };

    public:
        /** @brief One bounded action read, committed only after the physical consumption ledger also admits it. */
        class Transaction final {
        public:
            explicit Transaction(InputAccessibilityRouter &owner) noexcept : owner_(owner) {}

            /** @brief Pins an actively held/toggled mapping to its exact selected device until cancellation or neutralization. */
            [[nodiscard]] std::optional<GamepadDeviceId> PreferredDevice(const AccessibilityMappingKey &key) const noexcept;
            /** @brief Calculates one candidate binding without mutating committed mapping state.
             * @param key Exact context/action/binding/player incarnation. @param source Validated immutable binding.
             * @param sample Post-consumption physical facts, chord admission, explicit time and prior device-neutral evidence.
             * @return Synthetic value; CapacityFailed must be checked before commit.
             */
            [[nodiscard]] DigitalAffordanceValue Process(const AccessibilityMappingKey &key, const InputBinding &source,
                                                         const AccessibilityMappingSample &sample) noexcept;

            [[nodiscard]] bool CapacityFailed() const noexcept {
                return failed_;
            }

            [[nodiscard]] bool ConsumesSticky() const noexcept {
                return consumeSticky_;
            }

            /** @brief Publishes only the bounded validated candidate inventory; no allocation or callback occurs. */
            void Commit() noexcept;

        private:
            struct Candidate final {
                std::size_t slot{};
                Cell cell;
            };

            InputAccessibilityRouter &owner_;
            std::array<Candidate, MaximumAccessibilityBindingsPerAction> candidates_{};
            std::size_t count_{};
            bool failed_{}, consumeSticky_{};
        };

        /** @brief Starts a new committed raw sample and cancels stale device/time incarnations.
         * @param raw Exact immutable sample. @param previous Previous committed sample, if available.
         * @param assignments Current device authority.
         * @return Whether clock or device discontinuity also requires context modifier baselines to reset.
         */
        [[nodiscard]] bool BeginFrame(const RawInputSnapshot &raw, const RawInputSnapshot *previous,
                                      const InputDeviceAssignments &assignments) noexcept;
        /** @brief Retires all mappings while retaining pending releases for their original owner. */
        void CancelAll() noexcept;
        /** @brief Retires device-bound mappings after a device authority change. */
        void CancelDevices() noexcept;
        /** @brief Retires one context's mappings; removed owners transfer release through TryDequeueRetired.
         * @param context Exact non-reusable context identity. @param ownerRemoved Whether its live token has ended.
         */
        void CancelContext(std::uint64_t context, bool ownerRemoved = false) noexcept;
        /** @brief Retires a timed action when its explicit sample clock is unavailable.
         * @param context Exact context identity. @param action Typed action identity. @param player Exact optional assignment filter.
         */
        void CancelAction(std::uint64_t context, const ActionId &action, std::optional<PlayerId> player) noexcept;
        /** @brief Delivers one old-owner release even when that still-live context is temporarily ineligible. */
        [[nodiscard]] AccessibilityRelease TakeReleases(std::uint64_t context, const ActionId &action,
                                                        std::optional<PlayerId> player) noexcept;
        /** @brief Transfers an ended context's withdrawal to the host, preserving its exact original scope and typed name lease. */
        [[nodiscard]] bool TryDequeueRetired(RetiredInputActionRelease &release) noexcept;
        /** @brief Reclaims drained identity leases at explicit frame-end or the next committed-sample boundary. */
        void EndFrame() noexcept;

    private:
        /** @brief Retires a single cell, keeping an active cancellation release and its immutable name lease. */
        static void Cancel(Cell &cell, ActionReadStatus cause = ActionReadStatus::Resolved) noexcept;
        std::array<Cell, Capacity> cells_{};
        std::uint64_t serial_{};
        bool exhausted_{};
        std::optional<InputSampleTime> previousTime_;
    };
}  // namespace Horo::Input::Detail
