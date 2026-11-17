#include "PresentationAdmission.h"

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
}  // namespace Horo::AndroidQualification
