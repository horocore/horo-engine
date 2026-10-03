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
#include <span>
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

        class GnsDriver final : public Driver {
        public:
            explicit GnsDriver(const std::uint32_t maximumEventsPerPoll = 16)
                : transport_([&] {
                      auto created = CreateGnsTransport();
                      REQUIRE(created.HasValue());
                      return std::move(created).Value();
                  }()) {
                REQUIRE(
                    transport_
                        ->Initialize({.maximumConnections = 4, .maximumEventsPerPoll = maximumEventsPerPoll, .maximumMessageBytes = 1200})
                        .HasValue());
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
            std::unique_ptr<INetworkTransport> transport_;
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
