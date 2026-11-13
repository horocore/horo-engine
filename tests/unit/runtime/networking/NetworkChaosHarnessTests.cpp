#include "network/ChaosScenarioDriver.h"

#include <limits>

namespace Horo::Network::ChaosTests {
    namespace {
        struct Replay final {
            const std::array<ConnectionHandle, 2> topology{ConnectionHandle::Create(0, 2).Value(), ConnectionHandle::Create(1, 7).Value()};
            std::array<NetworkChaosAction, 9> actions;
            std::array<Traffic, 8> traffic{{{15, TrafficKind::Replica, 1, 99},
                                            {19, TrafficKind::RetainReplica, 1, 7},
                                            {25, TrafficKind::Rpc, 2},
                                            {29, TrafficKind::Replica, 3, 88},
                                            {42, TrafficKind::Replica, 1, 9},
                                            {43, TrafficKind::Rpc, 2},
                                            {45, TrafficKind::LateRetiredPacket},
                                            {46, TrafficKind::ReplayRetainedPacket}}};
            std::array<Checkpoint, 6> checkpoints{
                {{12, 2, 2, 0, 0}, {17, 2, 2, 0, 0}, {29, 2, 2, 1, 7}, {30, 0, 2, 1, 7}, {40, 2, 4, 1, 7}, {52, 0, 4, 2, 9}}};
            NetworkChaosScenarioDescriptor descriptor;

            Replay() {
                const auto initial =
                    TransportImpairmentScenarioV1{.revision = 1, .seed = 123, .latencyTicks = 1, .maximumFragmentBytes = 1200};
                auto loss = initial;
                loss.revision = 2;
                loss.lossPerTenThousand = 10000;
                auto chaos = initial;
                chaos.revision = 3;
                chaos.jitterTicks = 1;
                chaos.duplicatePerTenThousand = 10000;
                chaos.reorderPerTenThousand = 10000;
                auto clean = initial;
                clean.revision = 4;
                actions = {{{14, NetworkChaosActionKind::Impairment, {}, loss},
                            {18, NetworkChaosActionKind::Impairment, {}, chaos},
                            {22, NetworkChaosActionKind::Stall},
                            {24, NetworkChaosActionKind::Resume},
                            {28, NetworkChaosActionKind::Impairment, {}, clean},
                            {30, NetworkChaosActionKind::Restart, topology[0]},
                            {30, NetworkChaosActionKind::Restart, topology[1]},
                            {51, NetworkChaosActionKind::Disconnect, topology[0].NextGeneration().Value()},
                            {51, NetworkChaosActionKind::Disconnect, topology[1].NextGeneration().Value()}}};
                descriptor = {.profile = NetworkChaosProfile::Test,
                              .scope = {TestSupport::Session(100), TestSupport::Session(101)},
                              .revision = 1,
                              .durationTicks = 56,
                              .transport = {.mode = DeterministicTransportMode::Simulated,
                                            .maximumScheduledDeliveries = 32,
                                            .maximumPayloadBytes = 1200,
                                            .maximumChannels = 2,
                                            .budgetCapacity = {4, 32, 38400},
                                            .budgetPolicy = {.contractVersion = 1,
                                                             .revision = 1,
                                                             .maximumActiveConnections = 4,
                                                             .maximumQueuedMessages = 32,
                                                             .maximumQueuedBytes = 38400,
                                                             .maximumQueuedMessagesPerConnection = 32,
                                                             .maximumQueuedBytesPerConnection = 38400,
                                                             .maximumMessagesPerTick = 32,
                                                             .maximumBytesPerTick = 38400,
                                                             .maximumMessagesPerConnectionPerTick = 32,
                                                             .maximumBytesPerConnectionPerTick = 38400,
                                                             .saturationGraceTicks = 2},
                                            .scenario = initial},
                              .topology = topology,
                              .timeline = actions,
                              .maximumEventsPerTick = 32};
            }

            Fixture Input() const {
                return {descriptor, traffic, checkpoints};
            }
        };
    }  // namespace

