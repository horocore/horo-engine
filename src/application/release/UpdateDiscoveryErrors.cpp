#include "Horo/Release/UpdateDiscoveryErrors.h"

namespace Horo::Release::UpdateDiscoveryErrors {
    namespace {
        const ErrorDomainId Domain{"horo.release.update"};
    }

    const ErrorCodeDescriptor InvalidPolicy{.domain = Domain,
                                            .code = ErrorCode{"release.update_discovery.invalid_policy"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Update discovery policy is malformed or contradictory.",
                                            .remediationHint = "Choose a valid channel and check interval.",
                                            .userActionable = true};
    const ErrorCodeDescriptor ChannelChangeRequiresAction{.domain = Domain,
                                                          .code = ErrorCode{"release.update_discovery.channel_confirmation"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "Changing update channels requires an explicit action.",
                                                          .remediationHint = "Confirm the channel change before checking.",
                                                          .userActionable = true};
    const ErrorCodeDescriptor ClockMovedBackward{.domain = Domain,
                                                 .code = ErrorCode{"release.update_discovery.clock_backward"},
                                                 .defaultSeverity = ErrorSeverity::Warning,
                                                 .summary = "Host time predates the last successful update check.",
                                                 .remediationHint = "Correct the system clock or check manually.",
                                                 .userActionable = true};
}  // namespace Horo::Release::UpdateDiscoveryErrors
