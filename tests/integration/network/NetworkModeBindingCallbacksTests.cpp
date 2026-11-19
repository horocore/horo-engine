#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/Platform.h"
#include "NetworkModeHostTestSupport.h"

namespace Horo::Network {
    namespace {
        constexpr auto CommitPhase = Runtime::RuntimePhase::CommitDeferredLifecycleChanges;

        struct BindingService final : INetworkModeService {
            BindingService(NetworkModeServiceKind kind, const bool *failScene) : kind(kind), failScene(failScene) {}

            Result<void> Prepare() override {
                if (kind == NetworkModeServiceKind::Scene && failScene && *failScene)
                    return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
                return Result<void>::Success();
            }

            Result<void> Activate() override {
                return Result<void>::Success();
            }

            Result<void> RunPhase(Runtime::RuntimePhase) override {
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {}

            NetworkModeServiceKind kind;
            const bool *failScene;
        };

        NetworkModeComposition ListenMode(const NetworkWorldBindingSetup &setup, const bool *failScene = nullptr,
                                          NetworkClientPrefabInbox *clientInbox = nullptr,
                                          NetworkAuthorityObjectPublication *authorityObjects = nullptr) {
            NetworkModePlan plan;
            plan.mode = NetworkProjectRole::ListenServer;
            plan.hostGeneration = 1;
            plan.build = TestSupport::Id<NetworkProductBuildId>(1);
            plan.productRevision = TestSupport::Id<NetworkProductCapabilityRevision>(1);
            plan.hostRevision = TestSupport::Id<NetworkHostCapabilityRevision>(1);
            plan.projectRevision = TestSupport::Id<NetworkProjectSettingsRevision>(1);
            plan.provider = TestSupport::Id<NetworkTransportProviderId>(1);
            plan.protocolVersion = {1, 0};
            plan.presentation.localPlayer = true;
            plan.worlds = {NetworkModeWorld{NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{1}},
                           NetworkModeWorld{NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{2}}};
            plan.worldCount = 2;
            NetworkModeFactories factories;
            for (auto &factory : factories.services)
                factory = [failScene, clientInbox,
                           authorityObjects](NetworkModeServiceRequest request) -> Result<std::unique_ptr<INetworkModeService>> {
                    if (clientInbox && request.prefabInbox.IsBound()) {
                        CHECK(request.service == NetworkModeServiceKind::Scene);
                        CHECK(request.world == NetworkModeWorldKind::Client);
                        *clientInbox = request.prefabInbox;
                    }
                    if (authorityObjects && request.authorityObjects.IsBound())
                        *authorityObjects = request.authorityObjects;
                    return Result<std::unique_ptr<INetworkModeService>>::Success(
                        std::make_unique<BindingService>(request.service, failScene));
                };
            auto created = NetworkModeComposition::Create(plan, std::move(factories), NetworkWorldCompositionPurpose::Gameplay, setup);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        struct GenericControl final : IInboundMessageHandler {
            Result<void> Handle(const MessageEnvelope &) override {
                return Result<void>::Success();
            }
        };

        MessageEnvelopePayloadView ControlPayload(const std::span<const std::byte> payload) {
            return {TestSupport::WireIdentity<ProtocolId>(1),
                    TestSupport::WireIdentity<MessageTypeId>(12),
                    TestSupport::WireIdentity<MessageSchemaId>(13),
                    {1, 0},
                    {},
                    {},
                    0,
                    payload};
        }

        void RegisterControl(ModeBindingTests::BindingFixture &fixture, const std::shared_ptr<GenericControl> &handler,
                             const std::uint32_t maximumPayloadBytes = 4) {
            REQUIRE(fixture.dispatcher
                        ->RegisterHandler({TestSupport::WireIdentity<ProtocolId>(1), TestSupport::WireIdentity<MessageTypeId>(12),
                                           MessageTrafficClass::Control, Runtime::RuntimePhase::NetworkPoll, maximumPayloadBytes},
                                          handler)
                        .HasValue());
        }

        /** @brief Retain the actual product session clock and observer owner for callback admission. */
        std::shared_ptr<ProductOwners> SessionOwners() {
            auto owners = std::make_shared<ProductOwners>();
            owners->sessionSource = std::make_shared<DeterministicClock>();
            owners->sessionClock =
                std::make_shared<Application::Internal::NetworkProductSessionClock>(owners->sessionSource, Duration::FromNanoseconds(1),
                                                                                    Duration{});
            owners->sessions = std::make_shared<Application::Internal::ClientSessionOwner>(owners->sessionClock);
            return owners;
        }

        /** @brief Delivers an exact encoded control frame to the real dispatcher and authenticated session gate. */
        InboundDispatchReport DeliverPrefabFrame(ModeBindingTests::BindingFixture &fixture, ConnectionHandle connection,
                                                 std::span<const std::byte> bytes) {
            fixture.transport.event = {.kind = NetworkTransportEventKind::PacketReceived,
                                       .connection = connection,
                                       .channel = {},
                                       .delivery = DeliveryPolicy::ReliableOrdered,
                                       .payload = {bytes.begin(), bytes.end()}};
            fixture.transport.queued = true;
            return fixture.dispatcher->RunNetworkPoll(22).Value();
        }

        /** @brief Commits actual Scene groups against the authenticated network binding. */
        struct ClientSceneFixture final {
            JobSystem jobs{{1, 4}};
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider bytes;
            Assets::AssetLoadService loads{jobs, bytes};
            Runtime::RuntimeSceneService scenes;
            CancellationToken cancellation;
            std::uint64_t frame{};

            explicit ClientSceneFixture(Runtime::SceneRuntimeId scene) : scenes(registry, loads, {}, scene) {
                REQUIRE(registry.Publish(std::vector<Assets::AssetRecord>{}).status == Assets::AssetRegistryBuildStatus::Complete);
                REQUIRE(scenes.Startup(cancellation).HasValue());
                Runtime::SceneDefinitionBuilder definition{{1}, {1}};
                REQUIRE(scenes.QueuePreparation(std::move(definition).Build().Value()).HasValue());
                Pump();
            }

            void Pump() {
                REQUIRE(scenes.OnPhase(CommitPhase, {.frameNumber = ++frame, .cancellation = cancellation}).HasValue());
            }

            Runtime::SceneStructuralAdmission Admission() const {
                return {scenes.ActiveScene()->RuntimeId(), registry.Snapshot().Revision(), cancellation};
            }

            Runtime::StructuralCommitResult Publish(Runtime::SceneCommandBuffer commands,
                                                    const std::shared_ptr<const Runtime::SceneGroupPublication> &owner) {
                REQUIRE(commands.BindGroupPublication(owner).HasValue());
                const auto receipt = scenes.QueueTrackedStructuralCommands(std::move(commands)).Value();
                Pump();
                REQUIRE(receipt->ResultValue());
                return *receipt->ResultValue();
            }
        };

    }  // namespace

