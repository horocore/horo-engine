#pragma once
#include "ChaosScenarioGameplay.h"

#include <cmath>

namespace Horo::Network::ChaosTests {
    enum class TrafficKind : std::uint8_t {
        Rpc,
        Replica,
        RetainReplica,
        Malformed,
        LateRetiredPacket,
        ReplayRetainedPacket,
        Saturation,
        CancelAfterReceipt,
        Count
    };

    struct Traffic final {
        std::uint64_t tick{};
        TrafficKind kind{TrafficKind::Count};
        std::uint32_t sequence{};
        double value{};
        std::size_t count{1};
        constexpr auto operator<=>(const Traffic &) const noexcept = default;
    };

    struct Checkpoint final {
        std::uint64_t tick{};
        std::size_t active{};
        std::size_t activated{};
        std::size_t rpcCalls{};
        float replica{};
        constexpr auto operator<=>(const Checkpoint &) const noexcept = default;
    };

    struct Fixture final {
        NetworkChaosScenarioDescriptor scenario;
        std::span<const Traffic> traffic;
        std::span<const Checkpoint> checkpoints;
    };

    struct RecordedAction final {
        std::uint64_t tick{};
        NetworkChaosActionKind kind{};
        ConnectionHandle connection;
        TransportImpairmentScenarioV1 impairment;
        constexpr auto operator<=>(const RecordedAction &) const noexcept = default;
    };
    enum class RunStatus : std::uint8_t {
        Complete,
        CheckpointMismatch
    };

    /** @brief Fixed numeric artifact. Never retains wire bytes, principals, credentials, provider errors or endpoint strings. */
    struct Artifact final {
        std::uint64_t seed{};
        std::uint64_t revision{};
        std::uint64_t duration{};
        // Replay metadata is bounded synthetic fixture data, never captured native traffic.
        std::array<ConnectionHandle, 16> topology{};
        std::array<RecordedAction, 128> timeline{};
        std::array<Traffic, 64> traffic{};
        std::array<Checkpoint, 64> expected{};
        std::size_t topologyCount{};
        std::size_t actionCount{};
        std::size_t trafficCount{};
        std::size_t expectedCount{};
        std::uint64_t mismatchTick{};
        RunStatus status{};
        std::array<Checkpoint, 64> observations{};
        std::size_t observed{};
        std::uint64_t rejectedSends{};
        std::uint64_t rejectedPackets{};
        std::uint64_t malformedRejections{};
        std::uint64_t staleAdmissionRejections{};
        std::uint64_t duplicateRejections{};
        std::uint64_t terminalSessions{};
        NetworkChaosEvidence evidence;
        constexpr auto operator<=>(const Artifact &) const noexcept = default;
    };

    /** @brief Copies one immutable finite replay fixture and drives real admission/replica/RPC owners in a pinned tick order. */
    class Driver final {
    public:
        static bool Valid(const Fixture &fixture) {
            return ValidTraffic(fixture) && ValidCheckpoints(fixture);
        }

        static bool ValidTraffic(const Fixture &fixture) {
            if (fixture.traffic.size() > 64)
                return false;
            std::uint64_t previous{};
            for (const auto &traffic : fixture.traffic) {
                if (traffic.tick == 0 || traffic.tick < previous || traffic.tick > fixture.scenario.durationTicks)
                    return false;
                if (traffic.kind >= TrafficKind::Count || traffic.count == 0 || traffic.count > 64 || !std::isfinite(traffic.value))
                    return false;
                previous = traffic.tick;
            }
            return true;
        }

        static bool ValidCheckpoints(const Fixture &fixture) {
            if (fixture.checkpoints.size() > 64)
                return false;
            std::uint64_t previous{};
            for (const auto &checkpoint : fixture.checkpoints) {
                if (checkpoint.tick <= previous || checkpoint.tick > fixture.scenario.durationTicks || checkpoint.active > 2)
                    return false;
                if (!std::isfinite(checkpoint.replica))
                    return false;
                previous = checkpoint.tick;
            }
            return true;
        }

        explicit Driver(const Fixture &fixture) : wire(fixture.scenario) {
            REQUIRE(Valid(fixture));
            std::ranges::copy(fixture.traffic, traffic_.begin());
            std::ranges::copy(fixture.checkpoints, checkpoints_.begin());
            trafficCount_ = fixture.traffic.size();
            checkpointCount_ = fixture.checkpoints.size();
            const auto bytes = TestSupport::Bytes<32>(0x91);
            key_ = std::make_shared<Security::SecureBytes>(bytes);
            for (std::size_t side = 0; side < 2; ++side)
                Compose(side);
            wire.Start();
        }

        ~Driver() {
            Shutdown();
        }

        Driver(const Driver &) = delete;
        Driver &operator=(const Driver &) = delete;

