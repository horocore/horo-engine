#include "host/OpenXRNativeSession.h"

namespace Horo::XR::OpenXRInternal {
    /** @copydoc NativePassthroughExtensionRequest */
    std::optional<NativeExtensionRequest> NativePassthroughExtensionRequest() noexcept {
        return std::nullopt;
    }

    /** @copydoc OpenXRNativeSession::StartPassthrough */
    Result<void> OpenXRNativeSession::StartPassthrough(const XRPassthroughScope &) {
        return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
    }

    /** @copydoc OpenXRNativeSession::ReadPassthrough */
    NativePassthroughState OpenXRNativeSession::ReadPassthrough(const XRPassthroughScope &) const noexcept {
        return NativePassthroughState::Absent;
    }

    /** @copydoc OpenXRNativeSession::RevokePassthrough(const XRPassthroughScope &) */
    void OpenXRNativeSession::RevokePassthrough(const XRPassthroughScope &) noexcept {}

    /** @copydoc OpenXRNativeSession::StopPassthrough */
    Result<void> OpenXRNativeSession::StopPassthrough(const XRPassthroughScope &) {
        return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
    }

    /** @copydoc OpenXRNativeSession::ObservePassthroughEvent */
    void OpenXRNativeSession::ObservePassthroughEvent(const XrEventDataBuffer &) noexcept {}

    /** @copydoc OpenXRNativeSession::RevokePassthrough */
    void OpenXRNativeSession::RevokePassthrough() noexcept {}

    /** @copydoc OpenXRNativeSession::RetirePassthrough */
    Result<void> OpenXRNativeSession::RetirePassthrough() {
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::EndPassthroughFrame */
    Result<XrResult> OpenXRNativeSession::EndPassthroughFrame(const XRPassthroughScope &, XrSession, const XrFrameEndInfo &,
                                                              PFN_xrEndFrame) {
        return Result<XrResult>::Failure(MakeError(XRErrors::OperationUnsupported));
    }
}  // namespace Horo::XR::OpenXRInternal
