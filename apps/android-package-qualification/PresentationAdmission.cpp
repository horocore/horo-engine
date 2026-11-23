#include "PresentationAdmission.h"

#include <algorithm>
#include <limits>

namespace Horo::AndroidQualification {
    namespace PresentationAdmissionErrors {
        const ErrorCodeDescriptor InvalidReceipt{ErrorDomainId{"horo.android.qualification"},
                                                 ErrorCode{"android.qualification.presentation_receipt_invalid"}, ErrorSeverity::Error,
                                                 "Native readiness receipt does not match the committed Android window facts."};
    }

    namespace {
        /** @brief Validates authentic identity before any lifecycle observation is queued. */
        bool Matches(const Platform::Android::AndroidWindowSnapshot &window, const Platform::AndroidLifecycleSnapshot &current) {
            return window.activity == current.activityGeneration && window.window == current.windowGeneration && window.revision != 0;
        }

        /** @brief Distinguishes a newly admitted native surface's facts even when readiness never reports a gap. */
        bool Same(const Platform::Android::AndroidWindowSnapshot &a, const Platform::Android::AndroidWindowSnapshot &b) {
            return a.activity == b.activity && a.window == b.window && a.revision == b.revision && a.extent == b.extent;
        }

        /** @brief A drawable readiness claim requires actual committed native occurrence and nonzero extent. */
        bool Drawable(const Platform::Android::AndroidWindowSnapshot &window, const Platform::AndroidLifecycleSnapshot &current) {
            return window.available && current.hasWindow && window.extent.width != 0 && window.extent.height != 0;
        }

        /** @brief Rejects incomplete cutoffs and contradictory receipts before any canonical mutation. */
        Result<void> Validate(const Platform::Android::AndroidWindowSnapshot &window, const Platform::AndroidLifecycleSnapshot &current,
                              bool ready) {
            if (current.queuedObservationCount != 0)
                return Result<void>::Failure(MakeError(Platform::Android::AndroidWindowErrors::Pending));
            if (!Matches(window, current) || (ready && !Drawable(window, current)))
                return Result<void>::Failure(MakeError(PresentationAdmissionErrors::InvalidReceipt));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PresentationAdmission::PresentationAdmission */
    PresentationAdmission::PresentationAdmission(Platform::AndroidLifecycleController &lifecycle,
                                                 Platform::Android::AndroidLifecycleAdapter &adapter) noexcept
        : lifecycle_(lifecycle), adapter_(adapter) {}

    /** @brief Commits the canonical owner cutoff without replacing error identity. */
    Result<void> PresentationAdmission::Commit() {
        const auto committed = lifecycle_.Drain();
        return committed.HasError() ? Result<void>::Failure(committed.ErrorValue()) : Result<void>::Success();
    }

    /** @brief Retires only the exact published generation, preserving the window and Activity owners. */
    Result<void> PresentationAdmission::Lose(const Platform::AndroidLifecycleSnapshot &current) {
        if (current.hasPresentation) {
            if (auto lost = adapter_.OnPresentationLost(current.presentationGeneration); lost.HasError())
                return lost;
            if (auto committed = Commit(); committed.HasError())
                return committed;
        }
        ready_.reset();
        return Result<void>::Success();
    }

    /** @brief Issues a generation only after the native caller supplies actual ready facts. */
    Result<void> PresentationAdmission::Publish(const Platform::Android::AndroidWindowSnapshot &window) {
        const auto available = adapter_.OnPresentationAvailable(window.window);
        if (available.HasError())
            return Result<void>::Failure(available.ErrorValue());
        if (auto committed = Commit(); committed.HasError())
            return committed;
        ready_ = window;
        return Result<void>::Success();
    }

    /** @copydoc PresentationAdmission::Reconcile */
    Result<void> PresentationAdmission::Reconcile(const Platform::Android::AndroidWindowSnapshot &window, const bool ready) {
        const auto current = lifecycle_.Snapshot();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (auto valid = Validate(window, current.Value(), ready); valid.HasError())
            return valid;
        if (window.revision < latestRevision_)
            return Result<void>::Failure(MakeError(PresentationAdmissionErrors::InvalidReceipt));
        latestRevision_ = window.revision;
        if (!ready)
            return Lose(current.Value());
        if (current.Value().hasPresentation && ready_ && Same(*ready_, window))
            return Result<void>::Success();
        if (auto lost = Lose(current.Value()); lost.HasError())
            return lost;
        return Publish(window);
    }

    /** @copydoc FramePacing::FramePacing */
    FramePacing::FramePacing(const Clock::duration minimumInterval, const Clock::duration conservationInterval) noexcept
        : minimumInterval_(minimumInterval), conservationInterval_(std::max(minimumInterval, conservationInterval)) {}

    /** @copydoc FramePacing::TryAdmit */
    bool FramePacing::TryAdmit(const Platform::AndroidLifecycleSnapshot &state, const Clock::time_point now, const Mode mode) noexcept {
        if (minimumInterval_ <= Clock::duration::zero() || state.queuedObservationCount != 0 || !state.hasPresentation ||
            state.process != Platform::AndroidProcessState::Running || state.activity != Platform::AndroidActivityState::Resumed)
            return false;
        const bool conserving = mode > Mode::Baseline && mode <= Mode::MemoryConservation;
        const auto interval = conserving ? conservationInterval_ : minimumInterval_;
        if (lastAttempt_ && (now < *lastAttempt_ || now - *lastAttempt_ < interval))
            return false;
        lastAttempt_ = now;
        return true;
    }

    /** @copydoc FramePacingMailbox::Acquire */
    FramePacingMailbox::Token FramePacingMailbox::Acquire() noexcept {
        auto previous = state_.load(std::memory_order_acquire);
        constexpr Token maximumGeneration = static_cast<Token>(std::numeric_limits<std::int64_t>::max()) & ~Flags;
        if ((previous & Active) || previous >= maximumGeneration)
            return 0;
        const auto token = (previous + Flags + 1) | Active;
        return state_.compare_exchange_strong(previous, token, std::memory_order_acq_rel) ? token : 0;
    }

    /** @copydoc FramePacingMailbox::Revoke */
    void FramePacingMailbox::Revoke(const Token token) noexcept {
        // No replacement can acquire while Active remains set, and only this lease's owner revokes it.
        if (token && (state_.load(std::memory_order_acquire) & ~ModeBits) == token)
            state_.fetch_and(~Flags, std::memory_order_acq_rel);
    }

    /** @copydoc FramePacingMailbox::Publish */
    void FramePacingMailbox::Publish(const Token token, const FramePacing::Mode mode) noexcept {
        auto previous = state_.load(std::memory_order_acquire);
        if ((token & Flags) != Active || (previous & ~ModeBits) != token || mode > FramePacing::Mode::MemoryConservation)
            return;
        // Callbacks for one token are serialized. CAS failure means this owner was withdrawn.
        static_cast<void>(state_.compare_exchange_strong(previous, token | static_cast<Token>(mode), std::memory_order_acq_rel));
    }

    /** @copydoc FramePacingMailbox::Read */
    FramePacing::Mode FramePacingMailbox::Read(const Token token) const noexcept {
        const auto state = state_.load(std::memory_order_acquire);
        return (token & Flags) == Active && (state & ~ModeBits) == token ? static_cast<FramePacing::Mode>(state & ModeBits)
                                                                         : FramePacing::Mode::Baseline;
    }
}  // namespace Horo::AndroidQualification