    TEST_CASE("Chaos replay crosses real wire admission restart RPC and replica checkpoints", "[network][chaos][headless]") {
        Replay replay;
        Driver first{replay.Input()};
        const auto result = first.Run();
        CAPTURE(result.mismatchTick);
        REQUIRE(result.status == RunStatus::Complete);
        REQUIRE(result.observed == replay.checkpoints.size());
        CHECK(result.terminalSessions == 4);
        CHECK(result.rejectedPackets >= 2);  // retired handle and retained frame on replacement occurrence
        CHECK(result.malformedRejections == 0);
        CHECK(result.staleAdmissionRejections == 1);
        CHECK(result.duplicateRejections >= 1);
        CHECK(first.authorities[0]->credentialCalls == 2);
        CHECK(first.authorities[1]->credentialCalls == 2);
        REQUIRE(first.projections[1]->history[0]->Terminal());
        CHECK(first.projections[1]->history[0] != first.projections[1]->history[1]);
        CHECK(first.projections[1]->history[0]->Authentication()->secureChannel.bindingDigest !=
              first.projections[1]->history[1]->Authentication()->secureChannel.bindingDigest);  // independent authentication occurrence
        Driver second{replay.Input()};
        CHECK(second.Run() == result);
    }

    TEST_CASE("Chaos recording disabled preserves admission and replica checkpoint enforcement", "[network][chaos][headless]") {
        Replay replay;
        replay.descriptor.recordEvidence = false;
        Driver driver{replay.Input()};
        const auto result = driver.Run();
        CAPTURE(result.mismatchTick);
        REQUIRE(result.status == RunStatus::Complete);
        CHECK(result.evidence == NetworkChaosEvidence{});
        CHECK(result.observed == 6);
        CHECK(result.terminalSessions == 4);
    }

    TEST_CASE("Chaos mismatched application checkpoint terminalizes and cleans all owners", "[network][chaos][headless]") {
        Replay replay;
        replay.checkpoints[0].replica = 77;
        Driver driver{replay.Input()};
        const auto result = driver.Run();
        REQUIRE(result.status == RunStatus::CheckpointMismatch);
        CHECK(result.mismatchTick == 12);
        CHECK(result.terminalSessions == 2);
        CHECK(driver.gameplay.rpc->Handle(MessageEnvelope{}).HasError());
        driver.Shutdown();
        CHECK(driver.wire.scenario.Send(driver.wire.scope, replay.topology[0], {}, TransportTrafficClass::Reliable, 0, {}).HasError());
    }

    TEST_CASE("Chaos saturated stalled wire remains bounded and retires pending session work", "[network][chaos][headless]") {
        Replay replay;
        const std::array actions{NetworkChaosAction{13, NetworkChaosActionKind::Stall},
                                 NetworkChaosAction{18, NetworkChaosActionKind::Resume}};
        const std::array traffic{Traffic{14, TrafficKind::Saturation, 1, 0, 64}};
        const std::array checkpoints{Checkpoint{12, 2, 2, 0, 0}};
        replay.descriptor.timeline = actions;
        replay.descriptor.durationTicks = 24;
        Driver driver{{replay.descriptor, traffic, checkpoints}};
        const auto result = driver.Run();
        CHECK(result.status == RunStatus::Complete);
        CHECK(result.rejectedSends > 0);
        CHECK(result.terminalSessions == 2);
        CHECK(driver.gameplay.handler->calls <= 1);
    }

    TEST_CASE("Chaos owner issues replacement witness only after successful restart opening", "[network][chaos][headless]") {
        Replay replay;
        const auto old = replay.topology[0];
        const std::array actions{NetworkChaosAction{1, NetworkChaosActionKind::Stall},
                                 NetworkChaosAction{2, NetworkChaosActionKind::Restart, old},
                                 NetworkChaosAction{4, NetworkChaosActionKind::Resume}};
        replay.descriptor.timeline = actions;
        replay.descriptor.durationTicks = 6;
        auto scenario = Wire::Create(replay.descriptor);
        std::array<DeterministicTransportEvent, 32> events;
        CHECK(scenario.RestartedConnection(replay.descriptor.scope, old).HasError());
        REQUIRE(scenario.Advance(replay.descriptor.scope, 1, events).HasValue());
        REQUIRE(scenario.Advance(replay.descriptor.scope, 2, events).HasValue());
        CHECK(scenario.RestartedConnection(replay.descriptor.scope, old).HasError());
        auto moved = std::move(scenario);
        REQUIRE(moved.Advance(replay.descriptor.scope, 3, events).HasValue());
        CHECK(moved.RestartedConnection(replay.descriptor.scope, old).HasError());
        REQUIRE(moved.Advance(replay.descriptor.scope, 4, events).Value() == 1);
        const auto issued = moved.RestartedConnection(replay.descriptor.scope, old);
        REQUIRE(issued.HasValue());
        CHECK(issued.Value() == old.NextGeneration().Value());
        CHECK(moved.RestartedConnection({TestSupport::Session(999), TestSupport::Session(101)}, old).HasError());
        const std::array payload{std::byte{1}};
        CHECK(moved.Send(replay.descriptor.scope, issued.Value(), {}, TransportTrafficClass::Reliable, 0, payload).HasValue());
        REQUIRE(moved.Close(replay.descriptor.scope, issued.Value()).HasValue());
        CHECK(moved.RestartedConnection(replay.descriptor.scope, old).HasError());
        static_cast<void>(moved.Shutdown());
        CHECK(moved.RestartedConnection(replay.descriptor.scope, old).HasError());
    }

