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
#include <utility>

namespace Horo::Network {
    namespace {
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

            explicit SceneHandler(Runtime::RuntimeSceneService &owner) : scene(owner) {}

            Result<void> Execute(const RpcGameplayContext &context, std::span<const ReplicationRuntimeValue>) override {
                if (const auto current = scene.ActiveScene(); !current || current->Get(context.entity).HasError())
                    return Result<void>::Failure(MakeError(NetworkErrors::NetworkObjectMappingUnknown));
                Runtime::SceneCommandBuffer candidate;
                Math::Transform transform;
                transform.translation.x = 7.0F;
                candidate.SetLocalTransform(context.entity, transform);
                if (reject)
                    return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
                return scene.QueueStructuralCommands(std::move(candidate));
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

        RpcDescriptor Descriptor(const RpcId rpc, const bool outbound, const RpcTarget target, const bool withParameter) {
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
            ReplicationSerializerDescriptor serializerDescriptor{
                .valueType = ReplicationValueTypeId::Create(1).Value(),
                .codec = ReplicationCodecId::Create(1).Value(),
                .owner = ModuleId{"game.rpc"},
                .valueKind = ReplicationValueKind::SignedInteger,
                .maximumEncodedBytes = 64,
                .maximumElementCount = 1,
            };
            RpcDescriptorSnapshotPtr descriptors = [this] {
                const std::array all{descriptor};
                if (!descriptor.parameters.empty()) {
                    const std::array serializers{serializerDescriptor};
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

            template <typename Configure = decltype([](RpcDescriptor &) {
                      })>
            explicit Fixture(const bool fromAuthority = false, const RpcTarget target = RpcTarget::Authority, const bool localOwns = true,
                             const bool withValue = false, const Configure &configure = Configure{}, const RpcDispatchLimits &limits = {},
                             const Runtime::EntityRef mappedEntity = {Runtime::SceneRuntimeId{9}, Runtime::EntityId{7, 1}})
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
                dispatch = RpcGameplayDispatch::Create(descriptors, world, limits).Value();
                REQUIRE(dispatch
                            ->RegisterPeer(session, connection, generation, peer,
                                           outbound ? RpcRemoteRole::Authority : RpcRemoteRole::Client, 22,
                                           outbound ? localRecipient : NetworkPeerId{})
                            .HasValue());
                REQUIRE(dispatch->RegisterObject(object, role).HasValue());
                if (!descriptor.parameters.empty()) {
                    serializer = CanonicalScalarReplicationSerializer::Create(serializerDescriptor).Value();
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

            InboundMessageContext Context(const CancellationToken &cancellation = {}) const {
                return {connection, generation, session, 22, cancellation};
            }

            ReplicationWorldWorkRequest Work() const {
                return {entity.runtime, worldSession, Runtime::RuntimePhase::FixedUpdate, 23, {}};
            }
        };
    }  // namespace

    TEST_CASE("RPC receipt resolves an admitted object then executes once at the Gameplay safe point", "[unit][network][rpc]") {
        Fixture fixture;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.handler->calls == 0);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                  NetworkErrors::MessageDeliveryInvalid);
        const auto drained = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work());
        REQUIRE(drained.HasValue());
        REQUIRE(drained.Value().invoked == 1);
        REQUIRE(fixture.handler->calls == 1);
        REQUIRE(fixture.handler->lastEntity == fixture.entity);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                  NetworkErrors::MessageDeliveryInvalid);
    }

