#include "AllocationProbe.h"
#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkErrors.h"
#include "TransportQualificationContract.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Network::Qualification {
    namespace {
        DeterministicTransportDescriptor Descriptor(const DeterministicTransportMode mode) {
            return {
                .mode = mode,
                .maximumScheduledDeliveries = 32,
                .maximumPayloadBytes = 32,
                .maximumChannels = 2,
                .budgetCapacity = {1, 8, 128},
                .budgetPolicy = {.contractVersion = 1,
                                 .revision = 1,
                                 .maximumActiveConnections = 1,
                                 .maximumQueuedMessages = 8,
                                 .maximumQueuedBytes = 128,
                                 .maximumQueuedMessagesPerConnection = 8,
                                 .maximumQueuedBytesPerConnection = 128,
                                 .maximumMessagesPerTick = 8,
                                 .maximumBytesPerTick = 128,
                                 .maximumMessagesPerConnectionPerTick = 8,
                                 .maximumBytesPerConnectionPerTick = 128,
                                 .saturationGraceTicks = 2},
                .scenario = {.contractVersion = 1, .revision = 1, .seed = 0x1109, .latencyTicks = 1, .maximumFragmentBytes = 4},
            };
        }

        DeterministicTransportDescriptor MeasurementDescriptor(const std::size_t messages, const std::size_t payloadBytes) {
            auto descriptor = Descriptor(DeterministicTransportMode::Loopback);
            descriptor.maximumScheduledDeliveries = messages;
            descriptor.budgetCapacity = {1, messages, messages * payloadBytes};
            descriptor.budgetPolicy.maximumQueuedMessages = messages;
            descriptor.budgetPolicy.maximumQueuedMessagesPerConnection = messages;
            descriptor.budgetPolicy.maximumQueuedBytes = messages * payloadBytes;
            descriptor.budgetPolicy.maximumQueuedBytesPerConnection = messages * payloadBytes;
            descriptor.budgetPolicy.maximumMessagesPerTick = messages;
            descriptor.budgetPolicy.maximumMessagesPerConnectionPerTick = messages;
            descriptor.budgetPolicy.maximumBytesPerTick = messages * payloadBytes;
            descriptor.budgetPolicy.maximumBytesPerConnectionPerTick = messages * payloadBytes;
            descriptor.scenario.maximumFragmentBytes = payloadBytes;
            return descriptor;
        }

        class DeterministicDriver final : public Driver {
        public:
            explicit DeterministicDriver(const DeterministicTransportMode mode)
                : transport_([&] {
                      auto created = DeterministicTransport::Create(Descriptor(mode));
                      REQUIRE(created.HasValue());
                      return std::move(created).Value();
                  }()),
                  connection_(ConnectionHandle::Create(0, 1).Value()) {
                REQUIRE(transport_.Open(connection_).HasValue());
                std::array<DeterministicTransportEvent, 1> empty{};
                REQUIRE(transport_.Advance(1, empty).Value() == 0);
            }

            [[nodiscard]] std::size_t MaximumMessageBytes() const override {
                return 32;
            }

            [[nodiscard]] std::uint32_t MaximumChannels() const override {
                return 2;
            }

            [[nodiscard]] bool Send(const std::uint32_t channel, const std::span<const std::byte> bytes) override {
                const auto id = ChannelId::Create(channel, 3);
                REQUIRE(id.HasValue());
                const auto sent = transport_.Send(connection_, id.Value(), TransportTrafficClass::Reliable, 0, bytes);
                if (stopped_) {
                    REQUIRE(sent.HasError());
                    REQUIRE(sent.ErrorValue().code.Value() == NetworkErrors::TransportShuttingDown.code.Value());
                } else if (channel >= MaximumChannels() || bytes.size() > MaximumMessageBytes()) {
                    REQUIRE(sent.HasError());
                    REQUIRE(sent.ErrorValue().code.Value() == NetworkErrors::TransportBudgetInvalid.code.Value());
                }
                return sent.HasValue() && sent.Value().copyCount == 1;
            }

            [[nodiscard]] std::vector<Packet> Receive(const std::size_t expected) override {
                std::array<DeterministicTransportEvent, 32> events{};
                const auto count = transport_.Advance(++tick_, events);
                REQUIRE(count.HasValue());
                std::vector<Packet> packets;
                for (std::size_t index = 0; index < count.Value(); ++index) {
                    const auto &event = events[index];
                    REQUIRE(event.kind == DeterministicTransportEventKind::Packet);
                    if (event.fragmentIndex == 0)
                        packets.push_back({event.channel.Value(), {}});
                    REQUIRE_FALSE(packets.empty());
                    REQUIRE(event.fragmentIndex < event.fragmentCount);
                    packets.back().bytes.insert(packets.back().bytes.end(), event.payload.begin(), event.payload.end());
                }
                REQUIRE(packets.size() == expected);
                return packets;
            }

            void Shutdown() override {
                static_cast<void>(transport_.Shutdown());
                stopped_ = true;
            }

        private:
            DeterministicTransport transport_;
            ConnectionHandle connection_;
            std::uint64_t tick_{1};
            bool stopped_{};
        };
    }  // namespace

    TEST_CASE("Loopback transport meets the unchanged qualification delivery contract", "[network][qualification][loopback]") {
        DeterministicDriver driver(DeterministicTransportMode::Loopback);
        VerifyDeliveryAndBounds(driver);
    }

    TEST_CASE("Simulated transport without configured impairment meets the unchanged qualification delivery contract",
              "[network][qualification][simulated]") {
        DeterministicDriver driver(DeterministicTransportMode::Simulated);
        VerifyDeliveryAndBounds(driver);
    }

    TEST_CASE("Loopback reliable saturation reports rejection and preserves every accepted packet",
              "[network][qualification][saturation]") {
        auto created = DeterministicTransport::Create(Descriptor(DeterministicTransportMode::Loopback));
        REQUIRE(created.HasValue());
        auto transport = std::move(created).Value();
        const auto connection = ConnectionHandle::Create(0, 1).Value();
        const auto channel = ChannelId::Create(0, 2).Value();
        REQUIRE(transport.Open(connection).HasValue());
        std::array<DeterministicTransportEvent, 8> events{};
        REQUIRE(transport.Advance(1, events).Value() == 0);
        const std::array payload{std::byte{0x42}};
        for (std::size_t index = 0; index < 8; ++index)
            REQUIRE(transport.Send(connection, channel, TransportTrafficClass::Reliable, 0, payload).Value().copyCount == 1);
        const auto overflow = transport.Send(connection, channel, TransportTrafficClass::Reliable, 0, payload);
        REQUIRE(overflow.HasError());
        REQUIRE(overflow.ErrorValue().code.Value() == NetworkErrors::TransportReliableBackpressure.code.Value());
        REQUIRE(transport.Advance(2, events).Value() == 8);
        for (const auto &event : events) {
            REQUIRE(event.kind == DeterministicTransportEventKind::Packet);
            REQUIRE(event.payload.size() == 1);
            REQUIRE(event.payload.front() == payload.front());
        }
        REQUIRE(transport.Advance(3, events).Value() == 0);
        REQUIRE(transport.Send(connection, channel, TransportTrafficClass::Reliable, 0, payload).HasValue());
        REQUIRE(transport.Shutdown() == 1);
        REQUIRE(transport.Shutdown() == 0);
    }

    TEST_CASE("Bounded owner output postpones rather than silently loses accepted fragments", "[network][qualification][saturation]") {
        auto created = DeterministicTransport::Create(Descriptor(DeterministicTransportMode::Loopback));
        REQUIRE(created.HasValue());
        auto transport = std::move(created).Value();
        const auto connection = ConnectionHandle::Create(0, 1).Value();
        const auto channel = ChannelId::Create(0, 2).Value();
        REQUIRE(transport.Open(connection).HasValue());
        std::array<DeterministicTransportEvent, 1> output{};
        REQUIRE(transport.Advance(1, output).Value() == 0);
        const std::array payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}, std::byte{0x55}};
        REQUIRE(transport.Send(connection, channel, TransportTrafficClass::Reliable, 0, payload).Value().fragmentCount == 2);
        REQUIRE(transport.Send(connection, channel, TransportTrafficClass::Reliable, 0, payload).Value().fragmentCount == 2);
        for (std::uint64_t tick = 2; tick <= 5; ++tick) {
            REQUIRE(transport.Advance(tick, output).Value() == 1);
            REQUIRE(output[0].fragmentIndex == (tick - 2) % 2);
            REQUIRE(output[0].fragmentCount == 2);
        }
        REQUIRE(transport.Advance(6, output).Value() == 0);
        REQUIRE(transport.Shutdown() == 0);
    }

    TEST_CASE("Simulated complete-message loss is an explicit disposition", "[network][qualification][loss]") {
        auto descriptor = Descriptor(DeterministicTransportMode::Simulated);
        descriptor.scenario.lossPerTenThousand = 10'000;
        auto created = DeterministicTransport::Create(descriptor);
        REQUIRE(created.HasValue());
        auto transport = std::move(created).Value();
        const auto connection = ConnectionHandle::Create(0, 1).Value();
        const auto channel = ChannelId::Create(0, 2).Value();
        REQUIRE(transport.Open(connection).HasValue());
        std::array<DeterministicTransportEvent, 8> events{};
        REQUIRE(transport.Advance(1, events).Value() == 0);
        const std::array payload{std::byte{0x11}, std::byte{0x22}};
        const auto lost = transport.Send(connection, channel, TransportTrafficClass::Reliable, 0, payload);
        REQUIRE(lost.Value().outcome == DeterministicSendOutcome::SimulatedLoss);
        REQUIRE(lost.Value().copyCount == 0);
        REQUIRE(transport.Advance(2, events).Value() == 0);
        REQUIRE(transport.Shutdown() == 0);
    }

    TEST_CASE("Simulated duplication counts both accepted delivery copies", "[network][qualification][loss]") {
        auto descriptor = Descriptor(DeterministicTransportMode::Simulated);
        descriptor.scenario.duplicatePerTenThousand = 10'000;
        auto duplicated = DeterministicTransport::Create(descriptor);
        REQUIRE(duplicated.HasValue());
        auto second = std::move(duplicated).Value();
        const auto connection = ConnectionHandle::Create(0, 1).Value();
        const auto channel = ChannelId::Create(0, 2).Value();
        std::array<DeterministicTransportEvent, 8> events{};
        const std::array payload{std::byte{0x11}, std::byte{0x22}};
        REQUIRE(second.Open(connection).HasValue());
        REQUIRE(second.Advance(1, events).Value() == 0);
        const auto sent = second.Send(connection, channel, TransportTrafficClass::Reliable, 0, payload);
        REQUIRE(sent.Value().outcome == DeterministicSendOutcome::ScheduledWithDuplicate);
        REQUIRE(sent.Value().copyCount == 2);
        REQUIRE(second.Advance(2, events).Value() == 2);
        REQUIRE(second.Shutdown() == 0);
    }

    TEST_CASE("Loopback declared workload reports owner-thread allocation latency throughput and shutdown",
              "[network][qualification][measurement]") {
        // Measurement domain: global C++ new/new[] in this test process. C malloc,
        // backend-native allocations and Catch assertion/reporting are excluded.
        constexpr std::size_t messages = 128;
        constexpr std::size_t payloadBytes = 32;
        auto created = DeterministicTransport::Create(MeasurementDescriptor(messages, payloadBytes));
        REQUIRE(created.HasValue());
        auto transport = std::move(created).Value();
        const auto connection = ConnectionHandle::Create(0, 1).Value();
        const auto channel = ChannelId::Create(0, 2).Value();
        REQUIRE(transport.Open(connection).HasValue());
        std::array<DeterministicTransportEvent, messages> events{};
        REQUIRE(transport.Advance(1, events).Value() == 0);
        const std::array<std::byte, payloadBytes> payload{};
        const auto allocationsBefore = Horo::Tests::AllocationProbe::Count();
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < messages; ++index) {
            const auto accepted = transport.Send(connection, channel, TransportTrafficClass::Reliable, 0, payload);
            if (accepted.HasError() || accepted.Value().copyCount != 1)
                FAIL("Declared loopback workload failed admission");
        }
        const auto sent = std::chrono::steady_clock::now();
        const auto delivered = transport.Advance(2, events);
        const auto received = std::chrono::steady_clock::now();
        const auto allocations = Horo::Tests::AllocationProbe::Count() - allocationsBefore;
        REQUIRE(delivered.Value() == messages);
        const auto shutdownStart = std::chrono::steady_clock::now();
        const auto discardedAtShutdown = transport.Shutdown();
        const auto stopped = std::chrono::steady_clock::now();
        REQUIRE(discardedAtShutdown == 0);
        const auto sendUs = std::chrono::duration_cast<std::chrono::microseconds>(sent - start).count();
        const auto deliveryUs = std::chrono::duration_cast<std::chrono::microseconds>(received - sent).count();
        const auto shutdownUs = std::chrono::duration_cast<std::chrono::microseconds>(stopped - shutdownStart).count();
        std::cout << "HORO-1109 loopback workload: " << messages << " x " << payloadBytes << " B; C++ allocations=" << allocations
                  << "; send_us=" << sendUs << "; delivery_us=" << deliveryUs << "; shutdown_us=" << shutdownUs
                  << "; send_messages_per_second=" << (sendUs > 0 ? messages * 1'000'000 / sendUs : 0) << '\n';
    }
}  // namespace Horo::Network::Qualification
