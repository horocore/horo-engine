#include "NetworkProductGnsSelection.h"

#include "GnsTransportFactory.h"
#include "Horo/Network/NetworkErrors.h"

namespace Horo::Application::Internal {
    /** @copydoc SelectNetworkProductGns */
    Result<NetworkProductGnsSelection> SelectNetworkProductGns(const Network::NetworkAdmissionPolicy &policy,
                                                               Network::NetworkAdmissionProviders providers,
                                                               std::shared_ptr<const Network::MessageCodecRegistry> codecs,
                                                               std::span<const NetworkProductInboundHandler> handlers,
                                                               const Network::GnsNativeSecurityOptions &options,
                                                               std::optional<Network::NetworkWorldBindingPolicy> worldBinding) {
        namespace Net = Network;
        if (const auto &trust = policy.trust;
            providers.certificates || providers.peers || providers.privateKeys || providers.channel || trust.id != options.trust.id ||
            trust.revision != options.trust.revision || trust.exposure != options.trust.exposure ||
            trust.credential != options.trust.credential || trust.certificate != options.trust.certificate ||
            trust.privateKey != options.trust.privateKey || trust.maximumProofBytes != options.trust.maximumProofBytes ||
            trust.allowUnprotectedInMemoryLoopback != options.trust.allowUnprotectedInMemoryLoopback ||
            options.transport.maximumConnections != policy.maximumPeers || options.transport.maximumEventsPerPoll != 64 ||
            options.transport.maximumMessageBytes != policy.local.transport.maximumMessageBytes ||
            options.transport.maximumChannels != policy.local.transport.maximumChannels ||
            options.transport.maximumMessageBytes != policy.envelope.maximumFrameBytes)
            return Result<NetworkProductGnsSelection>::Failure(MakeError(Net::NetworkErrors::AdmissionPolicyInvalid));
        auto selected = Net::CreateSecureGnsTransport(options);
        if (selected.HasError())
            return Result<NetworkProductGnsSelection>::Failure(selected.ErrorValue());
        auto composition = std::move(selected).Value();
        providers.certificates = composition.native.certificates;
        providers.peers = composition.native.peers;
        providers.privateKeys = composition.native.privateKeys;
        providers.channel = composition.native.channel;
        // The selected factory is the sole authority for this inert concrete declaration.
        // Protocol, trust, timeout and resource requirements remain host-selected unchanged.
        auto capturedPolicy = policy;
        capturedPolicy.local.transport = composition.transport->Capabilities();
        auto captured =
            NetworkProductAdmission::Capture(capturedPolicy, std::move(providers), std::move(codecs), handlers, std::move(worldBinding));
        if (captured.HasError())
            return Result<NetworkProductGnsSelection>::Failure(captured.ErrorValue());
        return Result<NetworkProductGnsSelection>::Success({std::move(composition.transport), std::move(captured).Value()});
    }
}  // namespace Horo::Application::Internal
