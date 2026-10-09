#include "RenderFrontendErrors.h"
#include "RenderResourceRegistry.h"

#include <limits>

namespace Horo::Render::Detail {
    Result<void> RenderResourceRegistry::AddSubmissionPin(const RenderResourceClass resourceClass, const RenderResourceIdentity identity) {
        const auto validated = Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = entries_[validated.Value()];
        if (entry.state != RenderResourceState::Ready) {
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceNotReady, "Only a ready resource may enter a new submission."));
        }
        if (activeSubmissionPins_ >= limits_.maximumSubmissionPins) {
            return Result<void>::Failure(
                MakeError(FrontendErrors::ResourceSubmissionCapacityExceeded, "The shared resident submission-pin budget is exhausted."));
        }
        if (entry.submissionPins == std::numeric_limits<std::uint32_t>::max()) {
            return Result<void>::Failure(
                MakeError(FrontendErrors::ResourceCapacityExhausted, "The renderer resource submission pin count is exhausted."));
        }
        ++entry.submissionPins;
        ++activeSubmissionPins_;
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::ReleaseSubmissionPin(const RenderResourceClass resourceClass,
                                                              const RenderResourceIdentity identity) {
        const auto validated = Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = entries_[validated.Value()];
        if (entry.submissionPins == 0) {
            return Result<void>::Failure(
                MakeError(FrontendErrors::ResourceHandleMalformed, "The renderer resource has no submission pin to release."));
        }
        --entry.submissionPins;
        --activeSubmissionPins_;
        QueueRetirementIfEligible(identity.slot);
        return Result<void>::Success();
    }

}  // namespace Horo::Render::Detail
