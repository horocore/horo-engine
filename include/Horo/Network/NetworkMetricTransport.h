#pragma once

/**
 * @file NetworkMetricTransport.h
 * @brief Optional host-composed transport measurement adapter.
 */

#include "Horo/Network/NetworkMetrics.h"
#include "Horo/Network/NetworkTransport.h"

#include <memory>

namespace Horo::Network {
    /**
     * @brief Backend-neutral transport decorator that records actual admitted wire traffic.
     * @details The host owns metrics longer than this transport. No backend or sink is selected here.
     *          Transport-category counts are wire totals; protocol-category counts supplied by the
     *          owner are additional semantic observations, not partitions to sum with wire totals.
     */
    class NetworkMetricTransport final : public INetworkTransport {
    public:
        /** @brief Takes a non-null unique backend and borrows an owner-thread metrics accumulator.
         * @param backend Non-null transport owned exclusively by this adapter.
         * @param metrics Collector that outlives this adapter and is used on its owner thread. */
        NetworkMetricTransport(std::unique_ptr<INetworkTransport> backend, NetworkMetrics &metrics) noexcept;
        /** @copydoc INetworkTransport::Initialize */
        [[nodiscard]] Result<void> Initialize(const NetworkTransportConfig &config) override;
        /** @copydoc INetworkTransport::Listen */
        [[nodiscard]] Result<ListenerHandle> Listen(const NetworkListenRequest &request) override;
        /** @copydoc INetworkTransport::Connect */
        [[nodiscard]] Result<ConnectionHandle> Connect(const NetworkConnectRequest &request) override;
        /** @copydoc INetworkTransport::Send */
        [[nodiscard]] Result<void> Send(ConnectionHandle connection, ChannelId channel, std::span<const std::byte> payload,
                                        DeliveryPolicy delivery) override;
        /** @copydoc INetworkTransport::Close */
        [[nodiscard]] Result<void> Close(ConnectionHandle connection) override;
        /** @copydoc INetworkTransport::CloseListener */
        [[nodiscard]] Result<void> CloseListener(ListenerHandle listener) override;
        /** @copydoc INetworkTransport::PollEvents */
        [[nodiscard]] Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override;
        /** @copydoc INetworkTransport::Stats */
        [[nodiscard]] NetworkTransportStats Stats() const noexcept override;
        /** @copydoc INetworkTransport::ConnectionStats */
        [[nodiscard]] Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle connection) const override;
        /** @copydoc INetworkTransport::Capabilities */
        [[nodiscard]] TransportCapabilities Capabilities() const noexcept override;
        /** @copydoc INetworkTransport::Shutdown */
        void Shutdown() noexcept override;

    private:
        std::unique_ptr<INetworkTransport> backend_;
        NetworkMetrics &metrics_;
    };
}  // namespace Horo::Network