    TEST_CASE("Prefab ingress transfers only authenticated current-world records to its client Scene",
              "[integration][network][mode][binding][prefab]") {
        auto setup = ModeBindingTests::Setup(160);
        setup.policy.prefabLifecycle = NetworkPrefabLifecyclePolicy{{setup.policy.control.protocol,
                                                                     WireIdentity<MessageTypeId>(12),
                                                                     WireIdentity<MessageSchemaId>(13),
                                                                     {1, 0},
                                                                     false},
                                                                    setup.policy.feature};
        bool missingFeature{};
        bool wrongWorld{};
        bool wrongDirection{};
        bool cancelDelivery{};
        bool delayPublication{};
        SECTION("the exact publisher can stage and transfer a Prefab at the safe point") {}
        SECTION("another world session cannot enqueue work") {
            wrongWorld = true;
        }
        SECTION("a peer without the negotiated feature cannot enqueue work") {
            missingFeature = true;
            setup.policy.prefabLifecycle->feature = WireIdentity<ProtocolFeatureId>(2);
        }
        SECTION("the inbound server-side peer cannot publish a client Prefab") {
            wrongDirection = true;
        }
        SECTION("revoking delivery cancels already staged records") {
            cancelDelivery = true;
        }
        SECTION("ordered Prefab data may follow Initial before Scene publication") {
            delayPublication = true;
        }
        ModeBindingTests::BindingFixture fixture{setup};
        NetworkClientPrefabInbox inbox;
        auto mode = ListenMode(setup, nullptr, &inbox);
        fixture.Attach(mode);
        REQUIRE(mode.Start().HasValue());
        REQUIRE(inbox.IsBound());
        CHECK(inbox.Take(CommitPhase).HasError());
        const auto client = ModeBindingTests::Active(Connection(), Session());
        fixture.StageInitial(mode, client);
        const auto initial = DecodeMessageEnvelope(std::span(fixture.transport.sent).first(fixture.transport.size), *setup.codecs).Value();
        const auto authority = DecodeNetworkWorldBinding(initial.payload).Value();
        if (!delayPublication)
            REQUIRE(mode.RunPhase(CommitPhase).HasValue());
        const auto object = NetworkObjectId::Create(authority.remoteEpoch, 8, 2).Value();
        const auto session =
            wrongWorld ? NetworkSessionGeneration::Create(authority.worldSession.Value() + 1).Value() : authority.worldSession;
        const NetworkPrefabLifecycleRecord record{session, object,
                                                  NetworkPrefabSpawnRecord{{Assets::AssetId::FromBytes({1}), Sha256Digest{{2}}, 1},
                                                                           {{0, object, ReplicationSchemaId::Create(9).Value(), {1, 0}}}}};
        const auto payload = EncodeNetworkPrefabLifecycle(record).Value();
        auto control = ControlPayload(payload);
        if (wrongDirection) {
            fixture.RegisterClient(fixture.server);
        }
        control.sequence = MessageSequenceNumber{wrongDirection ? 1U : 2U};
        std::array<std::byte, 256> encoded{};
        const auto size = EncodeMessageEnvelopePayloadInto(control, *setup.codecs, encoded).Value();
        const auto received = DeliverPrefabFrame(fixture, wrongDirection ? fixture.server->Connection() : client->Connection(),
                                                 std::span(encoded).first(size));
        if (delayPublication) {
            CHECK(inbox.Take(CommitPhase).HasError());
            REQUIRE(mode.RunPhase(CommitPhase).HasValue());
        }
        CHECK(inbox.Take(Runtime::RuntimePhase::NetworkPoll).HasError());
        if (cancelDelivery)
            fixture.dispatcher->RevokeSession(client->Connection());
        auto staged = inbox.Take(CommitPhase).Value();
        if (missingFeature || wrongWorld || wrongDirection) {
            CHECK(received.rejectedPackets == 1);
            CHECK(staged.empty());
            REQUIRE(received.lastPacketRejection);
            const auto &expected = wrongWorld       ? NetworkErrors::ReplicationWorldStale
                                   : wrongDirection ? NetworkErrors::NetworkModeStale
                                                    : NetworkErrors::GameplayDispatchRejected;
            CHECK(received.lastPacketRejection->code.Value() == expected.code.Value());
        } else if (cancelDelivery) {
            CHECK(received.rejectedPackets == 0);
            CHECK(staged.empty());
        } else {
            CHECK(received.rejectedPackets == 0);
            CHECK(staged == std::vector{record});
            CHECK(inbox.Take(CommitPhase).Value().empty());
        }
        mode.Shutdown();
        CHECK(inbox.Take(CommitPhase).HasError());
    }

