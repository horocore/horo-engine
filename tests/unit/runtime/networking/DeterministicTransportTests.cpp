#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkMetrics.h"
#include "NetworkTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace Horo::Network {
    using TestSupport::RequireError;

    namespace {
        DeterministicTransportDescriptor Descriptor(const DeterministicTransportMode mode) {
            return {
                .mode = mode,
                .maximumScheduledDeliveries = 16,
                .maximumPayloadBytes = 32,
                .maximumChannels = 2,
                .budgetCapacity = {2, 8, 64},
                .budgetPolicy =
                    {
                        .contractVersion = 1,
                        .revision = 1,
                        .maximumActiveConnections = 2,
                        .maximumQueuedMessages = 8,
                        .maximumQueuedBytes = 64,
                        .maximumQueuedMessagesPerConnection = 8,
                        .maximumQueuedBytesPerConnection = 64,
                        .maximumMessagesPerTick = 8,
                        .maximumBytesPerTick = 64,
                        .maximumMessagesPerConnectionPerTick = 8,
                        .maximumBytesPerConnectionPerTick = 64,
                        .saturationGraceTicks = 2,
                    },
                .scenario =
                    {
                        .contractVersion = 1,
                        .revision = 1,
                        .seed = 0x1234'5678ULL,
                        .latencyTicks = 1,
                        .jitterTicks = 0,
                        .lossPerTenThousand = 0,
                        .duplicatePerTenThousand = 0,
                        .reorderPerTenThousand = 0,
                        .maximumFragmentBytes = 4,
                    },
            };
        }

        ConnectionHandle Connection(const std::uint32_t generation = 1) {
            return ConnectionHandle::Create(0, generation).Value();
        }

        ChannelId Channel(const std::uint32_t value = 0) {
            return ChannelId::Create(value, 2).Value();
        }

        DeterministicTransport Transport(DeterministicTransportDescriptor descriptor) {
            auto created = DeterministicTransport::Create(descriptor);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        struct ReadyLoopback {
            ReadyLoopback() {
                REQUIRE(transport.Open(connection).HasValue());
                std::array<DeterministicTransportEvent, 1> none{};
                REQUIRE(transport.Advance(1, none).Value() == 0);
            }

            DeterministicTransport transport{Transport(Descriptor(DeterministicTransportMode::Loopback))};
            ConnectionHandle connection{Connection()};
        };
    }  // namespace

    TEST_CASE("Null transport rejects networking without ambient fallback", "[unit][network][transport-null]") {
        auto transport = Transport(Descriptor(DeterministicTransportMode::RejectAll));
        RequireError(transport.Open(Connection()), NetworkErrors::TransportCapabilityUnavailable);
        const std::array payload{std::byte{1}};
        RequireError(transport.Send(Connection(), Channel(), TransportTrafficClass::Reliable, 0, payload),
                     NetworkErrors::TransportCapabilityUnavailable);
    }

    TEST_CASE("Deterministic transport reports committed simulated loss and owner queue state", "[unit][network][metrics]") {
        NetworkMetrics metrics{92, true};
        auto descriptor = Descriptor(DeterministicTransportMode::Simulated);
        descriptor.scenario.lossPerTenThousand = 10'000;
        auto transport = std::move(DeterministicTransport::Create(descriptor, &metrics)).Value();
        REQUIRE(transport.Open(Connection()).HasValue());
        std::array<DeterministicTransportEvent, 1> events{};
        REQUIRE(transport.Advance(1, events).Value() == 0);
        const std::array payload{std::byte{1}, std::byte{2}};
        REQUIRE(transport.Send(Connection(), Channel(), TransportTrafficClass::Reliable, 0, payload).Value().outcome ==
                DeterministicSendOutcome::SimulatedLoss);
        REQUIRE(transport.Advance(2, events).Value() == 0);
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        REQUIRE(snapshot.packetsLost == 1);
        REQUIRE(snapshot.messages[static_cast<std::size_t>(NetworkMetricDirection::Sent)]
                                 [static_cast<std::size_t>(NetworkMetricCategory::Transport)] == 1);
        REQUIRE(snapshot.activeConnections == 1);
        REQUIRE(snapshot.queueDepth[static_cast<std::size_t>(NetworkMetricQueue::Outbound)] == 0);
        REQUIRE(transport.Shutdown() == 0);
        REQUIRE(metrics.Close());
    }

    TEST_CASE("Loopback metrics count only emitted receive fragments and their bytes", "[unit][network][metrics]") {
        NetworkMetrics metrics{93, true};
        auto transport = std::move(DeterministicTransport::Create(Descriptor(DeterministicTransportMode::Loopback), &metrics)).Value();
        REQUIRE(transport.Open(Connection()).HasValue());
        std::array<DeterministicTransportEvent, 2> events{};
        REQUIRE(transport.Advance(1, events).Value() == 0);
        const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}};
        REQUIRE(transport.Send(Connection(), Channel(), TransportTrafficClass::Reliable, 0, payload).HasValue());
        REQUIRE(transport.Advance(2, std::span{events}.first(1)).Value() == 1);
        REQUIRE(transport.Advance(3, events).Value() == 1);
        REQUIRE(metrics.Publish());
        const auto snapshot = metrics.Snapshot();
        constexpr auto sent = static_cast<std::size_t>(NetworkMetricDirection::Sent);
        constexpr auto received = static_cast<std::size_t>(NetworkMetricDirection::Received);
        constexpr auto transportCategory = static_cast<std::size_t>(NetworkMetricCategory::Transport);
        REQUIRE(snapshot.messages[sent][transportCategory] == 1);
        REQUIRE(snapshot.bytes[sent][transportCategory] == payload.size());
        REQUIRE(snapshot.messages[received][transportCategory] == 2);
        REQUIRE(snapshot.bytes[received][transportCategory] == payload.size());
        REQUIRE(transport.Shutdown() == 0);
        REQUIRE(metrics.Close());
    }

    TEST_CASE_METHOD(ReadyLoopback, "Loopback copies and deterministically fragments caller payload", "[unit][network][transport-null]") {
        std::array payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6}};
        const auto sent = transport.Send(connection, Channel(), TransportTrafficClass::Reliable, 0, payload);
        REQUIRE(sent.Value().outcome == DeterministicSendOutcome::Scheduled);
        REQUIRE(sent.Value().fragmentCount == 2);
        payload.fill(std::byte{9});

        std::array<DeterministicTransportEvent, 2> events{};
        REQUIRE(transport.Advance(2, events).Value() == 2);
        REQUIRE(events[0].fragmentIndex == 0);
        REQUIRE(events[0].fragmentCount == 2);
        const std::array expectedFirst{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        REQUIRE(std::ranges::equal(events[0].payload, expectedFirst));
        REQUIRE(events[1].fragmentIndex == 1);
        const std::array expectedSecond{std::byte{5}, std::byte{6}};
        REQUIRE(std::ranges::equal(events[1].payload, expectedSecond));
    }

    TEST_CASE_METHOD(ReadyLoopback, "Replaceable loopback state removes superseded scheduled fragments",
                     "[unit][network][transport-null]") {
        const std::array first{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
        const std::array latest{std::byte{7}, std::byte{8}};
        REQUIRE(transport.Send(connection, Channel(), TransportTrafficClass::ReplaceableState, 77, first).Value().copyCount == 1);
        REQUIRE(transport.Send(connection, Channel(), TransportTrafficClass::ReplaceableState, 77, latest).Value().outcome ==
                DeterministicSendOutcome::Replaced);

        std::array<DeterministicTransportEvent, 3> events{};
        REQUIRE(transport.Advance(2, events).Value() == 1);
        REQUIRE(std::ranges::equal(events[0].payload, latest));
    }

    TEST_CASE("Seed and scenario reproduce impairment outcomes and delivery order", "[unit][network][transport-null]") {
        auto descriptor = Descriptor(DeterministicTransportMode::Simulated);
        descriptor.scenario.jitterTicks = 2;
        descriptor.scenario.lossPerTenThousand = 2'500;
        descriptor.scenario.duplicatePerTenThousand = 5'000;
        descriptor.scenario.reorderPerTenThousand = 5'000;
        auto first = Transport(descriptor);
        auto second = Transport(descriptor);
        REQUIRE(first.Open(Connection()).HasValue());
        REQUIRE(second.Open(Connection()).HasValue());
        std::array<DeterministicTransportEvent, 1> empty{};
        REQUIRE(first.Advance(1, empty).Value() == 0);
        REQUIRE(second.Advance(1, empty).Value() == 0);

        const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}};
        for (std::size_t index = 0; index < 4; ++index) {
            const auto left = first.Send(Connection(), Channel(), TransportTrafficClass::Reliable, 0, payload);
            const auto right = second.Send(Connection(), Channel(), TransportTrafficClass::Reliable, 0, payload);
            REQUIRE(left.Value().outcome == right.Value().outcome);
            REQUIRE(left.Value().fragmentCount == right.Value().fragmentCount);
            REQUIRE(left.Value().copyCount == right.Value().copyCount);
        }

        for (std::uint64_t tick = 2; tick <= 6; ++tick) {
            std::array<DeterministicTransportEvent, 8> left{};
            std::array<DeterministicTransportEvent, 8> right{};
            const auto leftCount = first.Advance(tick, left).Value();
            const auto rightCount = second.Advance(tick, right).Value();
            REQUIRE(leftCount == rightCount);
            for (std::size_t index = 0; index < leftCount; ++index) {
                REQUIRE(left[index].sequence == right[index].sequence);
                REQUIRE(left[index].fragmentIndex == right[index].fragmentIndex);
                REQUIRE(std::ranges::equal(left[index].payload, right[index].payload));
            }
        }
    }

    TEST_CASE("Jitter greater than latency clamps negative delay without unsigned underflow", "[unit][network][transport-null]") {
        auto descriptor = Descriptor(DeterministicTransportMode::Simulated);
        descriptor.scenario.latencyTicks = 0;
        descriptor.scenario.jitterTicks = 2;
        auto transport = Transport(descriptor);
        REQUIRE(transport.Open(Connection()).HasValue());
        std::array<DeterministicTransportEvent, 8> events{};
        REQUIRE(transport.Advance(1, events).Value() == 0);

        const std::array payload{std::byte{1}};
        for (std::size_t index = 0; index < events.size(); ++index)
            REQUIRE(transport.Send(Connection(), Channel(), TransportTrafficClass::Reliable, 0, payload).HasValue());

        REQUIRE(transport.Advance(3, events).Value() == events.size());
    }

    TEST_CASE("Deterministic transport preserves bounds cancellation disconnect and shutdown", "[unit][network][transport-null]") {
        auto descriptor = Descriptor(DeterministicTransportMode::Loopback);
        descriptor.maximumScheduledDeliveries = 1;
        auto transport = Transport(descriptor);
        RequireError(transport.Open(Connection(), TransportAdmissionState::Cancelled), NetworkErrors::TransportOperationCancelled);
        REQUIRE(transport.Open(Connection()).HasValue());
        std::array<DeterministicTransportEvent, 2> events{};
        REQUIRE(transport.Advance(1, events).Value() == 0);
        const std::array oversizedSchedule{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
        RequireError(transport.Send(Connection(), Channel(), TransportTrafficClass::Reliable, 0, oversizedSchedule),
                     NetworkErrors::TransportBudgetCapacityExceeded);
        const std::array fits{std::byte{6}};
        REQUIRE(transport.Send(Connection(), Channel(), TransportTrafficClass::Reliable, 0, fits).HasValue());
        REQUIRE(transport.Close(Connection()).Value() == 1);
        REQUIRE(transport.Advance(2, events).Value() == 1);
        REQUIRE(events[0].kind == DeterministicTransportEventKind::Disconnected);
        RequireError(transport.Open(Connection(3)), NetworkErrors::NetworkLifecycleOperationStale);
        REQUIRE(transport.Open(Connection(2)).HasValue());
        REQUIRE(transport.Shutdown() == 0);
        REQUIRE(transport.Shutdown() == 0);
        RequireError(transport.Advance(3, events), NetworkErrors::TransportShuttingDown);
        RequireError(transport.Send(Connection(), Channel(), TransportTrafficClass::Reliable, 0, fits),
                     NetworkErrors::TransportShuttingDown);

        descriptor.scenario.lossPerTenThousand = 10'001;
        RequireError(DeterministicTransport::Create(descriptor), NetworkErrors::TransportBudgetInvalid);
    }
}  // namespace Horo::Network
