#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/TransportBackendComposition.h"

namespace Horo::Network {
    /** @copydoc CaptureNetworkTargetHostFacts */
    Result<NetworkTargetHostFacts> CaptureNetworkTargetHostFacts(const TransportBackendComposition &composition,
                                                                 const NetworkHostCapabilityRevision revision,
                                                                 const NetworkTargetPlatform platform,
                                                                 const NetworkProjectRoleSet supportedRoles,
                                                                 const NetworkProjectProtocolPolicy protocol,
                                                                 const std::span<const TransportTargetBinding> bindings) {
        using State = TransportBackendCompositionState;
        if (composition.Lifecycle() == State::Cancelling)
            return Result<NetworkTargetHostFacts>::Failure(MakeError(NetworkErrors::TransportBackendCancelled));
        if (composition.Lifecycle() == State::Closed)
            return Result<NetworkTargetHostFacts>::Failure(MakeError(NetworkErrors::TransportBackendShuttingDown));
        if (composition.Lifecycle() == State::Configuring || !revision.IsValid() || platform >= NetworkTargetPlatform::Count)
            return Result<NetworkTargetHostFacts>::Failure(MakeError(NetworkErrors::TransportBackendInvalid));
        if (bindings.size() > MaximumNetworkTargetProviders)
            return Result<NetworkTargetHostFacts>::Failure(MakeError(NetworkErrors::TransportBackendCapacityExceeded));

        NetworkTargetHostFacts facts{};
        facts.revision = revision;
        facts.platform = platform;
        facts.supportedRoles = supportedRoles;
        facts.networkRuntimeInstalled = true;
        facts.protocol = protocol;
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            const auto &binding = bindings[index];
            if (!binding.provider.IsValid() || !binding.backend.IsValid())
                return Result<NetworkTargetHostFacts>::Failure(MakeError(NetworkErrors::TransportBackendInvalid));
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (bindings[earlier].provider == binding.provider || bindings[earlier].backend == binding.backend)
                    return Result<NetworkTargetHostFacts>::Failure(MakeError(NetworkErrors::TransportBackendConflict));
            }
            const auto evidence = composition.Evidence(binding.backend);
            if (evidence.HasError())
                return Result<NetworkTargetHostFacts>::Failure(evidence.ErrorValue());
            facts.providers[index] = {binding.provider, evidence.Value().status.installed, evidence.Value().status.hostSupported,
                                      evidence.Value().status.configured, evidence.Value().capabilities};
        }
        facts.providerCount = bindings.size();
        return Result<NetworkTargetHostFacts>::Success(facts);
    }
}  // namespace Horo::Network
