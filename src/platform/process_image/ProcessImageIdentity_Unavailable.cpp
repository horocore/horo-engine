#include "ProcessImageIdentity.h"

namespace Horo::Platform {
    namespace {
        const ErrorCodeDescriptor Unavailable{.domain = ErrorDomainId{"horo.platform.process_image"},
                                              .code = ErrorCode{"platform.process_image.unavailable"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The selected platform has no qualified process image provider.",
                                              .remediationHint =
                                                  "Use a supported native image provider; no image lease or digest was issued.",
                                              .retryable = false,
                                              .userActionable = true};
    }

    Result<void> ProcessImageIdentity::Verify() const {
        return Result<void>::Failure(MakeError(Unavailable));
    }

    Result<void> ProcessImageIdentity::VerifyStaticCodeAddress(const void *) const {
        return Result<void>::Failure(MakeError(Unavailable));
    }

    Result<ProcessImageIdentity> CaptureCurrentProcessImageIdentity() {
        return Result<ProcessImageIdentity>::Failure(MakeError(Unavailable));
    }
}  // namespace Horo::Platform
