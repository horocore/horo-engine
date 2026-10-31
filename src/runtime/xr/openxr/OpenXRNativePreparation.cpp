#include "host/OpenXRNativeSession.h"

#include <exception>
#include <limits>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Contains foreign native/host exceptions without losing the owning transaction's cleanup path. */
        template <typename Operation> Result<void> CapturePreparation(Operation operation) {
            try {
                return operation();
            } catch (const std::exception &) {
                return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable, "XR native preparation threw"));
            } catch (...) {  // Native callbacks must not bypass the retained owner's rollback.
                return Result<void>::Failure(
                    MakeError(XRErrors::OperationUnavailable, "XR native preparation raised an unknown exception"));
            }
        }

        /** @brief Checks only admitted selection input; never manufactures successful native discovery evidence. */
        Result<void> ValidateSelection(const NativeDiscoveryRequest &request) {
            const auto &selection = request.selection;
            if (const auto contract = RequireXRContractVersion(selection.contractVersion, CurrentXRContractVersion); contract.HasError())
                return contract;
            if (!selection.attempt.IsValid() || !selection.backend.IsValid() || !selection.installRecord.IsValid() ||
                !selection.productProfile.IsValid() || selection.loaderSource >= XRLoaderSourcePolicy::Count ||
                selection.productMode >= XRPreflightProductMode::Count || selection.runtimeSelection >= XRRuntimeSelectionPolicy::Count)
                return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightInvalid));
            if (selection.cancellationRequested)
                return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightCancelled));
            if (!selection.admittedLoaderVersions.minimum.IsValid() || !selection.admittedLoaderVersions.maximum.IsValid() ||
                selection.admittedLoaderVersions.minimum > selection.admittedLoaderVersions.maximum ||
                request.requestedApi < selection.admittedLoaderVersions.minimum ||
                request.requestedApi > selection.admittedLoaderVersions.maximum || request.requestedApi.major != 1 ||
                request.requestedApi.minor > XR_VERSION_MINOR(XR_CURRENT_API_VERSION))
                return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible));
            if (const bool developerOverride = selection.runtimeSelection == XRRuntimeSelectionPolicy::ApprovedDeveloperOverride;
                developerOverride != selection.developerOverrideApproved ||
                (developerOverride && selection.productMode != XRPreflightProductMode::Development))
                return Result<void>::Failure(MakeError(XRErrors::RuntimeOverrideRejected));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OpenXRNativeSession::PrepareDiscovery */
    Result<void> OpenXRNativeSession::PrepareDiscovery(const NativeDiscoveryRequest &request) {
        if (controlThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR discovery requires its control thread"));
        if (discovery_ || instance_ != XR_NULL_HANDLE || session_ != XR_NULL_HANDLE || owner_.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable, "Retire the previous native owner before preparation"));
        if (const auto selected = ValidateSelection(request); selected.HasError())
            return selected;
        discovery_.emplace(request.selection, request.requestedApi);
        const auto prepared = CapturePreparation([this, &request] {
            return PrepareDiscoveryTransaction(request);
        });
        return prepared.HasError() ? Fail(prepared.ErrorValue()) : prepared;
    }

    /** @copydoc OpenXRNativeSession::PrepareDiscoveryTransaction */
    Result<void> OpenXRNativeSession::PrepareDiscoveryTransaction(const NativeDiscoveryRequest &request) {
        auto prepared = ResolveGlobal();
        if (prepared.HasValue())
            prepared = DiscoverAndNegotiate(request);
        if (prepared.HasValue())
            prepared = CreateInstance(request);
        if (prepared.HasValue())
            prepared = ResolveInstance();
        if (prepared.HasValue())
            prepared = SelectSystem();
        if (prepared.HasValue()) {
            if (nativeSystemIncarnation_ == std::numeric_limits<std::uint32_t>::max())
                return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
            ++nativeSystemIncarnation_;
            discovery_->phase = DiscoveryPhase::Prepared;
        }
        return prepared;
    }

    /** @copydoc OpenXRNativeSession::BorrowDiscovery */
    Result<NativeDiscoveryBorrow> OpenXRNativeSession::BorrowDiscovery(const XRLoaderPreflightAttempt attempt) const {
        using Outcome = Result<NativeDiscoveryBorrow>;
        if (controlThread_ != std::this_thread::get_id())
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible, "XR discovery requires its control thread"));
        if (!discovery_ || discovery_->selection.attempt != attempt || instance_ == XR_NULL_HANDLE || system_ == XR_NULL_SYSTEM_ID ||
            (discovery_->phase != DiscoveryPhase::Prepared && discovery_->phase != DiscoveryPhase::SessionPrepared))
            return Outcome::Failure(MakeError(XRErrors::LoaderPreflightStale));
        return Outcome::Success(
            {instance_, system_, dispatch_.getProc, {names_.extensions.data(), names_.extensionCount}, discovery_->acceptedApi});
    }

    /** @copydoc OpenXRNativeSession::CheckPreparation */
    Result<void> OpenXRNativeSession::CheckPreparation(const NativeSessionPreparation &request) const {
        if (controlThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR preparation requires its control thread"));
        if (!discovery_ ||
            (discovery_->phase != DiscoveryPhase::Prepared && discovery_->phase != DiscoveryPhase::PreparingSession &&
             discovery_->phase != DiscoveryPhase::SessionPrepared) ||
            instance_ == XR_NULL_HANDLE || system_ == XR_NULL_SYSTEM_ID)
            return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightStale));
        const auto &selection = discovery_->selection;
        auto admitted = ValidateXRLoaderPreflight(request.preflight, request.activeAttempt, selection.backend, selection.installRecord,
                                                  selection.productProfile);
        if (admitted.HasError())
            return admitted;
        if (request.activeAttempt != selection.attempt || request.preflight.ContractVersion() != selection.contractVersion ||
            request.preflight.LoaderApiVersion() != discovery_->acceptedApi || request.preflight.LoaderSource() != selection.loaderSource ||
            request.preflight.RuntimeSelection() != selection.runtimeSelection ||
            (retainedPreflight_ && request.preflight.RuntimeGeneration() != retainedPreflight_->RuntimeGeneration()) ||
            (preparing_.IsValid() && preparing_.system.runtime != request.preflight.RuntimeGeneration()))
            return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightStale));
        admitted = fence_->ValidatePreparation(request.preflight, request.activeAttempt);
        return admitted.HasError() ? admitted : graphics_->ValidatePreparation();
    }

    /** @copydoc OpenXRNativeSession::PrepareSession */
    Result<void> OpenXRNativeSession::PrepareSession(const NativeSessionPreparation &request) {
        if (controlThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR preparation requires its control thread"));
        if (!discovery_ || discovery_->phase != DiscoveryPhase::Prepared)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (const auto current = CheckPreparation(request); current.HasError())
            return current;
        discovery_->phase = DiscoveryPhase::PreparingSession;
        if (const auto prepared = CapturePreparation([this, &request] {
            return CreateSession(request);
        });
            prepared.HasError())
            return Fail(prepared.ErrorValue());
        retainedPreflight_ = request.preflight;
        discovery_->phase = DiscoveryPhase::SessionPrepared;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::BorrowPreparedSession */
    Result<NativeSessionBorrow> OpenXRNativeSession::BorrowPreparedSession(const NativeSessionPreparation &request) const {
        using Outcome = Result<NativeSessionBorrow>;
        if (controlThread_ != std::this_thread::get_id())
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible, "XR discovery requires its control thread"));
        if (!discovery_ || discovery_->phase != DiscoveryPhase::SessionPrepared || session_ == XR_NULL_HANDLE)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (const auto current = CheckPreparation(request); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        // This borrow admits only native discovery; actions/frames remain unavailable before complete activation.
        return Outcome::Success({instance_, system_, session_, 0, 0, nativeSessionIncarnation_});
    }

    /** @copydoc OpenXRNativeSession::PreparedSystem */
    Result<XRSystemId> OpenXRNativeSession::PreparedSystem(const NativeSessionPreparation &request) const {
        if (const auto current = CheckPreparation(request); current.HasError())
            return Result<XRSystemId>::Failure(current.ErrorValue());
        if (!nativeSystemIncarnation_)
            return Result<XRSystemId>::Failure(MakeError(XRErrors::IdentityStale));
        return Result<XRSystemId>::Success({request.preflight.RuntimeGeneration(), {.index = 0, .generation = nativeSystemIncarnation_}});
    }

    /** @copydoc OpenXRNativeSession::Activate */
    Result<void> OpenXRNativeSession::Activate(const NativeSessionRequest &request) {
        if (controlThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR activation requires its control thread"));
        if (!discovery_ || discovery_->phase != DiscoveryPhase::SessionPrepared || session_ == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        // The canonical lifecycle issued this identity only after real session census produced a complete plan.
        // Existing Create may already have reserved this exact identity; staged preparation never issues one.
        if (!request.candidate.IsValid() || (preparing_.IsValid() && preparing_ != request.candidate) ||
            (preparing_ != request.candidate &&
             (request.candidate.system.runtime.Value() < lastRuntime_ ||
              (request.candidate.system.runtime.Value() == lastRuntime_ && request.candidate.slot.generation <= lastSessionGeneration_))))
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        preparing_ = request.candidate;
        lastRuntime_ = request.candidate.system.runtime.Value();
        lastSessionGeneration_ = request.candidate.slot.generation;
        if (const auto activated = CapturePreparation([this, &request] {
            auto current = CheckPreparation({request.preflight, request.activeAttempt});
            return current.HasError() ? current : CheckCurrent(request);
        });
            activated.HasError())
            // The staged caller may already own attached action/reference-space children.
            // Preserve their native parents until that caller retires them in reverse order.
            // Legacy Create still owns atomic rollback through FinishCreate.
            return activated;
        retainedPreflight_ = request.preflight;
        retainedPlan_ = request.plan;
        owner_ = request.candidate;
        preparing_ = {};
        discovery_->phase = DiscoveryPhase::Active;
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
