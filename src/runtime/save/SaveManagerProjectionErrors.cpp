#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Runtime::SaveErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.save"};
        constexpr auto kError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor ManagerProjectionInvalid{kDomain, ErrorCode{"save.manager_projection.invalid"}, kError,
                                                       "Save-manager projection input or query is invalid.",
                                                       "Refresh detached source values and use a bounded typed query."};
    const ErrorCodeDescriptor ManagerProjectionLimitExceeded{kDomain, ErrorCode{"save.manager_projection.limit_exceeded"}, kError,
                                                             "Save-manager projection exceeds a qualified bound.",
                                                             "Reduce the source or page size before publishing the view."};
    const ErrorCodeDescriptor ManagerProjectionStale{kDomain, ErrorCode{"save.manager_projection.stale"}, kError,
                                                     "Save-manager selection no longer matches the current publication.",
                                                     "Refresh the view and recapture its revision and slot generation."};
    const ErrorCodeDescriptor ManagerProjectionAllocationFailed{kDomain,
                                                                ErrorCode{"save.manager_projection.allocation_failed"},
                                                                kError,
                                                                "Detached save-manager presentation could not be allocated.",
                                                                "Retain the previous view and retry after reducing memory pressure.",
                                                                true};
}  // namespace Horo::Runtime::SaveErrors
