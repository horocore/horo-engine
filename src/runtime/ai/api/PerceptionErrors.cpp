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
    const ErrorCodeDescriptor PerceptionFilterInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.filter_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A perception filter policy or gameplay candidate fact is invalid.",
        .remediationHint = "Provide typed affiliation, team, layer, and gameplay visibility facts from the active simulation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor PerceptionFilterRevisionInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.perception.filter_revision_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A perception policy change is stale, duplicated, missing, or revision-exhausted.",
        .remediationHint = "Stage one change against the current revision and commit it once at the sensing safe point.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::AI::AIErrors
