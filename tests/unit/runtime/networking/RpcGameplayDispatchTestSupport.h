#pragma once

// Shared RPC admission, world and callback fixtures; target-private test support.
#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/RpcGameplayDispatch.h"
#include "Horo/Runtime/FrameScheduler.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "NetworkTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Horo::Network {
    namespace RpcDispatchTestSupport {
        template <typename Integer> void Append(std::vector<std::byte> &bytes, Integer value) {
            for (std::size_t index = 0; index < sizeof(Integer); ++index)
                bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
        }

        // Test adapter only: normalization retains the production deterministic impairment and copy ownership.
        class ImpairedTransport final : public INetworkTransport {
        public:
            static DeterministicTransport Make(const bool loss) {
                DeterministicTransportDescriptor descriptor{.mode = DeterministicTransportMode::Simulated,
                                                            .maximumScheduledDeliveries = 16,
                                                            .maximumPayloadBytes = 1024,
                                                            .maximumChannels = 1,
                                                            .budgetCapacity = {4, 16, 16384},
                                                            .budgetPolicy = {.contractVersion = 1,
                                                                             .revision = 1,
                                                                             .maximumActiveConnections = 4,
                                                                             .maximumQueuedMessages = 16,
                                                                             .maximumQueuedBytes = 16384,
                                                                             .maximumQueuedMessagesPerConnection = 16,
                                                                             .maximumQueuedBytesPerConnection = 16384,
                                                                             .maximumMessagesPerTick = 16,
                                                                             .maximumBytesPerTick = 16384,
                                                                             .maximumMessagesPerConnectionPerTick = 16,
                                                                             .maximumBytesPerConnectionPerTick = 16384,
                                                                             .saturationGraceTicks = 2},
                                                            .scenario = {.contractVersion = 1,
                                                                         .revision = 1,
                                                                         .seed = 123,
                                                                         .latencyTicks = 1,
                                                                         .lossPerTenThousand = static_cast<std::uint16_t>(loss ? 10000 : 0),
                                                                         .duplicatePerTenThousand = 10000,
                                                                         .maximumFragmentBytes = 1024}};
                auto created = DeterministicTransport::Create(descriptor);
                REQUIRE(created.HasValue());
                return std::move(created).Value();
            }

            DeterministicTransport transport;
            std::uint64_t tick{21};

            explicit ImpairedTransport(const bool loss) : transport(Make(loss)) {
                REQUIRE(transport.Open(TestSupport::Connection()).HasValue());
                std::array<DeterministicTransportEvent, 1> empty;
                REQUIRE(transport.Advance(tick, empty).Value() == 0);
            }

            Result<void> Initialize(const NetworkTransportConfig &) override {
                return Result<void>::Success();
            }

            Result<ListenerHandle> Listen(const NetworkListenRequest &) override {
                return Result<ListenerHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            }

            Result<ConnectionHandle> Connect(const NetworkConnectRequest &) override {
                return Result<ConnectionHandle>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
            }

            Result<void> Send(ConnectionHandle connection, ChannelId channel, std::span<const std::byte> payload, DeliveryPolicy) override {
                const auto sent = transport.Send(connection, channel, TransportTrafficClass::Reliable, 0, payload);
                return sent.HasError() ? Result<void>::Failure(sent.ErrorValue()) : Result<void>::Success();
            }

            Result<void> Close(ConnectionHandle connection) override {
                const auto closed = transport.Close(connection);
                return closed.HasError() ? Result<void>::Failure(closed.ErrorValue()) : Result<void>::Success();
            }

            Result<void> CloseListener(ListenerHandle) override {
                return Result<void>::Success();
            }

            Result<std::size_t> PollEvents(INetworkTransportEventConsumer &consumer) override {
                std::array<DeterministicTransportEvent, 16> events;
                auto advanced = transport.Advance(++tick, events);
                if (advanced.HasError())
                    return advanced;
                for (std::size_t index = 0; index < advanced.Value(); ++index) {
                    const auto &event = events[index];
                    consumer.Consume({.kind = event.kind == DeterministicTransportEventKind::Packet
                                                  ? NetworkTransportEventKind::PacketReceived
                                                  : NetworkTransportEventKind::Closed,
                                      .connection = event.connection,
                                      .channel = event.channel,
                                      .delivery = DeliveryPolicy::ReliableOrdered,
                                      .payload = {event.payload.begin(), event.payload.end()}});
                }
                return advanced;
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
                static_cast<void>(transport.Shutdown());
            }
        };

        class Handler final : public IRpcGameplayHandler {
        public:
            enum class Action {
                None,
                Shutdown,
                RevokePeer,
                RevokeObject,
                RevokeHandler,
                NestedDrain
            };
            std::size_t calls{};
            Runtime::EntityRef lastEntity;
            std::weak_ptr<RpcGameplayDispatch> dispatch;
            ConnectionHandle connection;
            RpcId rpc;
            Action action{Action::None};
            bool nestedRejected{};
            bool expectsParameter{};
            bool fail{};
            bool throwFailure{};
            int committed{};
            std::function<void()> duringExecute;
            std::function<void()> duringDestruction;

            Handler() = default;
            Handler(const Handler &) = delete;
            Handler &operator=(const Handler &) = delete;
            Handler(Handler &&) = delete;
            Handler &operator=(Handler &&) = delete;

            ~Handler() override {
                if (duringDestruction)
                    duringDestruction();
            }

            Result<void> Execute(const RpcGameplayContext &context, std::span<const ReplicationRuntimeValue> parameters) override {
                if (expectsParameter) {
                    REQUIRE(parameters.size() == 1);
                    REQUIRE(std::get<std::int64_t>(parameters.front()) == 7);
                } else {
                    REQUIRE(parameters.empty());
                }
                ++calls;
                lastEntity = context.entity;
                if (duringExecute)
                    duringExecute();
                const int candidate = committed + 1;
                if (throwFailure)
                    throw 42;
                if (fail)
                    return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
                if (auto owner = dispatch.lock()) {
                    using enum Action;
                    switch (action) {
                        case Shutdown:
                            owner->Shutdown();
                            break;
                        case RevokePeer:
                            owner->RevokePeer(connection);
                            break;
                        case RevokeObject:
                            owner->RevokeObject(context.object);
                            break;
                        case RevokeHandler:
                            owner->RevokeHandler(rpc);
                            break;
                        case NestedDrain:
                            nestedRejected =
                                owner
                                    ->DrainAtGameplaySafePoint(
                                        {context.scene, context.session, Runtime::RuntimePhase::FixedUpdate, context.simulationTick, {}})
                                    .HasError();
                            break;
                        case None:
                            break;
                    }
                }
                committed = candidate;
                return Result<void>::Success();
            }
        };

        class SceneHandler final : public IRpcGameplayHandler {
        public:
            Runtime::RuntimeSceneService &scene;
            bool reject{};
            std::size_t calls{};
            std::function<void()> afterStaging;

            explicit SceneHandler(Runtime::RuntimeSceneService &owner) : scene(owner) {}

            Result<void> Execute(const RpcGameplayContext &context, std::span<const ReplicationRuntimeValue>) override {
                ++calls;
                if (const auto current = scene.ActiveScene(); !current || current->Get(context.entity).HasError())
                    return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingUnknown));
                Runtime::SceneCommandBuffer candidate;
                Math::Transform transform;
                transform.translation.x = 7.0F;
                candidate.SetLocalTransform(context.entity, transform);
                if (afterStaging)
                    afterStaging();
                if (reject)
                    return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
                return scene.QueueStructuralCommands(std::move(candidate));
            }
        };

        /** @brief Real Scene owner for transaction rollback tests; callbacks borrow it until dispatch retires. */
        struct SceneFixture final {
            CancellationSource cancellation;
            const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation.Token()};
            Runtime::RuntimeSceneService scene;

            SceneFixture() {
                REQUIRE(scene.Startup({}).HasValue());
                Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
                Runtime::RuntimeEntityDefinition definition;
                definition.object = Runtime::SceneObjectId{1};
                builder.Add(definition);
                REQUIRE(scene.QueuePreparation(std::move(builder).Build().Value()).HasValue());
                Commit();
            }

            SceneFixture(const SceneFixture &) = delete;
            SceneFixture &operator=(const SceneFixture &) = delete;
            SceneFixture(SceneFixture &&) = delete;
            SceneFixture &operator=(SceneFixture &&) = delete;

            ~SceneFixture() {
                scene.Shutdown();
            }

            void Commit() {
                REQUIRE(scene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
            }

            Runtime::EntityRef Entity() const {
                return *scene.ActiveScene()->Find(Runtime::SceneObjectId{1});
            }
        };

        class CallbackSerializer final : public IReplicationFieldSerializer {
        public:
            std::shared_ptr<const IReplicationFieldSerializer> canonical;
            std::function<void()> duringDescriptor;
            mutable std::size_t descriptorCalls{};
            std::function<void()> duringDecode;
            std::function<void()> duringDestruction;
            bool wrongKind{};

            explicit CallbackSerializer(std::shared_ptr<const IReplicationFieldSerializer> codec) : canonical(std::move(codec)) {}

            CallbackSerializer(const CallbackSerializer &) = delete;
            CallbackSerializer &operator=(const CallbackSerializer &) = delete;
            CallbackSerializer(CallbackSerializer &&) = delete;
            CallbackSerializer &operator=(CallbackSerializer &&) = delete;

            ~CallbackSerializer() override {
                if (duringDestruction)
                    duringDestruction();
            }

            const ReplicationSerializerDescriptor &Descriptor() const noexcept override {
                ++descriptorCalls;
                if (duringDescriptor)
                    duringDescriptor();
                return canonical->Descriptor();
            }

            Result<std::vector<std::byte>> Encode(const ReplicationRuntimeValue &value) const override {
                return canonical->Encode(value);
            }

            Result<ReplicationRuntimeValue> Decode(std::span<const std::byte> bytes) const override {
                if (duringDecode)
                    duringDecode();
                if (wrongKind)
                    return Result<ReplicationRuntimeValue>::Success(std::string{"invalid"});
                return canonical->Decode(bytes);
            }

            Result<bool> CanonicallyEqual(const ReplicationRuntimeValue &left, const ReplicationRuntimeValue &right) const override {
                return canonical->CanonicallyEqual(left, right);
            }
        };

        inline RpcDescriptor Descriptor(const RpcId rpc, const bool outbound, const RpcTarget target, const bool withParameter) {
            RpcDescriptor descriptor{
                .id = rpc,
                .version = {1, 0},
                .compatibility = {{1, 0}, {1, 0}},
                .owner = ModuleId{"game.rpc"},
                .direction = outbound ? RpcDirection::AuthorityToClient : RpcDirection::ClientToAuthority,
                .delivery = RpcDelivery::ReliableOrdered,
                .target = target,
                .permission = outbound ? RpcCallerPermission::AuthorityOnly : RpcCallerPermission::ObjectOwner,
                .rateLimit = {30, 5},
                .maximumPayloadBytes = 256,
            };
            if (withParameter)
                descriptor.parameters.push_back({.id = RpcParameterId::Create(1).Value(),
                                                 .valueType = ReplicationValueTypeId::Create(1).Value(),
                                                 .codec = ReplicationCodecId::Create(1).Value(),
                                                 .introducedVersion = {1, 0},
                                                 .limits = {64, 1}});
            return descriptor;
        }

        struct Fixture final {
            bool outbound{};
            ConnectionHandle connection = TestSupport::Connection();
            NetworkOperationGeneration generation = TestSupport::Session();
            NetworkSessionGeneration worldSession = NetworkSessionGeneration::Create(1).Value();
            ReplicationAuthorityEpoch epoch = ReplicationAuthorityEpoch::Create(1).Value();
            NetworkPeerId peer;
            NetworkPeerId localRecipient = NetworkPeerId::Create(10).Value();
            RpcId rpc = RpcId::Create(42).Value();
            NetworkObjectId object = NetworkObjectId::Create(epoch, 7, 1).Value();
            Runtime::EntityRef entity{Runtime::SceneRuntimeId{9}, Runtime::EntityId{7, 1}};
            RpcDescriptor descriptor;

            static ReplicationSerializerDescriptor SerializerMetadata() {
                return {
                    .valueType = ReplicationValueTypeId::Create(1).Value(),
                    .codec = ReplicationCodecId::Create(1).Value(),
                    .owner = ModuleId{"game.rpc"},
                    .valueKind = ReplicationValueKind::SignedInteger,
                    .maximumEncodedBytes = 64,
                    .maximumElementCount = 1,
                };
            }

            RpcDescriptorSnapshotPtr descriptors = [this] {
                const std::array all{descriptor};
                if (!descriptor.parameters.empty()) {
                    const std::array serializers{SerializerMetadata()};
                    return BuildRpcDescriptorSnapshot(all, serializers).Value();
                }
                return BuildRpcDescriptorSnapshot(all, {}).Value();
            }();
            ReplicationWorldLifecycle world = std::move(ReplicationWorldLifecycle::Create()).Value();
            std::shared_ptr<PeerSessionLifecycle> session;
            std::shared_ptr<ReplicationRoleState> role;
            std::shared_ptr<Handler> handler = std::make_shared<Handler>();
            std::shared_ptr<const IReplicationFieldSerializer> serializer;
            std::shared_ptr<const void> moduleLease = std::make_shared<int>(1);
            std::shared_ptr<RpcGameplayDispatch> dispatch;
            std::optional<MessageCodecRegistry> envelopeCodecs;

            explicit Fixture(NetworkDebugger &debugger)
                : Fixture(false, RpcTarget::Authority, true, false, [](RpcDescriptor &) {
                  }, {}, {Runtime::SceneRuntimeId{9}, Runtime::EntityId{7, 1}}, &debugger) {}

            template <typename Configure = decltype([](RpcDescriptor &) {
                          // Default fixtures leave the declared RPC metadata unchanged.
                      })>
            explicit Fixture(const bool fromAuthority = false, const RpcTarget target = RpcTarget::Authority, const bool localOwns = true,
                             const bool withValue = false, const Configure &configure = Configure{}, const RpcDispatchLimits &limits = {},
                             const Runtime::EntityRef mappedEntity = {Runtime::SceneRuntimeId{9}, Runtime::EntityId{7, 1}},
                             NetworkDebugger *debugger = nullptr)
                : outbound(fromAuthority), peer(NetworkPeerId::Create(fromAuthority ? 90 : 10).Value()), entity(mappedEntity),
                  descriptor([this, fromAuthority, target, withValue, configure] {
                      auto value = Descriptor(rpc, fromAuthority, target, withValue);
                      configure(value);
                      return value;
                  }()) {
                const auto clientRole = localOwns ? ReplicationExecutionRole::AutonomousClient : ReplicationExecutionRole::SimulatedClient;
                const ReplicationWorldActivationDescriptor active{entity.runtime, worldSession, epoch,
                                                                  outbound ? clientRole : ReplicationExecutionRole::AuthorityServer,
                                                                  ReplicationWorldPhaseSet::Default()};
                REQUIRE(world.Stage(active).HasValue());
                REQUIRE(world.CommitAtSafePoint(entity.runtime, worldSession).HasValue());
                REQUIRE(world
                            .RegisterObject(entity.runtime, worldSession,
                                            {object, entity, {ReplicationSchemaId::Create(3).Value(), {1, 0}, {}}})
                            .HasValue());
                const ReplicationRoleBinding binding{ReplicationRoleRevision::Create(1).Value(),
                                                     worldSession,
                                                     object,
                                                     ReplicationSchemaId::Create(3).Value(),
                                                     {1, 0},
                                                     active.role,
                                                     outbound ? std::optional<NetworkPeerId>{localRecipient} : std::nullopt,
                                                     localOwns ? localRecipient : NetworkPeerId::Create(11).Value()};
                role = std::make_shared<ReplicationRoleState>(std::move(ReplicationRoleState::Create(binding)).Value());
                session = ActiveSession();
                dispatch = RpcGameplayDispatch::Create(descriptors, world, limits, debugger).Value();
                REQUIRE(dispatch
                            ->RegisterPeer(session, connection, generation, peer,
                                           outbound ? RpcRemoteRole::Authority : RpcRemoteRole::Client, 22,
                                           outbound ? localRecipient : NetworkPeerId{})
                            .HasValue());
                REQUIRE(dispatch->RegisterObject(object, role).HasValue());
                if (!descriptor.parameters.empty()) {
                    serializer = CanonicalScalarReplicationSerializer::Create(SerializerMetadata()).Value();
                    const std::array codecs{serializer};
                    REQUIRE(dispatch->RegisterHandler(rpc, handler, codecs, moduleLease).HasValue());
                    handler->expectsParameter = true;
                } else {
                    REQUIRE(dispatch->RegisterHandler(rpc, handler, {}, moduleLease).HasValue());
                }
                handler->dispatch = dispatch;
                handler->connection = connection;
                handler->rpc = rpc;
            }

            std::shared_ptr<PeerSessionLifecycle> ActiveSession() const {
                auto created = PeerSessionLifecycle::Create(connection, generation, {10, 20, 30, 100, 10});
                auto result = std::make_shared<PeerSessionLifecycle>(std::move(created).Value());
                REQUIRE(result->BeginNegotiation(connection, generation, 1).HasValue());
                HandshakeSelection negotiation;
                negotiation.connection = connection;
                negotiation.sessionGeneration = generation;
                negotiation.protocol = TestSupport::WireIdentity<ProtocolId>(1);
                negotiation.version = {1, 2};
                negotiation.schemaFingerprint = 42;
                negotiation.features.values[0] = TestSupport::WireIdentity<ProtocolFeatureId>(1);
                negotiation.features.count = 1;
                negotiation.compression = HandshakeCompression::None;
                negotiation.transport.capabilityRevision = 3;
                negotiation.transport.channelCount = 1;
                negotiation.transport.maximumMessageBytes = 1200;
                negotiation.transport.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
                REQUIRE(result->AcceptNegotiation(negotiation, 2).HasValue());
                AuthenticationResult auth;
                auth.connection = connection;
                auth.sessionGeneration = generation;
                auth.policy = TestSupport::Id<NetworkTrustPolicyId>(10);
                auth.policyRevision = 4;
                auth.principal.principal = TestSupport::Id<NetworkPrincipalId>(20);
                auth.principal.session.bytes = TestSupport::Bytes<NetworkSessionIdBytes>(0x80);
                auth.principal.trustLevel = NetworkTrustLevel::ProductAnchor;
                auth.principal.roles.values[0] = TestSupport::Id<NetworkRoleId>(30);
                auth.principal.roles.count = 1;
                auth.principal.capabilities.values[0] = TestSupport::Id<NetworkCapabilityId>(40);
                auth.principal.capabilities.count = 1;
                auth.principal.provenance = TestSupport::Id<CredentialProvenanceId>(50);
                auth.principal.expiresAtTick = 150;
                auth.secureChannel.stamp = {connection, generation, 5};
                auth.secureChannel.binding = TestSupport::Id<PrivateKeyBindingId>(11);
                auth.secureChannel.channel = TestSupport::Id<SecureChannelId>(12);
                auth.secureChannel.channelGeneration = 6;
                auth.secureChannel.bindingDigest = TestSupport::Bytes<AuthenticationDigestBytes>(0x20);
                REQUIRE(result->AcceptAuthentication(auth, 11).HasValue());
                REQUIRE(result->Activate({connection, generation, auth.secureChannel.channel, 6, auth.secureChannel.bindingDigest}, 21)
                            .HasValue());
                return result;
            }

            MessageEnvelope Message(const std::uint64_t sequence, NetworkObjectId target = {}, NetworkPeerId recipient = {}) const {
                if (!target.IsValid())
                    target = object;
                MessageEnvelope message;
                Append(message.payload, rpc.Value());
                Append(message.payload, target.Epoch().Value());
                Append(message.payload, target.Slot());
                Append(message.payload, target.Generation());
                Append<std::uint16_t>(message.payload, descriptor.version.major);
                Append<std::uint16_t>(message.payload, descriptor.version.minor);
                Append(message.payload, sequence);
                const auto defaultRecipient = outbound ? localRecipient.Value() : 0ULL;
                Append(message.payload, recipient.IsValid() ? recipient.Value() : defaultRecipient);
                Append<std::uint16_t>(message.payload, descriptor.parameters.empty() ? 0 : 1);
                if (!descriptor.parameters.empty()) {
                    const auto bytes = serializer->Encode(ReplicationRuntimeValue{std::int64_t{7}}).Value();
                    Append<std::uint32_t>(message.payload, 1);
                    Append<std::uint32_t>(message.payload, static_cast<std::uint32_t>(bytes.size()));
                    message.payload.insert(message.payload.end(), bytes.begin(), bytes.end());
                }
                return message;
            }

            /** @brief Composes a router borrowing the fixture's stable registry; the router must retire before this fixture. */
            std::unique_ptr<InboundMessageDispatcher> Router(ImpairedTransport &transport) {
                REQUIRE_FALSE(envelopeCodecs.has_value());
                const auto protocol = TestSupport::WireIdentity<ProtocolId>(1);
                const auto messageId = TestSupport::WireIdentity<MessageTypeId>(2);
                const auto schema = TestSupport::WireIdentity<MessageSchemaId>(3);
                const std::array protocols{ProtocolIdentityDescriptor{protocol, {{1, 0}, {1, 3}}}};
                const std::array messages{MessageIdentityDescriptor{protocol, messageId, schema, {1, 1}, false}};
                const auto identities = ProtocolIdentityRegistry::Create({protocols, messages, {}, {}}).Value();
                const std::array codecDescriptors{MessageCodecDescriptor{protocol, messageId, schema, {{1, 0}, {1, 2}}, 256}};
                envelopeCodecs.emplace(MessageCodecRegistry::Create({codecDescriptors, {}}, identities).Value());
                auto router = std::move(InboundMessageDispatcher::Create(transport, *envelopeCodecs)).Value();
                REQUIRE(router
                            ->RegisterHandler({protocol, messageId, MessageTrafficClass::Command, Runtime::RuntimePhase::NetworkPoll, 16},
                                              dispatch)
                            .HasValue());
                TransportSelectionEvidence selection;
                selection.capabilityRevision = 3;
                selection.channelCount = 1;
                selection.maximumMessageBytes = 1200;
                selection.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
                auto gate =
                    MessageDeliveryGate::Create(selection, 3, connection, generation,
                                                {{ChannelId{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Command, false}});
                REQUIRE(gate.HasValue());
                REQUIRE(router->RegisterSession(session, connection, generation, std::move(gate).Value(), 22).HasValue());
                return router;
            }

            InboundMessageContext Context(const CancellationToken &cancellation = {}) const {
                return {connection, generation, session, 22, cancellation};
            }

            ReplicationWorldWorkRequest Work() const {
                return {entity.runtime, worldSession, Runtime::RuntimePhase::FixedUpdate, 23, {}};
            }
        };
    }  // namespace RpcDispatchTestSupport

}  // namespace Horo::Network
