#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Editor::NavigationErrors {
    extern const ErrorCodeDescriptor HostShutdown;
    extern const ErrorCodeDescriptor HostNotStarted;
    extern const ErrorCodeDescriptor HostAlreadyStarted;
    extern const ErrorCodeDescriptor ScreenCreationFailed;
    extern const ErrorCodeDescriptor InvalidRouteParameters;
    extern const ErrorCodeDescriptor Busy;
    extern const ErrorCodeDescriptor FixedDispatchInvalid;
    extern const ErrorCodeDescriptor LeaveDenied;
    extern const ErrorCodeDescriptor InvalidLeaveRequirement;
    extern const ErrorCodeDescriptor LeaveResolutionLimitExceeded;

    extern const ErrorCodeDescriptor ProjectCreationStaleLeaveSubject;
    extern const ErrorCodeDescriptor ProjectCreationLeaveActionNotAllowed;
    extern const ErrorCodeDescriptor ProjectLoadingStaleLeaveSubject;
    extern const ErrorCodeDescriptor ProjectLoadingLeaveActionNotAllowed;
    extern const ErrorCodeDescriptor WorkspaceStaleLeaveSubject;
    extern const ErrorCodeDescriptor WorkspaceLeaveActionNotAllowed;
}  // namespace Horo::Editor::NavigationErrors
