#pragma once
/** @file NetworkModeSessionIngressTestSupport.h @brief Real dispatcher ingress driven through the canonical product lifecycle. */
#include "Horo/Network/InboundMessageDispatcher.h"
#include "NetworkModeHostTestSupport.h"

namespace Horo::Network {
    namespace {
        /** @brief Finite owned transport events; callbacks are staged by the real dispatcher before handlers execute. */
        class SessionTestTransport final : public INetworkTransport {
        public:
            std::array<NetworkTransportEvent, 4> pending{};
            std::size_t count{};
            std::size_t polls{};
            std::size_t closes{};
            bool polling{};
            bool stopped{};
            ConnectionHandle acceptedConnect{};  // Explicit opt-in pending-attempt fixture; never grants ready/authentication.
            std::function<void()> duringPoll;

            Result<void> Initialize(const NetworkTransportConfig &) override {
                return Result<void>::Success();
            }

            Result<ListenerHandle> Listen(const NetworkListenRequest &) override {
                return Result<ListenerHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            }

            Result<ConnectionHandle> Connect(const NetworkConnectRequest &) override {
                if (acceptedConnect.IsValid())
                    return Result<ConnectionHandle>::Success(acceptedConnect);
                return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            }

            Result<void> Send(ConnectionHandle, ChannelId, std::span<const std::byte>, DeliveryPolicy) override {
                return Result<void>::Success();
            }

            Result<void> Close(ConnectionHandle) override {
                ++closes;
                return Result<void>::Success();
            }

            Result<void> CloseListener(ListenerHandle) override {
                return Result<void>::Success();
            }

            Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override {
                ++polls;
                polling = true;
                if (auto action = std::exchange(duringPoll, {}))
                    action();
                const auto delivered = count;
                for (std::size_t index = 0; index < count; ++index)
                    consumer.Consume(std::move(pending[index]));
                count = 0;
                polling = false;
                return Result<std::size_t>::Success(delivered);
            }

            NetworkTransportStats Stats() const noexcept override {
                return {};
            }

            Result<NetworkConnectionStats> ConnectionStats(ConnectionHandle) const override {
                return Result<NetworkConnectionStats>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            }

            TransportCapabilities Capabilities() const noexcept override {
                TransportCapabilities capabilities;
                capabilities.revision = 3;
                capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = TransportSupport::Available;
                capabilities.maximumChannels = 2;
                capabilities.maximumMessageBytes = 1200;
                return capabilities;
            }

            void Shutdown() noexcept override {
                stopped = true;
                count = 0;
            }

            void Queue(NetworkTransportEvent event) {
                REQUIRE(count < pending.size());
                pending[count++] = std::move(event);
            }
        };

        class SessionPacketHandler final : public IInboundMessageHandler {
        public:
            std::size_t calls{};
            const bool *polling{};

            Result<void> Handle(const MessageEnvelope &) override {
                REQUIRE(polling != nullptr);
                REQUIRE_FALSE(*polling);
                ++calls;
                return Result<void>::Success();
            }
        };

        struct SessionIngress final {
            std::array<ProtocolIdentityDescriptor, 1> protocols{ProtocolIdentityDescriptor{WireIdentity<ProtocolId>(1), {{1, 0}, {1, 3}}}};
            std::array<MessageIdentityDescriptor, 2> messages{MessageIdentityDescriptor{protocols[0].id,
                                                                                        WireIdentity<MessageTypeId>(2),
                                                                                        WireIdentity<MessageSchemaId>(3),
                                                                                        {1, 1},
                                                                                        false},
                                                              MessageIdentityDescriptor{protocols[0].id,
                                                                                        WireIdentity<MessageTypeId>(10),
                                                                                        WireIdentity<MessageSchemaId>(11),
                                                                                        {1, 0},
                                                                                        false}};
            std::array<MessageCodecDescriptor, 2>
                codecs{MessageCodecDescriptor{protocols[0].id, messages[0].id, messages[0].schema, {{1, 0}, {1, 2}}, 64},
                       MessageCodecDescriptor{protocols[0].id,
                                              messages[1].id,
                                              messages[1].schema,
                                              {{1, 0}, {1, 0}},
                                              NetworkWorldBindingWireBytes}};
            std::array<FeatureIdentityDescriptor, 1> features{
                FeatureIdentityDescriptor{protocols[0].id, WireIdentity<ProtocolFeatureId>(1), {1, 0}}};
            ProtocolIdentityRegistry identities = ProtocolIdentityRegistry::Create({protocols, messages, features, {}}).Value();
            std::shared_ptr<const MessageCodecRegistry> registry =
                std::make_shared<const MessageCodecRegistry>(MessageCodecRegistry::Create({codecs, {}}, identities).Value());
            NetworkWorldBindingSetup setup{{messages[1], features[0].id, Id<NetworkPrincipalId>(20), Id<CredentialProvenanceId>(50),
                                            Id<NetworkCapabilityId>(40)},
                                           registry,
                                           42,
                                           {},
                                           4,
                                           50};
            SessionTestTransport transport;
            std::shared_ptr<SessionPacketHandler> handler = std::make_shared<SessionPacketHandler>();
            std::unique_ptr<InboundMessageDispatcher> router;
            std::shared_ptr<Application::Internal::ClientSessionOwner> sessions;
            std::optional<InboundDispatchReport> report;
            std::uint64_t observedTick{};
            bool failAfterAttach{};