    TEST_CASE("Authority flush automatically sends committed Prefab deltas after reliable retry",
              "[integration][network][mode][binding][prefab]") {
        auto setup = ModeBindingTests::Setup(1200);
        setup.policy.prefabLifecycle = NetworkPrefabLifecyclePolicy{{setup.policy.control.protocol,
                                                                     WireIdentity<MessageTypeId>(12),
                                                                     WireIdentity<MessageSchemaId>(13),
                                                                     {1, 0},
                                                                     false},
                                                                    setup.policy.feature};
        ModeBindingTests::BindingFixture fixture{setup};
        NetworkClientPrefabInbox inbox;
        NetworkAuthorityObjectPublication publication;
        auto mode = ListenMode(setup, nullptr, &inbox, &publication);
        fixture.Attach(mode);
        REQUIRE(mode.Start().HasValue());
        const auto client = ModeBindingTests::Active(Connection(), Session());
        fixture.PublishInitial(mode, client);
        auto owner = fixture.binding.lock();
        REQUIRE(owner);
        REQUIRE(publication.IsBound());
        const auto scene = Runtime::SceneRuntimeId{1};
        const NetworkPrefabTemplateIdentity source{Assets::AssetId::FromBytes({1}), Sha256Digest{{2}}, 8};
        const auto schema = ReplicationSchemaId::Create(9).Value();
        std::vector<ReplicationDynamicSceneObject> added;
        std::vector<Runtime::EntityRef> entities;
        for (std::uint32_t slot = 0; slot < source.entityCount; ++slot) {
            entities.push_back({scene, {slot + 1, 1}});
            added.push_back({entities.back(), schema, {1, 0}, NetworkPrefabMemberSource{source, slot}});
        }
        const auto commit = [](auto candidate) {
            REQUIRE(candidate.HasValue());
            REQUIRE(candidate.Value()->ValidatePublication().HasValue());
            candidate.Value()->Publish();
            REQUIRE(candidate.Value()->AfterPublication().HasValue());
        };
        const auto flush = [&] {
            fixture.transport.size = 0;
            REQUIRE(owner->Flush(22).HasValue());
            return std::span(fixture.transport.sent).first(fixture.transport.size);
        };
        const auto receive = [&](std::uint32_t sequence) {
            const auto bytes = flush();
            REQUIRE_FALSE(bytes.empty());
            const auto frame = DecodeMessageEnvelope(bytes, *setup.codecs).Value();
            CHECK(frame.sequence == MessageSequenceNumber{sequence});
            REQUIRE(DeliverPrefabFrame(fixture, client->Connection(), bytes).rejectedPackets == 0);
            auto messages = inbox.Take(CommitPhase).Value();
            REQUIRE(messages.size() == 1);
            return messages.front();
        };
        auto pending = publication.Prepare(added, {}, {});
        REQUIRE(pending.HasValue());
        CHECK(flush().empty());
        commit(std::move(pending));
        fixture.transport.backpressure = true;
        CHECK(flush().empty());
        CHECK(inbox.Take(CommitPhase).Value().empty());
        commit(publication.Prepare({}, entities, {}));
        fixture.transport.backpressure = false;
        const auto created = receive(2);
        REQUIRE(created.spawn);
        CHECK(created.spawn->recipe == source);
        CHECK(created.spawn->members.size() == 8);
        CHECK(fixture.transport.size > 256);
        const auto localScene = Runtime::SceneRuntimeId{2};
        ClientSceneFixture reconstruction{localScene};
        const auto bound = inbox.Bind(created, reconstruction.cancellation).Value();
        Runtime::SceneCommandBuffer creation;
        std::vector<Runtime::RuntimeEntityGroupEntry> definitions(source.entityCount);
        REQUIRE(creation.CreateGroup(std::move(definitions), {}, reconstruction.Admission()).HasValue());
        const auto published = reconstruction.Publish(std::move(creation), bound);
        const auto replicaView = [&] {
            return mode
                .AcquireReplication(NetworkModeWorldKind::Client, {localScene, created.session, Runtime::RuntimePhase::NetworkPoll},
                                    ReplicationWorldCapability::ApplyAuthoritativeState)
                .Value();
        };
        CHECK(replicaView().Mapping().Entries().size() == source.entityCount);
        const auto retired = receive(3);
        CHECK_FALSE(retired.spawn);
        CHECK(retired.occurrence == created.occurrence);
        std::vector<Runtime::EntityRef> replicaEntities;
        for (const auto &entry : published.created)
            replicaEntities.push_back(entry.entity);
        Runtime::SceneCommandBuffer destruction;
        REQUIRE(destruction.DestroyGroup(std::move(replicaEntities), reconstruction.Admission()).HasValue());
        (void)reconstruction.Publish(std::move(destruction), bound);
        CHECK(replicaView().Mapping().Entries().empty());
        CHECK(flush().empty());

        for (std::size_t index = 0; index < entities.size(); ++index) {
            ++entities[index].entity.generation;
            added[index].entity = entities[index];
        }
        commit(publication.Prepare(added, {}, {}));
        const auto replacement = receive(4);
        CHECK(replacement.occurrence == created.occurrence.NextGeneration().Value());
        auto late = ModeBindingTests::Active(ConnectionHandle::Create(4, 1).Value(), Session(9));
        REQUIRE(owner->BindPeer(AdmissionEndpoint::Server, late, Session(9), 22).HasValue());
        const auto lateWorld = DecodeMessageEnvelope(flush(), *setup.codecs).Value();
        REQUIRE(DecodeNetworkWorldBinding(lateWorld.payload).Value().kind == NetworkWorldBindingKind::Initial);
        const auto latePrefab = DecodeMessageEnvelope(flush(), *setup.codecs).Value();
        CHECK(latePrefab.sequence == MessageSequenceNumber{2});
        CHECK(DecodeNetworkPrefabLifecycle(latePrefab.payload).Value() == replacement);
        CHECK(flush().empty());
    }

