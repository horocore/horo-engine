#pragma once
/** @file NetworkModeHostTestSupport.h @brief Private composition, session and role fixtures for concrete mode host regressions. */
#include "../gameplay/GameplayReplicationAdmissionTestSupport.h"
#include "GameplayWorldComposition.h"
#include "HeadlessNetworkServices.h"
#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "NetworkModeBindingTestSupport.h"
#include "NetworkProductHost.h"
#include "NetworkTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
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
            NetworkWorldBindingSetup worldSetup = ModeBindingTests::Setup();
            std::shared_ptr<ModeBindingTests::BindingFixture> binding = std::make_shared<ModeBindingTests::BindingFixture>();
            std::shared_ptr<NetworkWorldBindingController> remotePublisher;
            std::shared_ptr<ModeBindingTests::BindingFixture> remoteWire;
            NetworkProjectRole selectedMode{NetworkProjectRole::Count};
            std::size_t factoryCalls{};
            std::size_t sceneFactoryCalls{};
            std::size_t constructedTransport{};
            std::size_t localPlayers{};
            NetworkModeServiceFactory transportOverride;
            std::function<void(Runtime::RuntimePhase)> beforeWorld;
            std::shared_ptr<DeterministicClock> sessionSource = std::make_shared<DeterministicClock>(Duration::FromNanoseconds(21));
            std::shared_ptr<Application::Internal::NetworkProductSessionClock> sessionClock =
                std::make_shared<Application::Internal::NetworkProductSessionClock>(sessionSource, Duration::FromNanoseconds(1),
                                                                                    Duration{});
            std::shared_ptr<Application::Internal::ClientSessionOwner> sessions =
                std::make_shared<Application::Internal::ClientSessionOwner>(sessionClock);
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

        [[nodiscard]] inline NetworkModeFactories ProductFactories(const std::shared_ptr<ProductOwners> &owners) {
            NetworkModeFactories factories;
            for (auto &factory : factories.services) {
                factory = [owners](const NetworkModeServiceRequest request) -> Result<std::unique_ptr<INetworkModeService>> {
                    ++owners->factoryCalls;
                    return Result<std::unique_ptr<INetworkModeService>>::Success(std::make_unique<ProductService>(owners, request));
                };
            }
            if (owners->transportOverride)
                factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Transport)] = owners->transportOverride;
            return factories;
        }

        [[nodiscard]] inline NetworkModeWorld World(const NetworkModeWorldKind kind, const std::uint64_t scene) {
            return {kind, Runtime::SceneRuntimeId{scene}};
        }

        [[nodiscard]] inline std::shared_ptr<PeerSessionLifecycle> AdmittedSession(
            const ConnectionHandle connection = Connection(), const NetworkOperationGeneration generation = Session(),
            const PeerSessionDeadlines deadlines = {10, 20, 30, 100, 10}, const std::uint64_t expiry = 150) {
            auto session = std::make_shared<PeerSessionLifecycle>(PeerSessionLifecycle::Create(connection, generation, deadlines).Value());
            REQUIRE(session->BeginNegotiation(connection, generation, 1).HasValue());
            HandshakeSelection selection;
            selection.connection = connection;
            selection.sessionGeneration = generation;
            selection.protocol = WireIdentity<ProtocolId>(1);
            selection.version = {1, 0};
            selection.schemaFingerprint = 42;
            selection.features.values[0] = WireIdentity<ProtocolFeatureId>(1);
            selection.features.values[1] = WireIdentity<ProtocolFeatureId>(2);
            selection.features.count = 2;
            selection.transport.capabilityRevision = 3;
            selection.transport.admittedDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            selection.transport.channelCount = 2;
            selection.transport.maximumMessageBytes = 1200;
            REQUIRE(session->AcceptNegotiation(selection, 2).HasValue());
            AuthenticationResult authenticated;
            authenticated.connection = connection;
            authenticated.sessionGeneration = generation;
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
            authenticated.principal.expiresAtTick = expiry;
            authenticated.secureChannel.stamp = {connection, generation, 5};
            authenticated.secureChannel.binding = Id<PrivateKeyBindingId>(11);
            authenticated.secureChannel.channel = Id<SecureChannelId>(12);
            authenticated.secureChannel.channelGeneration = 6;
            authenticated.secureChannel.bindingDigest = Bytes<AuthenticationDigestBytes>(0x20);
            REQUIRE(session->AcceptAuthentication(authenticated, 11).HasValue());
            REQUIRE(session->Activate({connection, generation, Id<SecureChannelId>(12), 6, Bytes<AuthenticationDigestBytes>(0x20)}, 21)
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

        [[nodiscard]] inline NetworkProductCapabilityManifest ProductManifest(const NetworkProjectSettings &project,
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

        [[nodiscard]] inline ProductCapabilities ProductFacts(const NetworkProjectSettings &project, const NetworkProjectRole mode,
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

        /** @brief Observe actual Scene entry while retaining the original production service and teardown. */
        class WorldObservationService final : public INetworkModeService {
        public:
            WorldObservationService(std::unique_ptr<INetworkModeService> service, std::shared_ptr<ProductOwners> owners)
                : service_(std::move(service)), owners_(std::move(owners)) {}

            Result<void> Prepare() override {
                return service_->Prepare();
            }

            Result<void> Activate() override {
                return service_->Activate();
            }

            Result<void> RunPhase(const Runtime::RuntimePhase phase) override {
                if (owners_->beforeWorld)
                    owners_->beforeWorld(phase);
                return service_->RunPhase(phase);
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &context) override {
                return service_->RunFixedTick(context);
            }

            void Shutdown() noexcept override {
                service_->Shutdown();
            }

        private:
            std::unique_ptr<INetworkModeService> service_;
            std::shared_ptr<ProductOwners> owners_;
        };

        /** @brief Compose the real minimal Scene and retain an observation seam before each production world callback. */
        inline NetworkModeFactories ProductWorldFactories(const NetworkProjectSettings &project,
                                                          const std::shared_ptr<ProductOwners> &owners,
                                                          const std::optional<Application::Internal::GameplayWorldSelection> &gameplay) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{19}, Runtime::SceneDefinitionRevision{1}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{1};
            if (gameplay)
                entity.components.behaviors.emplace_back(Gameplay::BehaviorInstanceId{1},
                                                         Gameplay::BehaviorTypeId::Parse("game.tests.replicated").Value(), 1, true,
                                                         std::vector<Gameplay::BehaviorField>{});
            builder.Add(std::move(entity));
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto factories = Application::Internal::ComposeHeadlessNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(
                                                                                       std::move(definition).Value()),
                                                                                   ProductFactories(owners), gameplay,
                                                                                   Gameplay::GameplayReplicationAdmissionPolicy{
                                                                                       .inventory = project.ReplicationInventory()});
            auto &sceneFactory = factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)];
            sceneFactory = [owners, original = std::move(sceneFactory)](
                               const NetworkModeServiceRequest &request) -> Result<std::unique_ptr<INetworkModeService>> {
                ++owners->sceneFactoryCalls;
                auto scene = original(request);
                if (scene.HasError())
                    return scene;
                return Result<std::unique_ptr<INetworkModeService>>::Success(
                    std::make_unique<WorldObservationService>(std::move(scene).Value(), owners));
            };
            return factories;
        }

        [[nodiscard]] inline Result<std::unique_ptr<Application::Internal::NetworkProductHost>> Product(
            Clock &clock, const NetworkProjectRole mode, const std::span<const NetworkModeWorld> worlds,
            const NetworkModePresentation presentation, const std::shared_ptr<ProductOwners> &owners,
            const bool omitRoleFromPackage = false, const bool unknownReplicationInventory = false,
            const std::optional<Application::Internal::GameplayWorldSelection> &gameplay = std::nullopt) {
            owners->selectedMode = mode;
            auto input = DefaultNetworkProjectSettings(Id<NetworkProjectSettingsId>(10)).Value();
            input.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client | NetworkProjectRoleSet::ListenServer |
                                   NetworkProjectRoleSet::DedicatedServer;
            input.transport.requirement = NetworkProjectTransportRequirement::Required;
            input.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            input.transport.capabilities.requiredChannels = 2;
            input.transport.capabilities.requiredMaximumMessageBytes = 1024;
            if (unknownReplicationInventory)
                input.replication.completeness = NetworkReplicationInventoryCompleteness::Unknown;
            if (gameplay && gameplay->replicationAdmission)
                input.replication = gameplay->replicationAdmission->inventory;
            const auto project = NetworkProjectSettings::Create(input);
            REQUIRE(project.HasValue());
            const auto facts = ProductFacts(project.Value(), mode, omitRoleFromPackage);
            auto factories = ProductWorldFactories(project.Value(), owners, gameplay);
            auto created =
                Application::Internal::NetworkProductHost::Create(clock,
                                                                  {project.Value(), facts.product, facts.inventory, facts.host,
                                                                   facts.requirements, facts.selection, 6, worlds, presentation,
                                                                   mode == NetworkProjectRole::Standalone ? nullptr : owners->sessions,
                                                                   NetworkWorldCompositionPurpose::Gameplay,
                                                                   mode == NetworkProjectRole::Standalone
                                                                       ? std::nullopt
                                                                       : std::optional{owners->worldSetup}},
                                                                  std::move(factories));
            if (created.HasValue() && mode != NetworkProjectRole::Standalone)
                owners->binding->binding = owners->sessions->WorldBinding();
            return created;
        }

        /** @brief A remote canonical Runtime owner issues identity; fixture code never constructs a world descriptor. */
        inline void PrepareRemotePublisher(const std::shared_ptr<ProductOwners> &owners) {
            if (owners->remotePublisher)
                return;
            NetworkModePlan plan;
            plan.mode = NetworkProjectRole::DedicatedServer;
            plan.hostGeneration = 1;
            plan.build = Id<NetworkProductBuildId>(1);
            plan.productRevision = Id<NetworkProductCapabilityRevision>(1);
            plan.hostRevision = Id<NetworkHostCapabilityRevision>(1);
            plan.projectRevision = Id<NetworkProjectSettingsRevision>(1);
            plan.provider = Id<NetworkTransportProviderId>(1);
            plan.protocolVersion = {1, 0};
            plan.worlds[0] = World(NetworkModeWorldKind::AuthorityServer, 900);
            plan.worldCount = 1;
            auto created = NetworkWorldBindingController::Create(plan, owners->worldSetup);
            REQUIRE(created.HasValue());
            owners->remotePublisher = std::move(created).Value();
            owners->remoteWire = std::make_shared<ModeBindingTests::BindingFixture>();
            owners->remoteWire->setup = owners->worldSetup;
            owners->remoteWire->AttachOwner(owners->remotePublisher);
            REQUIRE(owners->remotePublisher->Prepare().HasValue());
            REQUIRE(owners->remotePublisher->Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            owners->remoteWire->server = AdmittedSession(ConnectionHandle::Create(3, 1).Value(), Session(8));
            REQUIRE(owners->remotePublisher->BindPeer(AdmissionEndpoint::Server, owners->remoteWire->server, Session(8), 22).HasValue());
        }

        inline void DeliverRemoteProductControl(Application::Internal::NetworkProductHost &host,
                                                const std::shared_ptr<ProductOwners> &owners, const ConnectionHandle connection) {
            REQUIRE(owners->remotePublisher->Flush(22).HasValue());
            const auto &wire = owners->remoteWire->transport;
            auto &local = owners->binding->transport;
            local.event = {.kind = NetworkTransportEventKind::PacketReceived,
                           .connection = connection,
                           .channel = {},
                           .delivery = DeliveryPolicy::ReliableOrdered,
                           .payload = std::vector<std::byte>(wire.sent.begin(), wire.sent.begin() + wire.size)};
            local.queued = true;
            const auto delivered = owners->binding->dispatcher->RunNetworkPoll(22);
            REQUIRE(delivered.HasValue());
            REQUIRE(delivered.Value().rejectedPackets == 0);
            REQUIRE(host.RunFrame().HasValue());
        }

        inline void PublishProductClient(Application::Internal::NetworkProductHost &host, const std::shared_ptr<ProductOwners> &owners,
                                         const std::shared_ptr<PeerSessionLifecycle> &client) {
            owners->binding->setup = owners->worldSetup;
            if (!owners->binding->dispatcher)
                owners->binding->AttachOwner(owners->binding->binding);
            owners->binding->RegisterClient(client);
            REQUIRE(host.RunFrame().HasValue());
            if (owners->selectedMode == NetworkProjectRole::ListenServer) {
                owners->binding->server = AdmittedSession(ConnectionHandle::Create(3, 1).Value(), Session(8));
                auto owner = owners->binding->binding.lock();
                REQUIRE(owner);
                REQUIRE(owner->BindPeer(AdmissionEndpoint::Server, owners->binding->server, Session(8), 22).HasValue());
                owners->binding->Deliver();
            } else {
                PrepareRemotePublisher(owners);
                DeliverRemoteProductControl(host, owners, client->Connection());
                return;  // The same helper already advances the actual commit safe point exactly once.
            }
            REQUIRE(host.RunFrame().HasValue());
        }

        inline void CompleteProductTravel(Application::Internal::NetworkProductHost &host, const std::shared_ptr<ProductOwners> &owners) {
            REQUIRE(host.RunFrame().HasValue());  // Stage all local services at the actual product safe point.
            if (owners->selectedMode == NetworkProjectRole::ListenServer) {
                owners->binding->Deliver();
                REQUIRE(host.RunFrame().HasValue());
                owners->binding->Deliver();
                return;
            }
            REQUIRE(owners->selectedMode == NetworkProjectRole::Client);
            const std::array remoteWorld{World(NetworkModeWorldKind::AuthorityServer, 901)};
            REQUIRE(owners->remotePublisher->PrepareTravel(remoteWorld).HasValue());
            DeliverRemoteProductControl(host, owners, Connection());
            REQUIRE(owners->remotePublisher->Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            DeliverRemoteProductControl(host, owners, Connection());
        }

        inline void VerifyClientLifecycle(Application::Internal::NetworkProductHost &host, const NetworkProjectRole mode,
                                          const std::span<const NetworkModeWorld> worlds, const std::shared_ptr<ProductOwners> &owners) {
            const auto clientIndex = worlds.size() - 1;
            auto pending = PeerSessionLifecycle::Create(Connection(), Session(), {10, 20, 30, 100, 10});
            REQUIRE(pending.HasValue());
            CHECK_FALSE(host.AdmitClientSession(NetworkModeWorldKind::Client,
                                                std::make_shared<PeerSessionLifecycle>(std::move(pending).Value()), Connection(), Session())
                            .HasValue());
            CHECK(host.Role(NetworkModeWorldKind::Client, worlds[clientIndex].scene).HasError());
            auto active = AdmittedSession();
            REQUIRE(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session()).HasValue());
            PublishProductClient(host, owners, active);
            CHECK(host.Role(NetworkModeWorldKind::Client, worlds[clientIndex].scene).Value().session == Session());
            std::array<NetworkModeWorld, 2> replacement{};
            if (mode == NetworkProjectRole::Client) {
                replacement[0] = World(NetworkModeWorldKind::Client, 102);
            } else {
                replacement[0] = World(NetworkModeWorldKind::AuthorityServer, 103);
                replacement[1] = World(NetworkModeWorldKind::Client, 104);
            }
            REQUIRE(host.RequestTravel({replacement.data(), worlds.size()}, Session()).HasValue());
            CompleteProductTravel(host, owners);
            CHECK_FALSE(host.TakeTravelFailure().has_value());
            for (std::size_t index = 0; index < worlds.size(); ++index) {
                CHECK_FALSE(host.Role(worlds[index].kind, worlds[index].scene).HasValue());
                CHECK(host.Role(replacement[index].kind, replacement[index].scene).HasValue());
            }
            REQUIRE(host.DisconnectClient(NetworkModeWorldKind::Client, Session()).HasValue());
            CHECK(host.Role(NetworkModeWorldKind::Client, replacement[clientIndex].scene).HasError());
            CHECK_FALSE(host.AdmitClientSession(NetworkModeWorldKind::Client, active, Connection(), Session()).HasValue());
        }

        inline void VerifyNonClientTravel(Application::Internal::NetworkProductHost &host, const NetworkProjectRole mode,
                                          const std::span<const NetworkModeWorld> worlds) {
            const std::array replacement{mode == NetworkProjectRole::Standalone ? World(NetworkModeWorldKind::Standalone, 101)
                                                                                : World(NetworkModeWorldKind::AuthorityServer, 105)};
            REQUIRE(host.RequestTravel(worlds).HasValue());
            REQUIRE(host.RunFrame().HasValue());
            CHECK(host.TakeTravelFailure().has_value());
            CHECK(host.Role(worlds[0].kind, worlds[0].scene).HasValue());
            REQUIRE(host.RequestTravel(replacement).HasValue());
            REQUIRE(host.RunFrame().HasValue());
            if (mode == NetworkProjectRole::DedicatedServer) {
                CHECK(host.Role(worlds[0].kind, worlds[0].scene).HasValue());
                CHECK(host.Role(replacement[0].kind, replacement[0].scene).HasError());
                REQUIRE(host.RunFrame().HasValue());  // The next actual safe point publishes the prepared world.
            }
            CHECK_FALSE(host.TakeTravelFailure().has_value());
            CHECK_FALSE(host.Role(worlds[0].kind, worlds[0].scene).HasValue());
            CHECK(host.Role(replacement[0].kind, replacement[0].scene).HasValue());
        }

        /** @brief Construct the exact independently routed world set used by each concrete product mode. */
        inline std::size_t ProductWorlds(const NetworkProjectRole mode, std::array<NetworkModeWorld, 2> &worlds) {
            switch (mode) {
                case NetworkProjectRole::Standalone:
                    worlds[0] = World(NetworkModeWorldKind::Standalone, 1);
                    return 1;
                case NetworkProjectRole::Client:
                    worlds[0] = World(NetworkModeWorldKind::Client, 2);
                    return 1;
                case NetworkProjectRole::ListenServer:
                    worlds[0] = World(NetworkModeWorldKind::AuthorityServer, 3);
                    worlds[1] = World(NetworkModeWorldKind::Client, 4);
                    return 2;
                case NetworkProjectRole::DedicatedServer:
                    worlds[0] = World(NetworkModeWorldKind::AuthorityServer, 5);
                    return 1;
                case NetworkProjectRole::Count:
                    FAIL("Invalid closed network mode");
            }
            return 0;
        }

        /** @brief Require failed canonical startup to publish no partial roles when the solver is not installed. */
        inline void VerifyUnavailableRoles(const Application::Internal::NetworkProductHost &host,
                                           const std::span<const NetworkModeWorld> worlds) {
            for (const auto &world : worlds)
                CHECK(host.Role(world.kind, world.scene).HasError());
        }
    }  // namespace

}  // namespace Horo::Network
