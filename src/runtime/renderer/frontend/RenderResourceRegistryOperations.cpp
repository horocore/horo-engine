#include "RenderFrontendErrors.h"
#include "RenderResourceRegistry.h"

#include <algorithm>
#include <string>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Adds observation context without translating the registry error descriptor. */
        Error ObservationError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }
    }  // namespace

    /** @copydoc RenderResourceRegistry::LookupOperation */
    Result<const RenderResourceRegistry::OperationRecord *> RenderResourceRegistry::LookupOperation(
        const ResourceOperationId operation) const {
        using Lookup = Result<const OperationRecord *>;
        if (!operation.IsValid())
            return Lookup::Failure(
                ObservationError(FrontendErrors::ResourceOperationUnknown, "The resource operation identity is invalid."));
        const auto record = std::ranges::find(operations_, operation, &OperationRecord::id);
        if (record == operations_.end())
            return Lookup::Failure(
                ObservationError(FrontendErrors::ResourceOperationUnknown, "The resource operation does not belong to this registry."));
        return Lookup::Success(&*record);
    }

    /** @copydoc RenderResourceRegistry::OperationState */
    Result<RenderResourceOperationState> RenderResourceRegistry::OperationState(const ResourceOperationId operation) const {
        using Observation = Result<RenderResourceOperationState>;
        if (!acceptingRequests_)
            return Observation::Failure(
                ObservationError(FrontendErrors::ResourceRegistryStopped, "Resource observations are disabled during frontend shutdown."));
        const auto found = LookupOperation(operation);
        if (found.HasError())
            return Observation::Failure(found.ErrorValue());
        const auto &record = *found.Value();
        return Observation::Success(
            !record.complete ? RenderResourceOperationState::Pending
                             : (record.error.has_value() ? RenderResourceOperationState::Failed : RenderResourceOperationState::Succeeded));
    }

    Result<void> RenderResourceRegistry::OperationResult(const ResourceOperationId operation) const {
        const auto found = LookupOperation(operation);
        if (found.HasError())
            return Result<void>::Failure(found.ErrorValue());
        const auto &record = *found.Value();
        if (!record.complete)
            return Result<void>::Failure(
                ObservationError(FrontendErrors::ResourceOperationPending, "The renderer resource operation has not completed."));
        if (record.error.has_value())
            return Result<void>::Failure(*record.error);
        return Result<void>::Success();
    }

}  // namespace Horo::Render::Detail
