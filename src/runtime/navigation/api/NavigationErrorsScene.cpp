#include "Horo/Navigation/NavigationErrors.h"

namespace Horo::Navigation::NavigationErrors {
    namespace {
        const ErrorDomainId NavigationDomain{"horo.navigation"};
    }  // namespace

    const ErrorCodeDescriptor SceneComponentInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.scene_component_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A Scene navigation component is invalid.",
        .remediationHint = "Repair the component identity, version, bounds, definition, surface, profiles, area, direction, or cost.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SceneComponentConflict{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.scene_component_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Scene navigation component identities conflict.",
        .remediationHint = "Assign distinct stable identities within each navigation component domain before committing the Scene.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SceneSurfaceMissing{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.scene_surface_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A Scene navigation component references a missing surface.",
        .remediationHint = "Restore the exact surface or explicitly retarget the region, modifier, or link endpoint.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SceneProfileMismatch{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.scene_profile_mismatch"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A grounded navigation link has incompatible endpoint profiles.",
        .remediationHint = "Select only grounded profiles present on both exact endpoint surfaces.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AgentDescriptorInvalid{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.agent.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A Scene navigation-agent descriptor is invalid.",
        .remediationHint =
            "Use non-zero profile and filter identities, a finite positive radius override, and a supported movement capability.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AgentRegistryConflict{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.agent.registry_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The navigation-agent candidate contains a duplicate owner or active identity.",
        .remediationHint = "Publish at most one navigation agent for each exact runtime entity generation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AgentRegistryCapacityExceeded{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.agent.registry_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The navigation-agent registry cannot retain the complete candidate population.",
        .remediationHint = "Reduce the Scene agent population or select a profile with a larger declared agent capacity.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor AgentRegistryStale{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.agent.registry_stale"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "A navigation-agent registration belongs to a replaced Scene, world, or entity generation.",
        .remediationHint = "Resolve the active Scene entity and register the agent again at the next safe point.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AgentRegistryShuttingDown{
        .domain = NavigationDomain,
        .code = ErrorCode{"navigation.agent.registry_shutting_down"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "Navigation-agent registration is closed during teardown.",
        .remediationHint = "Do not admit new agent registration after Scene or application shutdown begins.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::Navigation::NavigationErrors
