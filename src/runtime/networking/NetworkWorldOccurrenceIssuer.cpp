#include "NetworkWorldOccurrenceIssuer.h"

#include "Horo/Network/NetworkErrors.h"

namespace Horo::Network::Detail {
    /** @copydoc NetworkWorldOccurrenceIssuer::Issue */
    Result<ReplicationWorldActivationDescriptor> NetworkWorldOccurrenceIssuer::Issue(const Runtime::SceneRuntimeId scene,
                                                                                     const CancellationToken &cancellation) {
        if (std::this_thread::get_id() != owner_ || !scene.IsValid())
            return Result<ReplicationWorldActivationDescriptor>::Failure(MakeError(NetworkErrors::ReplicationWorldInvalid));
        if (cancellation.IsCancellationRequested())
            return Result<ReplicationWorldActivationDescriptor>::Failure(MakeError(NetworkErrors::ReplicationWorldCancelled));
        if (sessions_ == maximum_ || epochs_ == maximum_)
            return Result<ReplicationWorldActivationDescriptor>::Failure(MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
        const auto session = NetworkSessionGeneration::Create(++sessions_);
        const auto authority = ReplicationAuthorityEpoch::Create(++epochs_);
        if (session.HasError())
            return Result<ReplicationWorldActivationDescriptor>::Failure(session.ErrorValue());
        if (authority.HasError())
            return Result<ReplicationWorldActivationDescriptor>::Failure(authority.ErrorValue());
        return Result<ReplicationWorldActivationDescriptor>::Success(
            {scene, session.Value(), authority.Value(), ReplicationExecutionRole::AuthorityServer});
    }
}  // namespace Horo::Network::Detail