    TEST_CASE("Chaos malformed envelope closes canonical admission without replica publication", "[network][chaos][headless]") {
        Replay replay;
        const std::array traffic{Traffic{13, TrafficKind::Malformed, 1}};
        const std::array<Checkpoint, 2> checkpoints{{{12, 2, 2, 0, 0}, {16, 1, 2, 0, 0}}};
        replay.descriptor.timeline = {};
        replay.descriptor.durationTicks = 18;
        Driver driver{{replay.descriptor, traffic, checkpoints}};
        const auto result = driver.Run();
        CAPTURE(result.mismatchTick);
        CHECK(result.status == RunStatus::Complete);
        CHECK(driver.gameplay.Value() == 0);
        REQUIRE(driver.projections[1]->current->Terminal());
        CHECK(driver.projections[1]->current->Terminal()->kind == PeerSessionTerminalKind::TransportFailed);
        CHECK(result.malformedRejections == 1);  // exact NetworkErrors::MessageEnvelopeInvalid domain/code
        CHECK(result.terminalSessions == 2);
    }

    TEST_CASE("Chaos cancellation after receipt discards staged replica before Scene publication", "[network][chaos][headless]") {
        Replay replay;
        const std::array<Traffic, 2> traffic{{{13, TrafficKind::Replica, 1, 7}, {14, TrafficKind::CancelAfterReceipt}}};
        const std::array<Checkpoint, 2> checkpoints{{{12, 2, 2, 0, 0}, {14, 1, 2, 0, 0}}};
        replay.descriptor.timeline = {};
        replay.descriptor.durationTicks = 16;
        Driver driver{{replay.descriptor, traffic, checkpoints}};
        const auto result = driver.Run();
        CAPTURE(result.mismatchTick);
        CHECK(result.status == RunStatus::Complete);
        CHECK(driver.gameplay.Value() == 0);
        REQUIRE(driver.projections[1]->current->Terminal());
        CHECK(driver.projections[1]->current->Terminal()->kind == PeerSessionTerminalKind::LocalCancellation);
        CHECK(result.terminalSessions == 2);
    }

    TEST_CASE("Chaos immutable application fixture rejects hostile and oversized schedules", "[network][chaos][headless]") {
        Replay replay;
        std::array<Traffic, 65> oversized{};
        CHECK_FALSE(Driver::Valid({replay.descriptor, oversized, replay.checkpoints}));
        replay.traffic[0].value = std::numeric_limits<double>::infinity();
        CHECK_FALSE(Driver::Valid(replay.Input()));
        replay.traffic[0].value = 0;
        replay.traffic[0].count = 65;
        CHECK_FALSE(Driver::Valid(replay.Input()));
        replay.traffic[0].count = 1;
        replay.checkpoints[0].tick = 0;
        CHECK_FALSE(Driver::Valid(replay.Input()));
    }

    TEST_CASE("Chaos copies application schedule before the caller replaces setup buffers", "[network][chaos][headless]") {
        Replay replay;
        Driver driver{replay.Input()};
        replay.traffic.fill({});
        replay.checkpoints.fill({});
        replay.actions.fill({});
        const auto result = driver.Run();
        CAPTURE(result.mismatchTick);
        CHECK(result.status == RunStatus::Complete);
        CHECK(result.observed == 6);
    }
}  // namespace Horo::Network::ChaosTests
