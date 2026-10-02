#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/NetworkErrors.h"
#include "NetworkTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Horo::Network {
    using TestSupport::Bytes;
    using TestSupport::Connection;
    using TestSupport::Id;
    using TestSupport::RequireError;
    using TestSupport::Session;
    using TestSupport::WireIdentity;

    namespace {
        class FakeTransport final : public INetworkTransport {
        public:
            std::vector<NetworkTransportEvent> pending;
            std::size_t closed{};
            bool stopped{};
            bool polling{};
            bool failPoll{};
            bool failClose{};

            Result<void> Initialize(const NetworkTransportConfig &) override {
                return Result<void>::Success();
            }

            Result<ListenerHandle> Listen(const NetworkListenRequest &) override {
                return Result<ListenerHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            }

            Result<ConnectionHandle> Connect(const NetworkConnectRequest &) override {
                return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            }

            Result<void> Send(ConnectionHandle, ChannelId, std::span<const std::byte>, DeliveryPolicy) override {
                return Result<void>::Success();
            }

            Result<void> Close(ConnectionHandle) override {
                ++closed;
                if (failClose)
                    return Result<void>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
                return Result<void>::Success();
            }

            Result<void> CloseListener(ListenerHandle) override {
                return Result<void>::Success();
            }

            Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override {
                const auto count = pending.size();
                polling = true;
                for (auto &event : pending)
                    consumer.Consume(std::move(event));
                polling = false;
                pending.clear();
                if (failPoll)
                    return Result<std::size_t>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
                return Result<std::size_t>::Success(count);
            }

            NetworkTransportStats Stats() const noexcept override {
                return {};
            }

            Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle) const override {
                return Result<NetworkConnectionStats>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            }

            TransportCapabilities Capabilities() const noexcept override {
                return {};
            }

            void Shutdown() noexcept override {
                stopped = true;
                pending.clear();
            }
        };

        class CountingHandler final : public IInboundMessageHandler {
        public:
            std::size_t called{};
            const bool *polling{};

            Result<void> Handle(const MessageEnvelope &) override {
                REQUIRE(polling != nullptr);
                REQUIRE_FALSE(*polling);
                ++called;
                return Result<void>::Success();
            }
        };

        class ThrowingHandler final : public IInboundMessageHandler {
        public:
            std::size_t called{};
            bool standardException{};

            [[noreturn]] Result<void> Handle(const MessageEnvelope &) override {
                ++called;
                if (standardException)
                    throw std::invalid_argument{"hostile inbound callback"};
                throw 42;
            }
        };

        struct Fixture final {
            std::array<ProtocolIdentityDescriptor, 1> protocols{
                ProtocolIdentityDescriptor{WireIdentity<ProtocolId>(1), {{1, 0}, {1, 3}}},
            };
            std::array<MessageIdentityDescriptor, 1> messages{
                MessageIdentityDescriptor{protocols[0].id, WireIdentity<MessageTypeId>(2), WireIdentity<MessageSchemaId>(3), {1, 1}, false},
            };
            std::array<MessageCodecDescriptor, 1> codecs{
                MessageCodecDescriptor{protocols[0].id, messages[0].id, messages[0].schema, {{1, 0}, {1, 2}}, 64},
            };
            ProtocolIdentityRegistry identities = ProtocolIdentityRegistry::Create({protocols, messages, {}, {}}).Value();
            MessageCodecRegistry registry = MessageCodecRegistry::Create({codecs, {}}, identities).Value();
            FakeTransport transport;
            std::shared_ptr<CountingHandler> handler = std::make_shared<CountingHandler>();
            std::unique_ptr<InboundMessageDispatcher> router = [](FakeTransport &source, const MessageCodecRegistry &codecs) {
                auto created = InboundMessageDispatcher::Create(source, codecs);
                return std::move(created).Value();
            }(transport, registry);

            TransportSelectionEvidence Selection() const {
                TransportSelectionEvidence selection;
                selection.capabilityRevision = 3;
                selection.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
                selection.channelCount = 1;
                selection.maximumMessageBytes = 1200;
                return selection;
            }

            MessageDeliveryGate Gate(const ConnectionHandle connection = Connection(),
                                     const NetworkOperationGeneration generation = Session()) const {
                auto created =
                    MessageDeliveryGate::Create(Selection(), 3, connection, generation,
                                                {{ChannelId{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Command, false}});
                return std::move(created).Value();
            }

            std::shared_ptr<PeerSessionLifecycle> ActiveSession(const ConnectionHandle connection = Connection(),
                                                                const NetworkOperationGeneration generation = Session(),
                                                                const ProtocolId negotiatedProtocol = {}) const {
                auto created = PeerSessionLifecycle::Create(connection, generation, {10, 20, 30, 100, 10});
                auto session = std::make_shared<PeerSessionLifecycle>(std::move(created).Value());
                REQUIRE(session->BeginNegotiation(connection, generation, 1).HasValue());
                HandshakeSelection negotiation;
                negotiation.connection = connection;
                negotiation.sessionGeneration = generation;
                negotiation.protocol = negotiatedProtocol.IsValid() ? negotiatedProtocol : protocols[0].id;
                negotiation.version = {1, 2};
                negotiation.schemaFingerprint = 42;
                negotiation.features.values[0] = WireIdentity<ProtocolFeatureId>(1);
                negotiation.features.count = 1;
                negotiation.compression = HandshakeCompression::None;
                negotiation.transport = Selection();
                REQUIRE(session->AcceptNegotiation(negotiation, 2).HasValue());
                AuthenticationResult auth;
                auth.connection = connection;
                auth.sessionGeneration = generation;
                auth.policy = Id<NetworkTrustPolicyId>(10);
                auth.policyRevision = 4;
                auth.principal.principal = Id<NetworkPrincipalId>(20);
                auth.principal.session.bytes = Bytes<NetworkSessionIdBytes>(0x80);
                auth.principal.trustLevel = NetworkTrustLevel::ProductAnchor;
                auth.principal.roles.values[0] = Id<NetworkRoleId>(30);
                auth.principal.roles.count = 1;
                auth.principal.capabilities.values[0] = Id<NetworkCapabilityId>(40);
                auth.principal.capabilities.count = 1;
                auth.principal.provenance = Id<CredentialProvenanceId>(50);
                auth.principal.expiresAtTick = 150;
                auth.secureChannel.stamp = {connection, generation, 5};
                auth.secureChannel.binding = Id<PrivateKeyBindingId>(11);
                auth.secureChannel.channel = Id<SecureChannelId>(12);
                auth.secureChannel.channelGeneration = 6;
                auth.secureChannel.bindingDigest = Bytes<AuthenticationDigestBytes>(0x20);
                REQUIRE(session->AcceptAuthentication(auth, 11).HasValue());
                REQUIRE(session->Activate({connection, generation, auth.secureChannel.channel, 6, auth.secureChannel.bindingDigest}, 21)
                            .HasValue());
                return session;
            }

            void Register(const std::uint32_t maximumPerTick = 4, const CancellationToken &parent = {}) {
                handler->polling = &transport.polling;
                REQUIRE(router
                            ->RegisterHandler({protocols[0].id, messages[0].id, MessageTrafficClass::Command,
                                               Runtime::RuntimePhase::NetworkPoll, maximumPerTick},
                                              handler)
                            .HasValue());
                REQUIRE(router->RegisterSession(ActiveSession(), Connection(), Session(), Gate(), 22, parent).HasValue());
            }

            void Enqueue(const std::uint32_t sequence, const ConnectionHandle connection = Connection()) {
                MessageEnvelope envelope;
                envelope.protocol = protocols[0].id;
                envelope.message = messages[0].id;
                envelope.schema = messages[0].schema;
                envelope.schemaVersion = {1, 1};
                envelope.sequence = MessageSequenceNumber{sequence};
                auto encoded = EncodeMessageEnvelope(envelope, registry);
                REQUIRE(encoded.HasValue());
                transport.pending.push_back({.kind = NetworkTransportEventKind::PacketReceived,
                                             .connection = connection,
                                             .channel = ChannelId{},
                                             .delivery = DeliveryPolicy::ReliableOrdered,
                                             .payload = std::move(encoded).Value()});
            }
        };

        void RequirePacketRejection(const Result<InboundDispatchReport> &polled, const ErrorCodeDescriptor &expected) {
            REQUIRE(polled.HasValue());
            REQUIRE(polled.Value().rejectedPackets == 1);
            REQUIRE(polled.Value().lastPacketRejection.has_value());
            REQUIRE(polled.Value().lastPacketRejection->code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Inbound dispatch invokes typed owner only after active session decode and replay admission", "[unit][network][dispatch]") {
        Fixture fixture;
        fixture.Register();
        fixture.Enqueue(1);
        REQUIRE(fixture.router->RunNetworkPoll(23).Value().processedEvents == 1);
        REQUIRE(fixture.handler->called == 1);
        fixture.Enqueue(1);
        RequirePacketRejection(fixture.router->RunNetworkPoll(24), NetworkErrors::MessageDeliveryDuplicate);
        REQUIRE(fixture.handler->called == 1);
        fixture.Enqueue(2);
        REQUIRE(fixture.router->RunNetworkPoll(25).HasValue());
        REQUIRE(fixture.handler->called == 2);
        fixture.Enqueue(2);
        fixture.Enqueue(3);
        auto mixed = fixture.router->RunNetworkPoll(26);
        REQUIRE(mixed.HasValue());
        REQUIRE(mixed.Value().processedEvents == 2);
        REQUIRE(mixed.Value().rejectedPackets == 1);
        REQUIRE(fixture.handler->called == 3);
    }

    TEST_CASE("Inbound dispatch converts standard and nonstandard handler throws without retrying consumed replay state",
              "[unit][network][dispatch]") {
        for (const bool standardException : {false, true}) {
            Fixture fixture;
            auto throwing = std::make_shared<ThrowingHandler>();
            throwing->standardException = standardException;
            REQUIRE(fixture.router
                        ->RegisterHandler({fixture.protocols[0].id, fixture.messages[0].id, MessageTrafficClass::Command,
                                           Runtime::RuntimePhase::NetworkPoll, 4},
                                          throwing)
                        .HasValue());
            REQUIRE(fixture.router->RegisterSession(fixture.ActiveSession(), Connection(), Session(), fixture.Gate(), 22).HasValue());
            fixture.Enqueue(1);
            RequirePacketRejection(fixture.router->RunNetworkPoll(23), NetworkErrors::GameplayDispatchRejected);
            REQUIRE(throwing->called == 1);
            fixture.Enqueue(1);
            RequirePacketRejection(fixture.router->RunNetworkPoll(24), NetworkErrors::MessageDeliveryDuplicate);
            REQUIRE(throwing->called == 1);
        }
    }

    TEST_CASE("Inbound dispatch rejects absent revoked malformed and expired sessions before handlers", "[unit][network][dispatch]") {
        Fixture fixture;
        fixture.Enqueue(1);
        RequirePacketRejection(fixture.router->RunNetworkPoll(23), NetworkErrors::GameplayDispatchRejected);
        REQUIRE(fixture.transport.closed == 1);
        fixture.Register();
        fixture.transport.pending.push_back({.kind = NetworkTransportEventKind::PacketReceived,
                                             .connection = Connection(),
                                             .delivery = DeliveryPolicy::ReliableOrdered,
                                             .payload = {std::byte{0xff}}});
        RequirePacketRejection(fixture.router->RunNetworkPoll(24), NetworkErrors::MessageEnvelopeInvalid);
        REQUIRE(fixture.handler->called == 0);
        fixture.Enqueue(1);
        RequirePacketRejection(fixture.router->RunNetworkPoll(25), NetworkErrors::GameplayDispatchRejected);
        REQUIRE(fixture.handler->called == 0);
    }

    TEST_CASE("Inbound dispatch refuses inactive expired incompatible and revoked generations", "[unit][network][dispatch]") {
        Fixture fixture;
        auto inactive = PeerSessionLifecycle::Create(Connection(), Session(), {10, 20, 30, 100, 10});
        RequireError(fixture.router->RegisterSession(std::make_shared<PeerSessionLifecycle>(std::move(inactive).Value()), Connection(),
                                                     Session(), fixture.Gate(), 1),
                     NetworkErrors::GameplayDispatchRejected);
        auto mismatchedSelection = fixture.Selection();
        mismatchedSelection.maximumMessageBytes = 1000;
        auto mismatchedGate =
            MessageDeliveryGate::Create(mismatchedSelection, 3, Connection(), Session(),
                                        {{ChannelId{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Command, false}});
        REQUIRE(mismatchedGate.HasValue());
        RequireError(fixture.router->RegisterSession(fixture.ActiveSession(), Connection(), Session(), std::move(mismatchedGate).Value(),
                                                     22),
                     NetworkErrors::MessageDeliveryInvalid);
        fixture.Register();
        fixture.Enqueue(1);
        fixture.transport.pending.back().delivery = DeliveryPolicy::UnreliableUnordered;
        RequirePacketRejection(fixture.router->RunNetworkPoll(23), NetworkErrors::MessageDeliveryInvalid);
        REQUIRE(fixture.handler->called == 0);
        fixture.Enqueue(1);
        RequirePacketRejection(fixture.router->RunNetworkPoll(31), NetworkErrors::SessionTimedOut);
        fixture.Enqueue(1);
        RequirePacketRejection(fixture.router->RunNetworkPoll(32), NetworkErrors::GameplayDispatchRejected);
        REQUIRE(fixture.handler->called == 0);
    }

    TEST_CASE("Inbound dispatch rejects a valid codec from another negotiated protocol", "[unit][network][dispatch]") {
        Fixture fixture;
        fixture.handler->polling = &fixture.transport.polling;
        REQUIRE(fixture.router
                    ->RegisterHandler({fixture.protocols[0].id, fixture.messages[0].id, MessageTrafficClass::Command,
                                       Runtime::RuntimePhase::NetworkPoll, 4},
                                      fixture.handler)
                    .HasValue());
        REQUIRE(fixture.router
                    ->RegisterSession(fixture.ActiveSession(Connection(), Session(), WireIdentity<ProtocolId>(99)), Connection(), Session(),
                                      fixture.Gate(), 22)
                    .HasValue());
        fixture.Enqueue(1);
        RequirePacketRejection(fixture.router->RunNetworkPoll(23), NetworkErrors::GameplayDispatchRejected);
        REQUIRE(fixture.handler->called == 0);
        REQUIRE(fixture.transport.closed == 1);
    }

    TEST_CASE("Inbound dispatch loses a destroyed handler owner and rejects cancelled work", "[unit][network][dispatch]") {
        Fixture fixture;
        fixture.Register();
        CancellationSource source;
        fixture.Enqueue(1);
        source.RequestCancellation();
        RequireError(fixture.router->RunNetworkPoll(23, source.Token()), NetworkErrors::SessionCancelled);
        REQUIRE(fixture.handler->called == 0);
        fixture.handler.reset();
        // A dead owner cannot be called, even though its descriptor is still registered.
        RequirePacketRejection(fixture.router->RunNetworkPoll(23), NetworkErrors::GameplayDispatchRejected);
        REQUIRE(fixture.transport.closed == 1);
    }

    TEST_CASE("Inbound dispatch honors parent cancellation ancestry after admission", "[unit][network][dispatch]") {
        Fixture fixture;
        CancellationSource parent;
        fixture.Register(4, parent.Token());
        fixture.Enqueue(1);
        parent.RequestCancellation();
        RequirePacketRejection(fixture.router->RunNetworkPoll(23), NetworkErrors::SessionCancelled);
        REQUIRE(fixture.handler->called == 0);
        REQUIRE(fixture.transport.closed == 1);
    }

    TEST_CASE("Inbound dispatch rejects revoked handlers and fails closed on queue overflow", "[unit][network][dispatch]") {
        Fixture revoked;
        revoked.Register();
        revoked.router->RevokeHandler(revoked.protocols[0].id, revoked.messages[0].id);
        revoked.Enqueue(1);
        RequirePacketRejection(revoked.router->RunNetworkPoll(23), NetworkErrors::GameplayDispatchRejected);
        REQUIRE(revoked.handler->called == 0);
        REQUIRE(revoked.transport.closed == 1);

        Fixture saturated;
        auto limited = InboundMessageDispatcher::Create(saturated.transport, saturated.registry,
                                                        {.maximumSessions = 1,
                                                         .maximumHandlers = 1,
                                                         .maximumQueuedPackets = 1,
                                                         .maximumPacketsPerPoll = 1});
        REQUIRE(limited.HasValue());
        saturated.router = std::move(limited).Value();
        saturated.Register();
        saturated.Enqueue(1);
        saturated.Enqueue(2);
        RequireError(saturated.router->RunNetworkPoll(23), NetworkErrors::NetworkIoCompletionQueueFull);
        REQUIRE(saturated.transport.stopped);
        REQUIRE(saturated.handler->called == 0);
    }

    TEST_CASE("Inbound dispatch revokes slow or destroyed owners and closes on finite backpressure", "[unit][network][dispatch]") {
        Fixture fixture;
        fixture.Register(1);
        fixture.Enqueue(1);
        REQUIRE(fixture.router->RunNetworkPoll(23).HasValue());
        fixture.Enqueue(2);
        RequirePacketRejection(fixture.router->RunNetworkPoll(23), NetworkErrors::TransportReliableBackpressure);
        REQUIRE(fixture.transport.closed == 1);
        REQUIRE(fixture.handler->called == 1);
        fixture.router->Shutdown();
        fixture.Enqueue(3);
        RequireError(fixture.router->RunNetworkPoll(24), NetworkErrors::SessionShuttingDown);
        REQUIRE(fixture.handler->called == 1);
    }

    TEST_CASE("Inbound dispatch carries bounded queued packets into the next owner poll", "[unit][network][dispatch]") {
        Fixture fixture;
        auto limited = InboundMessageDispatcher::Create(fixture.transport, fixture.registry,
                                                        {.maximumSessions = 1,
                                                         .maximumHandlers = 1,
                                                         .maximumQueuedPackets = 2,
                                                         .maximumPacketsPerPoll = 1});
        REQUIRE(limited.HasValue());
        fixture.router = std::move(limited).Value();
        fixture.Register();
        fixture.Enqueue(1);
        fixture.Enqueue(2);
        const auto first = fixture.router->RunNetworkPoll(23);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().processedEvents == 1);
        REQUIRE(fixture.handler->called == 1);
        const auto second = fixture.router->RunNetworkPoll(24);
        REQUIRE(second.HasValue());
        REQUIRE(second.Value().processedEvents == 1);
        REQUIRE(fixture.handler->called == 2);
    }

    TEST_CASE("Inbound dispatch discards a partial failed transport poll before any gameplay callback", "[unit][network][dispatch]") {
        Fixture fixture;
        fixture.Register();
        fixture.Enqueue(1);
        fixture.transport.failPoll = true;
        RequireError(fixture.router->RunNetworkPoll(23), NetworkErrors::TransportNativeUnavailable);
        REQUIRE(fixture.transport.stopped);
        REQUIRE(fixture.handler->called == 0);
        RequireError(fixture.router->RunNetworkPoll(24), NetworkErrors::SessionShuttingDown);
    }

    TEST_CASE("Inbound dispatch preserves later owned packets when a nonpacket event fails", "[unit][network][dispatch]") {
        Fixture fixture;
        fixture.Register();
        fixture.Enqueue(1);
        fixture.transport.pending.insert(fixture.transport.pending.begin(),
                                         {.kind = NetworkTransportEventKind::Accepted, .connection = Connection(4)});
        fixture.transport.failClose = true;
        RequireError(fixture.router->RunNetworkPoll(23), NetworkErrors::TransportNativeUnavailable);
        REQUIRE(fixture.handler->called == 0);
        fixture.transport.failClose = false;
        REQUIRE(fixture.router->RunNetworkPoll(24).HasValue());
        REQUIRE(fixture.handler->called == 1);
    }

    TEST_CASE("A hostile session cannot exhaust another session's typed handler rate", "[unit][network][dispatch]") {
        Fixture fixture;
        fixture.Register(1);
        const auto otherConnection = Connection(4);
        const auto otherGeneration = Session(8);
        REQUIRE(fixture.router
                    ->RegisterSession(fixture.ActiveSession(otherConnection, otherGeneration), otherConnection, otherGeneration,
                                      fixture.Gate(otherConnection, otherGeneration), 22)
                    .HasValue());
        fixture.Enqueue(1);
        REQUIRE(fixture.router->RunNetworkPoll(23).HasValue());
        fixture.Enqueue(1, otherConnection);
        REQUIRE(fixture.router->RunNetworkPoll(23).HasValue());
        REQUIRE(fixture.handler->called == 2);
        fixture.Enqueue(2);
        RequirePacketRejection(fixture.router->RunNetworkPoll(23), NetworkErrors::TransportReliableBackpressure);
        REQUIRE(fixture.transport.closed == 1);
        fixture.Enqueue(2, otherConnection);
        REQUIRE(fixture.router->RunNetworkPoll(24).HasValue());
        REQUIRE(fixture.handler->called == 3);
    }
}  // namespace Horo::Network
