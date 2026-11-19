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
            child.provenance.sourceObject = Prefab::PrefabObjectAddress::Create({}, {8}).Value();
            auto local = child;
            local.provenance.sourceObject = Prefab::PrefabObjectAddress::Create({}, {7}).Value();
            local.members.clear();
            auto cooked =
                Prefab::CookedPrefab::Create({.assetId = asset, .entities = {std::move(root), std::move(local), std::move(child)}},
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
        const auto mappings = spawned.Mapping().Entries();
        REQUIRE(mappings.size() == 2);
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
        CHECK(awaitMappings(0).Mapping().Entries().empty());
        host->Shutdown();
        CHECK(spawned.IsRevoked());
    }

}  // namespace Horo::Network
