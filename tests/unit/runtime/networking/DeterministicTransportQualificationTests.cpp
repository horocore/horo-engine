#include "AllocationProbe.h"
#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkChaosScenario.h"
#include "Horo/Network/NetworkErrors.h"
#include "TransportQualificationContract.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
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

    namespace {
        NetworkChaosScope ChaosScope(const std::uint64_t generation = 2) {
            return {NetworkOperationGeneration::Create(1).Value(), NetworkOperationGeneration::Create(generation).Value()};
        }

        NetworkChaosScenarioDescriptor ChaosDescriptor(const std::span<const ConnectionHandle> topology,
                                                       const std::span<const NetworkChaosAction> timeline = {},
                                                       const std::span<const NetworkChaosCheckpoint> checkpoints = {}) {
            return {.profile = NetworkChaosProfile::Test,
                    .scope = ChaosScope(),
                    .revision = 1,
                    .durationTicks = 10,
                    .transport = Descriptor(DeterministicTransportMode::Simulated),
                    .topology = topology,
                    .timeline = timeline,
                    .checkpoints = checkpoints};
        }

        struct ChaosPacket final {
            std::uint64_t tick;
            DeterministicTransportEventKind kind;
            ConnectionHandle connection;
            std::uint64_t sequence;
            std::size_t fragment;
            std::vector<std::byte> bytes;
            bool operator==(const ChaosPacket &) const = default;
        };

        /** @brief Replay the fixed send schedule and retain owned event bytes for an exact second-run comparison. */
        std::vector<ChaosPacket> ReplayScriptedChaosTraffic(const NetworkChaosScenarioDescriptor &descriptor) {
            const auto connection = descriptor.topology.front();
            auto created = NetworkChaosScenario::Create(descriptor);
            REQUIRE(created.HasValue());
            auto scenario = std::move(created).Value();
            std::vector<ChaosPacket> trace;
            const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
            for (std::uint64_t tick = 1; tick <= 10; ++tick) {
                std::array<DeterministicTransportEvent, 16> events{};
                const auto result = scenario.Advance(ChaosScope(), tick, events);
                REQUIRE(result.HasValue());
                for (const auto &event : std::span{events}.first(result.Value()))
                    trace.push_back({tick,
                                     event.kind,
                                     event.connection,
                                     event.sequence,
                                     event.fragmentIndex,
                                     {event.payload.begin(), event.payload.end()}});
                if (tick == 1 || tick == 3 || tick == 4 || tick == 5 || tick == 8) {
                    const auto active = tick == 8 ? connection.NextGeneration().Value() : connection;
                    const auto sent = scenario.Send(ChaosScope(), active, ChannelId{}, TransportTrafficClass::Reliable, 0, payload);
                    REQUIRE(sent.HasValue());
                    if (tick == 4)
                        REQUIRE(sent.Value().outcome == DeterministicSendOutcome::SimulatedLoss);
                }
            }
            REQUIRE(scenario.Evidence().checkpoints == 2);
            REQUIRE(scenario.Evidence().actions == descriptor.timeline.size());
            REQUIRE(scenario.Advance(ChaosScope(), 11, {}).HasError());
            return trace;
        }

        /** @brief Reject restart generation wrap before any scenario connection is opened. */
        void RequireExhaustedRestartRejected(NetworkChaosScenarioDescriptor descriptor) {
            const std::array exhausted{ConnectionHandle::Create(0, std::numeric_limits<std::uint32_t>::max()).Value()};
            const std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Restart, exhausted[0]}};
            descriptor.topology = exhausted;
            descriptor.timeline = timeline;
            REQUIRE(NetworkChaosScenario::Create(descriptor).HasError());
        }

        /** @brief Reject a replacement that would invalidate the scenario's prepared fragment storage. */
        void RequireFragmentStorageChangeRejected(NetworkChaosScenarioDescriptor descriptor) {
            auto replacement = descriptor.transport.scenario;
            ++replacement.revision;
            ++replacement.maximumFragmentBytes;
            const std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Impairment, {}, replacement}};
            descriptor.timeline = timeline;
            REQUIRE(NetworkChaosScenario::Create(descriptor).HasError());
        }
    }  // namespace

    TEST_CASE("Chaos timeline replays seed topology impairments and expected checkpoints", "[network][qualification][chaos]") {
        const auto connection = ConnectionHandle::Create(0, 1).Value();
        const std::array topology{connection};
        auto loss = Descriptor(DeterministicTransportMode::Simulated).scenario;
        loss.revision = 2;
        loss.lossPerTenThousand = 10'000;
        auto duplicate = loss;
        duplicate.revision = 3;
        duplicate.lossPerTenThousand = 0;
        duplicate.duplicatePerTenThousand = 10'000;
        duplicate.reorderPerTenThousand = 10'000;
        duplicate.jitterTicks = 1;
        const std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Stall},
                                  NetworkChaosAction{3, NetworkChaosActionKind::Resume},
                                  NetworkChaosAction{4, NetworkChaosActionKind::Impairment, {}, loss},
                                  NetworkChaosAction{5, NetworkChaosActionKind::Impairment, {}, duplicate},
                                  NetworkChaosAction{7, NetworkChaosActionKind::Disconnect, connection},
                                  NetworkChaosAction{8, NetworkChaosActionKind::Restart, connection}};
        const std::array checkpoints{NetworkChaosCheckpoint{3, 2, 0, 2}, NetworkChaosCheckpoint{4, 4, 0, 3}};
        const auto descriptor = ChaosDescriptor(topology, timeline, checkpoints);
        const auto first = ReplayScriptedChaosTraffic(descriptor);
        REQUIRE(first == ReplayScriptedChaosTraffic(descriptor));
        REQUIRE_FALSE(first.empty());
    }

    TEST_CASE("Chaos stalls retain bounded work and restart fences stale and foreign sessions", "[network][qualification][chaos]") {
        const auto connection = ConnectionHandle::Create(0, 1).Value();
        const std::array topology{connection};
        const std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Stall},
                                  NetworkChaosAction{3, NetworkChaosActionKind::Disconnect, connection},
                                  NetworkChaosAction{4, NetworkChaosActionKind::Resume},
                                  NetworkChaosAction{4, NetworkChaosActionKind::Restart, connection}};
        auto created = NetworkChaosScenario::Create(ChaosDescriptor(topology, timeline));
        REQUIRE(created.HasValue());
        auto scenario = std::move(created).Value();
        std::array<DeterministicTransportEvent, 16> events{};
        REQUIRE(scenario.Advance(ChaosScope(), 1, events).Value() == 0);
        const std::array payload{std::byte{0x42}};
        for (std::size_t index = 0; index < 8; ++index)
            REQUIRE(scenario.Send(ChaosScope(), connection, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasValue());
        REQUIRE(scenario.Advance(ChaosScope(), 2, events).Value() == 0);
        REQUIRE(scenario.Send(ChaosScope(), connection, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasError());
        const auto before = scenario.Evidence();
        REQUIRE(scenario.Advance(ChaosScope(99), 3, events).HasError());
        REQUIRE(scenario.Send(ChaosScope(99), connection, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasError());
        REQUIRE(scenario.Close(ChaosScope(99), connection).HasError());
        REQUIRE(scenario.Evidence() == before);
        REQUIRE(scenario.Advance(ChaosScope(), 3, events).Value() == 0);
        REQUIRE(scenario.Advance(ChaosScope(), 4, events).Value() == 1);
        REQUIRE(events[0].kind == DeterministicTransportEventKind::Disconnected);
        REQUIRE(events[0].connection == connection);
        REQUIRE(scenario.Send(ChaosScope(), connection, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasError());
        REQUIRE(scenario.Send(ChaosScope(), connection.NextGeneration().Value(), ChannelId{}, TransportTrafficClass::Reliable, 0, payload)
                    .HasValue());
        REQUIRE(scenario.Advance(ChaosScope(), 5, events).Value() == 1);
        REQUIRE(events[0].connection == connection.NextGeneration().Value());
        REQUIRE(scenario.Evidence().packets == 1);
        REQUIRE(scenario.Shutdown() == 0);
        REQUIRE(scenario.Shutdown() == 0);
        REQUIRE(scenario.Advance(ChaosScope(), 6, events).HasError());
    }

    TEST_CASE("Restart without a preceding disconnect returns the real old terminal before next generation traffic",
              "[network][qualification][chaos]") {
        const auto old = ConnectionHandle::Create(0, 1).Value();
        const auto next = old.NextGeneration().Value();
        const std::array topology{old};
        const std::array timeline{NetworkChaosAction{2, NetworkChaosActionKind::Restart, old}};
        auto descriptor = ChaosDescriptor(topology, timeline);
        descriptor.maximumEventsPerTick = 1;
        auto created = NetworkChaosScenario::Create(descriptor);
        REQUIRE(created.HasValue());
        auto scenario = std::move(created).Value();
        std::array<DeterministicTransportEvent, 1> events{};
        const std::array payload{std::byte{1}};
        REQUIRE(scenario.Advance(ChaosScope(), 1, events).Value() == 0);
        REQUIRE(scenario.Send(ChaosScope(), old, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasValue());
        REQUIRE(scenario.Advance(ChaosScope(), 2, events).Value() == 1);
        REQUIRE(events[0].kind == DeterministicTransportEventKind::Disconnected);
        REQUIRE(events[0].connection == old);
        REQUIRE(scenario.Send(ChaosScope(), old, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasError());
        REQUIRE(scenario.Send(ChaosScope(), next, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasValue());
        REQUIRE(scenario.Advance(ChaosScope(), 3, events).Value() == 1);
        REQUIRE(events[0].kind == DeterministicTransportEventKind::Packet);
        REQUIRE(events[0].connection == next);
    }

    TEST_CASE("Restart admission waits through stalls and a saturated single event drain", "[network][qualification][chaos]") {
        const auto old = ConnectionHandle::Create(0, 1).Value();
        const auto next = old.NextGeneration().Value();
        const auto other = ConnectionHandle::Create(1, 1).Value();
        const std::array topology{old, other};
        const std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Stall},
                                  NetworkChaosAction{3, NetworkChaosActionKind::Restart, old},
                                  NetworkChaosAction{4, NetworkChaosActionKind::Resume}};
        auto descriptor = ChaosDescriptor(topology, timeline);
        descriptor.maximumEventsPerTick = 1;
        descriptor.transport.budgetCapacity.maximumConnections = 2;
        descriptor.transport.budgetPolicy.maximumActiveConnections = 2;
        auto created = NetworkChaosScenario::Create(descriptor);
        REQUIRE(created.HasValue());
        auto scenario = std::move(created).Value();
        std::array<DeterministicTransportEvent, 1> events{};
        const std::array payload{std::byte{1}};
        REQUIRE(scenario.Advance(ChaosScope(), 1, events).Value() == 0);
        REQUIRE(scenario.Send(ChaosScope(), other, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasValue());
        REQUIRE(scenario.Advance(ChaosScope(), 2, events).Value() == 0);
        REQUIRE(scenario.Advance(ChaosScope(), 3, {}).HasError());
        REQUIRE(scenario.Advance(ChaosScope(), 3, events).Value() == 0);
        REQUIRE(scenario.Send(ChaosScope(), next, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasError());
        REQUIRE(scenario.Advance(ChaosScope(), 4, events).Value() == 1);
        REQUIRE(events[0].connection == other);
        REQUIRE(scenario.Send(ChaosScope(), next, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasError());
        REQUIRE(scenario.Advance(ChaosScope(), 5, events).Value() == 1);
        REQUIRE(events[0].kind == DeterministicTransportEventKind::Disconnected);
        REQUIRE(events[0].connection == old);
        REQUIRE(scenario.Send(ChaosScope(), next, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasValue());
        REQUIRE(scenario.Send(ChaosScope(), old, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasError());
        REQUIRE(scenario.Advance(ChaosScope(), 6, events).Value() == 1);
        REQUIRE(events[0].connection == next);
    }

    TEST_CASE("Insufficient event buffers preserve restart actions and the original active connection", "[network][qualification][chaos]") {
        const auto old = ConnectionHandle::Create(0, 1).Value();
        const std::array topology{old};
        const std::array timeline{NetworkChaosAction{2, NetworkChaosActionKind::Restart, old}};
        auto descriptor = ChaosDescriptor(topology, timeline);
        descriptor.maximumEventsPerTick = 2;
        auto created = NetworkChaosScenario::Create(descriptor);
        REQUIRE(created.HasValue());
        auto scenario = std::move(created).Value();
        std::array<DeterministicTransportEvent, 2> events{};
        REQUIRE(scenario.Advance(ChaosScope(), 1, events).Value() == 0);
        SECTION("Zero capacity") {
            REQUIRE(scenario.Advance(ChaosScope(), 2, {}).HasError());
        }
        SECTION("Tiny capacity") {
            REQUIRE(scenario.Advance(ChaosScope(), 2, std::span{events}.first(1)).HasError());
        }
        REQUIRE(scenario.Evidence().tick == 1);
        REQUIRE(scenario.Evidence().actions == 0);
        const std::array payload{std::byte{1}};
        REQUIRE(scenario.Send(ChaosScope(), old, ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasValue());
        REQUIRE(scenario.Advance(ChaosScope(), 2, events).Value() == 1);
        REQUIRE(events[0].kind == DeterministicTransportEventKind::Disconnected);
        REQUIRE(events[0].connection == old);
    }

    TEST_CASE("Duration shutdown and explicit shutdown cancel a pending restart", "[network][qualification][chaos]") {
        const auto old = ConnectionHandle::Create(0, 1).Value();
        const std::array topology{old};
        const std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Stall},
                                  NetworkChaosAction{2, NetworkChaosActionKind::Restart, old}};
        auto descriptor = ChaosDescriptor(topology, timeline);
        descriptor.maximumEventsPerTick = 1;
        descriptor.durationTicks = 3;
        auto created = NetworkChaosScenario::Create(descriptor);
        REQUIRE(created.HasValue());
        auto scenario = std::move(created).Value();
        std::array<DeterministicTransportEvent, 1> events{};
        REQUIRE(scenario.Advance(ChaosScope(), 1, events).Value() == 0);
        REQUIRE(scenario.Advance(ChaosScope(), 2, events).Value() == 0);
        SECTION("Explicit shutdown") {
            REQUIRE(scenario.Shutdown() == 1);
        }
        SECTION("Finite duration") {
            REQUIRE(scenario.Advance(ChaosScope(), 3, events).Value() == 0);
        }
        const std::array payload{std::byte{1}};
        REQUIRE(
            scenario.Send(ChaosScope(), old.NextGeneration().Value(), ChannelId{}, TransportTrafficClass::Reliable, 0, payload).HasError());
        REQUIRE(scenario.Advance(ChaosScope(), 4, events).HasError());
        REQUIRE(scenario.Shutdown() == 0);
    }

    TEST_CASE("Pending restart ownership survives moves and explicit close cancels deferred admission", "[network][qualification][chaos]") {
        const auto old = ConnectionHandle::Create(0, 1).Value();
        const std::array topology{old};
        const std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Stall},
                                  NetworkChaosAction{2, NetworkChaosActionKind::Restart, old},
                                  NetworkChaosAction{3, NetworkChaosActionKind::Resume}};
        auto descriptor = ChaosDescriptor(topology, timeline);
        descriptor.maximumEventsPerTick = 1;
        auto created = NetworkChaosScenario::Create(descriptor);
        REQUIRE(created.HasValue());
        auto scenario = std::move(created).Value();
        std::array<DeterministicTransportEvent, 1> events{};
        REQUIRE(scenario.Advance(ChaosScope(), 1, events).Value() == 0);
        REQUIRE(scenario.Advance(ChaosScope(), 2, events).Value() == 0);
        auto moved = std::move(scenario);
        REQUIRE(scenario.Shutdown() == 0);
        SECTION("Moved pending owner") {
            REQUIRE(moved.Advance(ChaosScope(), 3, events).Value() == 1);
            REQUIRE(events[0].connection == old);
            const std::array payload{std::byte{1}};
            REQUIRE(moved.Send(ChaosScope(), old.NextGeneration().Value(), ChannelId{}, TransportTrafficClass::Reliable, 0, payload)
                        .HasValue());
        }
        SECTION("Explicit close wins over queued restart") {
            REQUIRE(moved.Close(ChaosScope(), old).HasValue());
            REQUIRE(moved.Advance(ChaosScope(), 3, events).Value() == 1);
            const std::array payload{std::byte{1}};
            REQUIRE(moved.Send(ChaosScope(), old.NextGeneration().Value(), ChannelId{}, TransportTrafficClass::Reliable, 0, payload)
                        .HasError());
        }
    }

    TEST_CASE("Overlapping generation actions terminalize an undrained restart deterministically", "[network][qualification][chaos]") {
        const auto old = ConnectionHandle::Create(0, 1).Value();
        const std::array topology{old};
        const std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Stall},
                                  NetworkChaosAction{2, NetworkChaosActionKind::Restart, old},
                                  NetworkChaosAction{3, NetworkChaosActionKind::Restart, old.NextGeneration().Value()}};
        auto descriptor = ChaosDescriptor(topology, timeline);
        descriptor.maximumEventsPerTick = 1;
        auto created = NetworkChaosScenario::Create(descriptor);
        REQUIRE(created.HasValue());
        auto scenario = std::move(created).Value();
        std::array<DeterministicTransportEvent, 1> events{};
        REQUIRE(scenario.Advance(ChaosScope(), 1, events).Value() == 0);
        REQUIRE(scenario.Advance(ChaosScope(), 2, events).Value() == 0);
        const auto failed = scenario.Advance(ChaosScope(), 3, events);
        REQUIRE(failed.HasError());
        REQUIRE(failed.ErrorValue().code.Value() == NetworkChaosErrors::Invalid.code.Value());
        REQUIRE(scenario.Evidence().actions == 2);
        REQUIRE(scenario.Advance(ChaosScope(), 4, events).HasError());
        REQUIRE(scenario.Shutdown() == 0);
    }

    TEST_CASE("Chaos validates hostile replay input before preparing state and fails closed in release profiles",
              "[network][qualification][chaos]") {
        const auto connection = ConnectionHandle::Create(0, 1).Value();
        const std::array topology{connection};
        auto descriptor = ChaosDescriptor(topology);
        SECTION("release disabled") {
            descriptor.profile = NetworkChaosProfile::Disabled;
        }
        SECTION("unknown profile") {
            descriptor.profile = NetworkChaosProfile::Count;
        }
        SECTION("missing scope") {
            descriptor.scope = {};
        }
        SECTION("unbounded duration") {
            descriptor.durationTicks = std::numeric_limits<std::uint64_t>::max();
        }
        SECTION("production or null mode") {
            descriptor.transport.mode = DeterministicTransportMode::Loopback;
        }
        SECTION("duplicate slots") {
            const std::array duplicate{connection, connection.NextGeneration().Value()};
            descriptor.topology = duplicate;
            REQUIRE(NetworkChaosScenario::Create(descriptor).HasError());
            return;
        }
        SECTION("too many events") {
            const std::array<NetworkChaosAction, 129> oversized{};
            descriptor.timeline = oversized;
            REQUIRE(NetworkChaosScenario::Create(descriptor).HasError());
            return;
        }
        SECTION("late malformed lifecycle") {
            const std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Stall},
                                      NetworkChaosAction{2, NetworkChaosActionKind::Restart, ConnectionHandle::Create(0, 9).Value()}};
            descriptor.timeline = timeline;
            REQUIRE(NetworkChaosScenario::Create(descriptor).HasError());
            return;
        }
        SECTION("generation exhaustion") {
            RequireExhaustedRestartRejected(descriptor);
            return;
        }
        SECTION("unsafe fragment storage replacement") {
            RequireFragmentStorageChangeRejected(descriptor);
            return;
        }
        REQUIRE(NetworkChaosScenario::Create(descriptor).HasError());
    }

    TEST_CASE("Chaos copied replay metadata survives moves and optional instrumentation is disabled", "[network][qualification][chaos]") {
        const auto connection = ConnectionHandle::Create(0, 1).Value();
        std::array topology{connection};
        std::array timeline{NetworkChaosAction{1, NetworkChaosActionKind::Stall}, NetworkChaosAction{2, NetworkChaosActionKind::Resume}};
        auto descriptor = ChaosDescriptor(topology, timeline);
        descriptor.recordEvidence = false;
        auto created = NetworkChaosScenario::Create(descriptor);
        REQUIRE(created.HasValue());
        auto scenario = std::move(created).Value();
        topology[0] = {};
        timeline[0].kind = NetworkChaosActionKind::Count;
        REQUIRE(scenario.Descriptor().topology[0] == connection);
        REQUIRE(scenario.Descriptor().timeline[0].kind == NetworkChaosActionKind::Stall);
        REQUIRE(scenario.Advance(ChaosScope(), 1, {}).HasError());
        std::array<DeterministicTransportEvent, 16> events{};
        const auto before = Horo::Tests::AllocationProbe::Count();
        const auto first = scenario.Advance(ChaosScope(), 1, events);
        const auto resumed = scenario.Advance(ChaosScope(), 2, events);
        const std::array payload{std::byte{1}};
        const auto sent = scenario.Send(ChaosScope(), connection, ChannelId{}, TransportTrafficClass::Reliable, 0, payload);
        const auto delivered = scenario.Advance(ChaosScope(), 3, events);
        const auto allocated = Horo::Tests::AllocationProbe::Count() - before;
        REQUIRE(first.HasValue());
        REQUIRE(resumed.HasValue());
        REQUIRE(sent.HasValue());
        REQUIRE(delivered.Value() == 1);
        REQUIRE(allocated == 0);
        REQUIRE(scenario.Evidence() == NetworkChaosEvidence{});
    }

    TEST_CASE("Chaos checkpoint mismatch terminates the run and prevents late replay", "[network][qualification][chaos]") {
        const std::array topology{ConnectionHandle::Create(0, 1).Value()};
        const std::array checkpoints{NetworkChaosCheckpoint{1, 1, 0, 0}};
        auto descriptor = ChaosDescriptor(topology, {}, checkpoints);
        descriptor.recordEvidence = false;
        auto created = NetworkChaosScenario::Create(descriptor);
        REQUIRE(created.HasValue());
        auto scenario = std::move(created).Value();
        std::array<DeterministicTransportEvent, 16> events{};
        const auto failed = scenario.Advance(ChaosScope(), 1, events);
        REQUIRE(failed.HasError());
        REQUIRE(failed.ErrorValue().code.Value() == NetworkChaosErrors::CheckpointMismatch.code.Value());
        REQUIRE(scenario.Advance(ChaosScope(), 1, {}).HasError());
        REQUIRE(scenario.Evidence().checkpoints == 0);
    }
}  // namespace Horo::Network::Qualification