        Artifact Run() {
            Artifact artifact{.seed = wire.scenario.Descriptor().transport.scenario.seed,
                              .revision = wire.scenario.Descriptor().revision,
                              .duration = wire.scenario.Descriptor().durationTicks};
            Record(artifact);
            for (tick_ = 1; tick_ <= artifact.duration; ++tick_) {
                Step();
                if (!Check(artifact))
                    break;
            }
            artifact.rejectedSends = wire.rejectedSends;
            artifact.rejectedPackets = rejectedPackets_;
            artifact.malformedRejections = malformedRejections_;
            artifact.staleAdmissionRejections = staleAdmissionRejections_;
            artifact.duplicateRejections = duplicateRejections_;
            artifact.evidence = wire.scenario.Evidence();
            Shutdown();
            artifact.terminalSessions = TerminalSessions();
            return artifact;
        }

        void Step() {
            // Version-one ordering: publish prior drained restarts, advance actions/delivery, poll admission,
            // schedule this tick's traffic, then commit Gameplay and Scene, then compare checkpoints.
            PublishConnections();
            wire.Advance(tick_);
            for (std::size_t side = 0; side < 2; ++side)
                Poll(side);
            RetireTerminal();
            while (nextTraffic_ < trafficCount_ && traffic_[nextTraffic_].tick == tick_)
                Send(traffic_[nextTraffic_++]);
            gameplay.Commit(tick_);
        }

        void Shutdown() {
            if (shutdown_)
                return;
            shutdown_ = true;
            for (auto &controller : controllers)
                if (controller)
                    controller->Shutdown(std::min(tick_, wire.scenario.Descriptor().durationTicks));
            gameplay.Retire();
            gameplay.rpc->Shutdown();
            for (auto &router : routers)
                if (router)
                    router->Shutdown();
            static_cast<void>(wire.scenario.Shutdown());
        }

        Wire wire;
        Admission admission;
        Gameplay gameplay;
        std::array<std::unique_ptr<Transport>, 2> transports;
        std::array<std::unique_ptr<InboundMessageDispatcher>, 2> routers;
        std::array<std::shared_ptr<Projection>, 2> projections;
        std::array<std::shared_ptr<AdmissionTests::Authority>, 2> authorities;
        std::array<std::shared_ptr<NetworkAdmissionController>, 2> controllers;

    private:
        void Record(Artifact &artifact) const {
            const auto &descriptor = wire.scenario.Descriptor();
            artifact.topologyCount = descriptor.topology.size();
            artifact.actionCount = descriptor.timeline.size();
            artifact.trafficCount = trafficCount_;
            artifact.expectedCount = checkpointCount_;
            std::ranges::copy(descriptor.topology, artifact.topology.begin());
            for (std::size_t index = 0; index < artifact.actionCount; ++index) {
                const auto &action = descriptor.timeline[index];
                artifact.timeline[index] = {action.tick, action.kind, action.connection, action.impairment};
            }
            artifact.traffic = traffic_;
            artifact.expected = checkpoints_;
        }

        void Poll(const std::size_t side) {
            if (stopped_[side])
                return;
            const auto begun = controllers[side]->BeginPoll(tick_);
            if (begun.HasError())
                UNSCOPED_INFO("side=" << side << " tick=" << tick_ << " " << begun.ErrorValue().domain.Value() << ":"
                                      << begun.ErrorValue().code.Value() << ":" << begun.ErrorValue().message);
            REQUIRE(begun.HasValue());
            const auto polled = routers[side]->RunNetworkPoll(tick_);
            if (polled.HasError())
                UNSCOPED_INFO("side=" << side << " tick=" << tick_ << " " << polled.ErrorValue().domain.Value() << ":"
                                      << polled.ErrorValue().code.Value() << ":" << polled.ErrorValue().message);
            REQUIRE(polled.HasValue());
            ObserveRejection(polled.Value());
        }

        static bool Matches(const Error &error, const ErrorCodeDescriptor &descriptor) {
            return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
        }

        void ObserveRejection(const InboundDispatchReport &report) {
            rejectedPackets_ += report.rejectedPackets;
            if (!report.lastPacketRejection)
                return;
            const auto &error = *report.lastPacketRejection;
            if (Matches(error, NetworkErrors::MessageEnvelopeInvalid))
                ++malformedRejections_;
            if (Matches(error, NetworkErrors::AdmissionBindingInvalid))
                ++staleAdmissionRejections_;
            if (Matches(error, NetworkErrors::MessageDeliveryDuplicate))
                ++duplicateRejections_;
        }

        void PublishConnections() {
            for (std::size_t side = 0; side < 2; ++side)
                if (wire.reconnect[side])
                    ++authorities[side]->channelGeneration;
            wire.PublishRestarts();
        }

        void Compose(const std::size_t side) {
            transports[side] = std::make_unique<Transport>(wire, side);
            routers[side] = std::move(InboundMessageDispatcher::Create(*transports[side], *admission.codecs)).Value();
            projections[side] = std::make_shared<Projection>();
            authorities[side] = std::make_shared<AdmissionTests::Authority>(wire.evidence, side, key_);
            if (side == 1) {
                projections[side]->bind = [this] {
                    gameplay.Bind(*projections[1], admission, *routers[1], tick_);
                };
                REQUIRE(routers[side]
                            ->RegisterHandler({admission.protocols[0].id, admission.messages[4].id, MessageTrafficClass::Command,
                                               Runtime::RuntimePhase::NetworkPoll, 64},
                                              gameplay.rpc)
                            .HasValue());
            }
            const auto value = NetworkAdmissionController::Create(*transports[side], *routers[side], admission.codecs,
                                                                  admission.Policy(side, transports[side]->Capabilities()),
                                                                  authorities[side]->Providers(authorities[side]), projections[side]);
            if (value.HasError())
                UNSCOPED_INFO(value.ErrorValue().domain.Value()
                              << ":" << value.ErrorValue().code.Value() << ":" << value.ErrorValue().message);
            REQUIRE(value.HasValue());
            controllers[side] = value.Value();
            routers[side]->SetAdmissionController(controllers[side]);
        }

