#include "HeadlessNetworkServices.h"
#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "NetworkProductHost.h"
#include "NetworkTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace Horo::Network {
    using TestSupport::Bytes;
    using TestSupport::Connection;
    using TestSupport::Id;
    using TestSupport::Session;
    using TestSupport::WireIdentity;

    namespace {
        struct ProductOwners final {
            std::size_t factoryCalls{};
            std::size_t sceneFactoryCalls{};
            std::size_t constructedTransport{};
            std::size_t localPlayers{};
        };

        class ProductService final : public INetworkModeService {
        public:
            ProductService(std::shared_ptr<ProductOwners> owners, NetworkModeServiceRequest request)
                : owners_(std::move(owners)), request_(request) {}

            Result<void> Prepare() override {
                switch (request_.service) {
                    case NetworkModeServiceKind::Transport: {
                        DeterministicTransportDescriptor descriptor{.mode = DeterministicTransportMode::Loopback,
                                                                    .maximumScheduledDeliveries = 16,
                                                                    .maximumPayloadBytes = 32,
                                                                    .maximumChannels = 2,
                                                                    .budgetCapacity = {2, 8, 64},
                                                                    .budgetPolicy = {.contractVersion = 1,
                                                                                     .revision = 1,
                                                                                     .maximumActiveConnections = 2,
                                                                                     .maximumQueuedMessages = 8,
                                                                                     .maximumQueuedBytes = 64,
                                                                                     .maximumQueuedMessagesPerConnection = 8,
                                                                                     .maximumQueuedBytesPerConnection = 64,
                                                                                     .maximumMessagesPerTick = 8,
                                                                                     .maximumBytesPerTick = 64,
                                                                                     .maximumMessagesPerConnectionPerTick = 8,
                                                                                     .maximumBytesPerConnectionPerTick = 64,
                                                                                     .saturationGraceTicks = 2},
                                                                    .scenario = {.contractVersion = 1,
                                                                                 .revision = 1,
                                                                                 .seed = 42,
                                                                                 .maximumFragmentBytes = 4}};
                        auto created = DeterministicTransport::Create(descriptor);
                        if (created.HasError())
                            return Result<void>::Failure(created.ErrorValue());
                        transport_.emplace(std::move(created).Value());
                        ++owners_->constructedTransport;
                        return Result<void>::Success();
                    }
                    case NetworkModeServiceKind::LocalPlayer:
                        ++owners_->localPlayers;
                        return Result<void>::Success();
                    default:
                        return Result<void>::Success();
                }
            }

            Result<void> Activate() override {
                return Result<void>::Success();
            }

            Result<void> RunPhase(const Runtime::RuntimePhase phase) override {
                if (request_.service == NetworkModeServiceKind::Transport && phase == Runtime::RuntimePhase::NetworkPoll) {
                    std::array<DeterministicTransportEvent, 1> events{};
                    auto advanced = transport_->Advance(++tick_, events);
                    if (advanced.HasError())
                        return Result<void>::Failure(advanced.ErrorValue());
                }
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                if (transport_.has_value())
                    static_cast<void>(transport_->Shutdown());
            }

        private:
            std::shared_ptr<ProductOwners> owners_;
            NetworkModeServiceRequest request_;
            std::optional<DeterministicTransport> transport_;
            std::uint64_t tick_{};
        };

        [[nodiscard]] NetworkModeFactories ProductFactories(const std::shared_ptr<ProductOwners> &owners) {
            NetworkModeFactories factories;
            for (auto &factory : factories.services) {
                factory = [owners](const NetworkModeServiceRequest request) -> Result<std::unique_ptr<INetworkModeService>> {
                    ++owners->factoryCalls;
                    return Result<std::unique_ptr<INetworkModeService>>::Success(std::make_unique<ProductService>(owners, request));
                };
            }
            return factories;
        }

        [[nodiscard]] NetworkModeWorld World(const NetworkModeWorldKind kind, const std::uint64_t scene,
                                             const std::uint64_t authority = 0) {
            return {kind, Runtime::SceneRuntimeId{scene},
                    authority == 0 ? ReplicationAuthorityEpoch{} : ReplicationAuthorityEpoch::Create(authority).Value()};
        }

        [[nodiscard]] PeerSessionLifecycle AdmittedSession() {
            auto session = PeerSessionLifecycle::Create(Connection(), Session(), {10, 20, 30, 100, 10}).Value();
            REQUIRE(session.BeginNegotiation(Connection(), Session(), 1).HasValue());
            HandshakeSelection selection;
            selection.connection = Connection();
            selection.sessionGeneration = Session();
            selection.protocol = WireIdentity<ProtocolId>(1);
            selection.version = {1, 2};
            selection.schemaFingerprint = 42;
            selection.features.values[0] = WireIdentity<ProtocolFeatureId>(1);
            selection.features.values[1] = WireIdentity<ProtocolFeatureId>(2);
            selection.features.count = 2;
            selection.transport.capabilityRevision = 3;
            selection.transport.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            selection.transport.channelCount = 2;
            selection.transport.maximumMessageBytes = 1200;
            REQUIRE(session.AcceptNegotiation(selection, 2).HasValue());
            AuthenticationResult authenticated;
            authenticated.connection = Connection();
            authenticated.sessionGeneration = Session();
            authenticated.policy = Id<NetworkTrustPolicyId>(10);
            authenticated.policyRevision = 4;
            authenticated.principal.principal = Id<NetworkPrincipalId>(20);
            authenticated.principal.session.bytes = Bytes<NetworkSessionIdBytes>(0x80);
            authenticated.principal.trustLevel = NetworkTrustLevel::ProductAnchor;
            authenticated.principal.roles.values[0] = Id<NetworkRoleId>(30);
            authenticated.principal.roles.count = 1;
            authenticated.principal.capabilities.values[0] = Id<NetworkCapabilityId>(40);
            authenticated.principal.capabilities.count = 1;
            authenticated.principal.provenance = Id<CredentialProvenanceId>(50);
            authenticated.principal.expiresAtTick = 150;
            authenticated.secureChannel.stamp = {Connection(), Session(), 5};
            authenticated.secureChannel.binding = Id<PrivateKeyBindingId>(11);
            authenticated.secureChannel.channel = Id<SecureChannelId>(12);
            authenticated.secureChannel.channelGeneration = 6;
            authenticated.secureChannel.bindingDigest = Bytes<AuthenticationDigestBytes>(0x20);
            REQUIRE(session.AcceptAuthentication(authenticated, 11).HasValue());
            REQUIRE(session.Activate({Connection(), Session(), Id<SecureChannelId>(12), 6, Bytes<AuthenticationDigestBytes>(0x20)}, 21)
                        .HasValue());
            return session;
        }

        struct ProductCapabilities final {
            NetworkProductCapabilityManifest product;
            NetworkTargetPackageInventory inventory;
            NetworkTargetHostFacts host;
            NetworkTargetRequirements requirements;
            NetworkTargetSelection selection;
        };

#if defined(_WIN32)
        constexpr auto testPlatform = NetworkTargetPlatform::Windows;
#elif defined(__APPLE__)
        constexpr auto testPlatform = NetworkTargetPlatform::MacOS;
#else
        constexpr auto testPlatform = NetworkTargetPlatform::Linux;
#endif

        [[nodiscard]] NetworkProductCapabilityManifest ProductManifest(const NetworkProjectSettings &project,
                                                                       const TransportCapabilities &capabilities,
                                                                       const NetworkTransportProviderId provider) {
            NetworkProductCapabilityManifest product;
            product.build = Id<NetworkProductBuildId>(1);
            product.revision = Id<NetworkProductCapabilityRevision>(2);
            product.platform = testPlatform;
            product.supportedRoles = project.SupportedRoles();
            product.includesNetworkRuntime = true;
            product.profile = project.Profile().id;
            product.profileRevision = project.Profile().revision;
            product.protocol = project.Protocol();
            product.providerCount = 1;
            product.providers[0] = {provider, std::uint32_t{1} << static_cast<std::uint8_t>(testPlatform), capabilities};
            return product;
        }

        [[nodiscard]] ProductCapabilities ProductFacts(const NetworkProjectSettings &project, const NetworkProjectRole mode,
                                                       const bool omitRoleFromPackage) {
            TransportCapabilities capabilities;
            capabilities.revision = 1;
            capabilities.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = TransportSupport::Available;
            capabilities.maximumChannels = 4;
            capabilities.maximumMessageBytes = 4096;
            const auto provider = Id<NetworkTransportProviderId>(5);
            ProductCapabilities facts;
            facts.product = ProductManifest(project, capabilities, provider);
            const auto &product = facts.product;
            auto &inventory = facts.inventory;
            inventory.build = product.build;
            inventory.platform = testPlatform;
            inventory.supportedRoles = product.supportedRoles;
            inventory.includesNetworkRuntime = true;
            inventory.protocol = product.protocol;
            if (omitRoleFromPackage)
                inventory.supportedRoles = NetworkProjectRoleSet::Standalone;
            inventory.providerCount = 1;
            inventory.providers[0] = product.providers[0];
            auto &host = facts.host;
            host.revision = Id<NetworkHostCapabilityRevision>(3);
            host.platform = testPlatform;
            host.supportedRoles = product.supportedRoles;
            host.networkRuntimeInstalled = true;
            host.protocol = product.protocol;
            host.providerCount = 1;
            host.providers[0] = {provider, true, true, true, capabilities};
            auto &requirements = facts.requirements;
            requirements.requiredRoles = mode == NetworkProjectRole::Standalone     ? NetworkProjectRoleSet::Standalone
                                         : mode == NetworkProjectRole::Client       ? NetworkProjectRoleSet::Client
                                         : mode == NetworkProjectRole::ListenServer ? NetworkProjectRoleSet::ListenServer
                                                                                    : NetworkProjectRoleSet::DedicatedServer;
            requirements.requiredProvider = mode == NetworkProjectRole::Standalone ? NetworkTransportProviderId{} : provider;
            auto &selection = facts.selection;
            selection.role = mode;
            selection.expectedBuild = product.build;
            selection.expectedProductRevision = product.revision;
            selection.expectedHostRevision = host.revision;
            selection.expectedProjectRevision = project.Revision();
            if (mode != NetworkProjectRole::Standalone) {
                selection.provider = provider;
                selection.protocolVersion = {1, 0};
            }
            return facts;
        }

        [[nodiscard]] Result<std::unique_ptr<Application::Internal::NetworkProductHost>> Product(
            Clock &clock, const NetworkProjectRole mode, const std::span<const NetworkModeWorld> worlds,
            const NetworkModePresentation presentation, const std::shared_ptr<ProductOwners> &owners,
            const bool omitRoleFromPackage = false, const bool unknownReplicationInventory = false) {
            auto input = DefaultNetworkProjectSettings(Id<NetworkProjectSettingsId>(10)).Value();
            input.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client | NetworkProjectRoleSet::ListenServer |
                                   NetworkProjectRoleSet::DedicatedServer;
            input.transport.requirement = NetworkProjectTransportRequirement::Required;
            input.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            input.transport.capabilities.requiredChannels = 2;
            input.transport.capabilities.requiredMaximumMessageBytes = 1024;
            if (unknownReplicationInventory)
                input.replication.completeness = NetworkReplicationInventoryCompleteness::Unknown;
            const auto project = NetworkProjectSettings::Create(input);
            REQUIRE(project.HasValue());
            const auto facts = ProductFacts(project.Value(), mode, omitRoleFromPackage);
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{1}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{1};
            builder.Add(std::move(entity));
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto factories = Application::Internal::ComposeHeadlessNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(
                                                                                       std::move(definition).Value()),
                                                                                   ProductFactories(owners));
            auto &sceneFactory = factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)];
            sceneFactory = [owners, original = std::move(sceneFactory)](const NetworkModeServiceRequest &request) {
                ++owners->sceneFactoryCalls;
                return original(request);
            };
            return Application::Internal::NetworkProductHost::Create(clock,
                                                                     {project.Value(), facts.product, facts.inventory, facts.host,
                                                                      facts.requirements, facts.selection, 6, worlds, presentation},
                                                                     std::move(factories));
        }

        void VerifyClientLifecycle(Application::Internal::NetworkProductHost &host, const NetworkProjectRole mode,
                                   const std::span<const NetworkModeWorld> worlds) {
            const auto clientIndex = worlds.size() - 1;
            auto pending = PeerSessionLifecycle::Create(Connection(), Session(), {10, 20, 30, 100, 10});
            REQUIRE(pending.HasValue());
            CHECK_FALSE(host.AdmitClientSession(NetworkModeWorldKind::Client, pending.Value(), Connection(), Session(), 1).HasValue());
            CHECK_FALSE(host.Role(NetworkModeWorldKind::Client, worlds[clientIndex].scene).Value().session.has_value());
            auto active = AdmittedSession();
            REQUIRE(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session(), 22).HasValue());
            CHECK(host.Role(NetworkModeWorldKind::Client, worlds[clientIndex].scene).Value().session == Session());
            std::array<NetworkModeWorld, 2> replacement{};
            if (mode == NetworkProjectRole::Client) {
                replacement[0] = World(NetworkModeWorldKind::Client, 102);
            } else {
                replacement[0] = World(NetworkModeWorldKind::AuthorityServer, 103, 110);
                replacement[1] = World(NetworkModeWorldKind::Client, 104);
            }
            REQUIRE(host.RequestTravel({replacement.data(), worlds.size()}, Session()).HasValue());
            REQUIRE(host.RunFrame().HasValue());
            CHECK_FALSE(host.TakeTravelFailure().has_value());
            for (std::size_t index = 0; index < worlds.size(); ++index) {
                CHECK_FALSE(host.Role(worlds[index].kind, worlds[index].scene).HasValue());
                CHECK(host.Role(replacement[index].kind, replacement[index].scene).HasValue());
            }
            REQUIRE(host.DisconnectClient(NetworkModeWorldKind::Client, Session()).HasValue());
            CHECK_FALSE(host.Role(NetworkModeWorldKind::Client, replacement[clientIndex].scene).Value().session.has_value());
            CHECK_FALSE(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session(), 23).HasValue());
        }

        void VerifyNonClientTravel(Application::Internal::NetworkProductHost &host, const NetworkProjectRole mode,
                                   const std::span<const NetworkModeWorld> worlds) {
            const std::array replacement{mode == NetworkProjectRole::Standalone ? World(NetworkModeWorldKind::Standalone, 101)
                                                                                : World(NetworkModeWorldKind::AuthorityServer, 105, 111)};
            REQUIRE(host.RequestTravel(worlds).HasValue());
            REQUIRE(host.RunFrame().HasValue());
            CHECK(host.TakeTravelFailure().has_value());
            CHECK(host.Role(worlds[0].kind, worlds[0].scene).HasValue());
            REQUIRE(host.RequestTravel(replacement).HasValue());
            REQUIRE(host.RunFrame().HasValue());
            CHECK_FALSE(host.TakeTravelFailure().has_value());
            CHECK_FALSE(host.Role(worlds[0].kind, worlds[0].scene).HasValue());
            CHECK(host.Role(replacement[0].kind, replacement[0].scene).HasValue());
        }

        /** @brief Construct the exact independently routed world set used by each concrete product mode. */
        std::size_t ProductWorlds(const NetworkProjectRole mode, std::array<NetworkModeWorld, 2> &worlds) {
            switch (mode) {
                case NetworkProjectRole::Standalone:
                    worlds[0] = World(NetworkModeWorldKind::Standalone, 1);
                    return 1;
                case NetworkProjectRole::Client:
                    worlds[0] = World(NetworkModeWorldKind::Client, 2);
                    return 1;
                case NetworkProjectRole::ListenServer:
                    worlds[0] = World(NetworkModeWorldKind::AuthorityServer, 3, 10);
                    worlds[1] = World(NetworkModeWorldKind::Client, 4);
                    return 2;
                case NetworkProjectRole::DedicatedServer:
                    worlds[0] = World(NetworkModeWorldKind::AuthorityServer, 5, 11);
                    return 1;
                case NetworkProjectRole::Count:
                    FAIL("Invalid closed network mode");
            }
            return 0;
        }

        /** @brief Require failed canonical startup to publish no partial roles when the solver is not installed. */
        void VerifyUnavailableRoles(const Application::Internal::NetworkProductHost &host, const std::span<const NetworkModeWorld> worlds) {
            for (const auto &world : worlds)
                CHECK(host.Role(world.kind, world.scene).HasError());
        }
    }  // namespace

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
            for (std::size_t index = 0; index < count; ++index)
                CHECK(host->Role(worlds[index].kind, worlds[index].scene).HasValue());
            if (mode == NetworkProjectRole::Client || mode == NetworkProjectRole::ListenServer)
                VerifyClientLifecycle(*host, mode, {worlds.data(), count});
            else
                VerifyNonClientTravel(*host, mode, {worlds.data(), count});
            host->Shutdown();
            CHECK_FALSE(host->Role(worlds[0].kind, worlds[0].scene).HasValue());
        }
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
        const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 5, 11)};
        const auto owners = std::make_shared<ProductOwners>();
        auto created = Product(clock, NetworkProjectRole::DedicatedServer, worlds, {}, owners, true);
        CHECK(created.HasError());
        CHECK(owners->constructedTransport == 0);
    }
}  // namespace Horo::Network
