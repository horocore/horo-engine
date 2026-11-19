#include "../../helpers/GameplayModuleTestSupport.h"
#include "../../unit/runtime/physics/PhysicsSceneActivationTestSupport.h"
#include "HeadlessInitialStateAuthority.h"
#include "HeadlessPrefabIntent.h"
#include "Horo/AI/AIErrors.h"
#include "NetworkInitialStateProductTestSupport.h"
#include "NetworkModeHostTestSupport.h"
#include "NetworkProductPrefabTestSupport.h"
#include "ReplicationCaptureTestSupport.h"

#include <chrono>
#include <thread>

namespace Horo::Network {
    namespace {
        using namespace Application::Internal;
        using namespace PrefabProductTestSupport;
        constexpr auto CommitPhase = Runtime::RuntimePhase::CommitDeferredLifecycleChanges;
        constexpr auto ServerScene = Runtime::SceneRuntimeId{51};
        constexpr auto ReplicaScene = Runtime::SceneRuntimeId{52};
        constexpr auto AuthoredObject = Runtime::SceneObjectId{10};
        constexpr std::uint64_t AdmissionTick = 22;
        constexpr std::uint64_t AuthorityTick = 39;

        /** @brief Supplies a stable authored AI binding shared by both occurrences of the replicated object. */
        AI::AiAgentComponent AuthoredAiAgent() {
            return {.agent = AI::AgentId::Create(81).Value()};
        }

        /** @brief Builds the registered movement schema and its actual canonical scalar codec. */
        std::shared_ptr<const ReplicationSerializerRegistry> MovementRegistry() {
            const auto schema = NativePrefabDeclaration().schema;
            const auto &field = schema.fields.front();
            const auto serializer = CanonicalScalarReplicationSerializer::Create({.valueType = field.valueType,
                                                                                  .codec = field.codec,
                                                                                  .owner = schema.owner,
                                                                                  .valueKind = ReplicationValueKind::FloatingPoint,
                                                                                  .maximumEncodedBytes = 8,
                                                                                  .maximumElementCount = 1})
                                        .Value();
            const auto descriptors = BuildReplicationDescriptorSnapshot(std::span{&schema, 1}, TestSupport::Limits).Value();
            const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> serializers{serializer};
            return std::make_shared<const ReplicationSerializerRegistry>(
                ReplicationSerializerRegistry::Create(descriptors, serializers).Value());
        }

        /** @brief Resolves only the authentic server and agreed descriptor selected by this product fixture. */
        class ReplicaCodecAuthority final : public IHeadlessInitialReplicaAuthority {
        public:
            Result<std::uint64_t> DescriptorGeneration(const AuthenticationResult &accepted,
                                                       const HandshakeSelection &negotiated) const override {
                ++preparations;
                if (!expected || *expected != accepted || negotiated.connection != accepted.connection ||
                    negotiated.sessionGeneration != accepted.sessionGeneration)
                    return Result<std::uint64_t>::Failure(MakeError(NetworkErrors::AuthenticationRejected));
                return Result<std::uint64_t>::Success(1);
            }

            std::optional<AuthenticationResult> expected;
            mutable std::size_t preparations{};
        };

        /** @brief Drives the real capture, full-record codec and atomic apply using explicitly registered test fields. */
        class InitialFieldOwners final : public INetworkInitialStateHost {
        public:
            Result<NetworkPeerId> ClientRecipient(const PeerSessionLifecycle &) override {
                return NetworkPeerId::Create(7);
            }

            Result<std::optional<ReplicationInitialStateSnapshot>> Capture(const ReplicationWorldReadLease &world,
                                                                           const ReplicationWorldLifecycle &lifecycle,
                                                                           const PeerSessionLifecycle &peer, std::size_t budget) override {
                const auto &descriptor = world.Descriptor();
                const auto read =
                    lifecycle.AcquireCaptureRead({descriptor.scene, descriptor.session, Runtime::RuntimePhase::NetworkFlush, AuthorityTick})
                        .Value();
                std::vector<ReplicationCaptureTarget> targets;
                std::vector<std::shared_ptr<ReplicationStateCodec>> codecs;
                const auto recipient = ClientRecipient(peer).Value();
                for (const auto &mapping : world.Mapping().Entries()) {
                    auto source = std::make_shared<CaptureTestSupport::Owner>();
                    const auto value = mapping.provenance.authoredObject ? 7.5 : 10.0 + mapping.provenance.prefab->source.entitySlot;
                    source->Commit(AuthorityTick, value);
                    targets.push_back({mapping, std::move(source)});
                    const auto ownership = Ownership(mapping, recipient);
                    codecs.push_back(Codec(ProjectInitialReplicationRole(ownership, mapping, descriptor.session, recipient).Value()));
                }
                auto capture = ReplicationStateCapture::Prepare(read, registry, targets).Value();
                REQUIRE(capture->CaptureAtCommit(read, AuthorityTick).Value().published == targets.size());
                auto initial = EncodeReplicationInitialState(world, *capture, AuthorityTick, codecs, budget);
                if (initial.HasError())
                    return Result<std::optional<ReplicationInitialStateSnapshot>>::Failure(initial.ErrorValue());
                return Result<std::optional<ReplicationInitialStateSnapshot>>::Success(std::move(initial).Value());
            }

