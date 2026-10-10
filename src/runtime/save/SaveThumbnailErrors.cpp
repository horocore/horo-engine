#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Runtime::SaveErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.save"};
        constexpr auto kError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor ThumbnailInvalid{kDomain, ErrorCode{"save.thumbnail.invalid"}, kError,
                                               "Save thumbnail request or completion is invalid.",
                                               "Use bounded PNG dimensions, bytes and monotonic owner time."};
    const ErrorCodeDescriptor ThumbnailBusy{kDomain, ErrorCode{"save.thumbnail.busy"}, kError,
                                            "Save thumbnail admission has no free request capacity.",
                                            "Acknowledge terminal capture before admitting another request."};
    const ErrorCodeDescriptor ThumbnailUnavailable{kDomain, ErrorCode{"save.thumbnail.unavailable"}, kError,
                                                   "Save thumbnail capture is unavailable.",
                                                   "Use optional capture or a compatible renderer composition."};
    const ErrorCodeDescriptor ThumbnailExpired{kDomain, ErrorCode{"save.thumbnail.expired"}, kError,
                                               "Save thumbnail capture exceeded its finite deadline.",
                                               "Omit optional capture or retry required capture asynchronously."};
    const ErrorCodeDescriptor ThumbnailStale{kDomain, ErrorCode{"save.thumbnail.stale"}, kError,
                                             "Save thumbnail belongs to a different source or committed generation.",
                                             "Refresh exact source and publication evidence."};
    const ErrorCodeDescriptor ThumbnailCancelled{kDomain, ErrorCode{"save.thumbnail.cancelled"}, kError,
                                                 "Save thumbnail capture was cancelled or closed.",
                                                 "Retire renderer readback independently without publishing late results."};
    const ErrorCodeDescriptor ThumbnailAllocationFailed{kDomain,
                                                        ErrorCode{"save.thumbnail.allocation_failed"},
                                                        kError,
                                                        "Save thumbnail CPU ownership could not be allocated.",
                                                        "Reduce memory pressure or omit optional capture.",
                                                        true};
}  // namespace Horo::Runtime::SaveErrors
