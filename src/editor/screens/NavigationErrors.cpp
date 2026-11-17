#include "NavigationErrors.h"

namespace Horo::Editor::NavigationErrors {
    namespace {
        const ErrorDomainId ScreenHostDomain{"horo.editor.screens"};
        const ErrorDomainId ProjectCreationDomain{"horo.editor.project_creation"};
        const ErrorDomainId ProjectLoadingDomain{"horo.editor.project_loading"};
        const ErrorDomainId WorkspaceDomain{"horo.editor.workspace"};
    }  // namespace

    const ErrorCodeDescriptor HostShutdown{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.host_shutdown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Screen host is shut down.",
        .remediationHint = "Create a new screen host before requesting navigation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HostNotStarted{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.host_not_started"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Screen host has not entered its initial route.",
        .remediationHint = "Register composition-owned services and start the screen host before navigation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HostAlreadyStarted{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.host_already_started"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Screen host already entered its initial route.",
        .remediationHint = "Use Navigate for route changes after startup.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ScreenCreationFailed{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.screen_creation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "No screen factory could create the requested route.",
        .remediationHint = "Register the route factory and all of its required services before startup.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor InvalidRouteParameters{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.invalid_route_parameters"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Route parameters do not match the requested route.",
        .remediationHint = "Construct the route with its matching typed parameter payload.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor FixedDispatchInvalid{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.fixed_dispatch_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Screen fixed update does not match its active scheduler dispatch.",
        .remediationHint = "Bind the actual scheduler issuer and finalize the same fixed attempt before screen replacement.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor Busy{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.busy"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Navigation is already in progress.",
        .remediationHint = "Wait for the pending navigation or leave resolution to finish.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor LeaveDenied{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.leave_denied"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The active screen denied the leave transition.",
        .remediationHint = "Resolve the active screen state before navigating away.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidLeaveRequirement{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.invalid_leave_requirement"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A screen requested leave resolution without a requirement.",
        .remediationHint = "Return a complete typed leave requirement from the active screen.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor LeaveResolutionLimitExceeded{
        .domain = ScreenHostDomain,
        .code = ErrorCode{"navigation.leave_resolution_limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The leave resolution attempt limit was exceeded.",
        .remediationHint = "Ensure each leave resolution advances to a new subject or revision.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ProjectCreationStaleLeaveSubject{
        .domain = ProjectCreationDomain,
        .code = ErrorCode{"navigation.stale_leave_subject"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Project creation leave requirement is stale.",
        .remediationHint = "Refresh the current leave requirement before resolving it.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ProjectCreationLeaveActionNotAllowed{
        .domain = ProjectCreationDomain,
        .code = ErrorCode{"navigation.leave_action_not_allowed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Project creation leave action is not allowed.",
        .remediationHint = "Choose an action declared by the current leave requirement.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProjectLoadingStaleLeaveSubject{
        .domain = ProjectLoadingDomain,
        .code = ErrorCode{"navigation.stale_leave_subject"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Project loading leave requirement is stale.",
        .remediationHint = "Refresh the current leave requirement before resolving it.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ProjectLoadingLeaveActionNotAllowed{
        .domain = ProjectLoadingDomain,
        .code = ErrorCode{"navigation.leave_action_not_allowed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Project loading leave action is not allowed.",
        .remediationHint = "Choose an action declared by the current leave requirement.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor WorkspaceStaleLeaveSubject{
        .domain = WorkspaceDomain,
        .code = ErrorCode{"navigation.stale_leave_subject"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Workspace leave requirement is stale.",
        .remediationHint = "Refresh the current leave requirement before resolving it.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor WorkspaceLeaveActionNotAllowed{
        .domain = WorkspaceDomain,
        .code = ErrorCode{"navigation.leave_action_not_allowed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Workspace leave action is not allowed.",
        .remediationHint = "Choose an action declared by the current leave requirement.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::Editor::NavigationErrors
