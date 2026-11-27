#pragma once

#include "Horo/Network/SceneReplicationApplyOwner.h"
#include "ReplicationStateCodecTestSupport.h"
#include "RpcGameplayDispatchTestSupport.h"

namespace Horo::Network::InboundApplyTestSupport {
    using namespace StateCodecTestSupport;

    /** @brief Real receiving Scene plus server capture/codec and authenticated inbound evidence. */
    struct ApplyFixture {
        RpcDispatchTestSupport::Fixture peer{true, RpcTarget::AllClients, false};
        Fixture sender;
        std::shared_ptr<Owner> secondSource{std::make_shared<Owner>()};
        ReplicationWorldLifecycle world{std::move(ReplicationWorldLifecycle::Create()).Value()};
        std::shared_ptr<Runtime::RuntimeScene> scene;
        std::shared_ptr<SceneReplicationApplyOwner> owner;
        std::shared_ptr<ReplicationInboundApply> inbound;
        std::array<std::shared_ptr<ReplicationRoleState>, 2> roles;
        std::array<std::shared_ptr<ReplicationStateCodec>, 2> codecs;

        ReplicationInboundAuthority Authority() const {
            return {peer.session,
                    peer.connection,
                    peer.generation,
                    World().scene,
                    World().session,
                    World().authority,
                    *Recipient().localPeer,
                    TestSupport::WireIdentity<ProtocolId>(1),
                    TestSupport::WireIdentity<MessageTypeId>(2)};
        }

        explicit ApplyFixture(const ReplicationInboundLimits &limits = {},
                              ReplicationInboundApplyMode mode = ReplicationInboundApplyMode::Incremental) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, {1}};
            for (std::uint64_t slot = 1; slot <= 2; ++slot) {
                Runtime::RuntimeEntityDefinition entity;
                entity.object = Runtime::SceneObjectId{slot};
                builder.Add(entity);
            }
            scene = std::shared_ptr<Runtime::RuntimeScene>{
                std::move(Runtime::RuntimeScene::Create(std::move(builder).Build().Value(), World().scene)).Value()};
            const std::array bindings{SceneReplicationFieldBinding{FieldIdValue(1), SceneReplicationProperty::TranslationX}};
            owner = SceneReplicationApplyOwner::Create(scene, SchemaId(10), {1, 0}, bindings).Value();
            REQUIRE(world.Stage(World(1, ReplicationExecutionRole::SimulatedClient)).HasValue());
            REQUIRE(world.CommitAtSafePoint(World().scene, World().session).HasValue());
            std::array mappings{Object(), Object(2)};
            const bool initial = mode == ReplicationInboundApplyMode::InitialSnapshot;
            if (initial) {
                mappings[0].provenance.authoredObject = Runtime::SceneObjectId{1};
                mappings[1].provenance.prefab =
                    NetworkPrefabMemberProvenance{{{Assets::AssetId::FromBytes({5}), Sha256Digest{{1}}, 1}, 0}, mappings[1].object};
            }
            for (const auto &mapping : mappings)
                REQUIRE(world.RegisterObject(World().scene, World().session, mapping).HasValue());
            inbound = ReplicationInboundApply::Create(Authority(), world, owner, 22, limits, mode).Value();
            REQUIRE(sender.lifecycle.RegisterObject(World().scene, World().session, Object(2)).HasValue());
            const std::array targets{ReplicationCaptureTarget{Object(), sender.owner}, ReplicationCaptureTarget{Object(2), secondSource}};
            sender.capture = std::move(ReplicationStateCapture::Prepare(Read(sender.lifecycle), sender.registry, targets)).Value();
            for (std::size_t index{}; index < 2; ++index) {
                const auto &mapping = mappings[index];
                auto role = Recipient();
                role.object = mapping.object;
                roles[index] = std::make_shared<ReplicationRoleState>(std::move(ReplicationRoleState::Create(role)).Value());
                codecs[index] = std::shared_ptr<ReplicationStateCodec>{
                    std::move(ReplicationStateCodec::Create(sender.registry, role,
                                                            initial ? ReplicationRecordKind::Spawn : ReplicationRecordKind::Update, 1))
                        .Value()};
                REQUIRE(inbound->RegisterObject(mapping, roles[index], codecs[index]).HasValue());
            }
        }

        void Capture(const std::uint64_t tick, const double first, const double second) {
            sender.owner->Commit(tick, first);
            secondSource->Commit(tick, second);
            REQUIRE(sender.capture->CaptureAtCommit(Read(sender.lifecycle, tick), tick).Value().published == 2);
        }

        MessageEnvelope Message(const std::size_t index, const ReplicationAcknowledgedBaseline &baseline = {}) {
            MessageEnvelope message;
            message.protocol = TestSupport::WireIdentity<ProtocolId>(1);
            message.message = TestSupport::WireIdentity<MessageTypeId>(2);
            message.payload = codecs[index]->Encode(sender.capture->Latest(Object(index + 1).object).Value(), baseline).Value();
            return message;
        }

        Result<void> Stage(const MessageEnvelope &message, const CancellationToken &cancellation = {}) {
            return inbound->HandleAdmitted(peer.Context(cancellation), message);
        }

        ReplicationWorldWorkRequest Work(const CancellationToken &cancellation = {}) const {
            return {World().scene, World().session, Runtime::RuntimePhase::CommitDeferredLifecycleChanges, 1, cancellation};
        }

        Result<ReplicationApplyReport> Apply(const CancellationToken &cancellation = {}) {
            return inbound->ApplyAtSafePoint(Work(cancellation), 23);
        }

        float Value(const std::size_t index) const {
            return scene->View().Get(Object(index + 1).entity).Value().localTransform->translation.x;
        }
    };
}  // namespace Horo::Network::InboundApplyTestSupport
