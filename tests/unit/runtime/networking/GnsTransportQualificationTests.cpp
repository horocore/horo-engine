#include "AllocationProbe.h"
#include "GnsTransportFactory.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkTransport.h"
#include "TransportQualificationContract.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <steam/isteamnetworkingutils.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Network::Qualification {
    namespace {
        using namespace std::chrono_literals;

        class Collector final : public INetworkTransportEventConsumer {
        public:
            void Consume(NetworkTransportEvent event) noexcept override {
                events.push_back(std::move(event));
            }

            std::vector<NetworkTransportEvent> events;
        };

        /** @brief Probe immediate inbound readiness without allocating or asserting in the noexcept callback. */
        class ReadySendCollector final : public INetworkTransportEventConsumer {
        public:
            explicit ReadySendCollector(INetworkTransport &transport) : transport_(transport) {}

            void Consume(NetworkTransportEvent event) noexcept override {
                if (event.kind == NetworkTransportEventKind::Accepted) {
                    ++accepted;
                    listener = event.listener;
                    incoming = event.connection;
                    const std::array payload{std::byte{0x5a}};
                    try {
                        sent = transport_.Send(incoming, {}, payload, DeliveryPolicy::ReliableOrdered).HasValue();
                    } catch (...) {
                        threw = true;
                    }
                } else if (event.kind == NetworkTransportEventKind::Connected) {
                    ++connected;
                    outgoing = event.connection;
                } else if (event.kind == NetworkTransportEventKind::PacketReceived) {
                    ++received;
                    exactPacket = event.connection == outgoing && event.payload.size() == 1 && event.payload[0] == std::byte{0x5a};
                }
            }

            ListenerHandle listener{};
            ConnectionHandle incoming{};
            ConnectionHandle outgoing{};
            std::size_t accepted{};
            std::size_t connected{};
            std::size_t received{};
            bool sent{};
            bool threw{};
            bool exactPacket{};

        private:
            INetworkTransport &transport_;
        };

        /** @brief Restore the exact process-global native MTU before the sole adapter retires, even after a failed assertion. */
        class ScopedNativeMtu final {
        public:
            ScopedNativeMtu() {
                ESteamNetworkingConfigDataType type{};
                std::size_t bytes = sizeof(previous_);
                REQUIRE(SteamNetworkingUtils()->GetConfigValue(k_ESteamNetworkingConfig_MTU_PacketSize, k_ESteamNetworkingConfig_Global, 0,
                                                               &type, &previous_, &bytes) > 0);
                REQUIRE(type == k_ESteamNetworkingConfig_Int32);
                REQUIRE(bytes == sizeof(previous_));
            }

            ScopedNativeMtu(const ScopedNativeMtu &) = delete;
            ScopedNativeMtu &operator=(const ScopedNativeMtu &) = delete;

            void Apply() {
                REQUIRE(SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_MTU_PacketSize, 576));
                REQUIRE(Read(k_ESteamNetworkingConfig_MTU_PacketSize) == 576);
                const auto noFragmentBytes = Read(k_ESteamNetworkingConfig_MTU_DataSize);
                REQUIRE(noFragmentBytes > 0);
                REQUIRE(noFragmentBytes < 576);
            }

            ~ScopedNativeMtu() {
                if (!restored_)
                    (void)SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_MTU_PacketSize, previous_);
            }

            void Restore() {
                restored_ = SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_MTU_PacketSize, previous_);
                REQUIRE(restored_);
                REQUIRE(Read(k_ESteamNetworkingConfig_MTU_PacketSize) == previous_);
            }

        private:
            static std::int32_t Read(const ESteamNetworkingConfigValue option) {
                std::int32_t value{};
                ESteamNetworkingConfigDataType type{};
                std::size_t bytes = sizeof(value);
                REQUIRE(SteamNetworkingUtils()->GetConfigValue(option, k_ESteamNetworkingConfig_Global, 0, &type, &value, &bytes) > 0);
                // Pinned GNS's derived MTU_DataSize branch returns int32 bytes but leaves pOutDataType untouched.
                if (option != k_ESteamNetworkingConfig_MTU_DataSize)
                    REQUIRE(type == k_ESteamNetworkingConfig_Int32);
                REQUIRE(bytes == sizeof(value));
                return value;
            }

            std::int32_t previous_{};
            bool restored_{};
        };

        class GnsDriver final : public Driver {
        public:
            explicit GnsDriver(const std::uint32_t maximumEventsPerPoll = 16, const bool fragmentNativePackets = false)
                : transport_([&] {
                      auto created = CreateGnsTransport();
                      REQUIRE(created.HasValue());
                      return std::move(created).Value();
                  }()) {
                REQUIRE(
                    transport_
                        ->Initialize({.maximumConnections = 4, .maximumEventsPerPoll = maximumEventsPerPoll, .maximumMessageBytes = 1200})
                        .HasValue());
                if (fragmentNativePackets) {
                    mtu_.emplace();
                    mtu_->Apply();  // Before Listen/Connect: both native connections inherit the verified 576-byte packet bound.
                }
                ConnectLoopback();
            }

            [[nodiscard]] std::size_t MaximumMessageBytes() const override {
                return 1200;
            }

            [[nodiscard]] std::uint32_t MaximumChannels() const override {
                return transport_->Capabilities().maximumChannels;
            }

            [[nodiscard]] bool Send(const std::uint32_t channel, const std::span<const std::byte> bytes) override {
                const auto id = ChannelId::Create(channel, 2);
                REQUIRE(id.HasValue());
                const auto sent = transport_->Send(outgoing_, id.Value(), bytes, DeliveryPolicy::ReliableOrdered);
                if (stopped_) {
                    REQUIRE(sent.HasError());
                    REQUIRE(sent.ErrorValue().code.Value() == NetworkErrors::TransportShuttingDown.code.Value());
                } else if (channel >= MaximumChannels() || bytes.size() > MaximumMessageBytes()) {
                    REQUIRE(sent.HasError());
                    REQUIRE(sent.ErrorValue().code.Value() == NetworkErrors::TransportLimitExceeded.code.Value());
                }
                return sent.HasValue();
            }

            [[nodiscard]] std::vector<Packet> Receive(const std::size_t expected) override {
                std::vector<Packet> packets;
                const auto deadline = std::chrono::steady_clock::now() + (expected == 0 ? 50ms : 5s);
                while (std::chrono::steady_clock::now() < deadline && (expected == 0 || packets.size() < expected)) {
                    REQUIRE(transport_->PollEvents(collector_).HasValue());
                    for (const auto &event : collector_.events) {
                        if (event.kind == NetworkTransportEventKind::PacketReceived) {
                            REQUIRE(event.connection == incoming_);
                            packets.push_back({0, event.payload});
                        }
                    }
                    collector_.events.clear();
                    if (packets.size() < expected)
                        std::this_thread::sleep_for(5ms);
                }
                REQUIRE(packets.size() == expected);
                return packets;
            }

            void Shutdown() override {
                if (mtu_)
                    mtu_->Restore();
                transport_->Shutdown();
                stopped_ = true;
            }

            void VerifyPeerLossIsTerminal() {
                REQUIRE(transport_->Close(incoming_).HasValue());
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                std::size_t terminalEvents{};
                while (std::chrono::steady_clock::now() < deadline && terminalEvents == 0) {
                    REQUIRE(transport_->PollEvents(collector_).HasValue());
                    for (const auto &event : collector_.events) {
                        if (event.connection == outgoing_ &&
                            (event.kind == NetworkTransportEventKind::Closed || event.kind == NetworkTransportEventKind::Failed))
                            ++terminalEvents;
                    }
                    collector_.events.clear();
                    if (terminalEvents == 0)
                        std::this_thread::sleep_for(5ms);
                }
                REQUIRE(terminalEvents == 1);
                const std::array payload{std::byte{0x11}};
                const auto rejected = transport_->Send(outgoing_, ChannelId{}, payload, DeliveryPolicy::ReliableOrdered);
                REQUIRE(rejected.HasError());
                REQUIRE(rejected.ErrorValue().code.Value() == NetworkErrors::TransportConnectionFailed.code.Value());
                for (int poll = 0; poll < 10; ++poll) {
                    REQUIRE(transport_->PollEvents(collector_).HasValue());
                    for (const auto &event : collector_.events)
                        REQUIRE((event.connection != outgoing_ || event.kind != NetworkTransportEventKind::PacketReceived));
                    collector_.events.clear();
                    std::this_thread::sleep_for(5ms);
                }
                transport_->Shutdown();
                REQUIRE_FALSE(transport_->PollEvents(collector_).HasValue());
                REQUIRE(collector_.events.empty());
            }

        private:
            void ConnectLoopback() {
                for (int port = 42300; port < 42400; ++port) {
                    const auto candidate = NetworkAddress::Parse("127.0.0.1:" + std::to_string(port));
                    REQUIRE(candidate.HasValue());
                    const auto listening = transport_->Listen({.bindAddress = candidate.Value(), .maximumConnections = 2});
                    if (listening.HasValue()) {
                        address_ = candidate.Value();
                        break;
                    }
                }
                REQUIRE(address_.IsValid());
                const auto connected = transport_->Connect({.endpoint = address_, .timeout = 5s});
                REQUIRE(connected.HasValue());
                outgoing_ = connected.Value();
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                bool outgoingReady{};
                while (std::chrono::steady_clock::now() < deadline && (!incoming_.IsValid() || !outgoingReady)) {
                    REQUIRE(transport_->PollEvents(collector_).HasValue());
                    for (const auto &event : collector_.events) {
                        if (event.kind == NetworkTransportEventKind::Accepted)
                            incoming_ = event.connection;
                        if (event.kind == NetworkTransportEventKind::Connected && event.connection == outgoing_)
                            outgoingReady = true;
                    }
                    collector_.events.clear();
                    std::this_thread::sleep_for(5ms);
                }
                REQUIRE(incoming_.IsValid());
                REQUIRE(outgoingReady);
            }

            std::unique_ptr<INetworkTransport> transport_;
            std::optional<ScopedNativeMtu> mtu_;  // Destroyed before the native transport, including assertion unwinding.
            NetworkAddress address_;
            ConnectionHandle outgoing_;
            ConnectionHandle incoming_;
            Collector collector_;
            bool stopped_{};
        };
    }  // namespace

    TEST_CASE("Production GNS transport meets the unchanged qualification delivery contract", "[network][qualification][gns]") {
        GnsDriver driver;
        VerifyDeliveryAndBounds(driver);
    }

    TEST_CASE("Production GNS reassembles an admitted 1200-byte reliable message across native 576-byte UDP packets",
              "[network][qualification][gns][fragmentation]") {
        // Real ConnectByIPAddress/Listen loopback UDP, not CreateSocketPair or Null segmentation.
        // Native MTU_DataSize is verified smaller than this admitted payload before either connection exists.
        GnsDriver driver(16, true);
        std::vector<std::byte> payload(driver.MaximumMessageBytes());
        for (std::size_t index = 0; index < payload.size(); ++index)
            payload[index] = static_cast<std::byte>((index * 73 + 19) & 255);
        REQUIRE(payload.size() == 1200);
        REQUIRE(driver.Send(0, payload));
        const auto received = driver.Receive(1);
        REQUIRE(received.front().bytes == payload);
        REQUIRE(driver.Receive(0).empty());
        driver.Shutdown();  // Explicit restoration is asserted; RAII remains the failure-path restoration owner.
    }

    TEST_CASE("Production GNS peer loss terminates admission and shutdown emits no late callback", "[network][qualification][gns]") {
        GnsDriver driver;
        driver.VerifyPeerLossIsTerminal();
    }

    TEST_CASE("Production GNS bounded one-event polls drain every accepted coalesced message",
              "[network][qualification][gns][saturation]") {
        GnsDriver driver(1);
        const std::vector<std::byte> payload(32, std::byte{0x31});
        for (int index = 0; index < 8; ++index)
            REQUIRE(driver.Send(0, payload));
        const auto received = driver.Receive(8);
        for (const auto &packet : received)
            REQUIRE(packet.bytes == payload);
        driver.Shutdown();
    }

    TEST_CASE("Production GNS declared loopback workload reports allocation latency throughput and shutdown",
              "[network][qualification][gns][measurement]") {
        // 32 reliable 256-byte messages in 8 bounded batches. The C++ new
        // counter covers this process, not native C malloc or OS socket buffers.
        constexpr std::size_t messages = 32;
        constexpr std::size_t batchSize = 4;
        constexpr std::size_t payloadBytes = 256;
        GnsDriver driver;
        const std::vector<std::byte> payload(payloadBytes, std::byte{0x5a});
        const auto allocationsBefore = Horo::Tests::AllocationProbe::Count();
        const auto start = std::chrono::steady_clock::now();
        std::chrono::nanoseconds maximumBatchLatency{};
        for (std::size_t batch = 0; batch < messages / batchSize; ++batch) {
            const auto batchStart = std::chrono::steady_clock::now();
            for (std::size_t index = 0; index < batchSize; ++index)
                REQUIRE(driver.Send(0, payload));
            const auto received = driver.Receive(batchSize);
            for (const auto &packet : received)
                REQUIRE(packet.bytes == payload);
            maximumBatchLatency =
                std::max(maximumBatchLatency,
                         std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - batchStart));
        }
        const auto delivered = std::chrono::steady_clock::now();
        const auto allocations = Horo::Tests::AllocationProbe::Count() - allocationsBefore;
        const auto shutdownStart = std::chrono::steady_clock::now();
        driver.Shutdown();
        const auto stopped = std::chrono::steady_clock::now();
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(delivered - start).count();
        const auto shutdownUs = std::chrono::duration_cast<std::chrono::microseconds>(stopped - shutdownStart).count();
        std::cout << "HORO-1109 GNS 127.0.0.1 workload: " << messages << " x " << payloadBytes
                  << " B reliable, batches=" << messages / batchSize << "; C++ allocations=" << allocations << "; elapsed_us=" << elapsedUs
                  << "; max_batch_latency_us=" << std::chrono::duration_cast<std::chrono::microseconds>(maximumBatchLatency).count()
                  << "; shutdown_us=" << shutdownUs
                  << "; delivered_messages_per_second=" << (elapsedUs > 0 ? messages * 1'000'000 / elapsedUs : 0) << '\n';
    }
}  // namespace Horo::Network::Qualification