    TEST_CASE("RPC receipt and drain reject stale identity, revocation, cancellation and shutdown", "[unit][network][rpc]") {
        Fixture fixture;
        const auto stale = NetworkObjectId::Create(fixture.epoch, 7, 2).Value();
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1, stale)),
                                  NetworkErrors::GameplayDispatchRejected);
        CancellationSource cancellation;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(cancellation.Token()), fixture.Message(2)).HasValue());
        cancellation.RequestCancellation();
        const auto drained = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work());
        REQUIRE(drained.HasValue());
        REQUIRE(drained.Value().rejected == 1);
        REQUIRE(fixture.handler->calls == 0);
        fixture.dispatch->RevokeObject(fixture.object);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(3)),
                                  NetworkErrors::GameplayDispatchRejected);
        fixture.dispatch->Shutdown();
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(4)),
                                  NetworkErrors::SessionShuttingDown);
    }

    TEST_CASE("RPC drain survives callback revocation, shutdown and nested drain", "[unit][network][rpc]") {
        using enum Handler::Action;
        for (const auto action : {Shutdown, RevokePeer, RevokeObject, RevokeHandler}) {
            Fixture fixture;
            fixture.handler->action = action;
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
            const auto result = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work());
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().consumed == 1);
            REQUIRE(fixture.handler->calls == 1);
            if (action == RevokeHandler) {
                REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, {}, fixture.moduleLease).HasValue());
                TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)),
                                          NetworkErrors::MessageDeliveryInvalid);
            }
        }
        Fixture nested;
        nested.handler->action = NestedDrain;
        REQUIRE(nested.dispatch->HandleAdmitted(nested.Context(), nested.Message(1)).HasValue());
        REQUIRE(nested.dispatch->DrainAtGameplaySafePoint(nested.Work()).HasValue());
        REQUIRE(nested.handler->nestedRejected);
    }

    TEST_CASE("Authority RPC recipient must match the host grant and object owner policy", "[unit][network][rpc]") {
        using enum RpcTarget;
        Fixture owner(true, ObjectOwner);
        REQUIRE(owner.dispatch->HandleAdmitted(owner.Context(), owner.Message(1)).HasValue());
        TestSupport::RequireError(owner.dispatch->HandleAdmitted(owner.Context(), owner.Message(2, {}, NetworkPeerId::Create(11).Value())),
                                  NetworkErrors::ReplicationAuthorityDenied);
        REQUIRE(owner.dispatch->DrainAtGameplaySafePoint(owner.Work()).Value().invoked == 1);

        Fixture nonOwner(true, ObjectOwner, false);
        TestSupport::RequireError(nonOwner.dispatch->HandleAdmitted(nonOwner.Context(), nonOwner.Message(1)),
                                  NetworkErrors::ReplicationAuthorityDenied);
        Fixture invoking(true, InvokingClient);
        REQUIRE(invoking.dispatch->HandleAdmitted(invoking.Context(), invoking.Message(1)).HasValue());
        TestSupport::RequireError(invoking.dispatch->HandleAdmitted(invoking.Context(),
                                                                    invoking.Message(2, {}, NetworkPeerId::Create(11).Value())),
                                  NetworkErrors::ReplicationAuthorityDenied);
        Fixture broadcast(true, AllClients);
        REQUIRE(broadcast.dispatch->HandleAdmitted(broadcast.Context(), broadcast.Message(1)).HasValue());
    }

    TEST_CASE("RPC typed parameters are owned and decoded before Gameplay mutation", "[unit][network][rpc]") {
        Fixture fixture(false, RpcTarget::Authority, true, true);
        auto message = fixture.Message(1);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), message).HasValue());
        message.payload.clear();
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(fixture.handler->committed == 1);
    }

    TEST_CASE("RPC malformed framing and incompatible declarations never invoke Gameplay", "[unit][network][rpc]") {
        Fixture fixture(false, RpcTarget::Authority, true, true);
        const auto valid = fixture.Message(1);
        for (std::size_t size = 0; size < valid.payload.size(); ++size) {
            auto truncated = valid;
            truncated.payload.resize(size);
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), truncated).HasError());
        }
        for (const std::size_t offset : {0U, 8U, 16U, 24U, 28U, 48U, 50U, 54U}) {
            auto malformed = valid;
            malformed.payload[offset] = std::byte{0xff};
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), malformed).HasError());
        }
        auto trailing = valid;
        trailing.payload.push_back(std::byte{});
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), trailing).HasError());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 0);
        REQUIRE(fixture.handler->calls == 0);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), valid).HasValue());
    }

    TEST_CASE("RPC owner authority and supported caller policy are checked at receipt and drain", "[unit][network][rpc]") {
        Fixture denied(false, RpcTarget::Authority, false);
        TestSupport::RequireError(denied.dispatch->HandleAdmitted(denied.Context(), denied.Message(1)),
                                  NetworkErrors::ReplicationAuthorityDenied);
        REQUIRE(denied.handler->calls == 0);

        Fixture transferred;
        REQUIRE(transferred.dispatch->HandleAdmitted(transferred.Context(), transferred.Message(1)).HasValue());
        auto next = transferred.role->Snapshot().Value();
        const auto previous = next.revision;
        next.revision = ReplicationRoleRevision::Create(2).Value();
        next.autonomousOwner = NetworkPeerId::Create(11).Value();
        REQUIRE(transferred.role->Stage({previous, next}).HasValue());
        REQUIRE(transferred.role->CommitAtSafePoint(next.revision).HasValue());
        REQUIRE(transferred.dispatch->DrainAtGameplaySafePoint(transferred.Work()).Value().rejected == 1);
        REQUIRE(transferred.handler->calls == 0);

        Fixture custom(false, RpcTarget::Authority, true, false, [](RpcDescriptor &descriptor) {
            descriptor.permission = RpcCallerPermission::Custom;
            descriptor.customPermission = RpcPermissionId::Create(1).Value();
        });
        TestSupport::RequireError(custom.dispatch->HandleAdmitted(custom.Context(), custom.Message(1)),
                                  NetworkErrors::RpcPermissionUnsupported);
    }

    TEST_CASE("RPC queue, replay and invocation bounds reject without consuming an unqueued sequence", "[unit][network][rpc]") {
        RpcDispatchLimits limits;
        limits.maximumPending = 1;
        limits.maximumPerDrain = 1;
        limits.maximumReplayScopes = 1;
        Fixture fixture(false, RpcTarget::Authority, true, false, {}, limits);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)),
                                  NetworkErrors::RpcCapacityExceeded);
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        auto oversized = fixture.Message(3);
        oversized.payload.resize(limits.maximumInvocationBytes + 1);
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), oversized), NetworkErrors::RpcCapacityExceeded);
        REQUIRE(fixture.handler->calls == 2);
    }

    TEST_CASE("RPC drain budget and safe-point identity fence queued work", "[unit][network][rpc]") {
        RpcDispatchLimits limits;
        limits.maximumPerDrain = 1;
        Fixture fixture(false, RpcTarget::Authority, true, false, {}, limits);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
        auto wrong = fixture.Work();
        wrong.phase = Runtime::RuntimePhase::NetworkPoll;
        TestSupport::RequireError(fixture.dispatch->DrainAtGameplaySafePoint(wrong), NetworkErrors::ReplicationWorldPhaseInvalid);
        wrong = fixture.Work();
        wrong.session = NetworkSessionGeneration::Create(2).Value();
        TestSupport::RequireError(fixture.dispatch->DrainAtGameplaySafePoint(wrong), NetworkErrors::ReplicationWorldStale);
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 1);
        REQUIRE(fixture.handler->calls == 1);
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 1);
        REQUIRE(fixture.handler->calls == 2);
    }

    TEST_CASE("RPC logical replay survives peer and object rebinding within the same occurrence", "[unit][network][rpc]") {
        Fixture fixture;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        fixture.dispatch->RevokePeer(fixture.connection);
        REQUIRE(
            fixture.dispatch->RegisterPeer(fixture.session, fixture.connection, fixture.generation, fixture.peer, RpcRemoteRole::Client, 22)
                .HasValue());
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                  NetworkErrors::MessageDeliveryInvalid);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
        fixture.dispatch->RevokeObject(fixture.object);
        REQUIRE(fixture.dispatch->RegisterObject(fixture.object, fixture.role).HasValue());
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)),
                                  NetworkErrors::MessageDeliveryInvalid);
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 0);
    }

    TEST_CASE("RPC session closure and destroyed mappings reject queued invocations", "[unit][network][rpc]") {
        SECTION("destroyed object") {
            Fixture fixture;
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(fixture.world.RetireObject(fixture.entity.runtime, fixture.worldSession, fixture.object).HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().rejected == 1);
            REQUIRE(fixture.handler->calls == 0);
        }
        SECTION("expired session") {
            Fixture fixture;
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            auto work = fixture.Work();
            work.simulationTick = 150;
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(work).Value().rejected == 1);
            REQUIRE(fixture.handler->calls == 0);
        }
        SECTION("wrong admitted generation") {
            Fixture fixture;
            auto context = fixture.Context();
            context.generation = NetworkOperationGeneration::Create(2).Value();
            REQUIRE(fixture.dispatch->HandleAdmitted(context, fixture.Message(1)).HasError());
            REQUIRE(fixture.dispatch->Handle(fixture.Message(1)).HasError());
            REQUIRE(fixture.handler->calls == 0);
        }
    }

    TEST_CASE("RPC failed Gameplay transactions never partially commit or execute again", "[unit][network][rpc]") {
        for (const bool throws : {false, true}) {
            Fixture fixture;
            fixture.handler->fail = !throws;
            fixture.handler->throwFailure = throws;
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().rejected == 1);
            REQUIRE(fixture.handler->committed == 0);
            REQUIRE(fixture.handler->calls == 1);
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::MessageDeliveryInvalid);
        }
    }

    TEST_CASE("RPC execution pins the world and module while callbacks revoke their owners", "[unit][network][rpc]") {
        Fixture fixture;
        std::weak_ptr<const void> codeLease = fixture.moduleLease;
        fixture.moduleLease.reset();
        fixture.handler->duringExecute = [&fixture, codeLease] {
            fixture.dispatch->RevokeHandler(fixture.rpc);
            REQUIRE_FALSE(codeLease.expired());
            fixture.world.BeginShutdown();
            REQUIRE(fixture.world.CollectRetired() == ReplicationWorldLifecycleState::ShuttingDown);
            REQUIRE(fixture.world.ActiveDescriptor().HasValue());
        };
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(codeLease.expired());
        REQUIRE(fixture.world.CollectRetired() == ReplicationWorldLifecycleState::Closed);
    }

    TEST_CASE("Impaired transport routes admitted RPCs through envelope replay to the Gameplay transaction", "[unit][network][rpc]") {
        for (const bool lost : {false, true}) {
            Fixture fixture(false, RpcTarget::Authority, true, true);
            const auto protocol = TestSupport::WireIdentity<ProtocolId>(1);
            const auto messageId = TestSupport::WireIdentity<MessageTypeId>(2);
            const auto schema = TestSupport::WireIdentity<MessageSchemaId>(3);
            const std::array protocols{ProtocolIdentityDescriptor{protocol, {{1, 0}, {1, 3}}}};
            const std::array messages{MessageIdentityDescriptor{protocol, messageId, schema, {1, 1}, false}};
            const auto identities = ProtocolIdentityRegistry::Create({protocols, messages, {}, {}}).Value();
            const std::array codecDescriptors{MessageCodecDescriptor{protocol, messageId, schema, {{1, 0}, {1, 2}}, 256}};
            const auto codecs = MessageCodecRegistry::Create({codecDescriptors, {}}, identities).Value();
            ImpairedTransport transport(lost);
            auto router = std::move(InboundMessageDispatcher::Create(transport, codecs)).Value();
            REQUIRE(router
                        ->RegisterHandler({protocol, messageId, MessageTrafficClass::Command, Runtime::RuntimePhase::NetworkPoll, 16},
                                          fixture.dispatch)
                        .HasValue());
            TransportSelectionEvidence selection;
            selection.capabilityRevision = 3;
            selection.channelCount = 1;
            selection.maximumMessageBytes = 1200;
            selection.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            auto gate = MessageDeliveryGate::Create(selection, 3, fixture.connection, fixture.generation,
                                                    {{ChannelId{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Command, false}});
            REQUIRE(gate.HasValue());
            REQUIRE(
                router->RegisterSession(fixture.session, fixture.connection, fixture.generation, std::move(gate).Value(), 22).HasValue());
            auto envelope = fixture.Message(1);
            envelope.protocol = protocol;
            envelope.message = messageId;
            envelope.schema = schema;
            envelope.schemaVersion = {1, 1};
            envelope.sequence = MessageSequenceNumber{1};
            const auto bytes = EncodeMessageEnvelope(envelope, codecs).Value();
            REQUIRE(transport.Send(fixture.connection, ChannelId{}, bytes, DeliveryPolicy::ReliableOrdered).HasValue());
            REQUIRE(router->RunNetworkPoll(22).HasValue());
            REQUIRE(fixture.handler->calls == 0);
            const auto first = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work());
            REQUIRE(first.HasValue());
            REQUIRE(first.Value().invoked == (lost ? 0 : 1));
            REQUIRE(fixture.handler->committed == (lost ? 0 : 1));
            if (!lost) {
                // A fresh envelope sequence must not bypass logical RPC deduplication.
                envelope.sequence = MessageSequenceNumber{2};
                REQUIRE(transport
                            .Send(fixture.connection, ChannelId{}, EncodeMessageEnvelope(envelope, codecs).Value(),
                                  DeliveryPolicy::ReliableOrdered)
                            .HasValue());
                REQUIRE(router->RunNetworkPoll(23).HasValue());
                REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 0);
                REQUIRE(fixture.handler->calls == 1);
            }
            router->Shutdown();
            fixture.dispatch->Shutdown();
            transport.Shutdown();
        }
    }

    TEST_CASE("RPC Gameplay stages an actual Scene transaction for lifecycle publication", "[unit][network][rpc][scene]") {
        CancellationSource cancellation;
        const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation.Token()};
        Runtime::RuntimeSceneService scene;
        REQUIRE(scene.Startup({}).HasValue());
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
        Runtime::RuntimeEntityDefinition definition;
        definition.object = Runtime::SceneObjectId{1};
        builder.Add(definition);
        REQUIRE(scene.QueuePreparation(std::move(builder).Build().Value()).HasValue());
        REQUIRE(scene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        const auto entity = *scene.ActiveScene()->Find(Runtime::SceneObjectId{1});
        Fixture fixture(false, RpcTarget::Authority, true, false, {}, {}, entity);
        auto handler = std::make_shared<SceneHandler>(scene);
        fixture.dispatch->RevokeHandler(fixture.rpc);
        REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, handler, {}, fixture.moduleLease).HasValue());
        handler->reject = true;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().rejected == 1);
        REQUIRE(scene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        REQUIRE(scene.ActiveScene()->Get(entity).Value().localTransform->translation.x == 0.0F);
        handler->reject = false;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(scene.ActiveScene()->Get(entity).Value().localTransform->translation.x == 0.0F);
        REQUIRE(scene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        REQUIRE(scene.ActiveScene()->Get(entity).Value().localTransform->translation.x == 7.0F);
        fixture.dispatch->Shutdown();
        scene.Shutdown();
    }

    TEST_CASE("RPC serializer callbacks cannot publish after revocation or bypass the declared type", "[unit][network][rpc]") {
        for (const int mode : {0, 1, 2, 3}) {
            Fixture fixture(false, RpcTarget::Authority, true, true);
            auto codec = std::make_shared<CallbackSerializer>(fixture.serializer);
            fixture.dispatch->RevokeHandler(fixture.rpc);
            codec->wrongKind = mode == 0;
            if (mode == 1) {
                codec->duringDecode = [&fixture] {
                    fixture.dispatch->RevokeHandler(fixture.rpc);
                };
            } else if (mode == 2) {
                codec->duringDecode = [] {
                    throw 42;
                };
            } else if (mode == 3) {
                codec->duringDecode = [&fixture] {
                    fixture.dispatch->Shutdown();
                };
            }
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{codec};
            REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasValue());
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasError());
            if (mode == 3)
                REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).HasError());
            else
                REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 0);
            REQUIRE(fixture.handler->calls == 0);
        }
    }

    TEST_CASE("RPC optional parameters use only explicit canonical defaults", "[unit][network][rpc]") {
        Fixture fixture(false, RpcTarget::Authority, true, true, [](RpcDescriptor &descriptor) {
            auto &parameter = descriptor.parameters.front();
            parameter.requirement = RpcParameterRequirement::Optional;
            parameter.canonicalDefault = RpcParameterDefault{};
            const ReplicationSerializerDescriptor metadata{.valueType = parameter.valueType,
                                                           .codec = parameter.codec,
                                                           .owner = descriptor.owner,
                                                           .valueKind = ReplicationValueKind::SignedInteger,
                                                           .maximumEncodedBytes = parameter.limits.maximumEncodedBytes,
                                                           .maximumElementCount = parameter.limits.maximumElementCount};
            const auto codec = CanonicalScalarReplicationSerializer::Create(metadata).Value();
            parameter.canonicalDefault->canonicalBytes = codec->Encode(std::int64_t{7}).Value();
        });
        auto message = fixture.Message(1);
        message.payload.resize(50);
        message.payload[48] = std::byte{};
        message.payload[49] = std::byte{};
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), message).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
    }

    TEST_CASE("RPC object reuse and reconnect create distinct replay scopes while fencing old work", "[unit][network][rpc]") {
        Fixture fixture;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        fixture.dispatch->RevokePeer(fixture.connection);
        fixture.connection = TestSupport::Connection(4);
        fixture.generation = TestSupport::Session(8);
        fixture.session = fixture.ActiveSession();
        REQUIRE(
            fixture.dispatch->RegisterPeer(fixture.session, fixture.connection, fixture.generation, fixture.peer, RpcRemoteRole::Client, 22)
                .HasValue());
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        fixture.dispatch->RevokeObject(fixture.object);
        REQUIRE(fixture.world.RetireObject(fixture.entity.runtime, fixture.worldSession, fixture.object).HasValue());
        fixture.object = NetworkObjectId::Create(fixture.epoch, 7, 2).Value();
        fixture.entity.entity = Runtime::EntityId{7, 2};
        REQUIRE(fixture.world
                    .RegisterObject(fixture.entity.runtime, fixture.worldSession,
                                    {fixture.object, fixture.entity, {ReplicationSchemaId::Create(3).Value(), {1, 0}, {}}})
                    .HasValue());
        const ReplicationRoleBinding binding{ReplicationRoleRevision::Create(1).Value(),
                                             fixture.worldSession,
                                             fixture.object,
                                             ReplicationSchemaId::Create(3).Value(),
                                             {1, 0},
                                             ReplicationExecutionRole::AuthorityServer,
                                             {},
                                             fixture.peer};
        fixture.role = std::make_shared<ReplicationRoleState>(std::move(ReplicationRoleState::Create(binding)).Value());
        REQUIRE(fixture.dispatch->RegisterObject(fixture.object, fixture.role).HasValue());
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(fixture.handler->calls == 2);
    }

    TEST_CASE("RPC world replacement discards previously queued commands", "[unit][network][rpc]") {
        Fixture fixture;
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        const ReplicationWorldActivationDescriptor next{Runtime::SceneRuntimeId{10}, NetworkSessionGeneration::Create(2).Value(),
                                                        ReplicationAuthorityEpoch::Create(2).Value(),
                                                        ReplicationExecutionRole::AuthorityServer, ReplicationWorldPhaseSet::Default()};
        REQUIRE(fixture.world.Stage(next).HasValue());
        REQUIRE(fixture.world.CommitAtSafePoint(next.scene, next.session).HasValue());
        const auto drained =
            fixture.dispatch->DrainAtGameplaySafePoint({next.scene, next.session, Runtime::RuntimePhase::FixedUpdate, 23, {}});
        REQUIRE(drained.HasValue());
        REQUIRE(drained.Value().rejected == 1);
        REQUIRE(fixture.handler->calls == 0);
    }

    TEST_CASE("RPC handler metadata publication is guarded and snapshotted once", "[unit][network][rpc]") {
        Fixture fixture(false, RpcTarget::Authority, true, true);
        fixture.dispatch->RevokeHandler(fixture.rpc);
        auto serializer = std::make_shared<CallbackSerializer>(fixture.serializer);
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{serializer};
        bool nestedRejected = false;
        serializer->duringDescriptor = [&fixture, &codecs, &nestedRejected] {
            nestedRejected = fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasError();
        };
        REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasValue());
        REQUIRE(nestedRejected);
        REQUIRE(serializer->descriptorCalls == 1);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(serializer->descriptorCalls == 1);
    }

    TEST_CASE("RPC handler metadata shutdown or revocation aborts installation", "[unit][network][rpc]") {
        Fixture fixture(false, RpcTarget::Authority, true, true);
        fixture.dispatch->RevokeHandler(fixture.rpc);
        auto serializer = std::make_shared<CallbackSerializer>(fixture.serializer);
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{serializer};
        SECTION("shutdown") {
            serializer->duringDescriptor = [&fixture] {
                fixture.dispatch->Shutdown();
            };
            TestSupport::RequireError(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease),
                                      NetworkErrors::SessionShuttingDown);
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasError());
        }
        SECTION("revocation") {
            serializer->duringDescriptor = [&fixture] {
                fixture.dispatch->RevokeHandler(fixture.rpc);
            };
            TestSupport::RequireError(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease),
                                      NetworkErrors::GameplayDispatchRejected);
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasError());
            serializer->duringDescriptor = {};
            REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease).HasValue());
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        }
        REQUIRE(fixture.handler->calls <= 1);
    }

    TEST_CASE("RPC metadata callback can release external owners without retiring executing code", "[unit][network][rpc]") {
        bool serializerDestroyedWithLease = false;
        bool handlerDestroyedWithLease = false;
        bool dispatchPinned = false;
        bool inputsPinned = false;
        Fixture fixture(false, RpcTarget::Authority, true, true);
        fixture.dispatch->RevokeHandler(fixture.rpc);
        const std::weak_ptr<RpcGameplayDispatch> dispatchWeak = fixture.dispatch;
        const std::weak_ptr<const void> leaseWeak = fixture.moduleLease;
        const std::weak_ptr<Handler> handlerWeak = fixture.handler;
        auto serializer = std::make_shared<CallbackSerializer>(fixture.serializer);
        const std::weak_ptr<CallbackSerializer> serializerWeak = serializer;
        std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> codecs{serializer};
        serializer->duringDestruction = [&serializerDestroyedWithLease, leaseWeak] {
            serializerDestroyedWithLease = !leaseWeak.expired();
        };
        fixture.handler->duringDestruction = [&handlerDestroyedWithLease, leaseWeak] {
            handlerDestroyedWithLease = !leaseWeak.expired();
        };
        serializer->duringDescriptor = [&fixture, &codecs, &serializer, &dispatchPinned, &inputsPinned, dispatchWeak, handlerWeak,
                                        serializerWeak, leaseWeak] {
            fixture.dispatch->Shutdown();
            fixture.dispatch.reset();
            fixture.handler.reset();
            fixture.moduleLease.reset();
            codecs[0].reset();
            serializer.reset();
            dispatchPinned = !dispatchWeak.expired();
            inputsPinned = !handlerWeak.expired() && !serializerWeak.expired() && !leaseWeak.expired();
        };
        auto *const dispatch = fixture.dispatch.get();
        TestSupport::RequireError(dispatch->RegisterHandler(fixture.rpc, fixture.handler, codecs, fixture.moduleLease),
                                  NetworkErrors::SessionShuttingDown);
        REQUIRE(dispatchPinned);
        REQUIRE(inputsPinned);
        REQUIRE(serializerDestroyedWithLease);
        REQUIRE(handlerDestroyedWithLease);
        REQUIRE(dispatchWeak.expired());
        REQUIRE(serializerWeak.expired());
        REQUIRE(handlerWeak.expired());
        REQUIRE(leaseWeak.expired());
    }

    TEST_CASE("RPC vector binding retirement retains each module through its serializer destructor", "[unit][network][rpc]") {
        bool firstDestroyedWithLease = false;
        bool secondDestroyedWithLease = false;
        Fixture fixture(false, RpcTarget::Authority, true, true);
        RpcDescriptor second = fixture.descriptor;
        second.id = RpcId::Create(99).Value();
        const std::array declarations{fixture.descriptor, second};
        const std::array metadata{fixture.serializerDescriptor};
        auto descriptors = BuildRpcDescriptorSnapshot(declarations, metadata).Value();
        auto dispatch = RpcGameplayDispatch::Create(descriptors, fixture.world).Value();
        auto firstLease = std::make_shared<int>(1);
        auto secondLease = std::make_shared<int>(2);
        const std::weak_ptr<const void> firstWeak = firstLease;
        const std::weak_ptr<const void> secondWeak = secondLease;
        auto first = std::make_shared<CallbackSerializer>(fixture.serializer);
        auto last = std::make_shared<CallbackSerializer>(fixture.serializer);
        first->duringDestruction = [&firstDestroyedWithLease, firstWeak] {
            firstDestroyedWithLease = !firstWeak.expired();
        };
        last->duringDestruction = [&secondDestroyedWithLease, secondWeak] {
            secondDestroyedWithLease = !secondWeak.expired();
        };
        {
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> firstCodecs{first};
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> secondCodecs{last};
            REQUIRE(dispatch->RegisterHandler(fixture.rpc, fixture.handler, firstCodecs, firstLease).HasValue());
            REQUIRE(dispatch->RegisterHandler(second.id, fixture.handler, secondCodecs, secondLease).HasValue());
        }
        first.reset();
        last.reset();
        firstLease.reset();
        secondLease.reset();
        dispatch->RevokeHandler(fixture.rpc);
        REQUIRE(firstDestroyedWithLease);
        REQUIRE(firstWeak.expired());
        REQUIRE_FALSE(secondDestroyedWithLease);
        REQUIRE_FALSE(secondWeak.expired());
        dispatch->Shutdown();
        REQUIRE(secondDestroyedWithLease);
        REQUIRE(secondWeak.expired());
    }

    TEST_CASE("RPC handler retirement keeps its code lease through the final callback destructor", "[unit][network][rpc]") {
        bool destroyedWithLease = false;
        Fixture fixture;
        const std::weak_ptr<const void> codeLease = fixture.moduleLease;
        fixture.moduleLease.reset();
        fixture.handler->duringDestruction = [&destroyedWithLease, codeLease] {
            destroyedWithLease = !codeLease.expired();
        };
        fixture.handler->duringExecute = [&fixture, codeLease] {
            fixture.dispatch->Shutdown();
            fixture.handler.reset();
            REQUIRE_FALSE(codeLease.expired());
        };
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        REQUIRE(destroyedWithLease);
        REQUIRE(codeLease.expired());
    }

    TEST_CASE("RPC replay storage exhaustion fails closed for a fresh admitted occurrence", "[unit][network][rpc]") {
        RpcDispatchLimits limits;
        limits.maximumReplayScopes = 1;
        Fixture fixture(false, RpcTarget::Authority, true, false, {}, limits);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        fixture.dispatch->RevokePeer(fixture.connection);
        fixture.connection = TestSupport::Connection(4);
        fixture.generation = TestSupport::Session(8);
        fixture.session = fixture.ActiveSession();
        REQUIRE(
            fixture.dispatch->RegisterPeer(fixture.session, fixture.connection, fixture.generation, fixture.peer, RpcRemoteRole::Client, 22)
                .HasValue());
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                  NetworkErrors::RpcCapacityExceeded);
        REQUIRE(fixture.handler->calls == 1);
    }

    TEST_CASE("RPC delivery and expired handler bindings never invoke Gameplay", "[unit][network][rpc]") {
        Fixture fixture;
        auto unreliable = fixture.Context();
        unreliable.delivery = DeliveryPolicy::UnreliableUnordered;
        TestSupport::RequireError(fixture.dispatch->HandleAdmitted(unreliable, fixture.Message(1)),
                                  NetworkErrors::TransportDeliveryUnsupported);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
        fixture.handler.reset();
        REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().rejected == 1);
        REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasError());
    }

    TEST_CASE("Authority client targets resolve legitimate simulated recipients independently of ownership", "[unit][network][rpc]") {
        using enum RpcTarget;
        for (const auto target : {InvokingClient, AllClients}) {
            Fixture fixture(true, target, false);
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
        }
        Fixture authenticated(false, RpcTarget::Authority, false, false, [](RpcDescriptor &descriptor) {
            descriptor.permission = RpcCallerPermission::AuthenticatedPeer;
        });
        REQUIRE(authenticated.dispatch->HandleAdmitted(authenticated.Context(), authenticated.Message(1)).HasValue());
        REQUIRE(authenticated.dispatch->DrainAtGameplaySafePoint(authenticated.Work()).Value().invoked == 1);
    }
}  // namespace Horo::Network
