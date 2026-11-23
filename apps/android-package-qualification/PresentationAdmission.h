#pragma once
/** @file PresentationAdmission.h @brief App-private projection of genuine native readiness into canonical lifecycle generations. */
#include "Horo/Platform/Internal/AndroidLifecycleAdapter.h"
#include "Horo/Platform/Internal/AndroidWindowHost.h"

#include <atomic>
#include <chrono>
#include <optional>

namespace Horo::AndroidQualification {
    namespace PresentationAdmissionErrors {
        extern const ErrorCodeDescriptor InvalidReceipt;
    }

    /** @brief Retains only derived native-ready facts; the lifecycle controller/adapter remain the generation authority. */
    class PresentationAdmission final {
    public:
        /** @brief Borrows the existing serialized owner authorities for this helper's complete lifetime.
         * @param lifecycle Canonical publication state. @param adapter Exact generation issuer for that controller. */
        PresentationAdmission(Platform::AndroidLifecycleController &lifecycle,
                              Platform::Android::AndroidLifecycleAdapter &adapter) noexcept;
        /** @brief Reconciles one completed native readiness poll, including replacement without an intervening false result.
         * @param window Actual committed window facts used by the native owner. @param ready Actual native readiness result.
         * @return Success or original typed authority failure; invalid/stale/pending evidence cannot manufacture readiness.
         * @details Activity admission remains separate. Changed ready facts retire the old presentation then issue a fresh generation.
         */
        [[nodiscard]] Result<void> Reconcile(const Platform::Android::AndroidWindowSnapshot &window, bool ready);

    private:
        [[nodiscard]] Result<void> Lose(const Platform::AndroidLifecycleSnapshot &current);
        [[nodiscard]] Result<void> Publish(const Platform::Android::AndroidWindowSnapshot &window);
        [[nodiscard]] Result<void> Commit();
        Platform::AndroidLifecycleController &lifecycle_;
        Platform::Android::AndroidLifecycleAdapter &adapter_;
        std::optional<Platform::Android::AndroidWindowSnapshot> ready_;
        std::uint64_t latestRevision_{}; /**< Derived native-facts watermark, never a lifecycle generation issuer. */
    };

    /** @brief Bounds frame attempts independently of native event-loop wakeups. */
    class FramePacing final {
    public:
        using Clock = std::chrono::steady_clock;
        enum class Mode : std::uint8_t {
            Baseline = 0,
            PowerConservation = 1,
            ThermalConservation = 2,
            MemoryConservation = 3
        };

        /** @brief Sets the baseline and optional conservation cadence for the host's render attempts.
         * @param minimumInterval Baseline minimum time between attempts.
         * @param conservationInterval Optional longer delay when the current Activity requests conservation. */
        explicit FramePacing(Clock::duration minimumInterval, Clock::duration conservationInterval = {}) noexcept;

        /** @brief Consumes at most one attempt for a committed foreground presentation.
         * @param state Current committed lifecycle snapshot after native readiness reconciliation.
         * @param now Monotonic time on the frame owner.
         * @param mode Current owner-scoped attempt budget.
         * @return True if a frame may be attempted. No debt accrues while paused or on a late wakeup. */
        [[nodiscard]] bool TryAdmit(const Platform::AndroidLifecycleSnapshot &state, Clock::time_point now,
                                    Mode mode = Mode::Baseline) noexcept;

    private:
        const Clock::duration minimumInterval_;
        const Clock::duration conservationInterval_;
        std::optional<Clock::time_point> lastAttempt_;
    };

    /** @brief One process-lifetime slot for the exact Activity producer and native frame owner. */
    class FramePacingMailbox final {
    public:
        using Token = std::uint64_t;

        /** @brief Admits one non-reusable token. @return Exact token, or zero while unavailable. */
        [[nodiscard]] Token Acquire() noexcept;
        /** @brief The sole native owner revokes its token on its owner lane before best-effort Java detach. */
        void Revoke(Token token) noexcept;
        /** @brief The serialized Java producer can publish only while its exact token remains active. */
        void Publish(Token token, FramePacing::Mode mode) noexcept;
        /** @brief Reads the selected mode for the exact active owner, or baseline if unavailable. */
        [[nodiscard]] FramePacing::Mode Read(Token token) const noexcept;

    private:
        static constexpr Token ModeBits = 3;
        static constexpr Token Active = 4;
        static constexpr Token Flags = ModeBits | Active;
        static_assert(std::atomic<Token>::is_always_lock_free);
        std::atomic<Token> state_{}; /**< Non-reusable generation, active bit and pacing mode are atomically replaced. */
    };
}  // namespace Horo::AndroidQualification
