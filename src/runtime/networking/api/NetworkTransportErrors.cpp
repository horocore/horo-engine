#include "Horo/Network/NetworkErrors.h"

namespace Horo::Network::NetworkErrors {
    namespace {
        const ErrorDomainId NetworkDomain{"horo.network"};
    }

    const ErrorCodeDescriptor TransportNativeUnavailable{NetworkDomain,
                                                         ErrorCode{"network.transport.native_unavailable"},
                                                         ErrorSeverity::Error,
                                                         "The selected native transport is unavailable.",
                                                         "Check the selected backend's host support and endpoint availability.",
                                                         true,
                                                         false};
    const ErrorCodeDescriptor TransportConnectionFailed{NetworkDomain,
                                                        ErrorCode{"network.transport.connection_failed"},
                                                        ErrorSeverity::Error,
                                                        "The native connection failed or closed unexpectedly.",
                                                        "Retry only according to the caller's bounded connection policy.",
                                                        true,
                                                        false};
    const ErrorCodeDescriptor TransportMalformedPacket{NetworkDomain,
                                                       ErrorCode{"network.transport.malformed_packet"},
                                                       ErrorSeverity::Error,
                                                       "The native packet exceeds the negotiated transport bounds.",
                                                       "Close the malformed connection and reject its payload.",
                                                       false,
                                                       false};
}  // namespace Horo::Network::NetworkErrors