            Result<NetworkInitialStateTarget> PrepareReceiver(const ReplicationInboundAuthority &, ReplicationWorldLifecycle &,
                                                              std::span<const ReplicationInitialOwnership>, std::uint64_t) override {
                FAIL("The configured replica must use the Scene-owned receiver");
                return Result<NetworkInitialStateTarget>::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
            }

        private:
            static ReplicationInitialOwnership Ownership(const NetworkObjectMappingEntry &mapping, NetworkPeerId local) {
                ReplicationInitialOwnership ownership{mapping.object, ReplicationRoleRevision::Create(mapping.object.Slot() + 3).Value()};
                if (mapping.provenance.authoredObject)
                    ownership.autonomousOwner = local;
                else if (mapping.provenance.prefab->source.entitySlot == 0)
                    ownership.autonomousOwner = NetworkPeerId::Create(9).Value();
                return ownership;
            }

            std::shared_ptr<ReplicationStateCodec> Codec(const ReplicationRoleBinding &role) const {
                return std::shared_ptr<ReplicationStateCodec>{
                    ReplicationStateCodec::Create(registry, role, ReplicationRecordKind::Spawn, 1).Value()};
            }

            std::shared_ptr<const ReplicationSerializerRegistry> registry{MovementRegistry()};
        };

        /** @brief Records actual incremental publication at the product structural dispatch. */
        class SnapshotApplyRecorder final : public IReplicationApplyOwner {
        public:
            Result<std::unique_ptr<IReplicationApplyCandidate>> Prepare(std::span<const ReplicationApplyUpdate> updates) override {
                REQUIRE(updates.size() == 1);
                REQUIRE(updates.front().mapping.provenance.authoredObject == AuthoredObject);
                const auto fields = updates.front().state.Fields();
                REQUIRE(fields.size() == 1);
                return Result<std::unique_ptr<IReplicationApplyCandidate>>::Success(
                    std::make_unique<Candidate>(*this, std::get<double>(fields.front().value)));
            }

            std::optional<double> applied;

        private:
            class Candidate final : public IReplicationApplyCandidate {
            public:
                Candidate(SnapshotApplyRecorder &owner, double value) : owner_(owner), value_(value) {}

                Result<void> Commit(const CancellationToken &cancellation) override {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldCancelled));
                    owner_.applied = value_;
                    return Result<void>::Success();
                }

