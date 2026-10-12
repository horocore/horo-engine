#include "Horo/Animation/AnimationClip.h"
#include "Horo/Animation/AnimationErrors.h"

#include <string>

namespace Horo::Animation {
    /** @copydoc DescribeAnimationClipFailure */
    Error DescribeAnimationClipFailure(const AnimationClipFailureInfo failure) {
        const ErrorCodeDescriptor *descriptor = &AnimationErrors::ClipUnsupported;
        switch (failure.failure) {
            case AnimationClipFailure::Cancelled:
                descriptor = &AnimationErrors::ClipOperationCancelled;
                break;
            case AnimationClipFailure::AdmissionRejected:
                descriptor = &AnimationErrors::ClipAdmissionRejected;
                break;
            case AnimationClipFailure::IdentityInvalid:
                descriptor = &AnimationErrors::IdentityInvalid;
                break;
            case AnimationClipFailure::LimitExceeded:
                descriptor = &AnimationErrors::ClipLimitExceeded;
                break;
            case AnimationClipFailure::Malformed:
                descriptor = &AnimationErrors::ClipMalformed;
                break;
            case AnimationClipFailure::Unsupported:
                descriptor = &AnimationErrors::ClipUnsupported;
                break;
            case AnimationClipFailure::TimeOverflow:
                descriptor = &AnimationErrors::ClipTimeOverflow;
                break;
            case AnimationClipFailure::SkeletonMismatch:
                descriptor = &AnimationErrors::ClipSkeletonMismatch;
                break;
            case AnimationClipFailure::BindingStale:
                descriptor = &AnimationErrors::ClipBindingStale;
                break;
            case AnimationClipFailure::ReferencePoseMismatch:
                descriptor = &AnimationErrors::ClipReferencePoseMismatch;
                break;
            case AnimationClipFailure::None:
                break;
        }
        return MakeError(*descriptor, std::string{failure.detail});
    }

    /** @copydoc AnimationClipAsset::Traverse */
    Result<AnimationClipTraversal> AnimationClipAsset::Traverse(AnimationClipCursor cursor, AnimationTimeDelta delta,
                                                                const AnimationClipLimits &limits,
                                                                AnimationClipAdmissionState admission) const {
        const auto result = TraversePrepared(cursor, delta, limits, admission);
        if (result.HasError())
            return Result<AnimationClipTraversal>::Failure(DescribeAnimationClipFailure(result.ErrorValue()));
        return Result<AnimationClipTraversal>::Success(result.Value());
    }

    /** @copydoc AnimationClipAsset::Sample */
    Result<AnimationClipSample> AnimationClipAsset::Sample(AnimationTime time, const AnimationClipSampleContext &context) const {
        const auto result = SamplePrepared(time, context);
        if (result.HasError())
            return Result<AnimationClipSample>::Failure(DescribeAnimationClipFailure(result.ErrorValue()));
        return Result<AnimationClipSample>::Success(result.Value());
    }

}  // namespace Horo::Animation