        void RetireTerminal() {
            if (projections[1]->current && projections[1]->current->Terminal() && gameplay.bound.IsValid())
                gameplay.Retire();
        }

        std::vector<std::byte> Payload(const Traffic &traffic) {
            if (traffic.kind == TrafficKind::Replica || traffic.kind == TrafficKind::RetainReplica)
                return admission.Frame(5, traffic.sequence, gameplay.Replica(tick_, traffic.value));
            if (traffic.kind == TrafficKind::Malformed)
                return {std::byte{0xff}, std::byte{0xff}};
            return admission.Frame(4, traffic.sequence, gameplay.Rpc(traffic.sequence));
        }

        std::vector<std::byte> ReplayPayload(const Traffic &traffic) {
            if (traffic.kind == TrafficKind::LateRetiredPacket || traffic.kind == TrafficKind::ReplayRetainedPacket) {
                REQUIRE(retainedSize_ != 0);
                return {retained_.begin(), retained_.begin() + static_cast<std::ptrdiff_t>(retainedSize_)};
            }
            auto payload = Payload(traffic);
            if (traffic.kind == TrafficKind::RetainReplica) {
                REQUIRE(payload.size() <= retained_.size());
                std::ranges::copy(payload, retained_.begin());
                retainedSize_ = payload.size();
            }
            return payload;
        }

        void Send(const Traffic &traffic) {
            if (traffic.kind == TrafficKind::CancelAfterReceipt) {
                controllers[1]->Cancel(tick_);
                stopped_[1] = true;
                routers[1]->Shutdown();
                RetireTerminal();
                return;
            }
            SendPayload(traffic);
        }

        void SendPayload(const Traffic &traffic) {
            auto payload = ReplayPayload(traffic);
            if (traffic.kind == TrafficKind::LateRetiredPacket) {
                REQUIRE(projections[1]->prepared >= 2);
                REQUIRE(wire.evidence
                            ->Queue(1, {.kind = NetworkTransportEventKind::PacketReceived,
                                        .connection = projections[1]->history[0]->Connection(),
                                        .channel = ChannelId::Create(1, 2).Value(),
                                        .delivery = DeliveryPolicy::ReliableOrdered,
                                        .payload = payload})
                            .HasValue());
                return;
            }
            for (std::size_t index = 0; index < traffic.count; ++index) {
                const auto sent = transports[0]->Send(wire.evidence->ends[0].connection, ChannelId::Create(1, 2).Value(), payload,
                                                      DeliveryPolicy::ReliableOrdered);
                if (traffic.kind != TrafficKind::Saturation)
                    REQUIRE(sent.HasValue());
            }
        }

        Checkpoint Observe() const {
            Checkpoint observation{.tick = tick_, .rpcCalls = gameplay.handler->calls, .replica = gameplay.Value()};
            for (const auto &projection : projections) {
                observation.activated += projection->activated;
                if (projection->current && projection->current->State() == PeerSessionState::Active)
                    ++observation.active;
            }
            return observation;
        }

        bool Check(Artifact &artifact) {
            if (nextCheckpoint_ == checkpointCount_ || checkpoints_[nextCheckpoint_].tick != tick_)
                return true;
            const auto observation = Observe();
            artifact.observations[artifact.observed++] = observation;
            if (observation == checkpoints_[nextCheckpoint_++])
                return true;
            artifact.status = RunStatus::CheckpointMismatch;
            artifact.mismatchTick = tick_;
            return false;
        }

        std::uint64_t TerminalSessions() const {
            std::uint64_t count{};
            for (const auto &projection : projections)
                for (std::size_t index = 0; index < projection->prepared; ++index)
                    count += projection->history[index]->Terminal() != nullptr;
            return count;
        }

        std::uint64_t rejectedPackets_{};
        std::uint64_t malformedRejections_{};
        std::uint64_t staleAdmissionRejections_{};
        std::uint64_t duplicateRejections_{};
        std::array<bool, 2> stopped_{};
        std::array<std::byte, 1200> retained_{};
        std::size_t retainedSize_{};
        std::shared_ptr<Security::SecureBytes> key_;
        std::array<Traffic, 64> traffic_{};
        std::array<Checkpoint, 64> checkpoints_{};
        std::size_t trafficCount_{};
        std::size_t checkpointCount_{};
        std::size_t nextTraffic_{};
        std::size_t nextCheckpoint_{};
        std::uint64_t tick_{1};
        bool shutdown_{};
    };
}  // namespace Horo::Network::ChaosTests
