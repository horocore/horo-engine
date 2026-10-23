#pragma once
/** @file GnsObservedTransport.h @brief Test-only observation of the actual outbound handle; no authentication or event synthesis. */
#include "Horo/Network/NetworkTransport.h"

#include <array>
#include <memory>

namespace Horo::Network::NativeQualification {
    /** @brief Owner-thread-only observation lease; null before/after actual native transport lifetime. */
    struct TransportObservation final {
        INetworkTransport *transport{};
        ConnectionHandle outbound{};
        std::size_t events{};
        std::size_t readyEvents{};
        NetworkTransportStats retiredStats{};  // Read-only diagnostic snapshot before actual adapter shutdown, never admission authority.
        std::array<NetworkTransportEventKind, 8> eventPrefix{};
        NetworkTransportEventKind lastEvent{NetworkTransportEventKind::Failed};
        const ErrorCodeDescriptor *failure{};  // Stable canonical descriptor, never raw native diagnostics.
    };

    /** @brief Forward every operation to the same native adapter and retain only its actual Connect result. */
    class ObservedTransport final : public INetworkTransport {
    public:
        ObservedTransport(std::unique_ptr<INetworkTransport> native, std::shared_ptr<TransportObservation> observation)
            : native_(std::move(native)), observation_(std::move(observation)) {
            observation_->transport = this;
        }

        ~ObservedTransport() override {
            observation_->transport = nullptr;
        }

        Result<void> Initialize(const NetworkTransportConfig &config) override {
            return native_->Initialize(config);
        }

        Result<ListenerHandle> Listen(const NetworkListenRequest &request) override {
            return native_->Listen(request);
        }

        Result<ConnectionHandle> Connect(const NetworkConnectRequest &request) override {
            auto connected = native_->Connect(request);
            if (connected.HasValue())
                observation_->outbound = connected.Value();
            return connected;
        }

        Result<void> Send(ConnectionHandle connection, ChannelId channel, std::span<const std::byte> bytes,
                          DeliveryPolicy delivery) override {
            return native_->Send(connection, channel, bytes, delivery);
        }

        Result<void> Close(ConnectionHandle connection) override {
            return native_->Close(connection);
        }

        Result<void> CloseListener(ListenerHandle listener) override {
            return native_->CloseListener(listener);
        }

        Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override {
            RecordingConsumer recorded(*observation_, consumer);
            return native_->PollEvents(recorded);
        }

        NetworkTransportStats Stats() const noexcept override {
            return native_->Stats();
        }

        Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle connection) const override {
            return native_->ConnectionStats(connection);
        }

        TransportCapabilities Capabilities() const noexcept override {
            return native_->Capabilities();
        }

        void Shutdown() noexcept override {
            if (observation_->transport)
                observation_->retiredStats = native_->Stats();
            observation_->transport = nullptr;
            observation_->outbound = {};
            native_->Shutdown();
        }

    private:
        /** @brief Preserve the exact native event and poll order; retain only allocation-free public diagnostic facts. */
        class RecordingConsumer final : public INetworkTransportEventConsumer {
        public:
            RecordingConsumer(TransportObservation &observation, INetworkTransportEventConsumer &consumer)
                : observation_(observation), consumer_(consumer) {}

            void Consume(NetworkTransportEvent event) noexcept override {
                if (observation_.events < observation_.eventPrefix.size())
                    observation_.eventPrefix[observation_.events] = event.kind;
                ++observation_.events;
                if (event.kind == NetworkTransportEventKind::Accepted || event.kind == NetworkTransportEventKind::Connected)
                    ++observation_.readyEvents;
                observation_.lastEvent = event.kind;
                if (!observation_.failure)
                    observation_.failure = event.failure;
                consumer_.Consume(std::move(event));
            }

        private:
            TransportObservation &observation_;
            INetworkTransportEventConsumer &consumer_;
        };

        std::unique_ptr<INetworkTransport> native_;
        std::shared_ptr<TransportObservation> observation_;
    };
}  // namespace Horo::Network::NativeQualification
