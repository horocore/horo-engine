#pragma once
/** @file NetworkAdmissionTransportTestSupport.h @brief Bounded paired transport fixture exchanging actual canonical wire frames. */
#include "Horo/Network/NetworkTransport.h"
#include "NetworkTestUtils.h"

#include <array>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Horo::Network::AdmissionTests {
    struct Endpoint final {
        std::array<NetworkTransportEvent, 64> events;
        std::size_t queued{};
        ConnectionHandle connection;
        NetworkTransportConfig config;
        NetworkTransportStats stats;
        ConnectionHandle lastOtherClosed;
        bool initialized{};
        bool listening{};
        bool open{};
        bool throwClose{};
        bool throwCloseNonStandard{};
        bool failPoll{};
        bool failInitialize{};
        std::size_t closes{};
        std::size_t shutdowns{};
        std::size_t polls{};
    };

    struct Link final {
        std::array<Endpoint, 2> ends;

        Link() {
            ends[0].connection = ConnectionHandle::Create(3, 2).Value();
            ends[1].connection = ConnectionHandle::Create(9, 7).Value();
        }

        [[nodiscard]] Result<void> Queue(const std::size_t side, NetworkTransportEvent event) {
            auto &target = ends[side];
            if (target.queued == target.events.size())
                return Result<void>::Failure(MakeError(NetworkErrors::TransportReliableBackpressure));
            target.events[target.queued++] = std::move(event);
            return Result<void>::Success();
        }
    };

    /** @brief Explicit in-memory backend fixture; it supplies connectivity/bytes and never authentication. */
    class PairedTransport final : public INetworkTransport {
    public:
        PairedTransport(std::shared_ptr<Link> link, const std::size_t side) : link_(std::move(link)), side_(side) {}

        Result<void> Initialize(const NetworkTransportConfig &config) override {
            auto &self = link_->ends[side_];
            if (self.failInitialize)
                return Result<void>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable, "owned prepare infrastructure context"));
            if (self.initialized)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleTransitionInvalid));
            self.config = config;
            self.initialized = true;
            return Result<void>::Success();
        }

        Result<ListenerHandle> Listen(const NetworkListenRequest &) override {
            auto &self = link_->ends[side_];
            self.listening = true;
            return ListenerHandle::Create(0, 1);
        }

        Result<ConnectionHandle> Connect(const NetworkConnectRequest &) override {
            auto &self = link_->ends[side_];
            auto &remote = link_->ends[1 - side_];
            if (!self.initialized || !remote.listening)
                return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            self.open = remote.open = true;
            if (link_->Queue(side_, {.kind = NetworkTransportEventKind::Connected, .connection = self.connection}).HasError() ||
                link_
                    ->Queue(1 - side_, {.kind = NetworkTransportEventKind::Accepted,
                                        .listener = ListenerHandle::Create(0, 1).Value(),
                                        .connection = remote.connection})
                    .HasError())
                return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportReliableBackpressure));
            return Result<ConnectionHandle>::Success(self.connection);
        }

        Result<void> Send(const ConnectionHandle connection, const ChannelId channel, const std::span<const std::byte> bytes,
                          const DeliveryPolicy delivery) override {
            auto &self = link_->ends[side_];
            if (bytes.size() > self.config.maximumMessageBytes || channel.Value() != 0 || delivery != DeliveryPolicy::ReliableOrdered)
                return Result<void>::Failure(MakeError(NetworkErrors::TransportLimitExceeded));
            if (connection != self.connection)
                return Result<void>::Success();  // An isolated hostile third peer may receive control; it never joins the paired path.
            if (!self.open)
                return Result<void>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            auto queued = link_->Queue(1 - side_, {.kind = NetworkTransportEventKind::PacketReceived,
                                                   .connection = link_->ends[1 - side_].connection,
                                                   .channel = channel,
                                                   .delivery = delivery,
                                                   .payload = {bytes.begin(), bytes.end()}});
            if (queued.HasValue()) {
                ++self.stats.sentMessages;
                self.stats.sentBytes += bytes.size();
            }
            return queued;
        }

        Result<void> Close(const ConnectionHandle connection) override {
            auto &self = link_->ends[side_];
            ++self.closes;
            if (self.throwClose) {
                if (self.throwCloseNonStandard)
                    throw 23;
                throw std::runtime_error{"PRIVATE_TRANSPORT_CLOSE_SENTINEL"};
            }
            if (connection != self.connection) {
                self.lastOtherClosed = connection;
                return Result<void>::Success();
            }
            DisconnectEndpoint();
            return Result<void>::Success();
        }

        Result<void> CloseListener(ListenerHandle) override {
            link_->ends[side_].listening = false;
            return Result<void>::Success();
        }

        Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override {
            auto &self = link_->ends[side_];
            ++self.polls;
            if (self.failPoll)
                return Result<std::size_t>::Failure(
                    MakeError(NetworkErrors::TransportNativeUnavailable, "owned poll infrastructure context"));
            const auto count = std::min(self.queued, static_cast<std::size_t>(self.config.maximumEventsPerPoll));
            for (std::size_t i = 0; i < count; ++i) {
                if (self.events[i].kind == NetworkTransportEventKind::PacketReceived)
                    ++self.stats.receivedMessages;
                consumer.Consume(std::move(self.events[i]));
            }
            for (std::size_t i = count; i < self.queued; ++i)
                self.events[i - count] = std::move(self.events[i]);
            self.queued -= count;
            return Result<std::size_t>::Success(count);
        }

        NetworkTransportStats Stats() const noexcept override {
            return link_->ends[side_].stats;
        }

        Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle) const override {
            return Result<NetworkConnectionStats>::Success({});
        }

        TransportCapabilities Capabilities() const noexcept override {
            TransportCapabilities caps;
            caps.revision = 1;
            caps.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = TransportSupport::Available;
            caps.maximumChannels = 1;
            caps.maximumMessageBytes = 1200;
            return caps;
        }

        void Shutdown() noexcept override {
            auto &self = link_->ends[side_];
            if (!self.initialized)
                return;
            ++self.shutdowns;
            // Shutdown is a distinct noexcept primitive; it must not invoke the deliberately throwing connection Close API.
            DisconnectEndpoint();
            self.initialized = false;
            self.listening = false;
            self.queued = 0;
        }

    private:
        /** @brief Publish an empty terminal into existing bounded storage without allocating inside Shutdown. */
        static void QueueClosed(Endpoint &endpoint) noexcept {
            static_assert(std::is_nothrow_move_assignable_v<NetworkTransportEvent>);
            if (endpoint.queued == endpoint.events.size()) {
                endpoint.failPoll = true;
                return;
            }
            endpoint.events[endpoint.queued++] = {.kind = NetworkTransportEventKind::Closed, .connection = endpoint.connection};
        }

        /** @brief The shared backend state primitive has no foreign callbacks and never uses the Close fault-injection path. */
        void DisconnectEndpoint() noexcept {
            auto &self = link_->ends[side_];
            if (!self.open)
                return;
            self.open = false;
            auto &remote = link_->ends[1 - side_];
            remote.open = false;
            QueueClosed(self);
            QueueClosed(remote);
        }

        std::shared_ptr<Link> link_;
        std::size_t side_{};
    };
}  // namespace Horo::Network::AdmissionTests