    TEST_CASE("Actual product session callbacks admit before canonical worlds are ready", "[integration][network][mode][binding]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        auto owners = SessionOwners();
        DeterministicClock runtimeClock;
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 1), World(NetworkModeWorldKind::Client, 2)};
        auto created = Product(runtimeClock, NetworkProjectRole::ListenServer, worlds, {.localPlayer = true}, owners);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        auto &fixture = *owners->binding;
        fixture.setup = owners->worldSetup;
        fixture.AttachOwner(owners->sessions->WorldBinding());
        REQUIRE(host->Startup().HasValue());
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        REQUIRE(host->RunFrame().HasValue());  // Actual ModeParticipant publishes the ingress clock before callbacks.
        auto client = ModeBindingTests::Negotiating(Connection(), Session());
        auto server = ModeBindingTests::Negotiating(ConnectionHandle::Create(3, 1).Value(), Session(8));
        REQUIRE(owners->sessions->OnSessionPrepared(AdmissionEndpoint::Server, server, Session(8), 1).HasValue());
        REQUIRE(owners->sessions->OnSessionPrepared(AdmissionEndpoint::Client, client, Session(), 1).HasValue());
        REQUIRE(server->State() == PeerSessionState::Negotiating);
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        ModeBindingTests::CompleteAdmission(server, Session(8));
        ModeBindingTests::CompleteAdmission(client, Session());
        owners->sessionSource->Advance(Duration::FromNanoseconds(21));
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE(owners->sessions->OnSessionActivated(AdmissionEndpoint::Server, server, Session(8), 22).HasValue());
        REQUIRE(owners->sessions->OnSessionActivated(AdmissionEndpoint::Client, client, Session(), 22).HasValue());
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        fixture.RegisterClient(client);
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        fixture.Deliver();
        REQUIRE(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());  // Packet only stages a candidate.
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE(host->Role(NetworkModeWorldKind::AuthorityServer, worlds[0].scene).HasValue());
        const auto role = host->Role(NetworkModeWorldKind::Client, worlds[1].scene);
        REQUIRE(role.HasValue());
        REQUIRE(role.Value().worldSession.has_value());
        CHECK_FALSE(role.Value().authority.IsValid());
        REQUIRE(host->AcquireReplication(NetworkModeWorldKind::Client,
                                         {worlds[1].scene, *role.Value().worldSession, Runtime::RuntimePhase::NetworkPoll, 0, {}},
                                         ReplicationWorldCapability::ApplyAuthoritativeState)
                    .HasValue());
        host->Shutdown();
        CHECK(host->Role(NetworkModeWorldKind::Client, worlds[1].scene).HasError());
        CHECK(owners->sessions->OnSessionActivated(AdmissionEndpoint::Server, server, Session(8), 22).HasError());
    }

