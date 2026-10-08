#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Runtime::SaveErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.save"};
        constexpr auto kError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor CommandInvalid{kDomain, ErrorCode{"save.command.invalid"}, kError, "Save command inputs are invalid.",
                                             "Use exact typed command revisions and target identities."};
    const ErrorCodeDescriptor CommandDenied{kDomain, ErrorCode{"save.command.denied"}, kError,
                                            "Host authority or product policy denies this save command.",
                                            "Use an authorized session and an enabled product mode."};
    const ErrorCodeDescriptor CommandIneligible{kDomain, ErrorCode{"save.command.ineligible"}, kError,
                                                "Current runtime activity is ineligible for this save command.",
                                                "Retry only after the host reaches the product's permitted activity."};
    const ErrorCodeDescriptor CommandStale{kDomain, ErrorCode{"save.command.stale"}, kError,
                                           "Save command revisions or target generation are stale.",
                                           "Refresh the command view and recapture confirmation against the current target."};
    const ErrorCodeDescriptor CommandTargetUnavailable{kDomain, ErrorCode{"save.command.target_unavailable"}, kError,
                                                       "Save command target is missing, reserved, or outside retention capacity.",
                                                       "Select an existing load target or an available slot of the correct kind."};
    const ErrorCodeDescriptor CommandIncompatible{kDomain, ErrorCode{"save.command.incompatible"}, kError,
                                                  "The selected save generation lacks an admissible compatibility/integrity assessment.",
                                                  "Refresh exact generation assessments; do not bypass archive verification."};
    const ErrorCodeDescriptor CommandCooldown{kDomain,
                                              ErrorCode{"save.command.cooldown"},
                                              kError,
                                              "Save command cooldown has not elapsed or the host clock regressed.",
                                              "Retry with a nondecreasing monotonic clock after the product cooldown.",
                                              true};
}  // namespace Horo::Runtime::SaveErrors
