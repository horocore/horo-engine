#include "host/OpenXRNativeSession.h"

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Prevents recursive native destruction; failure returns with the owner eligible for explicit retry. */
        class RetirementOperation final {
        public:
            explicit RetirementOperation(bool &active) noexcept : active_(active) {
                active_ = true;
            }

            ~RetirementOperation() {
                active_ = false;
            }

            RetirementOperation(const RetirementOperation &) = delete;
            RetirementOperation &operator=(const RetirementOperation &) = delete;

        private:
            bool &active_;
        };
    }  // namespace

    /** @copydoc OpenXRNativeSession::DestroySession */
    Result<void> OpenXRNativeSession::DestroySession() {
        if (session_ != XR_NULL_HANDLE) {
            if (!dispatch_.destroySession)
                return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Native session has no retirement dispatch"));
            const auto result = dispatch_.destroySession(session_);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeFailure("xrDestroySession", result));
            session_ = XR_NULL_HANDLE;
        }
        if (graphicsPrepared_) {
            graphics_->Release();
            graphicsPrepared_ = false;
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::Close */
    Result<void> OpenXRNativeSession::Close() {
        if (controlThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR native owner requires its control thread"));
        if (retirementCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable, "Native retirement is already in progress"));
        const RetirementOperation operation{retirementCall_};
        if (discovery_)
            discovery_->phase = DiscoveryPhase::Retiring;
        owner_ = {};
        preparing_ = {};
        retainedPreflight_.reset();
        retainedPlan_.reset();
        if (auto retired = DestroySession(); retired.HasError())
            return retired;
        if (instance_ != XR_NULL_HANDLE) {
            if (!dispatch_.destroyInstance)
                return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Native instance has no retirement dispatch"));
            const auto result = dispatch_.destroyInstance(instance_);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeFailure("xrDestroyInstance", result));
            instance_ = XR_NULL_HANDLE;
        }
        system_ = XR_NULL_SYSTEM_ID;
        discovery_.reset();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::RetireSession */
    Result<void> OpenXRNativeSession::RetireSession() {
        if (controlThread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (retirementCall_ || !discovery_ || discovery_->phase == DiscoveryPhase::Retiring ||
            discovery_->phase == DiscoveryPhase::Preparing || discovery_->phase == DiscoveryPhase::PreparingSession)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const RetirementOperation operation{retirementCall_};
        discovery_->phase = DiscoveryPhase::RetiringSession;
        owner_ = {};
        preparing_ = {};
        retainedPreflight_.reset();
        retainedPlan_.reset();
        if (auto retired = DestroySession(); retired.HasError())
            return retired;
        discovery_->phase = DiscoveryPhase::Prepared;
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
