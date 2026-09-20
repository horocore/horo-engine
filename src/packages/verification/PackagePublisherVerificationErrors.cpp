#include "Horo/Packages/PackagePublisherVerificationErrors.h"

namespace Horo::Packages::PublisherVerificationErrors {
    namespace {
        const ErrorDomainId Domain{"horo.packages.publisher_verification"};

        [[nodiscard]] ErrorCodeDescriptor Descriptor(const char *code, const char *summary, const char *hint, const bool retryable = false,
                                                     const bool userActionable = false) {
            return {.domain = Domain,
                    .code = ErrorCode{code},
                    .defaultSeverity = ErrorSeverity::Error,
                    .summary = summary,
                    .remediationHint = hint,
                    .retryable = retryable,
                    .userActionable = userActionable};
        }
    }  // namespace

    const ErrorCodeDescriptor InvalidInput =
        Descriptor("publisher_verification.invalid_input", "Publisher verification input or policy is malformed.",
                   "Provide canonical bounded package, publisher, key, and signature data.", false, true);
    const ErrorCodeDescriptor ResourceLimit =
        Descriptor("publisher_verification.resource_limit", "Publisher verification input exceeds its resource policy.",
                   "Reduce the artifact, signature, key, publisher, or audit history size.", false, true);
    const ErrorCodeDescriptor Cancelled =
        Descriptor("publisher_verification.cancelled", "Publisher verification was cancelled before it could publish a decision.",
                   "Retry the verification when the operation is still required.", true, false);
    const ErrorCodeDescriptor AuditCapacityExceeded =
        Descriptor("publisher_verification.audit_capacity_exceeded", "Publisher verification audit capacity is exhausted.",
                   "Start a new bounded verification session or retire its audit history.");
    const ErrorCodeDescriptor LifecycleClosed =
        Descriptor("publisher_verification.lifecycle_closed", "Publisher verification admission is closed.",
                   "Create a new verification service for another installation session.");
}  // namespace Horo::Packages::PublisherVerificationErrors
