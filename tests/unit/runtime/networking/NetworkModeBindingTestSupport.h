#pragma once
/** @file NetworkModeBindingTestSupport.h @brief Canonical mode lifecycle tests with a bounded recording transport and real dispatcher. */
#include "NetworkModeHostAccess.h"
#include "NetworkTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace Horo::Network::ModeBindingTests {
    using TestSupport::Bytes;
    using TestSupport::Id;
    using TestSupport::WireIdentity;

    inline std::shared_ptr<PeerSessionLifecycle> Negotiating(const ConnectionHandle connection,
                                                             const NetworkOperationGeneration generation) {
        auto session = PeerSessionLifecycle::Create(connection, generation, {10, 20, 30, 150, 100});
        REQUIRE(session.HasValue());
        auto owner = std::make_shared<PeerSessionLifecycle>(std::move(session).Value());
        REQUIRE(owner->BeginNegotiation(connection, generation, 1).HasValue());
        return owner;
    }

    inline void CompleteAdmission(const std::shared_ptr<PeerSessionLifecycle> &owner, const NetworkOperationGeneration generation) {
        const auto connection = owner->Connection();
        HandshakeSelection selected;
        selected.connection = connection;
        selected.sessionGeneration = generation;
        selected.protocol = WireIdentity<ProtocolId>(1);
        selected.version = {1, 0};
        selected.schemaFingerprint = 42;
        selected.features.values[0] = WireIdentity<ProtocolFeatureId>(1);
        selected.features.count = 1;
        selected.transport.capabilityRevision = 3;
        selected.transport.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
        selected.transport.channelCount = 2;
        selected.transport.maximumMessageBytes = 1200;
        REQUIRE(owner->AcceptNegotiation(selected, 2).HasValue());
        AuthenticationResult authentication;
        authentication.connection = connection;
        authentication.sessionGeneration = generation;
        authentication.policy = Id<NetworkTrustPolicyId>(10);
        authentication.policyRevision = 4;
        authentication.principal.principal = Id<NetworkPrincipalId>(20);
        authentication.principal.session.bytes = Bytes<NetworkSessionIdBytes>(0x80);
        authentication.principal.trustLevel = NetworkTrustLevel::ProductAnchor;
        authentication.principal.roles.values[0] = Id<NetworkRoleId>(30);
        authentication.principal.roles.count = 1;
        authentication.principal.capabilities.values[0] = Id<NetworkCapabilityId>(40);
        authentication.principal.capabilities.count = 1;
        authentication.principal.provenance = Id<CredentialProvenanceId>(50);
        authentication.principal.expiresAtTick = 150;
        authentication.secureChannel = {{connection, generation, 5},
                                        Id<PrivateKeyBindingId>(11),
                                        Id<SecureChannelId>(12),
                                        6,
                                        Bytes<AuthenticationDigestBytes>(0x20)};
        REQUIRE(owner->AcceptAuthentication(authentication, 11).HasValue());
        REQUIRE(
            owner->Activate({connection, generation, Id<SecureChannelId>(12), 6, Bytes<AuthenticationDigestBytes>(0x20)}, 21).HasValue());
    }

    inline std::shared_ptr<PeerSessionLifecycle> Active(const ConnectionHandle connection, const NetworkOperationGeneration generation) {
        auto owner = Negotiating(connection, generation);
        CompleteAdmission(owner, generation);
        return owner;
    }

    inline NetworkWorldBindingSetup Setup(const std::size_t maximumGenericPayloadBytes = 8) {
        const ProtocolIdentityDescriptor protocol{WireIdentity<ProtocolId>(1), {{1, 0}, {1, 0}}};
        const MessageIdentityDescriptor message{protocol.id,
                                                WireIdentity<MessageTypeId>(10),
                                                WireIdentity<MessageSchemaId>(11),
                                                {1, 0},
                                                false};
        const FeatureIdentityDescriptor feature{protocol.id, WireIdentity<ProtocolFeatureId>(1), {1, 0}};
        const std::array messages{message, MessageIdentityDescriptor{protocol.id,
                                                                     WireIdentity<MessageTypeId>(12),
                                                                     WireIdentity<MessageSchemaId>(13),
                                                                     {1, 0},
                                                                     false}};
        const auto identities = ProtocolIdentityRegistry::Create({std::span{&protocol, 1}, messages, std::span{&feature, 1}, {}});
        REQUIRE(identities.HasValue());
        const MessageCodecDescriptor codec{protocol.id, message.id, message.schema, {{1, 0}, {1, 0}}, NetworkWorldBindingWireBytes};
        const std::array allCodecs{codec, MessageCodecDescriptor{protocol.id,
                                                                 messages[1].id,
                                                                 messages[1].schema,
                                                                 {{1, 0}, {1, 0}},
                                                                 maximumGenericPayloadBytes}};
        const auto codecs = MessageCodecRegistry::Create({allCodecs, {}}, identities.Value());
        REQUIRE(codecs.HasValue());
        return {{message, feature.id, Id<NetworkPrincipalId>(20), Id<CredentialProvenanceId>(50), Id<NetworkCapabilityId>(40)},
                std::make_shared<const MessageCodecRegistry>(codecs.Value()),
                42,
                {},
                4,
                50};
    }

    struct RecordingTransport final : INetworkTransport {
        explicit RecordingTransport(const std::uint64_t maximumMessageBytes = 1200) : maximumMessageBytes(maximumMessageBytes) {}

        Result<void> Initialize(const NetworkTransportConfig &) override {
            return Result<void>::Success();
        }

        Result<ListenerHandle> Listen(const NetworkListenRequest &) override {
            return Result<ListenerHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
        }

        Result<ConnectionHandle> Connect(const NetworkConnectRequest &) override {
            return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
        }

        Result<void> Send(ConnectionHandle, ChannelId channel, std::span<const std::byte> bytes, DeliveryPolicy delivery) override {
            if (bytes.size() > sent.size() || channel.Value() != 0 || delivery != DeliveryPolicy::ReliableOrdered)
                return Result<void>::Failure(MakeError(NetworkErrors::TransportLimitExceeded));
            if (backpressure)
                return Result<void>::Failure(MakeError(NetworkErrors::TransportReliableBackpressure));
            std::copy(bytes.begin(), bytes.end(), sent.begin());
            size = bytes.size();
            return Result<void>::Success();
        }

        Result<void> Close(ConnectionHandle) override {
            ++closed;
            return Result<void>::Success();
        }

        Result<void> CloseListener(ListenerHandle) override {
            return Result<void>::Success();
        }

        Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override {
            if (!queued)
                return Result<std::size_t>::Success(0);
            queued = false;
            consumer.Consume(std::move(event));
            return Result<std::size_t>::Success(1);
        }

        NetworkTransportStats Stats() const noexcept override {
            return {};
        }

        Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle) const override {
            return Result<NetworkConnectionStats>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
        }

        TransportCapabilities Capabilities() const noexcept override {
            TransportCapabilities caps;
            caps.revision = 3;
            caps.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = TransportSupport::Available;
            caps.maximumChannels = 2;
            caps.maximumMessageBytes = maximumMessageBytes;
            return caps;
        }

        void Shutdown() noexcept override {
            queued = false;
        }

        std::array<std::byte, 256> sent;
        const std::uint64_t maximumMessageBytes;
        std::size_t size{};
        NetworkTransportEvent event;
        bool queued{};
        bool backpressure{};
        std::size_t closed{};
    };

    struct BindingFixture final {
        explicit BindingFixture(NetworkWorldBindingSetup setup = Setup(), const std::uint64_t maximumMessageBytes = 1200)
            : setup(std::move(setup)), transport(maximumMessageBytes) {}

        ~BindingFixture() {
            if (auto owner = binding.lock())
                owner->Detach(transport);
        }

        void Attach(NetworkModeComposition &mode) {
            AttachOwner(NetworkModeHostAccess::Binding(mode));
        }

        void AttachOwner(std::weak_ptr<NetworkWorldBindingController> canonical) {
            binding = std::move(canonical);
            auto owner = binding.lock();
            REQUIRE(owner);
            auto created = InboundMessageDispatcher::Create(transport, *setup.codecs);
            REQUIRE(created.HasValue());
            dispatcher = std::move(created).Value();
            REQUIRE(owner->Attach(transport, *dispatcher, owner).HasValue());
        }

        void PublishInitial(NetworkModeComposition &mode, const std::shared_ptr<PeerSessionLifecycle> &client) {
            auto owner = binding.lock();
            REQUIRE(owner);
            server = Active(ConnectionHandle::Create(3, 1).Value(), TestSupport::Session(8));
            REQUIRE(owner->BindPeer(AdmissionEndpoint::Server, server, TestSupport::Session(8), 22).HasValue());
            REQUIRE(owner->BindPeer(AdmissionEndpoint::Client, client, TestSupport::Session(), 22).HasValue());
            RegisterClient(client);
            REQUIRE(mode.RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            Deliver();
            REQUIRE(mode.RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
        }

        void RegisterClient(const std::shared_ptr<PeerSessionLifecycle> &client) {
            const std::vector<MessageDeliveryChannel> channels{{{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Control, false},
                                                               {ChannelId::Create(1, 2).Value(), DeliveryPolicy::ReliableOrdered,
                                                                MessageTrafficClass::Command, false}};
            auto gate = MessageDeliveryGate::Create(client->Negotiation()->transport, 3, client->Connection(),
                                                    client->Negotiation()->sessionGeneration, channels);
            REQUIRE(gate.HasValue());
            REQUIRE(
                dispatcher
                    ->RegisterSession(client, client->Connection(), client->Negotiation()->sessionGeneration, std::move(gate).Value(), 22)
                    .HasValue());
            clientConnection = client->Connection();
        }

        void Deliver() {
            auto owner = binding.lock();
            REQUIRE(owner);
            const auto flushed = owner->Flush(22);
            if (flushed.HasError())
                UNSCOPED_INFO("Binding flush domain=" << flushed.ErrorValue().domain.Value()
                                                      << " code=" << flushed.ErrorValue().code.Value());
            REQUIRE(flushed.HasValue());
            REQUIRE(transport.size != 0);
            transport.event = {.kind = NetworkTransportEventKind::PacketReceived,
                               .connection = clientConnection,
                               .channel = {},
                               .delivery = DeliveryPolicy::ReliableOrdered,
                               .payload = std::vector<std::byte>(transport.sent.begin(), transport.sent.begin() + transport.size)};
            transport.queued = true;
            const auto processed = dispatcher->RunNetworkPoll(22);
            REQUIRE(processed.HasValue());
            REQUIRE(processed.Value().rejectedPackets == 0);
        }

        void CommitTravel(NetworkModeComposition &mode) {
            Deliver();  // The actual runtime-issued PREPARE traverses the real delivery gate/handler.
            REQUIRE(mode.RunPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            Deliver();  // The exact reserved COMMIT is idempotent for the same-host published client.
        }

        NetworkWorldBindingSetup setup;
        RecordingTransport transport;
        std::unique_ptr<InboundMessageDispatcher> dispatcher;
        std::weak_ptr<NetworkWorldBindingController> binding;
        std::shared_ptr<PeerSessionLifecycle> server;
        ConnectionHandle clientConnection;
    };
}  // namespace Horo::Network::ModeBindingTests
