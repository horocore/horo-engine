#include "../../helpers/GameplayModuleTestSupport.h"
#include "../../unit/runtime/physics/PhysicsSceneActivationTestSupport.h"
#include "HeadlessPrefabIntent.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Network/NetworkPrefabLifecycle.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "NetworkModeHostTestSupport.h"
#include "NetworkProductPrefabTestSupport.h"

#include <chrono>
#include <thread>

namespace Horo::Network {
    namespace {
        using namespace PrefabProductTestSupport;

        /** @brief Delivers Prefab records through the authenticated client dispatcher used by the product. */
        void DeliverReplicaRecord(ProductOwners &owners, const NetworkPrefabLifecycleRecord &record,
                                  MessageSequenceNumber::ValueType sequence) {
            const auto payload = EncodeNetworkPrefabLifecycle(record).Value();
            const auto &message = owners.worldSetup.policy.prefabLifecycle->control;
            MessageEnvelopePayloadView frame{message.protocol, message.id, message.schema, {1, 0}, MessageSequenceNumber{sequence}, {}, 0,
                                             payload};
            std::array<std::byte, 1024> bytes{};
            const auto size = EncodeMessageEnvelopePayloadInto(frame, *owners.worldSetup.codecs, bytes).Value();
            auto &transport = owners.binding->transport;
            transport.event = {.kind = NetworkTransportEventKind::PacketReceived,
                               .connection = Connection(),
                               .channel = {},
                               .delivery = DeliveryPolicy::ReliableOrdered,
                               .payload = {bytes.begin(), bytes.begin() + size}};
            transport.queued = true;
            const auto delivered = owners.binding->dispatcher->RunNetworkPoll(22);
            REQUIRE(delivered.HasValue());
            REQUIRE(delivered.Value().rejectedPackets == 0);
        }

        /** @brief Builds the authority wire fixture for an exact replicated parent reference. */
        std::vector<std::byte> IntentWithReplicatedParent(NetworkObjectId parent) {
            Runtime::CanonicalValueWriter writer;
            REQUIRE(writer.WriteUInt16(1).HasValue());
            REQUIRE(writer.WriteVec3({3, 4, 5}).HasValue());
            REQUIRE(writer.WriteQuaternion({}).HasValue());
            REQUIRE(writer.WriteVec3({1, 1, 1}).HasValue());
            REQUIRE(writer.WriteBool(true).HasValue());
            REQUIRE(writer.WriteUInt8(2).HasValue());
            REQUIRE(writer.WriteUInt64(parent.Epoch().Value()).HasValue());
            REQUIRE(writer.WriteUInt64(parent.Slot()).HasValue());
            REQUIRE(writer.WriteUInt32(parent.Generation()).HasValue());
            REQUIRE(writer.WriteUInt16(0).HasValue());
            REQUIRE(writer.WriteUInt16(0).HasValue());
            REQUIRE(writer.WriteBool(false).HasValue());
            const auto encoded = std::move(writer).Finalize().Value();
            return {encoded.Bytes().begin(), encoded.Bytes().end()};
        }
    }  // namespace