    TEST_CASE("World control outbox preserves empty exact-fit and over-budget queue semantics", "[integration][network][mode][binding]") {
        auto setup = ModeBindingTests::Setup(64);
        const std::array<std::byte, 60> exactPayload{};
        std::array<std::byte, 256> encoded{};
        auto exactMessage = ControlPayload(exactPayload);
        exactMessage.sequence = MessageSequenceNumber{1};
        const auto exactSize = EncodeMessageEnvelopePayloadInto(exactMessage, *setup.codecs, encoded);
        REQUIRE(exactSize.HasValue());
        ModeBindingTests::BindingFixture fixture{setup, exactSize.Value()};
        auto mode = ListenMode(setup);
        fixture.Attach(mode);
        REQUIRE(mode.Start().HasValue());
        auto owner = fixture.binding.lock();
        REQUIRE(owner);
        fixture.server = ModeBindingTests::Active(ConnectionHandle::Create(3, 1).Value(), TestSupport::Session(8));
        REQUIRE(owner->BindPeer(AdmissionEndpoint::Server, fixture.server, TestSupport::Session(8), 22).HasValue());
        auto handler = std::make_shared<GenericControl>();
        RegisterControl(fixture, handler, 64);
        const auto queue = [&](const std::span<const std::byte> payload) {
            return owner->QueueControl(fixture.server->Connection(), TestSupport::Session(8), ControlPayload(payload),
                                       Runtime::RuntimePhase::NetworkFlush);
        };
        REQUIRE(queue({}).HasValue());
        REQUIRE(owner->Flush(22).HasValue());
        REQUIRE(queue(exactPayload).HasValue());
        REQUIRE(owner->Flush(22).HasValue());
        CHECK(fixture.transport.size == exactSize.Value());
        const std::array<std::byte, 61> overBudget{};
        const auto rejected = queue(overBudget);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().domain.Value() == NetworkErrors::MessageEnvelopeInvalid.domain.Value());
        CHECK(rejected.ErrorValue().code.Value() == NetworkErrors::MessageEnvelopeInvalid.code.Value());
        REQUIRE(queue({}).HasValue());
        REQUIRE(owner->Flush(22).HasValue());
        const auto decoded = DecodeMessageEnvelope(std::span{fixture.transport.sent}.first(fixture.transport.size), *setup.codecs);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().payload.empty());
        CHECK(decoded.Value().sequence.Value() == 3);  // Rejection neither reserves an entry nor advances the sequence.
        CHECK(fixture.server->State() == PeerSessionState::Active);
    }

    TEST_CASE("Travel cancellation preserves accepted generic Control and its sequence", "[integration][network][mode][binding]") {
        ModeBindingTests::BindingFixture fixture;
        bool failScene{};
        auto mode = ListenMode(fixture.setup, &failScene);
        fixture.Attach(mode);
        REQUIRE(mode.Start().HasValue());
        auto client = ModeBindingTests::Active(TestSupport::Connection(), TestSupport::Session());
        REQUIRE(mode.AdmitClientSession(NetworkModeWorldKind::Client, client, TestSupport::Connection(), TestSupport::Session(), 22)
                    .HasValue());
        fixture.PublishInitial(mode, client);
        auto owner = fixture.binding.lock();
        REQUIRE(owner);
        auto handler = std::make_shared<GenericControl>();
        RegisterControl(fixture, handler);
        const std::array payload{std::byte{0x35}, std::byte{0x76}};
        REQUIRE(owner
                    ->QueueControl(fixture.server->Connection(), TestSupport::Session(8), ControlPayload(payload),
                                   Runtime::RuntimePhase::NetworkFlush)
                    .HasValue());
        fixture.transport.backpressure = true;
        REQUIRE(owner->Flush(22).HasValue());
        REQUIRE(owner->CancelTravel().HasValue());
        failScene = true;
        const std::array next{NetworkModeWorld{NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{3}},
                              NetworkModeWorld{NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{4}}};
        const auto failed = mode.Travel(next, CommitPhase, TestSupport::Session());
        REQUIRE(failed.HasError());
        REQUIRE(failed.ErrorValue().code.Value() == NetworkErrors::NetworkModeUnavailable.code.Value());
        CHECK(mode.Active());
        fixture.transport.backpressure = false;
        REQUIRE(owner->Flush(23).HasValue());
        const auto decoded = DecodeMessageEnvelope(std::span{fixture.transport.sent}.first(fixture.transport.size), *fixture.setup.codecs);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().message == TestSupport::WireIdentity<MessageTypeId>(12));
        CHECK(decoded.Value().sequence.Value() == 2);
        CHECK(decoded.Value().payload == std::vector<std::byte>(payload.begin(), payload.end()));
    }

    TEST_CASE("Cancelled remote PREPARE expiry cannot be extended by unrelated Control", "[integration][network][mode][binding]") {
        ModeBindingTests::BindingFixture fixture;
        auto mode = ListenMode(fixture.setup);
        fixture.Attach(mode);
        REQUIRE(mode.Start().HasValue());
        auto client = ModeBindingTests::Active(TestSupport::Connection(), TestSupport::Session());
        REQUIRE(mode.AdmitClientSession(NetworkModeWorldKind::Client, client, TestSupport::Connection(), TestSupport::Session(), 22)
                    .HasValue());
        fixture.PublishInitial(mode, client);
        auto owner = fixture.binding.lock();
        REQUIRE(owner);
        const std::array next{NetworkModeWorld{NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{3}},
                              NetworkModeWorld{NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{4}}};
        REQUIRE(owner->PrepareTravel(next).HasValue());
        fixture.Deliver();
        REQUIRE(owner->CancelTravel().HasValue());
        auto handler = std::make_shared<GenericControl>();
        RegisterControl(fixture, handler);
        const std::array payload{std::byte{0x42}};
        for (const auto tick : {30u, 50u, 71u}) {
            REQUIRE(owner->Reconcile(tick).HasValue());
            REQUIRE(owner
                        ->QueueControl(fixture.server->Connection(), TestSupport::Session(8), ControlPayload(payload),
                                       Runtime::RuntimePhase::NetworkFlush)
                        .HasValue());
            REQUIRE(owner->Flush(tick).HasValue());
            REQUIRE(owner->PrepareTravel(next).HasError());
        }
        REQUIRE(owner->Reconcile(72).HasValue());  // Original PREPARE was accepted at 22, with a 50 tick fence.
        REQUIRE(owner->PrepareTravel(next).HasValue());
    }
}  // namespace Horo::Network
