#include "Horo/Network/SceneReplicationApplyOwner.h"
#include "Horo/Runtime/RuntimeLifecycle.h"
#include "ReplicationStateCodecTestSupport.h"
#include "RpcGameplayDispatchTestSupport.h"

#include <limits>
#include <thread>

namespace Horo::Network {
    namespace {
        using namespace StateCodecTestSupport;

        /** @brief One-frame monotonic clock; the integration exercises phase dispatch without elapsed fixed steps. */
        class ApplyTestClock final : public Clock {
        public:
            Duration MonotonicNow() const override {
                return {};
            }
        };

        /** @brief Real receiving Scene plus server capture/codec and authenticated inbound evidence. */
        struct ApplyFixture final {
            RpcDispatchTestSupport::Fixture peer{true, RpcTarget::AllClients, false};
            Fixture sender;
            std::shared_ptr<Owner> secondSource{std::make_shared<Owner>()};
            ReplicationWorldLifecycle world{std::move(ReplicationWorldLifecycle::Create()).Value()};
            std::shared_ptr<Runtime::RuntimeScene> scene;
            std::shared_ptr<SceneReplicationApplyOwner> owner;
            std::shared_ptr<ReplicationInboundApply> inbound;
            std::array<std::shared_ptr<ReplicationRoleState>, 2> roles;
            std::array<std::shared_ptr<ReplicationStateCodec>, 2> codecs;