    TEST_CASE("Dedicated product authority maps a real native cooked Prefab through the canonical world",
              "[integration][network][mode][replication][prefab]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        using namespace Application::Internal;
        const auto kind = NetworkModeWorldKind::AuthorityServer;
        const auto mode = NetworkProjectRole::DedicatedServer;
        const std::array worlds{World(kind, 51)};
        const auto prefab = Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value();
        const auto type = Assets::AssetTypeId::Parse("core.prefab").Value();
        JobSystem jobs{{1, 8}};
        Assets::AssetRegistry registry;
        Assets::MemoryAssetProvider bytes;
        Assets::AssetLoadService loads{jobs, bytes};
        REQUIRE(registry
                    .Publish({{prefab, type, ProjectPath::Parse("assets/spawn.prefab").Value(),
                               ProjectPath::Parse("assets/spawn.prefab.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        const auto artifact = PutNetworkPrefab(bytes, prefab, type);
        Runtime::SceneDefinitionBuilder builder{{19}, {1}};
        Runtime::RuntimeEntityDefinition spawner{.object = {10}};
        spawner.components.behaviors.emplace_back(Gameplay::BehaviorInstanceId{1},
                                                  Gameplay::BehaviorTypeId::Parse("game.tests.prefab_spawner").Value(), 1, true,
                                                  std::vector<Gameplay::BehaviorField>{{"asset", prefab.ToString()},
                                                                                       {"digest", FormatSha256(artifact)}});
        builder.Add(std::move(spawner));
        auto definition = std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(builder).Build().Value());
        HeadlessSceneLoadBoundary load{{}, std::chrono::steady_clock::now() + std::chrono::seconds{5}};
        const auto declaration = NativePrefabDeclaration();
        HeadlessSceneActivationInput input{.mode = Physics::PhysicsRuntimeMode::Canonical,
                                           .settings = Physics::Settings(),
                                           .registry = registry,
                                           .loads = loads,
                                           .load = load,
                                           .prefabLoads = &loads,
                                           .publicTransforms =
                                               HeadlessSceneTransformReplication{declaration.schema.id,
                                                                                 declaration.schema.version,
                                                                                 {{declaration.schema.fields.front().id,
                                                                                   SceneReplicationProperty::TranslationX}}}};
        Gameplay::GameplayReplicationAdmissionPolicy admission;
        admission.inventory.completeness = NetworkReplicationInventoryCompleteness::Complete;
        admission.inventory.declarations = {declaration};
        admission.use = ReplicationDeclarationUse::ServerActivation;
        const GameplayWorldSelection selection{.moduleId = "game.tests",
                                               .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                               .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                               .prefabPermission = GameplayPrefabPermission::Granted,
                                               .moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>()};
        auto projectInput = DefaultNetworkProjectSettings(Id<NetworkProjectSettingsId>(10)).Value();
        projectInput.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::DedicatedServer;
        projectInput.transport.requirement = NetworkProjectTransportRequirement::Required;
        projectInput.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
        projectInput.transport.capabilities.requiredChannels = 2;
        projectInput.transport.capabilities.requiredMaximumMessageBytes = 1024;
        projectInput.replication = admission.inventory;
        const auto project = NetworkProjectSettings::Create(projectInput).Value();
        const auto facts = ProductFacts(project, mode, false);
        const auto owners = std::make_shared<ProductOwners>();
        std::uint64_t fixedTick{};
        owners->onTransportFixed = [&fixedTick](const Runtime::FixedStepContext &context) {
            fixedTick = context.simulationTick;
        };
        auto factories = ComposeHeadlessSceneNetworkServices(definition, input, ProductFactories(owners), selection, admission);
        REQUIRE(factories.HasValue());
        auto composition = std::move(factories).Value();
        const INetworkModeService *sceneOwner{};
        auto &sceneFactory = composition.factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)];
        sceneFactory = [factory = std::move(sceneFactory), &sceneOwner](const NetworkModeServiceRequest &request) {
            auto created = factory(request);
            if (created.HasValue())
                sceneOwner = created.Value().get();
            return created;
        };
        DeterministicClock clock;
        auto host = NetworkProductHost::Create(clock,
                                               {project,
                                                facts.product,
                                                facts.inventory,
                                                facts.host,
                                                facts.requirements,
                                                facts.selection,
                                                6,
                                                worlds,
                                                {},
                                                owners->sessions,
                                                NetworkWorldCompositionPurpose::Gameplay,
                                                owners->worldSetup,
                                                composition.fixedParticipant,
                                                composition.pollRetirement},
                                               std::move(composition.factories))
                        .Value();
        const auto started = host->Startup();
        if (started.HasError())
            UNSCOPED_INFO(started.ErrorValue().message);
        REQUIRE(started.HasValue());
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE(sceneOwner);
        const auto source = AcquireHeadlessPublicTransforms(*sceneOwner);
        REQUIRE(source);
        const auto scene = worlds.front().scene;
        const auto role = host->Role(kind, scene).Value();
        const auto snapshot = [&] {
            return host
                ->AcquireReplication(kind, {scene, *role.worldSession, Runtime::RuntimePhase::NetworkFlush, 1},
                                     ReplicationWorldCapability::CaptureCanonicalState)
                .Value();
        };
        REQUIRE(snapshot().Mapping().Entries().empty());
        const auto awaitMappings = [&](std::size_t count) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            do {
                clock.Advance(Duration::FromNanoseconds(16'666'667));
                REQUIRE(host->RunFrame().HasValue());
                auto captured = snapshot();
                if (captured.Mapping().Entries().size() == count)
                    return captured;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("Expected the complete Prefab mapping transition.");
            return snapshot();
        };
        const auto spawned = awaitMappings(2);
        const auto mappings = spawned.Mapping().Entries();
        REQUIRE(mappings.size() == 2);
        const auto spawnFrameTick = fixedTick;
        REQUIRE(spawnFrameTick > 0);
        // Deferred Scene publication follows fixed commitment and invalidates that frame's old Scene view.
        const auto beforeFixed = source->BeginRead(mappings.front(), spawnFrameTick);
        REQUIRE(beforeFixed.HasError());
        CHECK(beforeFixed.ErrorValue().code.Value() == ReplicationCaptureErrors::Uncommitted.code.Value());
        std::vector<std::uint32_t> slots;
        for (const auto &mapping : mappings) {
            CHECK(mapping.entity.runtime == scene);
            CHECK(mapping.provenance.schema == admission.inventory.declarations.front().schema.id);
            CHECK_FALSE(mapping.provenance.authoredObject);
            REQUIRE(mapping.provenance.prefab);
            const auto &origin = *mapping.provenance.prefab;
            CHECK(origin.occurrence == mappings.front().object);
            CHECK(origin.source.recipe == NetworkPrefabTemplateIdentity{prefab, artifact, 3});
            slots.push_back(origin.source.entitySlot);
        }
        CHECK(slots == std::vector<std::uint32_t>{0, 2});
        const auto encodedSpawn = CaptureNetworkPrefabSpawn(spawned, mappings.front().object).Value();
        const auto wire = EncodeNetworkPrefabLifecycle(encodedSpawn).Value();
        const auto decoded = DecodeNetworkPrefabLifecycle(wire).Value();
        REQUIRE(decoded.spawn);
        CHECK(decoded.session == *role.worldSession);
        CHECK(decoded.spawn->recipe == NetworkPrefabTemplateIdentity{prefab, artifact, 3});
        CHECK(decoded.spawn->members[1].entitySlot == 2);
        CHECK(decoded.spawn->members[1].object == mappings[1].object);
        REQUIRE_FALSE(decoded.spawn->intent.empty());
        auto replica = Runtime::RuntimeScene::Create(*definition, {52}).Value();
        const auto reconstructed = DecodeHeadlessPrefabIntent(decoded, AssetCookTargetId::Parse("linux-x64").Value(),
                                                              Prefab::PrefabLimitProfile::Create({}).Value(), replica->View(), {});
        REQUIRE(reconstructed.HasValue());
        CHECK(reconstructed.Value().source.asset == prefab);
        CHECK(reconstructed.Value().source.artifactDigest == artifact);
        std::optional<Result<ReplicationCommittedRead>> committed;
        bool currentAtFlush{};
        owners->onTransportFlush = [&] {
            committed.emplace(source->BeginRead(mappings.front(), fixedTick));
            if (committed->HasValue()) {
                currentAtFlush = source->IsCurrent(committed->Value());
                source->EndRead(committed->Value());
            }
        };
        clock.Advance(Duration::FromNanoseconds(16'666'667));
        const auto capturedFrame = host->RunFrame();
        owners->onTransportFlush = {};
        REQUIRE(capturedFrame.HasValue());
        REQUIRE(fixedTick > spawnFrameTick);
        REQUIRE(committed.has_value());
        if (committed->HasError())
            UNSCOPED_INFO(committed->ErrorValue().message);
        REQUIRE(committed->HasValue());
        CHECK(currentAtFlush);

        const auto committedTick = fixedTick;
        CHECK(awaitMappings(0).Mapping().Entries().empty());
        CHECK(CaptureNetworkPrefabSpawn(snapshot(), decoded.occurrence).HasError());
        host->Shutdown();
        CHECK(spawned.IsRevoked());
        CHECK(CaptureNetworkPrefabSpawn(spawned, decoded.occurrence).HasError());
        CHECK(source->BeginRead(mappings.front(), committedTick).HasError());
    }

    TEST_CASE("Network Prefab intent reconstructs typed inputs and authored references in the local Scene",
              "[integration][network][mode][replication][prefab]") {
        using namespace Application::Internal;
        Runtime::SceneDefinitionBuilder builder{{19}, {1}};
        builder.Add(Runtime::RuntimeEntityDefinition{.object = {10}});
        const auto definition = std::move(builder).Build().Value();
        auto authority = Runtime::RuntimeScene::Create(definition, {51}).Value();
        auto replica = Runtime::RuntimeScene::Create(definition, {52}).Value();
        const auto source = authority->View().Find({10}).value();
        const auto local = replica->View().Find({10}).value();
        REQUIRE(source != local);
        const auto binding = Prefab::PrefabPropertyId::Create(21).Value();
        const Prefab::PrefabSpawnRequest request{
            .placement = {.translation = {3.0F, 4.0F, 5.0F}, .scale = {2.0F, 3.0F, 4.0F}},
            .parent = source,
            .initialization = {{{7}, Math::Quaternion{}},
                               {{6}, Math::Vec3{1, 2, 3}},
                               {{5}, Math::Vec2{4, 5}},
                               {{4}, std::string{"ready"}},
                               {{3}, 3.5},
                               {{2}, std::int64_t{-24}},
                               {{1}, true}},
            .bindings = {{binding, source}},
        };
        const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
        auto encoded = EncodeHeadlessPrefabIntent(request, limits, authority->View(), {});
        REQUIRE(encoded.HasValue());
        const auto target = AssetCookTargetId::Parse("linux-x64").Value();
        NetworkPrefabLifecycleRecord record{.spawn = NetworkPrefabSpawnRecord{.intent = std::move(encoded).Value()}};
        const auto result = DecodeHeadlessPrefabIntent(record, target, limits, replica->View(), {});
        REQUIRE(result.HasValue());
        const auto &restored = result.Value();
        CHECK(restored.placement.translation == request.placement.translation);
        CHECK(restored.placement.scale == request.placement.scale);
        CHECK(restored.parent == local);
        REQUIRE(restored.bindings.size() == 1);
        CHECK(restored.bindings.front().id == binding);
        CHECK(restored.bindings.front().target == local);
        REQUIRE(restored.initialization.size() == request.initialization.size());
        for (std::size_t index = 0; index < restored.initialization.size(); ++index) {
            const auto &expected = request.initialization[request.initialization.size() - index - 1];
            CHECK(restored.initialization[index].id == expected.id);
            CHECK(restored.initialization[index].value == expected.value);
        }
        record.spawn->intent.push_back(std::byte{});
        CHECK(DecodeHeadlessPrefabIntent(record, target, limits, replica->View(), {}).HasError());
    }

    TEST_CASE("Configured network client automatically reconstructs and retires a protected cooked Prefab",
              "[integration][network][mode][replication][prefab]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        using namespace Application::Internal;
        constexpr auto kind = NetworkModeWorldKind::Client;
        constexpr auto mode = NetworkProjectRole::Client;
        const std::array worlds{World(kind, 52)};
        const auto asset = Assets::AssetId::FromBytes({8});
        const auto type = Assets::AssetTypeId::Parse("core.prefab").Value();
        JobSystem jobs{{1, 8}};
        Assets::AssetRegistry registry;
        Assets::MemoryAssetProvider bytes;
        Assets::AssetLoadService loads{jobs, bytes};
        REQUIRE(registry
                    .Publish({{asset, type, ProjectPath::Parse("assets/replica.prefab").Value(),
                               ProjectPath::Parse("assets/replica.prefab.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        const auto artifact = PutNetworkPrefab(bytes, asset, type);
        Runtime::SceneDefinitionBuilder builder{{19}, {1}};
        builder.Add(Runtime::RuntimeEntityDefinition{.object = {10}});
        auto definition = std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(builder).Build().Value());
        const HeadlessSceneLoadBoundary load{{}, std::chrono::steady_clock::now() + std::chrono::seconds{5}};
        const HeadlessSceneActivationInput input{.mode = Physics::PhysicsRuntimeMode::Canonical,
                                                 .settings = Physics::Settings(),
                                                 .registry = registry,
                                                 .loads = loads,
                                                 .load = load,
                                                 .prefabLoads = &loads,
                                                 .prefabTarget = AssetCookTargetId::Parse("linux-x64").Value()};
        Gameplay::GameplayReplicationAdmissionPolicy admission;
        admission.inventory.completeness = NetworkReplicationInventoryCompleteness::Complete;
        admission.inventory.declarations = {NativePrefabDeclaration()};
        const GameplayWorldSelection selection{.moduleId = "game.tests",
                                               .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                               .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                               .prefabPermission = GameplayPrefabPermission::Granted,
                                               .moduleRetirements = std::make_shared<Gameplay::GameModuleRetirementQueue>()};
        auto projectInput = DefaultNetworkProjectSettings(Id<NetworkProjectSettingsId>(10)).Value();
        projectInput.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client;
        projectInput.transport.requirement = NetworkProjectTransportRequirement::Required;
        projectInput.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
        projectInput.transport.capabilities.requiredChannels = 2;
        projectInput.transport.capabilities.requiredMaximumMessageBytes = 1024;
        projectInput.replication = admission.inventory;
        const auto project = NetworkProjectSettings::Create(projectInput).Value();
        const auto facts = ProductFacts(project, mode, false);
        const auto owners = std::make_shared<ProductOwners>();
        owners->selectedMode = mode;
        owners->worldSetup = ModeBindingTests::Setup(800);
        owners->worldSetup.policy.prefabLifecycle = NetworkPrefabLifecyclePolicy{{owners->worldSetup.policy.control.protocol,
                                                                                  WireIdentity<MessageTypeId>(12),
                                                                                  WireIdentity<MessageSchemaId>(13),
                                                                                  {1, 0},
                                                                                  false},
                                                                                 owners->worldSetup.policy.feature};
        auto factories = ComposeHeadlessSceneNetworkServices(definition, input, ProductFactories(owners), selection, admission);
        REQUIRE(factories.HasValue());
        DeterministicClock clock;
        auto host = NetworkProductHost::Create(clock,
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
                                               std::move(factories).Value().factories)
                        .Value();
        REQUIRE(host->Startup().HasValue());
        owners->binding->binding = owners->sessions->WorldBinding();
        auto peer = AdmittedSession();
        REQUIRE(host->AdmitClientSession(kind, peer, Connection(), Session()).HasValue());
        PublishProductClient(*host, owners, peer);
        const auto scene = worlds.front().scene;
        const auto session = host->Role(kind, scene).Value().worldSession.value();
        const auto read = [&] {
            return host
                ->AcquireReplication(kind, {scene, session, Runtime::RuntimePhase::NetworkPoll},
                                     ReplicationWorldCapability::ApplyAuthoritativeState)
                .Value();
        };
        REQUIRE(read().Mapping().Entries().empty());
        const auto epoch = read().Descriptor().authority;
        const auto anchor = NetworkObjectId::Create(epoch, 8, 2).Value();
        const auto child = NetworkObjectId::Create(epoch, 9, 3).Value();
        const auto schema = admission.inventory.declarations.front().schema.id;
        auto authorityScene = Runtime::RuntimeScene::Create(*definition, {51}).Value();
        const auto parent = authorityScene->View().Find({10}).value();
        const Prefab::PrefabSpawnRequest request{.placement = {.translation = {3, 4, 5}}, .parent = parent};
        const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
        auto intent = EncodeHeadlessPrefabIntent(request, limits, authorityScene->View(), {}).Value();
        const NetworkPrefabLifecycleRecord spawn{session, anchor,
                                                 NetworkPrefabSpawnRecord{{asset, artifact, 3},
                                                                          {{0, anchor, schema, {1, 0}}, {2, child, schema, {1, 0}}},
                                                                          std::move(intent)}};
        SECTION("unknown cooked template is rejected before publication") {
            const auto empty = read();
            auto unknown = spawn;
            unknown.spawn->recipe.asset = Assets::AssetId::FromBytes({99});
            DeliverReplicaRecord(*owners, unknown, 2);
            TestSupport::RequireError(host->RunFrame(), Prefab::PrefabErrors::AssetNotFound);
            CHECK(empty.Mapping().Entries().empty());
            CHECK(empty.IsRevoked());
        }
        SECTION("shutdown cancels accepted but unpublished reconstruction") {
            const auto empty = read();
            DeliverReplicaRecord(*owners, spawn, 2);
            host->Shutdown();
            CHECK(empty.Mapping().Entries().empty());
            CHECK(empty.IsRevoked());
        }
        const auto awaitMapping = [&](std::size_t count, std::size_t previous) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            do {
                clock.Advance(Duration::FromNanoseconds(16'666'667));
                auto frame = host->RunFrame();
                if (frame.HasError())
                    UNSCOPED_INFO(frame.ErrorValue().message);
                REQUIRE(frame.HasValue());
                const auto current = read();
                const auto entries = current.Mapping().Entries();
                if (entries.size() == count)
                    return;
                CHECK(entries.size() == previous);
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("Timed out waiting for the complete client Prefab mapping.");
        };
        SECTION("a replicated parent may arrive after its dependent") {
            const auto delayedAnchor = NetworkObjectId::Create(epoch, 6, 2).Value();
            const auto delayedChild = NetworkObjectId::Create(epoch, 7, 3).Value();
            const NetworkPrefabLifecycleRecord dependent{session, delayedAnchor,
                                                         NetworkPrefabSpawnRecord{spawn.spawn->recipe,
                                                                                  {{0, delayedAnchor, schema, {1, 0}},
                                                                                   {2, delayedChild, schema, {1, 0}}},
                                                                                  IntentWithReplicatedParent(anchor)}};
            DeliverReplicaRecord(*owners, dependent, 2);
            REQUIRE(host->RunFrame().HasValue());
            CHECK(read().Mapping().Entries().empty());
            SECTION("later arrival releases the dependent") {
                DeliverReplicaRecord(*owners, spawn, 3);
                awaitMapping(2, 0);
                awaitMapping(4, 2);
                const auto complete = read();
                CHECK(complete.Mapping().Entries().front().object == delayedAnchor);
                DeliverReplicaRecord(*owners, {session, delayedAnchor, std::nullopt}, 4);
                awaitMapping(2, 4);
                DeliverReplicaRecord(*owners, {session, anchor, std::nullopt}, 5);
                awaitMapping(0, 2);
                host->Shutdown();
            }
            SECTION("a lost prerequisite exhausts the recovery window without publishing a partial group") {
                const auto empty = read();
                auto result = Result<void>::Success();
                for (int frame = 0; frame < 256 && result.HasValue(); ++frame)
                    result = host->RunFrame();
                TestSupport::RequireError(result, NetworkErrors::NetworkObjectMappingUnknown);
                CHECK(empty.Mapping().Entries().empty());
                CHECK(empty.IsRevoked());
            }
        }
        SECTION("a complete replica is published before any later lifecycle change") {
            DeliverReplicaRecord(*owners, spawn, 2);
            awaitMapping(2, 0);
            const auto published = read();
            const auto entries = published.Mapping().Entries();
            REQUIRE(entries.size() == 2);
            CHECK(entries.front().object == anchor);
            CHECK(entries.front().entity.runtime == scene);
            CHECK(entries.back().object == child);
            REQUIRE(entries.back().provenance.prefab);
            CHECK(entries.back().provenance.prefab->source.entitySlot == 2);
            SECTION("despawn retires the whole group") {
                DeliverReplicaRecord(*owners, {session, anchor, std::nullopt}, 3);
                awaitMapping(0, 2);
                host->Shutdown();
            }
            SECTION("duplicate occurrence stops the client with the original mapping intact") {
                DeliverReplicaRecord(*owners, spawn, 3);
                TestSupport::RequireError(host->RunFrame(), NetworkErrors::NetworkObjectMappingConflict);
            }
            SECTION("stale despawn stops the client with the original mapping intact") {
                const auto stale = NetworkObjectId::Create(epoch, anchor.Slot(), 1).Value();
                DeliverReplicaRecord(*owners, {session, stale, std::nullopt}, 3);
                TestSupport::RequireError(host->RunFrame(), NetworkErrors::ReplicationWorldStale);
            }
            CHECK(published.IsRevoked());
            CHECK(published.Mapping().Entries().size() == 2);
        }
    }

}  // namespace Horo::Network
