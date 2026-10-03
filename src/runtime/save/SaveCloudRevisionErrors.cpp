#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Runtime::SaveErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.save"};
        constexpr ErrorSeverity kError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor CloudMetadataInvalid{kDomain, ErrorCode{"save.cloud_metadata.invalid"}, kError,
                                                   "Cloud revision metadata is malformed or outside its authority scope.",
                                                   "Discard the sidecar and reconcile from the local slot index."};
    const ErrorCodeDescriptor CloudMetadataStale{kDomain, ErrorCode{"save.cloud_metadata.stale"}, kError,
                                                 "Cloud revision metadata does not match the selected local generation.",
                                                 "Reconcile under the current local catalog and provider scope."};
    const ErrorCodeDescriptor CloudMetadataLimitExceeded{kDomain, ErrorCode{"save.cloud_metadata.limit_exceeded"}, kError,
                                                         "Cloud revision metadata exceeds a trusted bound.",
                                                         "Reduce metadata or use a qualified finite limit."};
    const ErrorCodeDescriptor CloudMetadataAllocationFailed{kDomain,
                                                            ErrorCode{"save.cloud_metadata.allocation_failed"},
                                                            kError,
                                                            "Cloud revision metadata could not be allocated.",
                                                            "Keep the prior catalog pair and retry after reducing memory pressure.",
                                                            true};
}  // namespace Horo::Runtime::SaveErrors
