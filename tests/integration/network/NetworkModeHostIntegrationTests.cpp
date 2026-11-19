#include "../../helpers/GameplayModuleTestSupport.h"
#include "../../unit/runtime/physics/PhysicsSceneActivationTestSupport.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "NetworkModeHostTestSupport.h"

#include <chrono>
#include <thread>

namespace Horo::Network {
    namespace {
        /** @brief The real native test module declares this component as its sole replicated entity owner. */
        ReplicationDeclaration NativePrefabDeclaration() {
            return {.schema = {.id = ReplicationSchemaId::Create(1001).Value(),
                               .version = {1, 0},
                               .compatibility = {{1, 0}, {1, 0}},
                               .owner = {.value = "game.tests"},
                               .fields = {{.id = FieldId::Create(1).Value(),
                                           .valueType = ReplicationValueTypeId::Create(1).Value(),
                                           .codec = ReplicationCodecId::Create(1).Value(),
                                           .introducedVersion = {1, 0},
                                           .limits = {8, 1}}}}};
        }

        /** @brief Publishes a cooked Prefab containing two replicated members and a local-only child. */
        Sha256Digest PutNetworkPrefab(Assets::MemoryAssetProvider &provider, Assets::AssetId asset, const Assets::AssetTypeId &type) {
            Prefab::RawComponentPayload component{.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
                                                  .component =
                                                      {.typeId = Gameplay::ComponentTypeId::Parse("game.tests.movement_settings").Value(),
                                                       .schemaVersion = 2,
                                                       .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson}};
            for (const unsigned char byte : std::string_view{R"({"speed":4.25})"})
                component.component.payload.push_back(static_cast<std::byte>(byte));
            Prefab::CookedPrefabEntity root;
            root.provenance = {asset, {}, ComputeSha256(std::as_bytes(std::span("source", 6)))};
            root.members.emplace_back(std::move(component));
            auto child = root;
            child.parent = Prefab::CookedPrefabEntitySlot{0};
            child.provenance.sourceObject = Prefab::PrefabObjectAddress::Create({}, {7}).Value();
            auto local = child;
            local.provenance.sourceObject = Prefab::PrefabObjectAddress::Create({}, {8}).Value();
            local.members.clear();
            auto cooked =
                Prefab::CookedPrefab::Create({.assetId = asset, .entities = {std::move(root), std::move(child), std::move(local)}},
                                             Prefab::PrefabLimitProfile::Create({}).Value());
            REQUIRE(cooked.HasValue());
            std::vector<std::uint8_t> payload;
            for (const auto byte : cooked.Value().Bytes())
                payload.push_back(std::to_integer<std::uint8_t>(byte));
            const auto target = AssetCookTargetId::Parse("linux-x64").Value();
            const auto digest = ComputeSha256(std::as_bytes(std::span(payload)));
            auto bytes = Assets::EncodeCookedArtifact({asset, type, target, {}, {}, digest, std::move(payload)});
            REQUIRE(bytes.HasValue());
            const auto envelope = ComputeSha256(std::as_bytes(std::span(bytes.Value())));
            provider.Insert(asset, std::move(bytes).Value());
            return envelope;
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
        HeadlessSceneActivationInput input{.mode = Physics::PhysicsRuntimeMode::Canonical,
                                           .settings = Physics::Settings(),
                                           .registry = registry,
                                           .loads = loads,
                                           .load = load,
                                           .prefabLoads = &loads};
        Gameplay::GameplayReplicationAdmissionPolicy admission;
        admission.inventory.completeness = NetworkReplicationInventoryCompleteness::Complete;
        admission.inventory.declarations = {NativePrefabDeclaration()};
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
                                                {},
                                                owners->sessions,
                                                NetworkWorldCompositionPurpose::Gameplay,
                                                owners->worldSetup},
                                               std::move(factories).Value().factories)
                        .Value();
        const auto started = host->Startup();
        if (started.HasError())
            UNSCOPED_INFO(started.ErrorValue().message);
        REQUIRE(started.HasValue());
        REQUIRE(host->RunFrame().HasValue());
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
        for (const auto &mapping : spawned.Mapping().Entries()) {
            CHECK(mapping.entity.runtime == scene);
            CHECK(mapping.provenance.schema == admission.inventory.declarations.front().schema.id);
            CHECK_FALSE(mapping.provenance.authoredObject);
        }
        CHECK(awaitMappings(0).Mapping().Entries().empty());
        host->Shutdown();
        CHECK(spawned.IsRevoked());
    }

    TEST_CASE("Authority host publishes one committed authored Scene mapping per world generation",
              "[integration][network][mode][replication]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        using namespace Application::Internal;
        Application::Internal::TestSupport::ReplicatedScriptFixture script{false, true};
        const auto &admission = *script.selection.replicationAdmission;
        const auto &schema = admission.inventory.declarations.front().schema;
        const auto behavior = Gameplay::BehaviorTypeId::Parse("game.tests.replicated").Value();
        bool duplicateOwners{};
        bool discover{};
        SECTION("explicit host bindings publish at the actual world safe point and travel replaces them") {}
        SECTION("cooked enabled behavior owners supply the root and child mappings") {
            discover = true;
        }
        SECTION("ambiguous cooked behavior occurrences fail startup without publishing a role") {
            discover = true;
            duplicateOwners = true;
        }
        Runtime::SceneDefinitionBuilder builder{{19}, {1}};
        const auto components = [&](const bool enabled) {
            Runtime::RuntimeComponentSet result;
            result.behaviors.push_back({Gameplay::BehaviorInstanceId{1}, behavior, 1, enabled, {}});
            return result;
        };
        auto root = components(true);
        if (duplicateOwners)
            root.behaviors.push_back({Gameplay::BehaviorInstanceId{2}, behavior, 1, true, {}});
        builder.Add({.object = {10}, .components = std::move(root)});
        builder.Add({.object = {20}, .parent = Runtime::SceneObjectId{10}, .components = components(true)});
        builder.Add({.object = {30}, .components = components(false)});
        builder.Add({.object = {40}});
        auto authored = std::move(builder).Build();
        REQUIRE(authored.HasValue());
        const auto cooked = SceneCook::EncodeCookedSceneDefinition(authored.Value());
        REQUIRE(cooked.HasValue());
        auto decoded = SceneCook::DecodeCookedSceneDefinition(cooked.Value(), authored.Value().Id(), authored.Value().Revision());
        REQUIRE(decoded.HasValue());
        auto definition = std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(decoded).Value());

        JobSystem jobs{{1, 8}};
        Assets::AssetRegistry registry;
        Assets::MemoryAssetProvider bytes;
        Assets::AssetLoadService loads{jobs, bytes};
        HeadlessSceneLoadBoundary load{{}, std::chrono::steady_clock::now() + std::chrono::seconds{5}};
        HeadlessSceneActivationInput selection{.mode = Physics::PhysicsRuntimeMode::Canonical,
                                               .settings = Physics::Settings(),
                                               .registry = registry,
                                               .loads = loads,
                                               .load = load,
                                               .replicatedSceneObjects = {{schema.id, schema.version, Runtime::SceneObjectId{20}},
                                                                          {schema.id, schema.version, Runtime::SceneObjectId{10}}}};
        if (discover)
            selection.replicatedSceneObjects.clear();
        auto projectInput = DefaultNetworkProjectSettings(Id<NetworkProjectSettingsId>(10)).Value();
        projectInput.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::DedicatedServer;
        projectInput.transport.requirement = NetworkProjectTransportRequirement::Required;
        projectInput.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
        projectInput.transport.capabilities.requiredChannels = 2;
        projectInput.transport.capabilities.requiredMaximumMessageBytes = 1024;
        projectInput.replication = admission.inventory;
        const auto project = NetworkProjectSettings::Create(projectInput).Value();
        const auto mode = NetworkProjectRole::DedicatedServer;
        const auto kind = NetworkModeWorldKind::AuthorityServer;
        const std::array worlds{World(kind, 5)};
        const auto facts = ProductFacts(project, mode, false);
        const auto owners = std::make_shared<ProductOwners>();
        auto factories = ComposeHeadlessSceneNetworkServices(std::move(definition), std::move(selection), ProductFactories(owners),
                                                             script.selection, admission);
        REQUIRE(factories.HasValue());
        DeterministicClock clock;
        auto created = NetworkProductHost::Create(clock,
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
                                                   owners->worldSetup},
                                                  std::move(factories).Value().factories);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        const auto started = host->Startup();
        if (started.HasError())
            UNSCOPED_INFO(started.ErrorValue().message);
        CHECK(host->Role(kind, worlds.front().scene).HasError());
        if (duplicateOwners) {
            REQUIRE(started.HasError());
            CHECK(started.ErrorValue().code.Value() == NetworkErrors::NetworkObjectMappingConflict.code.Value());
            return;
        }
        REQUIRE(started.HasValue());
        const auto capture = [&](const NetworkModeWorld world) {
            const auto role = host->Role(kind, world.scene);
            REQUIRE(role.HasValue());
            REQUIRE(role.Value().worldSession);
            auto read = host->AcquireReplication(kind, {world.scene, *role.Value().worldSession, Runtime::RuntimePhase::NetworkFlush, 1},
                                                 ReplicationWorldCapability::CaptureCanonicalState);
            REQUIRE(read.HasValue());
            const auto entries = read.Value().Mapping().Entries();
            REQUIRE(entries.size() == 2);
            CHECK(entries[0].provenance.authoredObject == Runtime::SceneObjectId{10});
            CHECK(entries[1].provenance.authoredObject == Runtime::SceneObjectId{20});
            CHECK(entries[0].entity.runtime == world.scene);
            CHECK(entries[1].entity.runtime == world.scene);
            return std::move(read).Value();
        };
        REQUIRE(host->RunFrame().HasValue());
        const auto initial = capture(worlds.front());
        const std::array next{World(kind, 105)};
        REQUIRE(host->RequestTravel(next).HasValue());
        CHECK_FALSE(initial.IsRevoked());
        CHECK(host->Role(kind, next.front().scene).HasError());
        REQUIRE(host->RunFrame().HasValue());
        CHECK_FALSE(initial.IsRevoked());
        CHECK(host->Role(kind, next.front().scene).HasError());
        REQUIRE(host->RunFrame().HasValue());
        REQUIRE_FALSE(host->TakeTravelFailure());
        const auto replacement = capture(next.front());
        CHECK(initial.IsRevoked());
        CHECK(replacement.Mapping().Epoch() != initial.Mapping().Epoch());
        host->Shutdown();
    }

    TEST_CASE("Real loaded replication generations reject travel without replacing either active listen world",
              "[integration][network][mode][replication]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        for (const auto mode : {NetworkProjectRole::DedicatedServer, NetworkProjectRole::ListenServer}) {
            Application::Internal::TestSupport::ReplicatedScriptFixture script;
            DeterministicClock clock;
            std::array<NetworkModeWorld, 2> worlds{};
            const auto count = ProductWorlds(mode, worlds);
            auto owners = std::make_shared<ProductOwners>();
            auto created = Product(clock, mode, {worlds.data(), count}, {.localPlayer = mode == NetworkProjectRole::ListenServer}, owners,
                                   false, false, script.selection);
            if (created.HasError()) {
                UNSCOPED_INFO(created.ErrorValue().domain.Value());
                UNSCOPED_INFO(created.ErrorValue().code.Value());
                UNSCOPED_INFO(created.ErrorValue().message);
            }
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            REQUIRE(host->Startup().HasValue());
            std::optional<NetworkOperationGeneration> admittedSession;
            if (mode == NetworkProjectRole::ListenServer) {
                auto active = AdmittedSession();
                REQUIRE(host->AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session()).HasValue());
                PublishProductClient(*host, owners, active);
                admittedSession = Session();
            }
            REQUIRE(host->RunFrame().HasValue());
            script.ChangeCodec();
            std::array replacement{World(NetworkModeWorldKind::AuthorityServer, 203), World(NetworkModeWorldKind::Client, 204)};
            REQUIRE(host->RequestTravel({replacement.data(), count}, admittedSession).HasValue());
            REQUIRE(host->RunFrame().HasValue());
            auto failure = host->TakeTravelFailure();
            REQUIRE(failure.has_value());
            CHECK(failure->code.Value() == NetworkErrors::ReplicationDescriptorIncompatible.code.Value());
            for (std::size_t index = 0; index < count; ++index) {
                CHECK(host->Role(worlds[index].kind, worlds[index].scene).HasValue());
                CHECK(host->Role(replacement[index].kind, replacement[index].scene).HasError());
            }
            REQUIRE(host->RunFrame().HasValue());
            host->Shutdown();
        }
    }

    TEST_CASE("Actual no-gameplay world activation requires explicit complete replication admission",
              "[integration][network][replication]") {
        if constexpr (!HORO_TEST_PHYSICS_NATIVE)
            SKIP("Canonical Physics is required for actual product world activation.");
        Application::Internal::TestSupport::ReplicatedScriptFixture script;
        std::optional<Gameplay::GameplayReplicationAdmissionPolicy> policy;
        bool admitted{};
        auto world = NetworkModeWorldKind::AuthorityServer;
        SECTION("absent policy") {}
        SECTION("network mode cannot disguise a standalone world") {
            world = NetworkModeWorldKind::Standalone;
        }
        SECTION("unknown inventory") {
            policy.emplace();
        }
        SECTION("mismatched operation") {
            policy = script.selection.replicationAdmission;
            policy->use = ReplicationDeclarationUse::ClientActivation;
        }
        SECTION("missing actual module") {
            policy = script.selection.replicationAdmission;
        }
        SECTION("complete empty inventory") {
            policy.emplace();
            policy->inventory.completeness = NetworkReplicationInventoryCompleteness::Complete;
            admitted = true;
        }
        Runtime::SceneDefinitionBuilder builder{{1}, {1}};
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        auto factories = Application::Internal::ComposeHeadlessNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(
                                                                                   std::move(definition).Value()),
                                                                               {}, std::nullopt, policy);
        NetworkModeServiceRequest request{NetworkModeServiceKind::Scene, NetworkProjectRole::DedicatedServer, {}, world, {75}};
        auto scene = factories.services[static_cast<std::size_t>(request.service)](request);
        REQUIRE(scene.HasValue());
        request.service = NetworkModeServiceKind::Physics;
        auto physics = factories.services[static_cast<std::size_t>(request.service)](request);
        REQUIRE(physics.HasValue());
        REQUIRE(scene.Value()->Prepare().HasValue());
        REQUIRE(physics.Value()->Prepare().HasValue());
        REQUIRE(scene.Value()->Activate().HasValue());
        CHECK(physics.Value()->Activate().HasValue() == admitted);
        physics.Value()->Shutdown();
        scene.Value()->Shutdown();
    }

    TEST_CASE("Concrete headless product compositions honor canonical Physics availability across all four modes",
              "[integration][network][mode][headless]") {
        for (const auto mode : {NetworkProjectRole::Standalone, NetworkProjectRole::Client, NetworkProjectRole::ListenServer,
                                NetworkProjectRole::DedicatedServer}) {
            DeterministicClock clock;
            std::array<NetworkModeWorld, 2> worlds{};
            const auto count = ProductWorlds(mode, worlds);
            const auto owners = std::make_shared<ProductOwners>();
            auto created = Product(clock, mode, {worlds.data(), count},
                                   {.localPlayer = mode == NetworkProjectRole::Client || mode == NetworkProjectRole::ListenServer}, owners);
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            const auto started = host->Startup();
            INFO((started.HasError() ? started.ErrorValue().message : "Startup succeeded"));
            if constexpr (!HORO_TEST_PHYSICS_NATIVE) {
                REQUIRE(started.HasError());
                CHECK(started.ErrorValue().code.Value() == Physics::PhysicsErrors::CapabilityUnavailable.code.Value());
                VerifyUnavailableRoles(*host, {worlds.data(), count});
                host->Shutdown();
                continue;
            }
            REQUIRE(started.HasValue());
            REQUIRE(host->RunFrame().HasValue());
            clock.Advance(Duration::FromNanoseconds(16'666'667));
            REQUIRE(host->RunFrame().HasValue());
            CHECK(owners->constructedTransport == (mode == NetworkProjectRole::Standalone ? 0 : 1));
            CHECK(owners->localPlayers == (mode == NetworkProjectRole::Client || mode == NetworkProjectRole::ListenServer ? 1 : 0));
            if (mode == NetworkProjectRole::Client || mode == NetworkProjectRole::ListenServer) {
                for (std::size_t index = 0; index < count; ++index)
                    CHECK(host->Role(worlds[index].kind, worlds[index].scene).HasError());
                VerifyClientLifecycle(*host, mode, {worlds.data(), count}, owners);
            } else
                VerifyNonClientTravel(*host, mode, {worlds.data(), count});
            host->Shutdown();
            CHECK_FALSE(host->Role(worlds[0].kind, worlds[0].scene).HasValue());
        }
    }

    TEST_CASE("Concrete host disconnect and shutdown fence queued client travel at the canonical safe point",
              "[integration][network][mode][lifecycle]") {
        DeterministicClock clock;
        const std::array worlds{World(NetworkModeWorldKind::Client, 2)};
        const std::array replacement{World(NetworkModeWorldKind::Client, 102)};
        const auto owners = std::make_shared<ProductOwners>();
        auto created = Product(clock, NetworkProjectRole::Client, worlds, {.localPlayer = true}, owners);
        REQUIRE(created.HasValue());
        auto host = std::move(created).Value();
        const auto started = host->Startup();
        if constexpr (!HORO_TEST_PHYSICS_NATIVE) {
            REQUIRE(started.HasError());
            VerifyUnavailableRoles(*host, worlds);
            host->Shutdown();
            return;
        }
        REQUIRE(started.HasValue());
        auto active = AdmittedSession();
        REQUIRE(host->AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session()).HasValue());
        PublishProductClient(*host, owners, active);
        REQUIRE(host->RequestTravel(replacement, Session()).HasValue());
        SECTION("Disconnect before safe-point publication") {
            REQUIRE(host->DisconnectClient(NetworkModeWorldKind::Client, Session()).HasValue());
            REQUIRE(host->RunFrame().HasValue());
            REQUIRE(host->RequestTravel(replacement, Session()).HasError());  // Preserve the unconsumed prior failure.
            const auto failed = host->TakeTravelFailure();
            REQUIRE(failed.has_value());
            REQUIRE(failed->code.Value() == NetworkErrors::NetworkModeStale.code.Value());
            REQUIRE(host->Role(worlds[0].kind, worlds[0].scene).HasError());
            REQUIRE(host->Role(replacement[0].kind, replacement[0].scene).HasError());
        }
        SECTION("Shutdown discards queued travel") {
            host->Shutdown();
            REQUIRE(host->RunFrame().HasError());
            REQUIRE_FALSE(host->TakeTravelFailure().has_value());
            REQUIRE(host->Role(worlds[0].kind, worlds[0].scene).HasError());
            REQUIRE(host->Role(replacement[0].kind, replacement[0].scene).HasError());
            REQUIRE(host->AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session()).HasError());
            REQUIRE(host->RequestTravel(replacement, Session()).HasError());
        }
        host->Shutdown();
    }

    TEST_CASE("Product launch rejects unknown authored replication inventory before client or server publication",
              "[integration][network][mode][inventory]") {
        for (const auto mode : {NetworkProjectRole::Client, NetworkProjectRole::ListenServer, NetworkProjectRole::DedicatedServer}) {
            DeterministicClock clock;
            std::array<NetworkModeWorld, 2> worlds{};
            const auto count = ProductWorlds(mode, worlds);
            const auto owners = std::make_shared<ProductOwners>();
            const auto created = Product(clock, mode, {worlds.data(), count}, {}, owners, false, true);
            REQUIRE(created.HasError());
            REQUIRE(created.ErrorValue().diagnostics.front().code.Value() == "replication.inventory.incomplete");
            REQUIRE(owners->factoryCalls == 0);
            REQUIRE(owners->sceneFactoryCalls == 0);
            REQUIRE(owners->constructedTransport == 0);
            REQUIRE(owners->localPlayers == 0);
        }
        DeterministicClock clock;
        const std::array worlds{World(NetworkModeWorldKind::Standalone, 1)};
        const auto owners = std::make_shared<ProductOwners>();
        auto offline = Product(clock, NetworkProjectRole::Standalone, worlds, {}, owners, false, true);
        REQUIRE(offline.HasValue());
        const auto started = offline.Value()->Startup();
        INFO((started.HasError() ? started.ErrorValue().message : "Startup succeeded"));
        if constexpr (HORO_TEST_PHYSICS_NATIVE) {
            REQUIRE(started.HasValue());
            REQUIRE(offline.Value()->Role(worlds.front().kind, worlds.front().scene).HasValue());
        } else {
            REQUIRE(started.HasError());
            REQUIRE(started.ErrorValue().code.Value() == Physics::PhysicsErrors::CapabilityUnavailable.code.Value());
            VerifyUnavailableRoles(*offline.Value(), worlds);
        }
        REQUIRE(owners->sceneFactoryCalls == 1);
        offline.Value()->Shutdown();
    }

    TEST_CASE("Product launch rejects a declared mode absent from actual package inventory before factories",
              "[integration][network][mode][package]") {
        DeterministicClock clock;
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 5)};
        const auto owners = std::make_shared<ProductOwners>();
        auto created = Product(clock, NetworkProjectRole::DedicatedServer, worlds, {}, owners, true);
        CHECK(created.HasError());
        CHECK(owners->constructedTransport == 0);
    }
}  // namespace Horo::Network

namespace {
    template <typename Slot>
    concept BorrowableGameplaySlot =
        requires(Slot &&slot) { Horo::Application::Internal::MakeGameplayStructuralParticipant(std::forward<Slot>(slot)); };
    static_assert(BorrowableGameplaySlot<Horo::Application::Internal::GameplayWorldComposition *&>);
    static_assert(!BorrowableGameplaySlot<Horo::Application::Internal::GameplayWorldComposition *>);
}  // namespace