            explicit SessionIngress(std::shared_ptr<Application::Internal::ClientSessionOwner> owner) : sessions(std::move(owner)) {}

            Result<void> Prepare() {
                auto created = InboundMessageDispatcher::Create(transport, *registry);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                router = std::move(created).Value();
                auto attached = sessions->AttachTransport(transport);
                if (attached.HasError())
                    return attached;
                router->SetSessionHost(sessions);
                if (auto binding = sessions->WorldBinding().lock()) {
                    auto bound = binding->Attach(transport, *router, binding);
                    if (bound.HasError())
                        return bound;
                }
                if (failAfterAttach)
                    return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
                handler->polling = &transport.polling;
                return router->RegisterHandler({protocols[0].id, messages[0].id, MessageTrafficClass::Command,
                                                Runtime::RuntimePhase::NetworkPoll, 4},
                                               handler);
            }

            void Register(const std::shared_ptr<PeerSessionLifecycle> &peer) {
                REQUIRE(peer->Negotiation() != nullptr);
                auto gate =
                    MessageDeliveryGate::Create(peer->Negotiation()->transport, 3, peer->Connection(),
                                                peer->Negotiation()->sessionGeneration,
                                                {{ChannelId{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Control, false},
                                                 {ChannelId::Create(1, 2).Value(), DeliveryPolicy::ReliableOrdered,
                                                  MessageTrafficClass::Command, false}});
                REQUIRE(gate.HasValue());
                REQUIRE(
                    router->RegisterSession(peer, peer->Connection(), peer->Negotiation()->sessionGeneration, std::move(gate).Value(), 22)
                        .HasValue());
            }

            void Packet(const std::uint32_t sequence, const ConnectionHandle connection = Connection()) {
                MessageEnvelope envelope;
                envelope.protocol = protocols[0].id;
                envelope.message = messages[0].id;
                envelope.schema = messages[0].schema;
                envelope.schemaVersion = {1, 1};
                envelope.sequence = MessageSequenceNumber{sequence};
                auto encoded = EncodeMessageEnvelope(envelope, *registry);
                REQUIRE(encoded.HasValue());
                transport.Queue({.kind = NetworkTransportEventKind::PacketReceived,
                                 .connection = connection,
                                 .channel = ChannelId::Create(1, 2).Value(),
                                 .delivery = DeliveryPolicy::ReliableOrdered,
                                 .payload = std::move(encoded).Value()});
            }

            Result<void> Poll() {
                auto now = sessions->PollTick();
                if (now.HasError())
                    return Result<void>::Failure(now.ErrorValue());
                observedTick = now.Value();
                auto polled = router->RunNetworkPoll(observedTick);
                if (polled.HasError())
                    return Result<void>::Failure(polled.ErrorValue());
                report = polled.Value();
                return Result<void>::Success();
            }

            void Shutdown() noexcept {
                if (auto binding = sessions->WorldBinding().lock())
                    binding->Detach(transport);
                if (router)
                    router->Shutdown();
                sessions->DetachTransport(transport);
                transport.Shutdown();
            }
        };

        class SessionIngressService final : public INetworkModeService {
        public:
            explicit SessionIngressService(std::shared_ptr<SessionIngress> ingress) : ingress_(std::move(ingress)) {}

            Result<void> Prepare() override {
                return ingress_->Prepare();
            }

            Result<void> Activate() override {
                if (!ingress_->transport.acceptedConnect.IsValid())
                    return Result<void>::Success();
                auto connected = ingress_->transport.Connect({});
                if (connected.HasError())
                    return Result<void>::Failure(connected.ErrorValue());
                return ingress_->sessions->BeginConnectionAttempt(connected.Value());
            }

            Result<void> RunPhase(const Runtime::RuntimePhase phase) override {
                return phase == Runtime::RuntimePhase::NetworkPoll ? ingress_->Poll() : Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                ingress_->Shutdown();
            }

        private:
            std::shared_ptr<SessionIngress> ingress_;
        };

        inline std::shared_ptr<SessionIngress> InstallIngress(const std::shared_ptr<ProductOwners> &owners) {
            auto ingress = std::make_shared<SessionIngress>(owners->sessions);
            owners->worldSetup = ingress->setup;
            owners->transportOverride = [ingress](const NetworkModeServiceRequest &) {
                return Result<std::unique_ptr<INetworkModeService>>::Success(std::make_unique<SessionIngressService>(ingress));
            };
            return ingress;
        }

