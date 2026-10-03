#include "RpcGameplayDispatchTestSupport.h"

namespace Horo::Network {
    using RpcDispatchTestSupport::Fixture;
    using RpcDispatchTestSupport::ImpairedTransport;
    using RpcDispatchTestSupport::SceneHandler;

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

    TEST_CASE("RPC failed Gameplay transactions never partially commit or execute again", "[unit][network][rpc]") {
        for (const int failure : {0, 1, 2}) {
            Fixture fixture;
            fixture.handler->fail = failure == 0;
            fixture.handler->throwFailure = failure == 1;
            if (failure == 2)
                fixture.handler->duringExecute = [] {
                    throw std::invalid_argument{"hostile gameplay callback"};
                };
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().rejected == 1);
            REQUIRE(fixture.handler->committed == 0);
            REQUIRE(fixture.handler->calls == 1);
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::MessageDeliveryInvalid);
        }
    }

    TEST_CASE("RPC thrown Scene staging rolls back without publication or logical replay", "[unit][network][rpc][scene]") {
        for (const bool standardException : {false, true}) {
            RpcDispatchTestSupport::SceneFixture owner;
            const auto entity = owner.Entity();
            Fixture fixture(false, RpcTarget::Authority, true, false, {}, {}, entity);
            auto handler = std::make_shared<SceneHandler>(owner.scene);
            fixture.dispatch->RevokeHandler(fixture.rpc);
            REQUIRE(fixture.dispatch->RegisterHandler(fixture.rpc, handler, {}, fixture.moduleLease).HasValue());
            bool staged = false;
            handler->afterStaging = [&staged, standardException] {
                staged = true;
                if (standardException)
                    throw std::invalid_argument{"Scene candidate rejected after staging"};
                throw 42;
            };
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)).HasValue());
            const auto rejected = fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value();
            REQUIRE(staged);
            REQUIRE(rejected.consumed == 1);
            REQUIRE(rejected.rejected == 1);
            REQUIRE(rejected.invoked == 0);
            REQUIRE(rejected.lastError.has_value());
            REQUIRE(rejected.lastError->code.Value() == MakeError(NetworkErrors::RpcGameplayFailed).code.Value());
            owner.Commit();
            REQUIRE_FALSE(owner.scene.TakeStructuralCommitResult().has_value());
            REQUIRE(owner.scene.ActiveScene()->Get(entity).Value().localTransform->translation.x == 0.0F);
            TestSupport::RequireError(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(1)),
                                      NetworkErrors::MessageDeliveryInvalid);
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().consumed == 0);
            REQUIRE(handler->calls == 1);
            handler->afterStaging = {};
            REQUIRE(fixture.dispatch->HandleAdmitted(fixture.Context(), fixture.Message(2)).HasValue());
            REQUIRE(fixture.dispatch->DrainAtGameplaySafePoint(fixture.Work()).Value().invoked == 1);
            REQUIRE(owner.scene.ActiveScene()->Get(entity).Value().localTransform->translation.x == 0.0F);
            owner.Commit();
            REQUIRE(owner.scene.TakeStructuralCommitResult().has_value());
            REQUIRE(owner.scene.ActiveScene()->Get(entity).Value().localTransform->translation.x == 7.0F);
            REQUIRE(handler->calls == 2);
        }
    }

    TEST_CASE("Impaired transport routes admitted RPCs through envelope replay to the Gameplay transaction", "[unit][network][rpc]") {
        for (const bool lost : {false, true}) {
            Fixture fixture(false, RpcTarget::Authority, true, true);
            const auto protocol = TestSupport::WireIdentity<ProtocolId>(1);
            const auto messageId = TestSupport::WireIdentity<MessageTypeId>(2);
            const auto schema = TestSupport::WireIdentity<MessageSchemaId>(3);
            ImpairedTransport transport(lost);
            auto router = fixture.Router(transport);
            const auto &codecs = *fixture.envelopeCodecs;
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
