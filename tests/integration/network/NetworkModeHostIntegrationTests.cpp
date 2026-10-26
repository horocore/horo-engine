#include "NetworkModeHostTestSupport.h"

namespace Horo::Network {
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
