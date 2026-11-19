#include "../../helpers/GameplayModuleTestSupport.h"
#include "../../unit/runtime/physics/PhysicsSceneActivationTestSupport.h"
#include "HeadlessPrefabIntent.h"
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

        /** @brief Observes the actual decoded owner transaction, retaining the exact local mapping provenance. */
        struct AcceptedField final {
            NetworkObjectMappingEntry mapping;
            double value;
            ReplicationRoleBinding role;
        };

        class AppliedFields final : public IReplicationApplyOwner {
            class Candidate final : public IReplicationApplyCandidate {
            public:
                Candidate(AppliedFields &owner, std::vector<AcceptedField> values) : owner_(owner), values_(std::move(values)) {}

                Result<void> Commit(const CancellationToken &cancellation) override {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldCancelled));
                    owner_.values = std::move(values_);
                    ++owner_.commits;
                    return Result<void>::Success();
                }

            private:
                AppliedFields &owner_;
                std::vector<AcceptedField> values_;
            };

        public:
            Result<std::unique_ptr<IReplicationApplyCandidate>> Prepare(std::span<const ReplicationApplyUpdate> updates) override {
                std::vector<AcceptedField> accepted;
                for (const auto &update : updates) {
                    REQUIRE(update.state.SimulationTick() == AuthorityTick);
                    REQUIRE(update.state.Fields().size() == 1);
                    const auto &field = update.state.Fields().front();
                    REQUIRE(field.field == TestSupport::FieldIdValue(1));
                    accepted.push_back({update.mapping, std::get<double>(field.value), update.role});
                }
                return Result<std::unique_ptr<IReplicationApplyCandidate>>::Success(
                    std::make_unique<Candidate>(*this, std::move(accepted)));
            }

            std::vector<AcceptedField> values;
            std::size_t commits{};
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

            Result<NetworkInitialStateTarget> PrepareReceiver(const ReplicationInboundAuthority &authority,
                                                              ReplicationWorldLifecycle &world,
                                                              std::span<const ReplicationInitialOwnership> ownership,
                                                              std::uint64_t now) override {
                ++receiverPreparations;
                auto receiver =
                    ReplicationInboundApply::Create(authority, world, applied, now, {}, ReplicationInboundApplyMode::InitialSnapshot)
                        .Value();
                const auto mappings = world.Acquire({authority.scene, authority.networkSession, CommitPhase, 1}).Value();
                const auto entries = mappings.Mapping().Entries();
                REQUIRE(entries.size() == ownership.size());
                for (std::size_t i{}; i < entries.size(); ++i) {
                    const auto &mapping = entries[i];
                    const auto projected =
                        ProjectInitialReplicationRole(ownership[i], mapping, authority.networkSession, authority.localRecipient);
                    auto codec = Codec(projected.Value());
                    auto role = std::make_shared<ReplicationRoleState>(ReplicationRoleState::Create(codec->Recipient()).Value());
                    REQUIRE(receiver->RegisterObject(mapping, std::move(role), std::move(codec)).HasValue());
                }
                return Result<NetworkInitialStateTarget>::Success({applied, std::move(receiver)});
            }

            std::shared_ptr<AppliedFields> applied{std::make_shared<AppliedFields>()};
            std::size_t receiverPreparations{};

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
                                                          const std::shared_ptr<const Runtime::RuntimeSceneDefinition> &definition) {
            Gameplay::GameplayReplicationAdmissionPolicy admission;
            admission.inventory.completeness = NetworkReplicationInventoryCompleteness::Complete;
            admission.inventory.declarations = {NativePrefabDeclaration()};
            const GameplayWorldSelection selection{.moduleId = "game.tests",
                                                   .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                                   .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                                   .prefabPermission = GameplayPrefabPermission::Granted,
                                                   .moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>()};
            const HeadlessSceneActivationInput input{.mode = Physics::PhysicsRuntimeMode::Canonical,
                                                     .settings = Physics::Settings(),
                                                     .registry = resources.registry,
                                                     .loads = resources.loads,
                                                     .load = resources.load,
                                                     .prefabLoads = &resources.loads,
                                                     .prefabTarget = AssetCookTargetId::Parse("linux-x64").Value()};
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
                                               owners->worldSetup},
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
                ModeBindingTests::CompleteAdmission(peer, Session(8), setup.policy.initialState->feature);
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
                                              std::vector<std::byte> payload) {
                wire.transport.event = {.kind = NetworkTransportEventKind::PacketReceived,
                                        .connection = connection,
                                        .channel = {},
                                        .delivery = DeliveryPolicy::ReliableOrdered,
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
            std::shared_ptr<InitialFieldOwners> fields{std::make_shared<InitialFieldOwners>()};
            std::shared_ptr<ProductOwners> owners{std::make_shared<ProductOwners>()};
            NetworkWorldBindingSetup setup{InitialProductTestSupport::InitialSetup(fields)};
            std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition{resources.Definition()};
            RemotePublisher remote{setup, resources, *definition};
            DeterministicClock clock;
            std::unique_ptr<NetworkProductHost> host;
            std::shared_ptr<PeerSessionLifecycle> peer{ModeBindingTests::Negotiating(Connection(), Session())};

            NonemptyInitialProduct() {
                owners->worldSetup = setup;
                host = CreateReplica(resources, owners, clock, definition);
                auto &wire = *owners->binding;
                wire.setup = setup;
                wire.AttachOwner(owners->sessions->WorldBinding());
                REQUIRE(host->Startup().HasValue());
                REQUIRE(host->RunFrame().HasValue());
                ModeBindingTests::CompleteAdmission(peer, Session(), setup.policy.initialState->feature);
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
                    if (ReplicaMapping().Mapping().Entries().size() < 3)
                        CHECK(fields->receiverPreparations == 0);
                    if (fields->applied->commits != 0)
                        return;
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

            void VerifyPublishedFields(NetworkSessionGeneration originalSession) const {
                CHECK(fields->receiverPreparations == 1);
                CHECK(fields->applied->commits == 1);
                const auto current = ReplicaMapping();
                const auto &values = fields->applied->values;
                REQUIRE(values.size() == 3);
                CHECK(values[0].mapping.provenance.authoredObject == AuthoredObject);
                CHECK(values[0].value == 7.5);
                CHECK(values[0].role.role == ReplicationExecutionRole::AutonomousClient);
                CHECK(values[0].role.autonomousOwner == NetworkPeerId::Create(7).Value());
                CHECK(values[1].role.autonomousOwner == NetworkPeerId::Create(9).Value());
                CHECK_FALSE(values[2].role.autonomousOwner);
                for (std::size_t i = 1; i < values.size(); ++i) {
                    CHECK_FALSE(values[i].mapping.provenance.authoredObject);
                    REQUIRE(values[i].mapping.provenance.prefab);
                    const auto &origin = *values[i].mapping.provenance.prefab;
                    CHECK(origin.occurrence == values[1].mapping.object);
                    CHECK(origin.source.recipe.artifactDigest == resources.artifact);
                    CHECK(values[i].value == 10.0 + origin.source.entitySlot);
                    CHECK(values[i].role.role == ReplicationExecutionRole::SimulatedClient);
                    CHECK(values[i].role.revision.Value() == values[i].mapping.object.Slot() + 3);
                }
                REQUIRE(current.Mapping().Entries().size() == values.size());
                CHECK(std::ranges::equal(current.Mapping().Entries(), values, {}, std::identity{}, &AcceptedField::mapping));
                for (const auto &value : values) {
                    const auto ownership =
                        ClientController()->InitialClientOwnership(ReplicaScene, originalSession, value.mapping.object).Value();
                    CHECK(ownership.revision == value.role.revision);
                    CHECK(ownership.autonomousOwner == value.role.autonomousOwner);
                }
            }

            void ReplaceWorld(std::span<const std::byte> staleFrame) {
                const auto previous = ReplicaMapping();
                constexpr auto replacement = Runtime::SceneRuntimeId{62};
                const std::array next{NetworkModeWorld{NetworkModeWorldKind::Client, replacement}};
                resources.load = {{}, std::chrono::steady_clock::now() + std::chrono::seconds{5}};
                REQUIRE(host->RequestTravel(next, Session()).HasValue());
                REQUIRE(host->RunFrame().HasValue());
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
                CHECK(fields->applied->commits == 1);
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
        CHECK(product.fields->receiverPreparations == 0);
        CHECK(product.fields->applied->values.empty());
        CHECK(product.ClientController()->InitialClientOwnership(ReplicaScene, originalSession, originalObject).HasError());
        REQUIRE(product.ReplicaMapping().Mapping().Entries().size() == 1);
        SECTION("Shutdown cancels the complete field snapshot before Prefab reconstruction") {
            product.host->Shutdown();
            CHECK(original.IsRevoked());
            CHECK(product.fields->applied->commits == 0);
            CHECK(product.ClientController()->InitialClientOwnership(ReplicaScene, originalSession, originalObject).HasError());
        }
        SECTION("Cooked children get authoritative field values in the same owner commit as the authored object") {
            product.WaitForFields();
            product.VerifyPublishedFields(originalSession);
            product.ConfirmFields();
            product.ReplaceWorld(lastField);
        }
    }
}  // namespace Horo::Network