        /** @brief Real client Scene/Physics plus the real protocol dispatcher with one exact shared lifecycle. */
        struct SessionProduct final {
            DeterministicClock runtimeClock;
            std::shared_ptr<ProductOwners> owners = std::make_shared<ProductOwners>();
            std::shared_ptr<SessionIngress> ingress = InstallIngress(owners);
            std::array<NetworkModeWorld, 1> worlds{World(NetworkModeWorldKind::Client, 2)};
            std::unique_ptr<Application::Internal::NetworkProductHost> host;

            SessionProduct() {
                auto created = Product(runtimeClock, NetworkProjectRole::Client, worlds, {.localPlayer = true}, owners);
                REQUIRE(created.HasValue());
                host = std::move(created).Value();
            }

            void Start(const std::shared_ptr<PeerSessionLifecycle> &peer) {
                const auto started = host->Startup();
                if (started.HasError()) {
                    INFO("Startup " << started.ErrorValue().domain.Value() << ":" << started.ErrorValue().code.Value() << " "
                                    << started.ErrorValue().message);
                    REQUIRE(started.HasValue());
                }
                REQUIRE(started.HasValue());
                REQUIRE(
                    host->AdmitClientSession(NetworkModeWorldKind::Client, peer, peer->Connection(), peer->Negotiation()->sessionGeneration)
                        .HasValue());
                ingress->Register(peer);
                PublishBinding(peer);
            }

            void PublishBinding(const std::shared_ptr<PeerSessionLifecycle> &peer, const bool replacement = false) {
                PrepareRemotePublisher(owners);
                if (replacement) {
                    auto &server = owners->remoteWire->server;
                    const auto oldConnection = server->Connection();
                    const auto oldGeneration = server->Negotiation()->sessionGeneration;
                    REQUIRE(server->Cancel(oldConnection, oldGeneration, 22).HasValue());
                    REQUIRE(owners->remotePublisher->Reconcile(22).HasValue());
                    const auto nextGeneration = NetworkOperationGeneration::Create(oldGeneration.Value() + 1).Value();
                    server = AdmittedSession(oldConnection.NextGeneration().Value(), nextGeneration);
                    REQUIRE(owners->remotePublisher->BindPeer(AdmissionEndpoint::Server, server, nextGeneration, 22).HasValue());
                }
                REQUIRE(owners->remotePublisher->Flush(22).HasValue());
                const auto &wire = owners->remoteWire->transport;
                ingress->transport.Queue({.kind = NetworkTransportEventKind::PacketReceived,
                                          .connection = peer->Connection(),
                                          .channel = {},
                                          .delivery = DeliveryPolicy::ReliableOrdered,
                                          .payload = std::vector<std::byte>(wire.sent.begin(), wire.sent.begin() + wire.size)});
                REQUIRE(host->RunFrame().HasValue());
                REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[0].scene).HasValue());
            }
        };

        /** @brief Retire the entire process composition before another product acquires canonical Physics globals. */
        inline void RequireIndependentProductAfterRetirement(SessionProduct &retired) {
            {
                SessionProduct overlapping;
                const auto rejected = overlapping.host->Startup();
                REQUIRE(rejected.HasError());
                INFO("Overlapping process owner " << rejected.ErrorValue().domain.Value() << ":" << rejected.ErrorValue().code.Value()
                                                  << " " << rejected.ErrorValue().message);
                CHECK(rejected.ErrorValue().domain.Value() == Physics::PhysicsErrors::InvalidState.domain.Value());
                CHECK(rejected.ErrorValue().code.Value() == Physics::PhysicsErrors::InvalidState.code.Value());
                CHECK(rejected.ErrorValue().message == "Jolt process globals are already owned by another runtime.");
            }
            // World shutdown retains the factory-owned process runtime until this composition is destroyed.
            retired.host.reset();
            SessionProduct independent;
            independent.Start(AdmittedSession());
            CHECK(independent.host->RunFrame().HasValue());
            CHECK_FALSE(independent.host->TakeTravelFailure().has_value());
        }

        inline void RequireRevokedTravel(SessionProduct &product, const std::shared_ptr<PeerSessionLifecycle> &peer,
                                         const PeerSessionTerminalKind expected) {
            REQUIRE(product.host->RunFrame().HasValue());
            REQUIRE(peer->Terminal() != nullptr);
            CHECK(peer->Terminal()->kind == expected);
            CHECK(product.host->ClientSessionTerminal() == peer->Terminal());
            REQUIRE(product.host->TakeTravelFailure().has_value());
            CHECK(product.host->Role(NetworkModeWorldKind::Client, product.worlds[0].scene).HasError());
            CHECK(product.host->Role(NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{202}).HasError());
        }

        inline void QueueSessionTravel(SessionProduct &product) {
            const std::array replacement{World(NetworkModeWorldKind::Client, 202)};
            REQUIRE(product.host->RequestTravel(replacement, Session()).HasValue());
        }
    }  // namespace
}  // namespace Horo::Network