TEST_CASE("GNS publishes one directional ready event and accepts Send inside inbound readiness", "[network][gns][readiness]") {
    using namespace Horo::Network;
    using namespace Horo::Network::Qualification;
    using namespace std::chrono_literals;
    auto created = CreateGnsTransport();
    REQUIRE(created.HasValue());
    auto transport = std::move(created).Value();
    REQUIRE(transport->Initialize({4, 16, 1200}).HasValue());
    ListenerHandle listener{};
    NetworkAddress endpoint;
    for (int port = 42400; port < 42500 && !listener.IsValid(); ++port) {
        auto address = NetworkAddress::Parse("127.0.0.1:" + std::to_string(port));
        REQUIRE(address.HasValue());
        auto listening = transport->Listen({address.Value(), 2});
        if (listening.HasValue()) {
            listener = listening.Value();
            endpoint = address.Value();
        }
    }
    REQUIRE(listener.IsValid());
    auto outgoing = transport->Connect({endpoint, 5s});
    REQUIRE(outgoing.HasValue());
    ReadySendCollector collector(*transport);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (collector.received == 0 && std::chrono::steady_clock::now() < deadline) {
        REQUIRE(transport->PollEvents(collector).HasValue());
        std::this_thread::sleep_for(2ms);
    }
    for (int i = 0; i < 5; ++i)
        REQUIRE(transport->PollEvents(collector).HasValue());
    REQUIRE(collector.accepted == 1);
    REQUIRE(collector.connected == 1);
    REQUIRE(collector.listener == listener);
    REQUIRE(collector.incoming != outgoing.Value());
    REQUIRE(collector.outgoing == outgoing.Value());
    REQUIRE_FALSE(collector.threw);
    REQUIRE(collector.sent);
    REQUIRE(collector.received == 1);
    REQUIRE(collector.exactPacket);
    transport->Shutdown();
}
