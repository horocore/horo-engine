#pragma once

/**
 * @file TransportBackendInstance.h
 * @brief Backend-neutral ownership of one explicitly activated transport composition.
 */

#include <memory>

namespace Horo::Network {
    /**
     * @brief Unique composition lifetime of a transport backend, before packet-runtime integration.
     *
     * This is not the future handle-based INetworkTransport packet interface. A concrete backend owns
     * its transport resources behind this lifetime; registration and selection never construct it.
     */
    class ITransportBackendInstance {
    public:
        virtual ~ITransportBackendInstance() = default;

        /** @brief Stop admitting work and request cooperative cancellation once. */
        virtual void RequestCancellation() noexcept = 0;
        /** @brief Complete bounded teardown; repeated calls must be harmless. */
        virtual void Shutdown() noexcept = 0;
    };

    /** @brief Unique ownership returned by a concrete backend factory. */
    using TransportBackendInstance = std::unique_ptr<ITransportBackendInstance>;
}  // namespace Horo::Network