            private:
                SnapshotApplyRecorder &owner_;
                double value_;
            };
        };

        /** @brief Supplies an explicit full-record sender and transaction recorder through the normal world binding. */
        class SnapshotFieldPublisher final : public INetworkStateUpdateHost {
        public:
            Result<std::optional<NetworkStateUpdateBatch>> CaptureUpdates(const ReplicationWorldReadLease &world,
                                                                          const ReplicationWorldLifecycle &lifecycle,
                                                                          const PeerSessionLifecycle &,
                                                                          std::span<const ReplicationInitialOwnership> cohort,
                                                                          std::uint64_t, const NetworkStateUpdatePolicy &) override {
                ++captures;
                REQUIRE(cohort.size() == 3);
                const auto &descriptor = world.Descriptor();
                const auto &mapping = world.Mapping().Entries().front();
                REQUIRE(cohort.front().object == mapping.object);
                const auto read =
                    lifecycle.AcquireCaptureRead({descriptor.scene, descriptor.session, Runtime::RuntimePhase::NetworkFlush, tick}).Value();
                auto source = std::make_shared<CaptureTestSupport::Owner>();
                source->Commit(tick, 18.25);
                const std::array targets{ReplicationCaptureTarget{mapping, source}};
                ReplicationCaptureLimits limits;
                limits.publicationMode = ReplicationCapturePublicationMode::ExactCommit;
                auto capture = ReplicationStateCapture::Prepare(read, registry, targets, limits).Value();
                REQUIRE(capture->CaptureAtCommit(read, tick).Value().published == 1);
                auto ownership = cohort.front();
                if (wrongRevision)
                    ownership.revision = ReplicationRoleRevision::Create(93).Value();
                const auto role =
                    ProjectInitialReplicationRole(ownership, mapping, descriptor.session, NetworkPeerId::Create(recipient).Value());
                REQUIRE(role.HasValue());
                auto codec = ReplicationStateCodec::Create(registry, role.Value(), ReplicationRecordKind::Update, generation).Value();
                auto encoded = codec->Encode(capture->Latest(mapping.object).Value());
                REQUIRE(encoded.HasValue());
                NetworkStateUpdateBatch batch{tick};
                batch.records.emplace_back(std::move(encoded).Value());
                return Result<std::optional<NetworkStateUpdateBatch>>::Success(std::move(batch));
            }

            Result<NetworkInitialStateTarget> PrepareUpdates(const ReplicationInboundAuthority &authority, ReplicationWorldLifecycle &world,
                                                             std::span<const ReplicationInitialOwnership> cohort,
                                                             std::uint64_t now) override {
                ++preparations;
                auto receiver = ReplicationInboundApply::Create(authority, world, recorder, now).Value();
                const auto mapping = world.Acquire({authority.scene, authority.networkSession, Runtime::RuntimePhase::NetworkPoll}).Value();
                const auto entries = mapping.Mapping().Entries();
                REQUIRE(entries.size() == cohort.size());
                for (std::size_t index{}; index < entries.size(); ++index) {
                    const auto role =
                        ProjectInitialReplicationRole(cohort[index], entries[index], authority.networkSession, authority.localRecipient)
                            .Value();
                    auto codec = ReplicationStateCodec::Create(registry, role, ReplicationRecordKind::Update, 1).Value();
                    auto roleState = std::make_shared<ReplicationRoleState>(ReplicationRoleState::Create(role).Value());
                    REQUIRE(receiver->RegisterObject(entries[index], std::move(roleState), std::move(codec)).HasValue());
                }
                observedReceiver = receiver;
                return Result<NetworkInitialStateTarget>::Success({recorder, std::move(receiver)});
            }

            std::shared_ptr<const ReplicationSerializerRegistry> registry{MovementRegistry()};
            std::shared_ptr<SnapshotApplyRecorder> recorder{std::make_shared<SnapshotApplyRecorder>()};
            std::weak_ptr<ReplicationInboundApply> observedReceiver;
            std::uint64_t tick{AuthorityTick + 1};
            std::uint64_t recipient{7};
            std::uint64_t generation{1};
            std::size_t captures{};
            std::size_t preparations{};
            bool wrongRevision{};
        };

        /** @brief Keeps the cooked template and actual headless replica resources alive through shutdown. */
        struct ReplicaResources final {
            Assets::AssetId asset{Assets::AssetId::FromBytes({8})};
            Assets::AssetTypeId type{Assets::AssetTypeId::Parse("core.prefab").Value()};
            JobSystem jobs{{1, 8}};
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider bytes;
            Assets::AssetLoadService loads{jobs, bytes};
            HeadlessSceneLoadBoundary load{{}, std::chrono::steady_clock::now() + std::chrono::seconds{5}};
            Sha256Digest artifact;

            ReplicaResources() {
                REQUIRE(registry
                            .Publish({{asset, type, ProjectPath::Parse("assets/replica.prefab").Value(),
                                       ProjectPath::Parse("assets/replica.prefab.horo").Value()}})
                            .status == Assets::AssetRegistryBuildStatus::Complete);
                artifact = PutNetworkPrefab(bytes, asset, type);
            }

            std::shared_ptr<const Runtime::RuntimeSceneDefinition> Definition() const {
                Runtime::SceneDefinitionBuilder builder{{19}, {1}};
                Runtime::RuntimeEntityDefinition authored{.object = AuthoredObject};
                authored.components.aiAgent = AuthoredAiAgent();
                authored.components.aiController =
                    AI::AiControllerComponent{.controller = AI::ControllerTypeId::Create(82).Value(),
                                              .decisionAsset = AI::DecisionGraphAssetId::Create(83).Value(),
                                              .blackboardSchema = AI::BlackboardSchemaId::Create(84).Value()};
                const auto json = std::as_bytes(std::span(std::string_view{R"({"speed":1.25})"}));
                authored.components.gameplayComponents.push_back({Gameplay::ComponentTypeId::Parse("game.tests.movement_settings").Value(),
                                                                  2,
                                                                  Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                                                  {json.begin(), json.end()}});
                builder.Add(std::move(authored));
                return std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(builder).Build().Value());
            }
        };

        /** @brief Builds the actual product's client mode with the declared movement schema and cooked Prefab loader. */
        std::unique_ptr<NetworkProductHost> CreateReplica(ReplicaResources &resources, const std::shared_ptr<ProductOwners> &owners,
                                                          Clock &clock,
                                                          const std::shared_ptr<const Runtime::RuntimeSceneDefinition> &definition,
                                                          const INetworkModeService *&observedScene,
                                                          std::shared_ptr<HeadlessInitialStateHost> initialSceneState) {
            Gameplay::GameplayReplicationAdmissionPolicy admission;
            admission.inventory.completeness = NetworkReplicationInventoryCompleteness::Complete;
            admission.inventory.declarations = {NativePrefabDeclaration()};
            const GameplayWorldSelection selection{.moduleId = "game.tests",
                                                   .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                                   .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                                   .prefabPermission = GameplayPrefabPermission::Granted,
                                                   .moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>()};
            const auto &schema = admission.inventory.declarations.front().schema;
            const HeadlessSceneActivationInput input{.mode = Physics::PhysicsRuntimeMode::Canonical,
                                                     .settings = Physics::Settings(),
                                                     .registry = resources.registry,
                                                     .loads = resources.loads,
                                                     .load = resources.load,
                                                     .prefabLoads = &resources.loads,
                                                     .prefabTarget = AssetCookTargetId::Parse("linux-x64").Value(),
                                                     .publicTransforms =
                                                         HeadlessSceneTransformReplication{schema.id,
                                                                                           schema.version,
                                                                                           {{schema.fields.front().id,
                                                                                             SceneReplicationProperty::TranslationX}}},
                                                     .initialSceneState = std::move(initialSceneState)};
            auto settings = DefaultNetworkProjectSettings(Id<NetworkProjectSettingsId>(10)).Value();
            settings.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client;
            settings.transport.requirement = NetworkProjectTransportRequirement::Required;
            settings.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            settings.transport.capabilities.requiredChannels = 2;
            settings.transport.capabilities.requiredMaximumMessageBytes = 1024;
            settings.replication = admission.inventory;
            const auto project = NetworkProjectSettings::Create(settings).Value();
            const auto facts = ProductFacts(project, NetworkProjectRole::Client, false);
            auto services = ComposeHeadlessSceneNetworkServices(definition, input, ProductFactories(owners), selection, admission).Value();
            auto &sceneFactory = services.factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)];
            sceneFactory = [factory = std::move(sceneFactory), &observedScene](const NetworkModeServiceRequest &request) {
                auto created = factory(request);
                if (created.HasValue())
                    observedScene = created.Value().get();
                return created;
            };
            const std::array worlds{NetworkModeWorld{NetworkModeWorldKind::Client, ReplicaScene}};
            return NetworkProductHost::Create(clock,
                                              {project,
                                               facts.product,
                                               facts.inventory,
                                               facts.host,
                                               facts.requirements,
                                               facts.selection,
                                               6,
                                               worlds,
                                               {.localPlayer = true},
                                               owners->sessions,
                                               NetworkWorldCompositionPurpose::Gameplay,
                                               owners->worldSetup,
                                               services.fixedParticipant,
                                               services.pollRetirement},
                                              std::move(services.factories))
                .Value();
        }

        /** @brief Issues real authority world, authored publication and dynamic mapping records on its own reliable lane. */
        class RemotePublisher final {
        public:
            RemotePublisher(const NetworkWorldBindingSetup &setup, ReplicaResources &resources,
                            const Runtime::RuntimeSceneDefinition &definition)
                : wire_(setup) {
                NetworkModePlan plan;
                plan.mode = NetworkProjectRole::DedicatedServer;
                plan.hostGeneration = 1;
                plan.protocolVersion = {1, 0};
                plan.worlds[0] = {NetworkModeWorldKind::AuthorityServer, ServerScene};
                plan.worldCount = 1;
                owner = NetworkWorldBindingController::Create(plan, setup).Value();
                wire_.AttachOwner(owner);
                auto scene = Runtime::RuntimeScene::Create(definition, ServerScene).Value();
                const auto schema = NativePrefabDeclaration().schema;
                const NetworkAuthoredSceneProjection authored{{{scene->View().Find(AuthoredObject).value(),
                                                                {schema.id, schema.version, AuthoredObject}}},
                                                              {}};
                REQUIRE(owner->Prepare(authored).HasValue());
                REQUIRE(owner->Commit(CommitPhase).HasValue());
                PublishPrefab(resources, scene->View(), schema);
                peer = ModeBindingTests::Negotiating(ConnectionHandle::Create(3, 1).Value(), Session(8));
                const auto &updates = setup.policy.stateUpdates;
                ModeBindingTests::CompleteAdmission(peer, Session(8), setup.policy.initialState->feature,
                                                    updates ? updates->feature : ProtocolFeatureId{});
                REQUIRE(owner->BindPeer(AdmissionEndpoint::Server, peer, Session(8), AdmissionTick).HasValue());
                wire_.RegisterClient(peer);
            }

            std::vector<std::byte> SendNext() {
                wire_.transport.size = 0;
                REQUIRE(owner->Flush(AdmissionTick).HasValue());
                REQUIRE(wire_.transport.size != 0);
                const auto bytes = std::span{wire_.transport.sent}.first(wire_.transport.size);
                return {bytes.begin(), bytes.end()};
            }

            InboundDispatchReport AcceptReceipt(std::vector<std::byte> payload) {
                return Poll(wire_, peer->Connection(), std::move(payload));
            }

            void FlushAt(std::uint64_t now) {
                REQUIRE(owner->Flush(now).HasValue());
            }

            const ModeBindingTests::RecordingTransport &Transport() const {
                return wire_.transport;
            }

            void PrepareReplacement(Runtime::SceneRuntimeId replacement, const Runtime::RuntimeSceneDefinition &definition) {
                auto scene = Runtime::RuntimeScene::Create(definition, replacement).Value();
                const auto schema = NativePrefabDeclaration().schema;
                const NetworkAuthoredSceneProjection authored{{{scene->View().Find(AuthoredObject).value(),
                                                                {schema.id, schema.version, AuthoredObject}}},
                                                              {}};
                const std::array next{NetworkModeWorld{NetworkModeWorldKind::AuthorityServer, replacement}};
                REQUIRE(owner->PrepareTravel(next, authored).HasValue());
            }

            static InboundDispatchReport Poll(ModeBindingTests::BindingFixture &wire, ConnectionHandle connection,
                                              std::vector<std::byte> payload, ChannelId channel = {},
                                              DeliveryPolicy delivery = DeliveryPolicy::ReliableOrdered) {
                wire.transport.event = {.kind = NetworkTransportEventKind::PacketReceived,
                                        .connection = connection,
                                        .channel = channel,
                                        .delivery = delivery,
                                        .payload = std::move(payload)};
                wire.transport.queued = true;
                return wire.dispatcher->RunNetworkPoll(AdmissionTick).Value();
            }

            std::shared_ptr<NetworkWorldBindingController> owner;
            std::shared_ptr<PeerSessionLifecycle> peer;

        private:
            void PublishPrefab(ReplicaResources &resources, const Runtime::RuntimeSceneView &scene,
                               const ReplicationSchemaDescriptor &schema) {
                const Prefab::PrefabSpawnRequest request{.placement = {.translation = {3, 4, 5}}};
                const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
                auto intent =
                    std::make_shared<const std::vector<std::byte>>(EncodeHeadlessPrefabIntent(request, limits, scene, {}).Value());
                const NetworkPrefabTemplateIdentity recipe{resources.asset, resources.artifact, 3};
                const std::array<ReplicationDynamicSceneObject, 2> members{ReplicationDynamicSceneObject{{ServerScene, {1, 1}},
                                                                                                         schema.id,
                                                                                                         schema.version,
                                                                                                         NetworkPrefabMemberSource{recipe,
                                                                                                                                   0},
                                                                                                         intent},
                                                                           ReplicationDynamicSceneObject{{ServerScene, {3, 1}},
                                                                                                         schema.id,
                                                                                                         schema.version,
                                                                                                         NetworkPrefabMemberSource{recipe,
                                                                                                                                   2}}};
                auto prepared = owner->PrepareAuthorityObjects(ServerScene, members, {}, {}).Value();
                REQUIRE(prepared->ValidatePublication().HasValue());
                prepared->Publish();
                REQUIRE(prepared->AfterPublication().HasValue());
            }

            ModeBindingTests::BindingFixture wire_;
        };

        /** @brief Composes the host and advances only the actual client scheduler between Control frames. */
        struct NonemptyInitialProduct final {
            ReplicaResources resources;
            std::shared_ptr<ReplicaCodecAuthority> codec{std::make_shared<ReplicaCodecAuthority>()};
            std::shared_ptr<InitialFieldOwners> fields{std::make_shared<InitialFieldOwners>()};
            std::shared_ptr<ProductOwners> owners{std::make_shared<ProductOwners>()};
            NetworkWorldBindingSetup setup;
            std::shared_ptr<HeadlessInitialStateHost> initialSceneState{std::make_shared<HeadlessInitialStateHost>(nullptr, fields, codec)};
            std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition{resources.Definition()};
            RemotePublisher remote;
            DeterministicClock clock;
            const INetworkModeService *observedScene{}; /**< Borrow valid only while the host owns the current Scene service. */
            std::unique_ptr<NetworkProductHost> host;
            std::shared_ptr<PeerSessionLifecycle> peer{ModeBindingTests::Negotiating(Connection(), Session())};

            explicit NonemptyInitialProduct(const std::shared_ptr<INetworkStateUpdateHost> &authorityUpdates = {})
                : setup(InitialProductTestSupport::InitialSetup(fields, authorityUpdates)), remote(setup, resources, *definition) {
                owners->worldSetup = setup;
                owners->worldSetup.initialStateHost = initialSceneState;
                host = CreateReplica(resources, owners, clock, definition, observedScene, initialSceneState);
                auto &wire = *owners->binding;
                wire.setup = owners->worldSetup;
                wire.AttachOwner(owners->sessions->WorldBinding());
                REQUIRE(host->Startup().HasValue());
                REQUIRE(host->RunFrame().HasValue());
                const auto &updates = setup.policy.stateUpdates;
                ModeBindingTests::CompleteAdmission(peer, Session(), setup.policy.initialState->feature,
                                                    updates ? updates->feature : ProtocolFeatureId{});
                REQUIRE(peer->Authentication());
                codec->expected = *peer->Authentication();
                REQUIRE(owners->sessions->OnSessionActivated(AdmissionEndpoint::Client, peer, Session(), AdmissionTick).HasValue());
                wire.RegisterClient(peer);
            }

            std::vector<std::byte> Receive() {
                auto frame = remote.SendNext();
                REQUIRE(RemotePublisher::Poll(*owners->binding, peer->Connection(), frame).rejectedPackets == 0);
                return frame;
            }

            ReplicationWorldReadLease ReplicaMapping(Runtime::SceneRuntimeId scene = ReplicaScene) const {
                constexpr auto kind = NetworkModeWorldKind::Client;
                const auto role = host->Role(kind, scene).Value();
                return host
                    ->AcquireReplication(kind, {scene, role.worldSession.value(), Runtime::RuntimePhase::NetworkPoll},
                                         ReplicationWorldCapability::ApplyAuthoritativeState)
                    .Value();
            }

            void WaitForFields() {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                do {
                    clock.Advance(Duration::FromNanoseconds(16'666'667));
                    REQUIRE(host->RunFrame().HasValue());
                    const auto mapped = ReplicaMapping();
                    if (mapped.Mapping().Entries().size() < 3) {
                        CHECK(codec->preparations == 0);
                    } else {
                        const auto object = mapped.Mapping().Entries().front().object;
                        if (ClientController()->InitialClientOwnership(ReplicaScene, mapped.Descriptor().session, object).HasValue())
                            return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                } while (std::chrono::steady_clock::now() < deadline);
                FAIL("The product did not publish reconstructed initial fields.");
            }

            std::shared_ptr<NetworkWorldBindingController> ClientController() const {
                auto controller = owners->binding->binding.lock();
                REQUIRE(controller);
                return controller;
            }

            void ConfirmFields() {
                CHECK(remote.owner->AdmitWorldDispatch(remote.peer->Connection(), Session(8), *remote.peer, AdmissionTick).HasError());
                REQUIRE(ClientController()->Flush(AdmissionTick).HasValue());
                const auto &wire = owners->binding->transport;
                const auto receipt = std::span{wire.sent}.first(wire.size);
                REQUIRE(remote.AcceptReceipt({receipt.begin(), receipt.end()}).rejectedPackets == 0);
                CHECK(remote.owner->AdmitWorldDispatch(remote.peer->Connection(), Session(8), *remote.peer, AdmissionTick).HasValue());
            }

            float AuthoredX() const {
                const auto scene = InspectHeadlessScene(*observedScene).value();
                return scene.Get(scene.Find(AuthoredObject).value()).Value().localTransform->translation.x;
            }

            void VerifyPublishedFields(NetworkSessionGeneration session) const {
                CHECK(codec->preparations == 1);
                CHECK(AuthoredX() == 7.5F);
                const auto current = ReplicaMapping();
                const auto rows = current.Mapping().Entries();
                REQUIRE(rows.size() == 3);
                CHECK(rows[0].provenance.authoredObject == AuthoredObject);
                const auto scene = InspectHeadlessScene(*observedScene).value();
                for (std::size_t i{}; i < rows.size(); ++i) {
                    const auto &row = rows[i];
                    const auto ownership = ClientController()->InitialClientOwnership(ReplicaScene, session, row.object).Value();
                    CHECK(ownership.revision.Value() == row.object.Slot() + 3);
                    if (i == 0) {
                        CHECK(ownership.autonomousOwner == NetworkPeerId::Create(7).Value());
                        continue;
                    }
                    REQUIRE(row.provenance.prefab);
                    const auto &origin = *row.provenance.prefab;
                    CHECK(origin.occurrence == rows[1].object);
                    CHECK(origin.source.recipe.artifactDigest == resources.artifact);
                    CHECK(scene.Get(row.entity).Value().localTransform->translation.x == 10.0F + origin.source.entitySlot);
                    if (i == 1)
                        CHECK(ownership.autonomousOwner == NetworkPeerId::Create(9).Value());
                    else
                        CHECK_FALSE(ownership.autonomousOwner);
                }
            }

            /** @brief Checks that the public field committed for a live AI-authored object confers no client AI access. */
            void VerifyPrivateAiUnavailable(const Runtime::SceneRuntimeId expectedScene) const {
                REQUIRE(observedScene);
                const auto scene = InspectHeadlessScene(*observedScene);
                REQUIRE(scene);
                REQUIRE(scene->RuntimeId() == expectedScene);
                const auto entity = scene->Find(AuthoredObject);
                REQUIRE(entity);
                const auto view = scene->Get(*entity);
                REQUIRE(view.HasValue());
                REQUIRE(view.Value().components);
                CHECK(view.Value().components->aiAgent == AuthoredAiAgent());
                REQUIRE(view.Value().components->aiController);

                const auto expected = MakeError(AI::AIErrors::RuntimeUnavailable).code.Value();
                const auto privateValue =
                    InspectHeadlessDecisionValue(*observedScene, AuthoredAiAgent().agent, AI::BlackboardKeyId::Create(85).Value());
                REQUIRE(privateValue.HasError());
                CHECK(privateValue.ErrorValue().code.Value() == expected);
                const auto memory = InspectHeadlessPerceptionMemory(*observedScene, *entity);
                REQUIRE(memory.HasError());
                CHECK(memory.ErrorValue().code.Value() == expected);
                CHECK_FALSE(InspectHeadlessPerception(*observedScene));
                const auto failure = InspectHeadlessDecisionFailure(*observedScene);
                REQUIRE(failure.HasError());
                CHECK(failure.ErrorValue().code.Value() == expected);
            }

            void ReplaceWorld(std::span<const std::byte> staleFrame) {
                const auto previous = ReplicaMapping();
                constexpr auto replacement = Runtime::SceneRuntimeId{62};
                const std::array next{NetworkModeWorld{NetworkModeWorldKind::Client, replacement}};
                resources.load = {{}, std::chrono::steady_clock::now() + std::chrono::seconds{5}};
                REQUIRE(host->RequestTravel(next, Session()).HasValue());
                REQUIRE(host->RunFrame().HasValue());
                const auto preparationFailure = host->TakeTravelFailure();
                if (preparationFailure)
                    UNSCOPED_INFO(preparationFailure->message);
                REQUIRE_FALSE(preparationFailure);
                remote.PrepareReplacement({61}, *definition);
                Receive();  // PREPARE.
                Receive();  // Authored replacement inventory.
                REQUIRE(remote.owner->Commit(CommitPhase).HasValue());
                Receive();  // COMMIT.
                REQUIRE(host->RunFrame().HasValue());
                CHECK(previous.IsRevoked());
                const auto current = ReplicaMapping(replacement);
                CHECK(current.Descriptor().authority != previous.Descriptor().authority);
                REQUIRE(current.Mapping().Entries().size() == 1);
                CHECK(current.Mapping().Entries()[0].entity.runtime == replacement);
                const auto stale = DecodeMessageEnvelope(staleFrame, *setup.codecs).Value();
                CHECK(ClientController()->HandleAdmitted({peer->Connection(), Session(), peer, AdmissionTick}, stale).HasError());
                CHECK(ClientController()
                          ->InitialClientOwnership(ReplicaScene, previous.Descriptor().session, previous.Mapping().Entries()[0].object)
                          .HasError());
                CHECK(codec->preparations == 1);
                CHECK(AuthoredX() == 0.0F);
                VerifyPrivateAiUnavailable(replacement);
            }
        };
    }  // namespace

    TEST_CASE("Product late join publishes authored and cooked Prefab fields after reconstruction",
              "[integration][network][mode][replication]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        NonemptyInitialProduct product;
        product.Receive();
        product.Receive();
        REQUIRE(product.host->RunFrame().HasValue());
        const auto original = product.ReplicaMapping();
        REQUIRE(original.Mapping().Entries().size() == 1);
        const auto originalObject = original.Mapping().Entries().front().object;
        const auto originalSession = original.Descriptor().session;
        product.Receive();  // The Prefab is queued, but its cooked entities are not committed yet.
        product.Receive();  // Manifest.
        std::vector<std::byte> lastField;
        for (int i = 0; i < 3; ++i)
            lastField = product.Receive();
        CHECK(product.codec->preparations == 0);
        CHECK(product.AuthoredX() == 0.0F);
        CHECK(product.ClientController()->InitialClientOwnership(ReplicaScene, originalSession, originalObject).HasError());
        REQUIRE(product.ReplicaMapping().Mapping().Entries().size() == 1);
        product.VerifyPrivateAiUnavailable(ReplicaScene);
        SECTION("Shutdown cancels the complete field snapshot before Prefab reconstruction") {
            product.host->Shutdown();
            CHECK(original.IsRevoked());
            CHECK(product.codec->preparations == 0);
            CHECK(product.ClientController()->InitialClientOwnership(ReplicaScene, originalSession, originalObject).HasError());
        }
        SECTION("Cooked children get authoritative field values in the same owner commit as the authored object") {
            product.WaitForFields();
            product.VerifyPublishedFields(originalSession);
            product.VerifyPrivateAiUnavailable(ReplicaScene);
            product.ConfirmFields();
            product.ReplaceWorld(lastField);
        }
    }

    TEST_CASE("Authority sends full snapshots only to the exact acknowledged initial cohort", "[integration][network][mode][replication]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        auto updates = std::make_shared<SnapshotFieldPublisher>();
        NonemptyInitialProduct product{updates};
        product.Receive();
        product.Receive();
        REQUIRE(product.host->RunFrame().HasValue());
        for (int index{}; index < 5; ++index)
            product.Receive();  // Reconstruct Prefab and deliver the full reliable initial manifests.
        product.WaitForFields();
        CHECK(updates->preparations == 1);
        REQUIRE_FALSE(updates->observedReceiver.expired());
        product.remote.FlushAt(AdmissionTick + 1);
        CHECK(updates->captures == 0);
        product.ConfirmFields();

        product.remote.FlushAt(AdmissionTick + 2);
        const auto &sent = product.remote.Transport();
        REQUIRE(updates->captures == 1);
        CHECK(sent.lastDelivery == DeliveryPolicy::UnreliableUnordered);
        CHECK(sent.lastChannel == ChannelId::Create(2, 3).Value());
        const auto frame = DecodeMessageEnvelope(std::span{sent.sent}.first(sent.size), *product.setup.codecs).Value();
        CHECK(frame.message == WireIdentity<MessageTypeId>(20));
        CHECK(frame.sequence == MessageSequenceNumber{1});
        const auto wire = std::span{sent.sent}.first(sent.size);
        auto &client = *product.owners->binding;
        const auto wrong = RemotePublisher::Poll(client, product.peer->Connection(), {wire.begin(), wire.end()},
                                                 ChannelId::Create(3, 4).Value(), DeliveryPolicy::UnreliableUnordered);
        REQUIRE(wrong.lastPacketRejection);
        CHECK(wrong.lastPacketRejection->code.Value() == MakeError(NetworkErrors::MessageDeliveryInvalid).code.Value());
        CHECK_FALSE(updates->recorder->applied);
        const auto delivered =
            RemotePublisher::Poll(client, product.peer->Connection(), {wire.begin(), wire.end()}, sent.lastChannel, sent.lastDelivery);
        if (delivered.lastPacketRejection)
            UNSCOPED_INFO(delivered.lastPacketRejection->message);
        REQUIRE(delivered.rejectedPackets == 0);
        CHECK_FALSE(updates->recorder->applied);
        product.clock.Advance(Duration::FromNanoseconds(16'666'667));
        REQUIRE(product.host->RunFrame().HasValue());
        CHECK(updates->recorder->applied == 18.25);

        updates->wrongRevision = true;
        ++updates->tick;
        product.remote.FlushAt(AdmissionTick + 3);
        CHECK(updates->captures == 2);
        CHECK(DecodeMessageEnvelope(std::span{sent.sent}.first(sent.size), *product.setup.codecs).Value().sequence ==
              MessageSequenceNumber{1});
        updates->wrongRevision = false;
        updates->recipient = 9;
        product.remote.FlushAt(AdmissionTick + 4);
        REQUIRE(updates->captures == 3);
        CHECK(DecodeMessageEnvelope(std::span{sent.sent}.first(sent.size), *product.setup.codecs).Value().sequence ==
              MessageSequenceNumber{1});
        updates->recipient = 7;
        updates->generation = 2;
        product.remote.FlushAt(AdmissionTick + 5);
        REQUIRE(updates->captures == 4);
        CHECK(DecodeMessageEnvelope(std::span{sent.sent}.first(sent.size), *product.setup.codecs).Value().sequence ==
              MessageSequenceNumber{1});
        updates->generation = 1;
        product.remote.FlushAt(AdmissionTick + 6);
        REQUIRE(updates->captures == 5);
        const auto secondWire = std::span{sent.sent}.first(sent.size);
        CHECK(DecodeMessageEnvelope(secondWire, *product.setup.codecs).Value().sequence == MessageSequenceNumber{2});
        const auto overBudget = RemotePublisher::Poll(client, product.peer->Connection(), {secondWire.begin(), secondWire.end()},
                                                      sent.lastChannel, sent.lastDelivery);
        REQUIRE(overBudget.lastPacketRejection);
        CHECK(overBudget.lastPacketRejection->code.Value() == MakeError(NetworkErrors::TransportLimitExceeded).code.Value());
        CHECK(product.peer->State() == PeerSessionState::Active);
        CHECK(client.transport.closed == 0);
        product.host->Shutdown();
        CHECK(updates->observedReceiver.expired());
    }
}  // namespace Horo::Network
