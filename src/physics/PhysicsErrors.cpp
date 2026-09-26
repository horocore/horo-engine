#include "Horo/Physics/PhysicsErrors.h"

#include <array>

namespace Horo::Physics::PhysicsErrors {
    namespace {
        const ErrorDomainId PhysicsDomain{"horo.physics"};
    }

    const ErrorCodeDescriptor WorldInvalid{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.world.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The physics world identity is invalid.",
        .remediationHint = "Use the non-zero world generation published by the owning scene activation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HandleMalformed{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.handle.malformed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The physics handle is malformed.",
        .remediationHint = "Use a typed handle issued by the owning world with a valid index and non-zero slot generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HandleWorldMismatch{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.handle.world_mismatch"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The physics handle belongs to another world generation.",
        .remediationHint = "Resolve the stable scene binding again against the active physics world.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HandleStale{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.handle.stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The physics object slot is absent or its generation has retired.",
        .remediationHint = "Discard the handle and resolve its stable binding before submitting new work.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor GenerationExhausted{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.generation.exhausted"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "The physics identity generation range is exhausted.",
        .remediationHint = "Retire exhausted object slots; never wrap identities or reuse a published world generation.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CapabilityUnavailable{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.capability.unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required physics capability is unavailable.",
        .remediationHint =
            "Select a composition and qualified profile providing the required capability; do not silently substitute a null world.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OperationUnsupported{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.operation.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The physics profile does not support this operation.",
        .remediationHint = "Use a supported operation or explicitly admit a qualified profile that supports it.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor TransformAuthorityViolation{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.transform.authority_violation"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A transform write conflicts with the body's declared Physics authority.",
        .remediationHint =
            "Submit a static update or kinematic target at the pre-step safe point, or use an explicit dynamic teleport/reset.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor InvalidState{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.state.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The physics world lifecycle phase cannot admit this operation.",
        .remediationHint = "Submit work only during its declared owner-thread phase and before world admission closes.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ThreadAffinityViolation{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.thread_affinity.violation"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A mutable physics operation ran outside its owning thread.",
        .remediationHint = "Route world mutation and fixed-tick control through the Physics owner thread.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor SolverDeadlineExceeded{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.solver.deadline_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Physics solver child work exceeded its fixed-tick deadline.",
        .remediationHint = "Reduce the admitted batch or make each child observe cooperative cancellation promptly.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor SolverValidationMessage{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.solver.validation"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The native physics solver reported a validation message.",
        .remediationHint = "Inspect the bounded solver evidence and correct the originating Physics configuration or content.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SolverAssertionFailed{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.solver.assertion_failed"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "A native physics solver invariant failed.",
        .remediationHint = "Retain the prior coherent publication, reset the world only after inspecting the bounded assertion evidence.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor SolverFatalCondition{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.solver.fatal_condition"},
        .defaultSeverity = ErrorSeverity::Critical,
        .summary = "The native physics solver reported a fatal condition.",
        .remediationHint =
            "Retain the prior coherent publication and rebuild the world only after resolving the reported solver condition.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor DescriptorInvalid{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.descriptor.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The physics descriptor metadata is invalid.",
        .remediationHint = "Provide the supported schema, known typed values and finite data before native preparation.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CommandOrderInvalid{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.command_order.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A physics command lacks valid canonical ordering evidence.",
        .remediationHint = "Provide the supported protocol and exact tick, generations, stable target, source and source sequence.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor SeedPolicyInvalid{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.seed_policy.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A physics seed policy or consumption tuple is invalid.",
        .remediationHint = "Use the supported algorithm/version and explicit world, stream, owner and consumption sequence.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ProfileUnsupported{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.profile.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested physics profile is unsupported.",
        .remediationHint = "Select an explicitly supported profile; do not silently change solver or numeric policy.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CapacityExceeded{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.capacity.exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested physics capacity exceeds its profile or has no plan budget.",
        .remediationHint = "Lower requested limits or admit a separately qualified profile and reserve sufficient plan memory.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CapabilityStale{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.capability.stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Physics capability evidence changed during admission.",
        .remediationHint = "Capture the current owner-published snapshot and revalidate the complete preparation request.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CapabilityRevoked{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.capability.revoked"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The physics client capability has been revoked.",
        .remediationHint = "Request a newly issued capability from the owning host before accessing Physics again.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor QuerySnapshotStale{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.query.snapshot_stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The physics query snapshot generation is stale.",
        .remediationHint = "Capture the active world, scene and filter generations again before submitting the query.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor InitializationFailed{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.initialization.failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Physics initialization failed before publication.",
        .remediationHint = "Inspect the failed preparation stage and its resource policy before retrying; the prior world is unchanged.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ShapeCookSourceInvalid{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.shape_cook.source_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Physics shape source geometry is invalid.",
        .remediationHint = "Repair the named source vertices or choose an explicit supported authoring operation before cooking.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ShapeCookLimitExceeded{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.shape_cook.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Physics shape cooking exceeded a configured limit.",
        .remediationHint = "Reduce the source hull or select a separately qualified profile; topology is never silently truncated.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ShapeCookCancelled{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.shape_cook.cancelled"},
        .defaultSeverity = ErrorSeverity::Info,
        .summary = "Physics shape cooking was cancelled.",
        .remediationHint = "Retry only while the captured source, settings and target identity remain current.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ShapeMotionUnsupported{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.shape.motion_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The cooked physics shape does not support the requested body motion mode.",
        .remediationHint = "Use static motion for concave geometry or choose an explicitly authored convex shape.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ShapeArtifactInvalid{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.shape_artifact.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The cooked Physics shape artifact is invalid.",
        .remediationHint = "Discard the artifact and recook it from validated source for the exact active Physics target.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MaterialDescriptorInvalid{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.material.descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The reusable Physics material asset descriptor is invalid.",
        .remediationHint = "Provide the supported schema, stable asset identity, finite bounded SI values and non-zero revision.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor MaterialCombineUnsupported{
        .domain = PhysicsDomain,
        .code = ErrorCode{"physics.material.combine_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The Physics material combine mode is unsupported.",
        .remediationHint = "Use Average, Minimum, Multiply or Maximum for each physical contact coefficient.",
        .retryable = false,
        .userActionable = true,
    };

    /** @copydoc Descriptors */
    std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept {
        static const std::array descriptors{
            &WorldInvalid,
            &HandleMalformed,
            &HandleWorldMismatch,
            &HandleStale,
            &GenerationExhausted,
            &CapabilityUnavailable,
            &OperationUnsupported,
            &TransformAuthorityViolation,
            &InvalidState,
            &ThreadAffinityViolation,
            &SolverDeadlineExceeded,
            &SolverValidationMessage,
            &SolverAssertionFailed,
            &SolverFatalCondition,
            &DescriptorInvalid,
            &CommandOrderInvalid,
            &SeedPolicyInvalid,
            &ProfileUnsupported,
            &CapacityExceeded,
            &CapabilityStale,
            &CapabilityRevoked,
            &QuerySnapshotStale,
            &InitializationFailed,
            &ShapeCookSourceInvalid,
            &ShapeCookLimitExceeded,
            &ShapeCookCancelled,
            &ShapeMotionUnsupported,
            &ShapeArtifactInvalid,
            &MaterialDescriptorInvalid,
            &MaterialCombineUnsupported,
        };
        return descriptors;
    }
}  // namespace Horo::Physics::PhysicsErrors
