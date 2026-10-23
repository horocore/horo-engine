#pragma once
/** @file NetworkProductGnsSelection.h @brief Private concrete native selection shared by product composition roots. */
#include "NetworkProductTransport.h"

namespace Horo::Network {
    struct GnsNativeSecurityOptions;
}

namespace Horo::Application::Internal {
    /** @brief One unique inert native candidate and the immutable capture retaining its exact authorities. */
    struct NetworkProductGnsSelection final {
        std::unique_ptr<Network::INetworkTransport> transport;
        std::shared_ptr<const NetworkProductAdmission> admission;
    };

    /**
     * @brief Select explicit native trust and capture the actual candidate declaration before native initialization.
     * @param policy Host-selected protocol/trust/bounds, borrowed for this call. Concrete delivery metadata comes from the selected
     * factory.
     * @param providers Explicit credential/proof/entropy capabilities; native authority slots must be absent.
     * @param codecs Canonical immutable codec owner retained through shutdown.
     * @param handlers Exact existing bounded weak gameplay bindings.
     * @param options Explicit native root/provisioning/transport choice, never a default or ambient authority.
     * @return Unique candidate and same owned product capture, or the original typed selection/capture failure.
     * @details This is the GNS-enabled host composition operation, not a second backend selector or authority.
     * No listener, connection, issuer or native startup runs here. The real product service owns initialization,
     * admission, detach and native retirement. GNS-disabled hosts do not compile or link this operation.
     */
    [[nodiscard]] Result<NetworkProductGnsSelection> SelectNetworkProductGns(const Network::NetworkAdmissionPolicy &policy,
                                                                             Network::NetworkAdmissionProviders providers,
                                                                             std::shared_ptr<const Network::MessageCodecRegistry> codecs,
                                                                             std::span<const NetworkProductInboundHandler> handlers,
                                                                             const Network::GnsNativeSecurityOptions &options);
}  // namespace Horo::Application::Internal
