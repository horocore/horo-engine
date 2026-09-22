#include "Horo/AI/AIErrors.h"

namespace Horo::AI::AIErrors {
    namespace {
        const ErrorDomainId AiDomain{"horo.ai"};
    }

    const ErrorCodeDescriptor SceneComponentInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.scene_component.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An authored AI scene component has invalid typed lifecycle data.",
        .remediationHint = "Use non-zero identities, the current component schema, valid capabilities, and an admitted startup policy.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ControllerDescriptorMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.controller.descriptor_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An enabled AI controller has no matching immutable descriptor.",
        .remediationHint = "Load the controller descriptor and its decision and blackboard dependencies before scene activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ControllerDescriptorIncompatible{
        .domain = AiDomain,
        .code = ErrorCode{"ai.controller.descriptor_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An authored AI controller does not match its immutable descriptor dependencies.",
        .remediationHint = "Repair the controller, decision-asset, and blackboard-schema identities as one typed binding.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CapabilityUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.scene.capability_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The active host cannot stage a required AI capability.",
        .remediationHint = "Compose the behavior, navigation, or perception capability before publishing the SceneRuntime generation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SceneActivationInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.scene.activation_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The AI scene activation candidate is invalid or stale.",
        .remediationHint = "Discard the candidate and prepare a complete generation-fenced AI population again.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor AgentCapacityExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.scene.agent_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The bounded AI scene-agent capacity has been exhausted.",
        .remediationHint = "Reduce admitted scene agents or compose a larger explicit AI capacity.",
        .retryable = true,
        .userActionable = false,
    };
}  // namespace Horo::AI::AIErrors
