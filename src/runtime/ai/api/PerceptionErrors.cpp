#include "Horo/AI/AIErrors.h"

namespace Horo::AI::AIErrors {
    namespace {
        const ErrorDomainId AiDomain{"horo.ai"};
    }

    const ErrorCodeDescriptor PerceptionDescriptorInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A perception descriptor has invalid identity, source, version, capability, or dependency metadata.",
        .remediationHint = "Correct the typed descriptor metadata before composing the SceneRuntime perception registry.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionDescriptorLimitExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.descriptor_limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The perception descriptor registry exceeds a configured bounded capacity.",
        .remediationHint = "Reduce or partition perception contributions before SceneRuntime activation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionDescriptorConflict{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.descriptor_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Multiple perception descriptors claim the same stable type identity.",
        .remediationHint = "Assign one globally unique stable identity to each sense, stimulus, and listener type.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionDependencyMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.dependency_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required perception listener dependency is not registered.",
        .remediationHint = "Register the exact referenced sense and stimulus descriptors or make the listener optional.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionDescriptorIncompatible{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.descriptor_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A perception sense or stimulus descriptor version is incompatible with its listener.",
        .remediationHint = "Install a descriptor version inside the listener's declared inclusive compatibility interval.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionCapabilityUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.capability_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The composition root cannot satisfy a required perception listener capability.",
        .remediationHint = "Provide the declared gameplay query service or disable the dependent listener explicitly.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionRegistryStorageUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.registry_storage_unavailable"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "Storage for an immutable perception descriptor registry is unavailable.",
        .remediationHint = "Release memory pressure and retry registry composition before SceneRuntime activation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor PerceptionMemoryInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.memory_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A perception memory policy, observation, source, or liveness adapter is invalid.",
        .remediationHint = "Supply bounded policy and typed sources from the active scene incarnation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionMemoryTimeInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.memory_time_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Perception memory received a backward committed simulation tick.",
        .remediationHint = "Use the active fixed-tick simulation clock or reset memory before replay rewind.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionEventInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.event_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A gameplay perception event or admission adapter is malformed.",
        .remediationHint = "Supply typed facts and exact scene identities from the current authoritative event producer.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor PerceptionEventUnauthorized{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.event_unauthorized"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Gameplay did not authorize this recipient or disclosure of the perception event.",
        .remediationHint = "Deliver through the Gameplay owner after applying team and network authority policy.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor PerceptionEventStale{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.event_stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "A perception event references a retired source, sender or recipient generation.",
        .remediationHint = "Discard the stale delivery and acquire current scene participants before retrying.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor PerceptionEventFiltered{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.event_filtered"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "The recipient's current perception filter rejected the gameplay event.",
        .remediationHint = "No action is needed unless the listener's declared filter policy is incorrect.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor PerceptionSpatialInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.spatial_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A perception spatial record or query has invalid identity, sense, layer, range, or filter data.",
        .remediationHint = "Capture finite typed participants at the scene safe point and submit a bounded query.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionSpatialLimitExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.spatial_limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The declared perception spatial population exceeds a hard scene capacity.",
        .remediationHint = "Reduce or partition declared listeners and sources before publication.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionSpatialConflict{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.spatial_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A perception spatial publication repeats an exact listener or source entity identity.",
        .remediationHint = "Publish each declared participant once per scene safe point.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionSpatialStale{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.spatial_stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The perception spatial scene view or publication revision is stale.",
        .remediationHint = "Acquire a fresh post-commit scene view and publish an increasing revision.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor PerceptionSpatialListenerMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.spatial_listener_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested listener is absent from the immutable perception spatial snapshot.",
        .remediationHint = "Use a listener declared in the same published scene revision.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::AI::AIErrors
