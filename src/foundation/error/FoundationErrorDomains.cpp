#include "Horo/Foundation/FoundationErrorDomains.h"

#include "../FoundationErrors.h"

namespace Horo {
    /** @copydoc JobSystemErrorDomain */
    ModuleErrorDomainDescriptor JobSystemErrorDomain() {
        return {.id = JobErrors::Cancelled.domain,
                .descriptors = {&JobErrors::Cancelled, &JobErrors::Failed, &JobErrors::InvalidHandle, &JobErrors::NotFound,
                                &JobErrors::QueueFull, &JobErrors::QueueShed, &JobErrors::InvalidSubmission, &JobErrors::Shutdown,
                                &JobErrors::TaskGroupClosed, &JobErrors::WaitForbidden, &JobErrors::WaitTimedOut,
                                &JobErrors::WaitCapacityDeadlock, &JobErrors::InvalidProgress, &JobErrors::ProgressRegressed,
                                &JobErrors::TerminalImmutable}};
    }

    /** @copydoc SceneMathErrorDomain */
    ModuleErrorDomainDescriptor SceneMathErrorDomain() {
        return {.id = Math::Errors::NonFiniteInput.domain,
                .descriptors = {&Math::Errors::InvalidAffineMatrix, &Math::Errors::InvalidBounds, &Math::Errors::InvalidHomogeneousPoint,
                                &Math::Errors::InvalidPlane, &Math::Errors::InvalidProjection, &Math::Errors::InvalidRay,
                                &Math::Errors::InvalidView, &Math::Errors::NonFiniteInput, &Math::Errors::SingularMatrix,
                                &Math::Errors::ZeroLength}};
    }
}  // namespace Horo
