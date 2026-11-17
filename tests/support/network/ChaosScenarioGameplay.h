#pragma once
#include "ChaosScenarioAdmission.h"
#include "Horo/Network/SceneReplicationApplyOwner.h"
#include "ReplicationStateCodecTestSupport.h"
#include "RpcGameplayDispatchTestSupport.h"

namespace Horo::Network::ChaosTests {
    /** @brief Real replica Scene and RPC safe-point consumers; neither constructs a session. */
    struct Gameplay final {
        CaptureTestSupport::Fixture sender;
        ReplicationWorldLifecycle world{std::move(ReplicationWorldLifecycle::Create()).Value()};
        std::shared_ptr<Runtime::RuntimeScene> scene;
        std::shared_ptr<SceneReplicationApplyOwner> owner;
        std::shared_ptr<ReplicationRoleState> role;
        std::shared_ptr<ReplicationStateCodec> codec;
        std::shared_ptr<ReplicationInboundApply> inbound;
        std::shared_ptr<RpcGameplayDispatch> rpc;
        std::shared_ptr<RpcDispatchTestSupport::Handler> handler{std::make_shared<RpcDispatchTestSupport::Handler>()};
        std::shared_ptr<const void> module{std::make_shared<int>(1)};
        const RpcId rpcId{RpcId::Create(42).Value()};
        ConnectionHandle bound;
        std::uint64_t applied{};

        Gameplay() {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, {1}};
            builder.Add(Runtime::RuntimeEntityDefinition{.object = Runtime::SceneObjectId{1}});
            scene = std::shared_ptr<Runtime::RuntimeScene>{
                std::move(Runtime::RuntimeScene::Create(std::move(builder).Build().Value(), CaptureTestSupport::World().scene)).Value()};
            const std::array bindings{
                SceneReplicationFieldBinding{CaptureTestSupport::FieldIdValue(1), SceneReplicationProperty::TranslationX}};
            owner = SceneReplicationApplyOwner::Create(scene, CaptureTestSupport::SchemaId(10), {1, 0}, bindings).Value();
            const auto active = CaptureTestSupport::World(1, ReplicationExecutionRole::SimulatedClient);
            REQUIRE(world.Stage(active).HasValue());
            REQUIRE(world.CommitAtSafePoint(active.scene, active.session).HasValue());
            REQUIRE(world.RegisterObject(active.scene, active.session, CaptureTestSupport::Object()).HasValue());
            auto binding = StateCodecTestSupport::Recipient();
            role = std::make_shared<ReplicationRoleState>(std::move(ReplicationRoleState::Create(binding)).Value());
            codec = std::shared_ptr<ReplicationStateCodec>{
                std::move(ReplicationStateCodec::Create(sender.registry, binding, ReplicationRecordKind::Update, 1)).Value()};
            BuildRpc();
        }

        void BuildRpc() {
            const std::array descriptors{RpcDispatchTestSupport::Descriptor(rpcId, true, RpcTarget::AllClients, false)};
            const auto snapshot = BuildRpcDescriptorSnapshot(descriptors, {}).Value();
            rpc = RpcGameplayDispatch::Create(snapshot, world).Value();
            REQUIRE(rpc->RegisterObject(CaptureTestSupport::Object().object, role).HasValue());
            REQUIRE(rpc->RegisterHandler(rpcId, handler, {}, module).HasValue());
        }

        void Bind(const Projection &projection, const Admission &admission, InboundMessageDispatcher &router, const std::uint64_t tick) {
            Retire();
            router.RevokeHandler(admission.protocols[0].id, admission.messages[5].id);
            bound = projection.current->Connection();
            const auto active = CaptureTestSupport::World();
            inbound = ReplicationInboundApply::Create({projection.current, bound, projection.generation, active.scene, active.session,
                                                       active.authority, *StateCodecTestSupport::Recipient().localPeer,
                                                       admission.protocols[0].id, admission.messages[5].id},
                                                      world, owner, tick)
                          .Value();
            REQUIRE(inbound->RegisterObject(CaptureTestSupport::Object(), role, codec).HasValue());
            REQUIRE(router
                        .RegisterHandler({admission.protocols[0].id, admission.messages[5].id, MessageTrafficClass::Command,
                                          Runtime::RuntimePhase::NetworkPoll, 64},
                                         inbound)
                        .HasValue());
            REQUIRE(rpc->RegisterPeer(projection.current, bound, projection.generation, NetworkPeerId::Create(90).Value(),
                                      RpcRemoteRole::Authority, tick, *StateCodecTestSupport::Recipient().localPeer)
                        .HasValue());
        }

        void Retire() {
            if (bound.IsValid())
                rpc->RevokePeer(bound);
            if (inbound)
                REQUIRE(inbound->Shutdown().HasValue());
            inbound.reset();
            bound = {};
        }

        std::vector<std::byte> Replica(const std::uint64_t tick, const double value) {
            static_cast<void>(sender.Capture(tick, value));
            return codec->Encode(sender.Pin()).Value();
        }

        std::vector<std::byte> Rpc(const std::uint64_t sequence) const {
            std::vector<std::byte> payload;
            const auto object = CaptureTestSupport::Object().object;
            RpcDispatchTestSupport::Append(payload, rpcId.Value());
            RpcDispatchTestSupport::Append(payload, object.Epoch().Value());
            RpcDispatchTestSupport::Append(payload, object.Slot());
            RpcDispatchTestSupport::Append(payload, object.Generation());
            RpcDispatchTestSupport::Append<std::uint16_t>(payload, 1);
            RpcDispatchTestSupport::Append<std::uint16_t>(payload, 0);
            RpcDispatchTestSupport::Append(payload, sequence);
            RpcDispatchTestSupport::Append(payload, StateCodecTestSupport::Recipient().localPeer->Value());
            RpcDispatchTestSupport::Append<std::uint16_t>(payload, 0);
            return payload;
        }

        void Commit(const std::uint64_t tick) {
            const auto active = CaptureTestSupport::World();
            const auto dispatched =
                rpc->DrainAtGameplaySafePoint({active.scene, active.session, Runtime::RuntimePhase::FixedUpdate, tick, {}});
            REQUIRE(dispatched.HasValue());
            if (!inbound)
                return;
            const auto result =
                inbound->ApplyAtSafePoint({active.scene, active.session, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, tick, {}},
                                          tick);
            REQUIRE(result.HasValue());
            applied += result.Value().applied;
        }

        float Value() const {
            return scene->View().Get(CaptureTestSupport::Object().entity).Value().localTransform->translation.x;
        }
    };
}  // namespace Horo::Network::ChaosTests