            explicit ApplyFixture(const ReplicationInboundLimits &limits = {}) {
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
                inbound =
                    ReplicationInboundApply::Create({peer.session, peer.connection, peer.generation, World().scene, World().session,
                                                     World().authority, *Recipient().localPeer, TestSupport::WireIdentity<ProtocolId>(1),
                                                     TestSupport::WireIdentity<MessageTypeId>(2)},
                                                    world, owner, 22, limits)
                        .Value();
                REQUIRE(sender.lifecycle.RegisterObject(World().scene, World().session, Object(2)).HasValue());
                const std::array targets{ReplicationCaptureTarget{Object(), sender.owner},
                                         ReplicationCaptureTarget{Object(2), secondSource}};
                sender.capture = std::move(ReplicationStateCapture::Prepare(Read(sender.lifecycle), sender.registry, targets)).Value();
                for (std::size_t index{}; index < 2; ++index) {
                    const auto mapping = Object(index + 1);
                    REQUIRE(world.RegisterObject(World().scene, World().session, mapping).HasValue());
                    auto role = Recipient();
                    role.object = mapping.object;
                    roles[index] = std::make_shared<ReplicationRoleState>(std::move(ReplicationRoleState::Create(role)).Value());
                    codecs[index] = std::shared_ptr<ReplicationStateCodec>{
                        std::move(ReplicationStateCodec::Create(sender.registry, role, ReplicationRecordKind::Update, 1)).Value()};
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

        /** @brief Explicit host composition routes receipt and commits only at the scheduler's declared safe point. */
        class ApplyParticipant final : public Runtime::RuntimeLifecycleParticipant {
        public:
            ApplyParticipant(ApplyFixture &fixture, InboundMessageDispatcher &router) : fixture_(fixture), router_(router) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            Result<void> OnPhase(const Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
                if (phase == Runtime::RuntimePhase::NetworkPoll) {
                    auto polled = router_.RunNetworkPoll(22, context.cancellation);
                    if (polled.HasError())
                        return Result<void>::Failure(polled.ErrorValue());
                    REQUIRE(fixture_.Value(0) == 0);
                }
                if (phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges) {
                    auto work = fixture_.Work(context.cancellation);
                    work.phase = phase;
                    work.simulationTick = context.completedSimulationTick + 1;
                    auto result = fixture_.inbound->ApplyAtSafePoint(work, 23);
                    if (result.HasError())
                        return Result<void>::Failure(result.ErrorValue());
                    applied = result.Value().applied;
                }
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                router_.Shutdown();
            }

            std::size_t applied{};

        private:
            ApplyFixture &fixture_;
            InboundMessageDispatcher &router_;
        };

        /** @brief Exercises foreign owner preparation reentrancy without granting commit authority on rejection. */
        class CallbackOwner final : public IReplicationApplyOwner {
        public:
            std::shared_ptr<SceneReplicationApplyOwner> target;
            std::function<void()> callback;
            bool throwFault{};

            Result<std::unique_ptr<IReplicationApplyCandidate>> Prepare(const std::span<const ReplicationApplyUpdate> updates) override {
                auto candidate = target->Prepare(updates);
                if (callback)
                    callback();
                if (throwFault)
                    throw CaptureTestSupport::ForeignCallbackFault{};
                return candidate;
            }
        };

        /** @brief Binds one foreign owner to the fixture's exact authority and first object. */
        std::shared_ptr<CallbackOwner> BindCallbackOwner(ApplyFixture &fixture) {
            auto owner = std::make_shared<CallbackOwner>();
            owner->target = fixture.owner;
            const auto authority = ReplicationInboundAuthority{fixture.peer.session,
                                                               fixture.peer.connection,
                                                               fixture.peer.generation,
                                                               World().scene,
                                                               World().session,
                                                               World().authority,
                                                               *Recipient().localPeer,
                                                               TestSupport::WireIdentity<ProtocolId>(1),
                                                               TestSupport::WireIdentity<MessageTypeId>(2)};
            fixture.inbound = ReplicationInboundApply::Create(authority, fixture.world, owner, 22).Value();
            REQUIRE(fixture.inbound->RegisterObject(Object(), fixture.roles[0], fixture.codecs[0]).HasValue());
            return owner;
        }

        /** @brief Verifies typed containment clears the pending projection and releases the busy guard. */
        void CheckDiscardedOwnerFault(ApplyFixture &fixture, CallbackOwner &owner, const Result<ReplicationApplyReport> &result,
                                      std::string_view expected) {
            CHECK(result.ErrorValue().code.Value() == expected);
            owner.throwFault = false;
            owner.callback = {};
            const auto retry = fixture.Apply();
            REQUIRE(retry.HasValue());
            CHECK(retry.Value().applied == 0);
            CHECK(fixture.Value(0) == 0);
        }
    }  // namespace

    TEST_CASE("Inbound receipt stages complete replicas and one Scene safe point atomically applies all objects", "[network][apply]") {
        ApplyFixture fixture;
        fixture.Capture(1, 3.0, 4.0);
        const auto first = fixture.Message(0);
        REQUIRE(fixture.inbound->Handle(first).HasError());
        REQUIRE(fixture.Stage(first).HasValue());
        REQUIRE(fixture.Stage(fixture.Message(1)).HasValue());
        REQUIRE(fixture.Value(0) == 0);
        REQUIRE(fixture.Value(1) == 0);
        auto wrongPhase = fixture.Work();
        wrongPhase.phase = Runtime::RuntimePhase::NetworkPoll;
        REQUIRE(fixture.inbound->ApplyAtSafePoint(wrongPhase, 23).HasError());
        REQUIRE(fixture.Apply().Value().applied == 2);
        REQUIRE(fixture.Value(0) == 3);
        REQUIRE(fixture.Value(1) == 4);
        REQUIRE(fixture.Stage(first).HasValue());
        REQUIRE(fixture.Apply().Value().applied == 0);
    }

    TEST_CASE("Impaired transport receipt waits for the actual runtime scheduler Scene safe point", "[network][apply][runtime]") {
        for (const bool lost : {false, true}) {
            ApplyFixture fixture;
            fixture.Capture(1, 7.0, 8.0);
            RpcDispatchTestSupport::ImpairedTransport transport(lost);
            auto router = fixture.peer.Router(transport);
            const auto protocol = TestSupport::WireIdentity<ProtocolId>(1);
            const auto messageId = TestSupport::WireIdentity<MessageTypeId>(2);
            router->RevokeHandler(protocol, messageId);
            router->RevokeSession(fixture.peer.connection);
            REQUIRE(router
                        ->RegisterHandler({protocol, messageId, MessageTrafficClass::Snapshot, Runtime::RuntimePhase::NetworkPoll, 16},
                                          fixture.inbound)
                        .HasValue());
            TransportSelectionEvidence selection;
            selection.capabilityRevision = 3;
            selection.channelCount = 1;
            selection.maximumMessageBytes = 1200;
            selection.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            auto gate = MessageDeliveryGate::Create(selection, 3, fixture.peer.connection, fixture.peer.generation,
                                                    {{ChannelId{}, DeliveryPolicy::ReliableOrdered, MessageTrafficClass::Snapshot, false}});
            REQUIRE(gate.HasValue());
            REQUIRE(
                router->RegisterSession(fixture.peer.session, fixture.peer.connection, fixture.peer.generation, std::move(gate).Value(), 22)
                    .HasValue());
            auto envelope = fixture.Message(0);
            envelope.schema = TestSupport::WireIdentity<MessageSchemaId>(3);
            envelope.schemaVersion = {1, 1};
            envelope.sequence = MessageSequenceNumber{1};
            const auto bytes = EncodeMessageEnvelope(envelope, *fixture.peer.envelopeCodecs).Value();
            REQUIRE(transport.Send(fixture.peer.connection, ChannelId{}, bytes, DeliveryPolicy::ReliableOrdered).HasValue());
            ApplyTestClock clock;
            auto scheduler = std::move(Runtime::FrameScheduler::Create(clock)).Value();
            Runtime::RuntimeLifecycle lifecycle;
            auto participant = std::make_unique<ApplyParticipant>(fixture, *router);
            const auto *observed = participant.get();
            REQUIRE(lifecycle.AddParticipant(std::move(participant)).HasValue());
            REQUIRE(lifecycle.Startup({}).HasValue());
            REQUIRE(scheduler->RunFrame(lifecycle, {}, false).HasValue());
            REQUIRE(observed->applied == (lost ? 0 : 1));
            REQUIRE(fixture.Value(0) == (lost ? 0 : 7));
            lifecycle.Shutdown();
            transport.Shutdown();
        }
    }

    TEST_CASE("A semantically invalid later object rejects the entire received Scene batch", "[network][apply]") {
        ApplyFixture fixture;
        fixture.Capture(1, 3.0, 1e100);
        REQUIRE(fixture.Stage(fixture.Message(0)).HasValue());
        REQUIRE(fixture.Stage(fixture.Message(1)).HasValue());
        REQUIRE(fixture.Apply().HasError());
        REQUIRE(fixture.Value(0) == 0);
        REQUIRE(fixture.Value(1) == 0);
        fixture.Capture(2, 5.0, 6.0);
        REQUIRE(fixture.Stage(fixture.Message(0)).HasValue());
        REQUIRE(fixture.Stage(fixture.Message(1)).HasValue());
        REQUIRE(fixture.Apply().Value().applied == 2);
        REQUIRE(fixture.Value(0) == 5);
        REQUIRE(fixture.Value(1) == 6);
    }

    TEST_CASE("Malformed incompatible and lost delta records preserve prior state and duplicate receipt is idempotent",
              "[network][apply]") {
        ApplyFixture fixture;
        fixture.Capture(1, 1.0, 2.0);
        const auto initial = fixture.Message(0);
        const auto first = fixture.sender.capture->Latest(Object().object).Value();
        ReplicationAcknowledgedBaseline baseline{first, first->PublicationRevision(), Recipient().revision,
                                                 fixture.codecs[0]->ProjectionFingerprint(), 1};
        fixture.Capture(2, 8.0, 9.0);
        const auto delta = fixture.Message(0, baseline);
        REQUIRE(fixture.Stage(delta).HasError());
        REQUIRE(fixture.Value(0) == 0);
        REQUIRE(fixture.Stage(initial).HasValue());
        REQUIRE(fixture.Apply().Value().applied == 1);
        for (const auto length : {std::size_t{0}, std::size_t{171}, initial.payload.size() - 1}) {
            auto malformed = initial;
            malformed.payload.resize(length);
            REQUIRE(fixture.Stage(malformed).HasError());
        }
        auto incompatible = initial;
        incompatible.payload[16] ^= std::byte{1};
        REQUIRE(fixture.Stage(incompatible).HasError());
        REQUIRE(fixture.Value(0) == 1);
        REQUIRE(fixture.Stage(delta).HasValue());
        REQUIRE(fixture.Stage(delta).HasValue());
        REQUIRE(fixture.Apply().Value().applied == 1);
        REQUIRE(fixture.Value(0) == 8);
        REQUIRE(fixture.Stage(initial).HasError());
        REQUIRE(fixture.Stage(delta).HasValue());
        REQUIRE(fixture.Apply().Value().applied == 0);
    }

    TEST_CASE("Safe-point role mapping and actual entity generations reject stale queued updates", "[network][apply]") {
        SECTION("Role publication changes") {
            ApplyFixture fixture;
            fixture.Capture(1, 3.0, 4.0);
            REQUIRE(fixture.Stage(fixture.Message(0)).HasValue());
            auto role = fixture.roles[0]->Snapshot().Value();
            role.revision = ReplicationRoleRevision::Create(2).Value();
            REQUIRE(fixture.roles[0]->Stage({Recipient().revision, role}).HasValue());
            REQUIRE(fixture.roles[0]->CommitAtSafePoint(role.revision).HasValue());
            REQUIRE(fixture.Apply().HasError());
            REQUIRE(fixture.Value(0) == 0);
        }
        SECTION("Mapping retires") {
            ApplyFixture fixture;
            fixture.Capture(1, 3.0, 4.0);
            REQUIRE(fixture.Stage(fixture.Message(0)).HasValue());
            REQUIRE(fixture.world.RetireObject(World().scene, World().session, Object().object).HasValue());
            REQUIRE(fixture.Apply().HasError());
            REQUIRE(fixture.Value(0) == 0);
        }
        SECTION("Scene entity retires without a mapping update") {
            ApplyFixture fixture;
            fixture.Capture(1, 3.0, 4.0);
            REQUIRE(fixture.Stage(fixture.Message(0)).HasValue());
            REQUIRE(fixture.Stage(fixture.Message(1)).HasValue());
            Runtime::SceneCommandBuffer destroy;
            destroy.Destroy(Object(2).entity);
            REQUIRE(fixture.scene->Commit(destroy).HasValue());
            REQUIRE(fixture.Apply().HasError());
            REQUIRE(fixture.Value(0) == 0);
        }
    }

    TEST_CASE("Authority session world shutdown and codec retirement fence receive and safe-point apply", "[network][apply]") {
        ApplyFixture fixture;
        fixture.Capture(1, 3.0, 4.0);
        const auto wire = fixture.Message(0);
        auto stale = fixture.peer.Context();
        stale.generation = TestSupport::Session(99);
        REQUIRE(fixture.inbound->HandleAdmitted(stale, wire).HasError());
        REQUIRE(fixture.Stage(wire).HasValue());
        SECTION("Codec retires") {
            fixture.codecs[0]->Shutdown();
            REQUIRE(fixture.Apply().HasError());
        }
        SECTION("Session shuts down") {
            REQUIRE(fixture.peer.session->Shutdown(23));
            REQUIRE(fixture.Apply().HasError());
        }
        SECTION("World retires") {
            REQUIRE(fixture.world.Unload(World().scene, World().session).HasValue());
            REQUIRE(fixture.Apply().HasError());
        }
        SECTION("Explicit route shuts down") {
            REQUIRE(fixture.inbound->Shutdown().HasValue());
            REQUIRE(fixture.inbound->Shutdown().HasValue());
            REQUIRE(fixture.Stage(wire).HasError());
            REQUIRE(fixture.Apply().HasError());
        }
        REQUIRE(fixture.Value(0) == 0);
    }

    TEST_CASE("Receipt cancellation and safe-point cancellation never publish a partial batch", "[network][apply]") {
        ApplyFixture fixture;
        fixture.Capture(1, 3.0, 4.0);
        CancellationSource receipt;
        REQUIRE(fixture.Stage(fixture.Message(0), receipt.Token()).HasValue());
        REQUIRE(fixture.Stage(fixture.Message(1)).HasValue());
        receipt.RequestCancellation();
        REQUIRE(fixture.Apply().HasError());
        REQUIRE(fixture.Value(0) == 0);
        REQUIRE(fixture.Value(1) == 0);
        REQUIRE(fixture.Stage(fixture.Message(1)).HasValue());
        CancellationSource apply;
        apply.RequestCancellation();
        REQUIRE(fixture.Apply(apply.Token()).HasError());
        REQUIRE(fixture.Value(1) == 0);
        REQUIRE(fixture.Apply().Value().applied == 1);
    }

    TEST_CASE("Owner preparation reentrancy cancellation and exceptions discard detached candidates", "[network][apply]") {
        ApplyFixture fixture;
        auto owner = BindCallbackOwner(fixture);
        fixture.Capture(1, 3.0, 4.0);
        REQUIRE(fixture.Stage(fixture.Message(0)).HasValue());
        CancellationSource cancellation;
        std::string_view expectedFault;
        SECTION("Cancellation during preparation") {
            owner->callback = [&] {
                cancellation.RequestCancellation();
            };
        }
        SECTION("Shutdown during preparation") {
            owner->callback = [&] {
                REQUIRE(fixture.inbound->Shutdown().HasValue());
            };
        }
        SECTION("Nested safe point") {
            owner->callback = [&] {
                REQUIRE(fixture.Apply().HasError());
                REQUIRE(fixture.inbound->RevokeObject(Object().object).HasValue());
            };
        }
        SECTION("Non-standard owner exception") {
            owner->throwFault = true;
            expectedFault = ReplicationStateErrors::CallbackFault.code.Value();
        }
        SECTION("Standard owner exception") {
            owner->callback = [] {
                throw std::runtime_error("owner fault");
            };
            expectedFault = ReplicationStateErrors::CallbackFault.code.Value();
        }
        SECTION("Owner allocation failure") {
            owner->callback = [] {
                throw std::bad_alloc{};
            };
            expectedFault = ReplicationStateErrors::Capacity.code.Value();
        }
        const auto result = fixture.Apply(cancellation.Token());
        REQUIRE(result.HasError());
        REQUIRE(fixture.Value(0) == 0);
        if (!expectedFault.empty())
            CheckDiscardedOwnerFault(fixture, *owner, result, expectedFault);
    }

    TEST_CASE("Inbound bounds and owner affinity reject excess work while preserving an accepted projection", "[network][apply]") {
        ApplyFixture fixture{{2, 1, 4096, 1024}};
        fixture.Capture(1, 3.0, 4.0);
        REQUIRE(fixture.Stage(fixture.Message(0)).HasValue());
        REQUIRE(fixture.Stage(fixture.Message(1)).HasError());
        const auto message = fixture.Message(0);
        bool rejected{};
        std::thread foreign{[&] {
            rejected = fixture.Stage(message).HasError();
        }};
        foreign.join();
        REQUIRE(rejected);
        REQUIRE(fixture.Apply().Value().applied == 1);
        REQUIRE(fixture.Value(0) == 3);
        REQUIRE(fixture.Value(1) == 0);
    }

    TEST_CASE("Pending delta roots supersede staged projections and retained-byte saturation preserves local state", "[network][apply]") {
        SECTION("A delta uses the complete pending root before publication") {
            ApplyFixture fixture;
            fixture.Capture(1, 3.0, 4.0);
            const auto first = fixture.sender.capture->Latest(Object().object).Value();
            REQUIRE(fixture.Stage(fixture.Message(0)).HasValue());
            const ReplicationAcknowledgedBaseline baseline{first, first->PublicationRevision(), Recipient().revision,
                                                           fixture.codecs[0]->ProjectionFingerprint(), 1};
            fixture.Capture(2, 9.0, 5.0);
            REQUIRE(fixture.Stage(fixture.Message(0, baseline)).HasValue());
            REQUIRE(fixture.Value(0) == 0);
            REQUIRE(fixture.Apply().Value().applied == 1);
            REQUIRE(fixture.Value(0) == 9);
        }
        SECTION("Persistent storage has an independent byte ceiling") {
            ApplyFixture fixture{{2, 2, 1, 1024}};
            fixture.Capture(1, 3.0, 4.0);
            REQUIRE(fixture.Stage(fixture.Message(0)).HasError());
            REQUIRE(fixture.Apply().Value().applied == 0);
            REQUIRE(fixture.Value(0) == 0);
        }
    }

    TEST_CASE("Scene transform admission samples cancellation and exact Scene identity at publication", "[network][apply][scene]") {
        ApplyFixture fixture;
        Runtime::SceneCommandBuffer batch;
        Math::Transform changed;
        changed.translation.x = 9;
        CancellationSource cancellation;
        batch.SetLocalTransform(Object().entity, changed, {World().scene, {}, cancellation.Token(), {}, {}});
        batch.SetLocalTransform(Object(2).entity, changed);
        cancellation.RequestCancellation();
        REQUIRE(fixture.scene->Commit(batch).HasError());
        REQUIRE(fixture.Value(0) == 0);
        REQUIRE(fixture.Value(1) == 0);
        Runtime::SceneCommandBuffer foreign;
        foreign.SetLocalTransform(Object().entity, changed, {Runtime::SceneRuntimeId{99}, {}, {}, {}, {}});
        REQUIRE(fixture.scene->Commit(foreign).HasError());
        REQUIRE(fixture.Value(0) == 0);
    }
}  // namespace Horo::Network
