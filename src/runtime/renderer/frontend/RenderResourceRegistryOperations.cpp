#include "RenderFrontendErrors.h"
#include "RenderResourceRegistry.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Adds observation context without translating the registry error descriptor. */
        Error ObservationError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }
    }  // namespace

    /** @copydoc RenderResourceRegistry::OperationJournal::Lookup */
    Result<const RenderResourceRegistry::OperationRecord *> RenderResourceRegistry::OperationJournal::Lookup(
        const ResourceOperationId operation) const {
        using Lookup = Result<const OperationRecord *>;
        if (!operation.IsValid())
            return Lookup::Failure(
                ObservationError(FrontendErrors::ResourceOperationUnknown, "The resource operation identity is invalid."));
        const auto record = std::ranges::find(records_, operation, &OperationRecord::id);
        if (record == records_.end())
            return Lookup::Failure(
                ObservationError(FrontendErrors::ResourceOperationUnknown, "The resource operation does not belong to this registry."));
        return Lookup::Success(std::to_address(record));
    }

    /** @copydoc RenderResourceRegistry::OperationState */
    Result<RenderResourceOperationState> RenderResourceRegistry::OperationState(const ResourceOperationId operation) const {
        using Observation = Result<RenderResourceOperationState>;
        using enum RenderResourceOperationState;
        if (!acceptingRequests_)
            return Observation::Failure(
                ObservationError(FrontendErrors::ResourceRegistryStopped, "Resource observations are disabled during frontend shutdown."));
        const auto found = journal_.Lookup(operation);
        if (found.HasError())
            return Observation::Failure(found.ErrorValue());
        const auto &record = *found.Value();
        if (!record.complete)
            return Observation::Success(Pending);
        return Observation::Success(record.error ? Failed : Succeeded);
    }

    Result<void> RenderResourceRegistry::OperationResult(const ResourceOperationId operation) const {
        const auto found = journal_.Lookup(operation);
        if (found.HasError())
            return Result<void>::Failure(found.ErrorValue());
        const auto &record = *found.Value();
        if (!record.complete)
            return Result<void>::Failure(
                ObservationError(FrontendErrors::ResourceOperationPending, "The renderer resource operation has not completed."));
        if (record.error)
            return Result<void>::Failure(*record.error);
        return Result<void>::Success();
    }

}  // namespace Horo::Render::Detail
