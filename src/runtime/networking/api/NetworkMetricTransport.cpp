#include "Horo/Network/NetworkMetricTransport.h"

#include "Horo/Network/NetworkErrors.h"

#include <utility>

namespace Horo::Network {
    namespace {
        class MeasuringConsumer final : public INetworkTransportEventConsumer {
        public:
            MeasuringConsumer(INetworkTransportEventConsumer &consumer, NetworkMetrics &metrics) noexcept
                : consumer_(consumer), metrics_(metrics) {}

            void Consume(NetworkTransportEvent event) noexcept override {
                if (event.kind == NetworkTransportEventKind::PacketReceived)
                    (void)metrics_.RecordMessage(NetworkMetricDirection::Received, NetworkMetricCategory::Transport, event.payload.size());
                if (event.kind == NetworkTransportEventKind::Failed)
                    (void)metrics_.RecordFailure(NetworkMetricFailure::Transport);
                consumer_.Consume(std::move(event));
            }

        private:
            INetworkTransportEventConsumer &consumer_;
            NetworkMetrics &metrics_;
        };
    }  // namespace

    /** @copydoc NetworkMetricTransport::NetworkMetricTransport */
    NetworkMetricTransport::NetworkMetricTransport(std::unique_ptr<INetworkTransport> backend, NetworkMetrics &metrics) noexcept
        : backend_(std::move(backend)), metrics_(metrics) {}

    /** @copydoc NetworkMetricTransport::Initialize */
    Result<void> NetworkMetricTransport::Initialize(const NetworkTransportConfig &config) {
        auto result = backend_->Initialize(config);
        if (result.HasError())
            (void)metrics_.RecordFailure(NetworkMetricFailure::Transport);
        return result;
    }

    /** @copydoc NetworkMetricTransport::Listen */
    Result<ListenerHandle> NetworkMetricTransport::Listen(const NetworkListenRequest &request) {
        auto result = backend_->Listen(request);
        if (result.HasError())
            (void)metrics_.RecordFailure(NetworkMetricFailure::Connect);
        return result;
    }

    /** @copydoc NetworkMetricTransport::Connect */
    Result<ConnectionHandle> NetworkMetricTransport::Connect(const NetworkConnectRequest &request) {
        auto result = backend_->Connect(request);
        if (result.HasError())
            (void)metrics_.RecordFailure(NetworkMetricFailure::Connect);
        return result;
    }

    /** @copydoc NetworkMetricTransport::Send */
    Result<void> NetworkMetricTransport::Send(const ConnectionHandle connection, const ChannelId channel,
                                              const std::span<const std::byte> payload, const DeliveryPolicy delivery) {
        auto result = backend_->Send(connection, channel, payload, delivery);
        if (result.HasValue())
            (void)metrics_.RecordMessage(NetworkMetricDirection::Sent, NetworkMetricCategory::Transport, payload.size());
        else if (result.ErrorValue().code.Value() == NetworkErrors::TransportReliableBackpressure.code.Value())
            (void)metrics_.RecordDrop(NetworkMetricDrop::Capacity);
        else
            (void)metrics_.RecordFailure(NetworkMetricFailure::Transport);
        return result;
    }

    /** @copydoc NetworkMetricTransport::Close */
    Result<void> NetworkMetricTransport::Close(const ConnectionHandle connection) {
        auto result = backend_->Close(connection);
        if (result.HasValue() && metrics_.IsCollecting())
            (void)metrics_.SetActiveConnections(backend_->Stats().activeConnections);
        return result;
    }

    /** @copydoc NetworkMetricTransport::CloseListener */
    Result<void> NetworkMetricTransport::CloseListener(const ListenerHandle listener) {
        return backend_->CloseListener(listener);
    }

    /** @copydoc NetworkMetricTransport::PollEvents */
    Result<std::size_t> NetworkMetricTransport::PollEvents(INetworkTransportEventConsumer &consumer) {
        MeasuringConsumer measuring{consumer, metrics_};
        auto result = backend_->PollEvents(measuring);
        if (result.HasError())
            (void)metrics_.RecordFailure(NetworkMetricFailure::Transport);
        if (metrics_.IsCollecting())
            (void)metrics_.SetActiveConnections(backend_->Stats().activeConnections);
        return result;
    }

    /** @copydoc NetworkMetricTransport::Stats */
    NetworkTransportStats NetworkMetricTransport::Stats() const noexcept {
        return backend_->Stats();
    }

    /** @copydoc NetworkMetricTransport::ConnectionStats */
    Result<NetworkConnectionStats> NetworkMetricTransport::ConnectionStats(const ConnectionHandle connection) const {
        auto result = backend_->ConnectionStats(connection);
        if (result.HasValue() && metrics_.IsCollecting() && result.Value().pingMilliseconds.has_value())
            (void)metrics_.RecordRttMilliseconds(*result.Value().pingMilliseconds);
        return result;
    }

    /** @copydoc NetworkMetricTransport::Capabilities */
    TransportCapabilities NetworkMetricTransport::Capabilities() const noexcept {
        return backend_->Capabilities();
    }

    /** @copydoc NetworkMetricTransport::Shutdown */
    void NetworkMetricTransport::Shutdown() noexcept {
        backend_->Shutdown();
        (void)metrics_.SetActiveConnections(0);
    }
}  // namespace Horo::Network
