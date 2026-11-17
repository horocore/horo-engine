#include "AndroidWindowErrors.h"

namespace Horo::Platform::Android {
    namespace AndroidWindowErrors {
        const ErrorCodeDescriptor OperationInProgress{
            .domain = ErrorDomainId{"horo.platform"},
            .code = ErrorCode{"android_window.operation_in_progress"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "An Android native window operation is already in progress.",
            .remediationHint = "Return from native dispatch before calling this owner again.",
            .retryable = true,
            .userActionable = false,
        };

        const ErrorCodeDescriptor UnsupportedBackend{
            .domain = ErrorDomainId{"horo.platform"},
            .code = ErrorCode{"android_window.unsupported_backend"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The fixed Android backend selection does not support native windows.",
            .remediationHint = "Select an admitted Android composition before forwarding windows.",
            .retryable = false,
            .userActionable = false,
        };

        const ErrorCodeDescriptor NoPresentation{
            .domain = ErrorDomainId{"horo.platform"},
            .code = ErrorCode{"android_window.no_presentation"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "This Android host selected no presentation.",
            .remediationHint = "Keep headless lifecycle operation independent of native presentation.",
            .retryable = false,
            .userActionable = false,
        };

        const ErrorCodeDescriptor AcquireFailed{
            .domain = ErrorDomainId{"horo.platform"},
            .code = ErrorCode{"android_window.acquire_failed"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The native Android window reference could not be acquired.",
            .remediationHint = "Do not publish the candidate; retry only while its native callback borrow remains valid.",
            .retryable = true,
            .userActionable = false,
        };
    }  // namespace AndroidWindowErrors

    /** @copydoc AndroidWindowErrorDomain */
    ModuleErrorDomainDescriptor AndroidWindowErrorDomain() {
        return {.id = ErrorDomainId{"horo.platform"},
                .descriptors = {&AndroidWindowErrors::OperationInProgress, &AndroidWindowErrors::UnsupportedBackend,
                                &AndroidWindowErrors::NoPresentation, &AndroidWindowErrors::AcquireFailed}};
    }
}  // namespace Horo::Platform::Android
