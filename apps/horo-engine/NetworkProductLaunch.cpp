#include "NetworkProductLaunch.h"

#include "HeadlessNetworkServices.h"
#include "HeadlessProjectScene.h"
#include "Horo/Network/InboundMessageDispatcher.h"
#include "Horo/Network/MessageCodecRegistry.h"
#include "Horo/Network/NetworkAddress.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkMetricTransport.h"
#include "Horo/Network/NetworkTransport.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "NetworkProductHost.h"
#include "NetworkProductLaunchOptions.h"

#include <exception>
#if HORO_PRODUCT_HAS_GNS
#include "GnsTransportFactory.h"
#endif

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        namespace Net = Network;

        constexpr auto AllRoles = Net::NetworkProjectRoleSet::Standalone | Net::NetworkProjectRoleSet::Client |
                                  Net::NetworkProjectRoleSet::ListenServer | Net::NetworkProjectRoleSet::DedicatedServer;

        [[nodiscard]] constexpr Net::NetworkProjectRoleSet PackagedRoles() noexcept {
#if HORO_PRODUCT_HAS_GNS
            return AllRoles;
#else
            return Net::NetworkProjectRoleSet::Standalone;
#endif
        }

        [[nodiscard]] constexpr Net::NetworkTransportProviderId EmptyProvider() noexcept {
            return {};
        }

        [[nodiscard]] Net::NetworkTransportProviderId GnsProvider() {
            return Net::NetworkTransportProviderId::Create(1).Value();
        }

        [[nodiscard]] Net::TransportCapabilities GnsCapabilities() {
            Net::TransportCapabilities capabilities;
            capabilities.revision = 1;
            capabilities.delivery[static_cast<std::size_t>(Net::DeliveryPolicy::UnreliableUnordered)] = Net::TransportSupport::Available;
            capabilities.delivery[static_cast<std::size_t>(Net::DeliveryPolicy::ReliableOrdered)] = Net::TransportSupport::Available;
            capabilities.maximumChannels = 1;
            capabilities.maximumMessageBytes = 1200;
            return capabilities;
        }

        [[nodiscard]] constexpr Net::NetworkProjectRoleSet RoleSet(const Net::NetworkProjectRole role) noexcept {
            switch (role) {
                case Net::NetworkProjectRole::Standalone:
                    return Net::NetworkProjectRoleSet::Standalone;
                case Net::NetworkProjectRole::Client:
                    return Net::NetworkProjectRoleSet::Client;
                case Net::NetworkProjectRole::ListenServer:
                    return Net::NetworkProjectRoleSet::ListenServer;
                case Net::NetworkProjectRole::DedicatedServer:
                    return Net::NetworkProjectRoleSet::DedicatedServer;
                case Net::NetworkProjectRole::Count:
                    return Net::NetworkProjectRoleSet::None;
            }
            return Net::NetworkProjectRoleSet::None;
        }

        template <class Identity> [[nodiscard]] Identity IdentityOf(const std::uint64_t value) {
            return Identity::Create(value).Value();
        }

        [[nodiscard]] constexpr std::uint64_t BuildFingerprint(const std::string_view revision) noexcept {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const char character : revision) {
                hash ^= static_cast<unsigned char>(character);
                hash *= 1099511628211ULL;
            }
            return hash == 0 ? 1 : hash;
        }

        [[nodiscard]] constexpr Net::NetworkTargetPlatform Platform() noexcept {
#if defined(_WIN32)
            return Net::NetworkTargetPlatform::Windows;
#elif defined(__APPLE__)
            return Net::NetworkTargetPlatform::MacOS;
#else
            return Net::NetworkTargetPlatform::Linux;
#endif
        }

        [[nodiscard]] Result<Net::NetworkProjectSettings> ProjectSettings() {
            auto input = Net::DefaultNetworkProjectSettings(IdentityOf<Net::NetworkProjectSettingsId>(1));
            if (input.HasError())
                return Result<Net::NetworkProjectSettings>::Failure(input.ErrorValue());
            auto settings = std::move(input).Value();
            settings.supportedRoles = PackagedRoles();
            settings.defaultRole = Net::NetworkProjectRole::Standalone;
            settings.transport.requirement = Net::NetworkProjectTransportRequirement::Required;
            settings.transport.capabilities.requiredDelivery[static_cast<std::size_t>(Net::DeliveryPolicy::ReliableOrdered)] = true;
            settings.transport.capabilities.requiredChannels = 1;
            settings.transport.capabilities.requiredMaximumMessageBytes = 1200;
            return Net::NetworkProjectSettings::Create(settings);
        }

        [[nodiscard]] Net::NetworkProductCapabilityManifest BuiltProduct(const Net::NetworkProjectSettings &project) {
            Net::NetworkProductCapabilityManifest product;
            product.build = IdentityOf<Net::NetworkProductBuildId>(BuildFingerprint(HORO_SOURCE_REVISION));
            product.revision = IdentityOf<Net::NetworkProductCapabilityRevision>(1);
            product.platform = Platform();
            product.supportedRoles = PackagedRoles();
            product.includesNetworkRuntime = true;
            product.profile = project.Profile().id;
            product.profileRevision = project.Profile().revision;
            product.protocol = project.Protocol();
#if HORO_PRODUCT_HAS_GNS
            product.providerCount = 1;
            product.providers[0] = {GnsProvider(), std::uint32_t{1} << static_cast<std::uint8_t>(Platform()), GnsCapabilities()};
#endif
            return product;
        }

        [[nodiscard]] Net::NetworkTargetPackageInventory LinkedPackageInventory(const Net::NetworkProjectSettings &project) {
            Net::NetworkTargetPackageInventory inventory;
            inventory.build = IdentityOf<Net::NetworkProductBuildId>(BuildFingerprint(HORO_SOURCE_REVISION));
            inventory.platform = Platform();
            inventory.supportedRoles = PackagedRoles();
            inventory.includesNetworkRuntime = true;
            inventory.protocol = project.Protocol();
#if HORO_PRODUCT_HAS_GNS
            // Compiled from the actual optional link edge, separately from product intent.
            inventory.providerCount = 1;
            inventory.providers[0] = {GnsProvider(), std::uint32_t{1} << static_cast<std::uint8_t>(Platform()), GnsCapabilities()};
#else
            inventory.providerCount = 0;
#endif
            return inventory;
        }

        [[nodiscard]] Net::NetworkTargetHostFacts HeadlessHostFacts(const Net::NetworkProjectSettings &project) {
            Net::NetworkTargetHostFacts host;
            host.revision = IdentityOf<Net::NetworkHostCapabilityRevision>(1);
            host.platform = Platform();
            host.supportedRoles = PackagedRoles();
            host.networkRuntimeInstalled = true;
            host.protocol = project.Protocol();
#if HORO_PRODUCT_HAS_GNS
            host.providerCount = 1;
            host.providers[0] = {GnsProvider(), true, true, true, GnsCapabilities()};
#endif
            return host;
        }

        [[nodiscard]] Result<std::shared_ptr<const Runtime::RuntimeSceneDefinition>> ProductScene(
            const std::optional<GameplayWorldSelection> &gameplay) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
            Runtime::RuntimeEntityDefinition entity;
            entity.object = Runtime::SceneObjectId{1};
            if (gameplay && !gameplay->scriptSource.empty()) {
                auto program = Gameplay::LuaBehaviorProgram::LoadFiles(gameplay->scriptSource, gameplay->scriptSidecar);
                if (program.HasError())
                    return Result<std::shared_ptr<const Runtime::RuntimeSceneDefinition>>::Failure(program.ErrorValue());
                const auto &descriptor = program.Value()->Descriptor();
                entity.components.behaviors.emplace_back(Gameplay::BehaviorInstanceId{1}, descriptor.typeId, descriptor.schemaVersion, true,
                                                         std::vector<Gameplay::BehaviorField>{});
            }
            builder.Add(std::move(entity));
            auto created = std::move(builder).Build();
            if (created.HasError())
                return Result<std::shared_ptr<const Runtime::RuntimeSceneDefinition>>::Failure(created.ErrorValue());
            return Result<std::shared_ptr<const Runtime::RuntimeSceneDefinition>>::Success(
                std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(created).Value()));
        }

        class SessionOrLocalPlayerService final : public Net::INetworkModeService {
        public:
            explicit SessionOrLocalPlayerService(const Net::NetworkModeServiceRequest &request) : request_(request) {}

            Result<void> Prepare() override {
                if (request_.service == Net::NetworkModeServiceKind::LocalPlayer && !request_.scene.IsValid())
                    return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid));
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

            void Shutdown() noexcept override {
                // This service owns only a request value and no external resource.
            }

        private:
            Net::NetworkModeServiceRequest request_;
        };

#if HORO_PRODUCT_HAS_GNS
        class GnsProductTransport final : public Net::INetworkModeService {
        public:
            GnsProductTransport(const Net::NetworkProjectRole role, const Net::NetworkAddress &bind, const Net::NetworkAddress &connect,
                                std::shared_ptr<NetworkDebuggerService> debugger)
                : debugger_(std::move(debugger)), metrics_(debugger_->Producer().Source().session, true, &debugger_->Producer()),
                  role_(role), bind_(bind), connect_(connect) {}

            Result<void> Prepare() override {
                auto created = Net::CreateGnsTransport();
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                transport_ = std::make_unique<Net::NetworkMetricTransport>(std::move(created).Value(), metrics_, &debugger_->Producer());
                if (auto initialized =
                        transport_->Initialize({.maximumConnections = 8, .maximumEventsPerPoll = 64, .maximumMessageBytes = 1200});
                    initialized.HasError())
                    return initialized;
                // This reference product installs no credential authority or gameplay descriptors.
                // Its real poll path therefore uses the router's fail-closed pre-active behavior.
                auto identities = Net::ProtocolIdentityRegistry::Create({});
                if (identities.HasError())
                    return Result<void>::Failure(identities.ErrorValue());
                auto codecs = Net::MessageCodecRegistry::Create({}, identities.Value());
                if (codecs.HasError())
                    return Result<void>::Failure(codecs.ErrorValue());
                codecs_.emplace(std::move(codecs).Value());
                auto router = Net::InboundMessageDispatcher::Create(*transport_, *codecs_);
                if (router.HasError())
                    return Result<void>::Failure(router.ErrorValue());
                router_ = std::move(router).Value();
                return Result<void>::Success();
            }

            Result<void> Activate() override {
                using enum Net::NetworkProjectRole;
                if (role_ == ListenServer || role_ == DedicatedServer) {
                    auto listening = transport_->Listen({.bindAddress = bind_, .maximumConnections = 8});
                    if (listening.HasError())
                        return Result<void>::Failure(listening.ErrorValue());
                    listener_ = listening.Value();
                }
                if (role_ == Client || role_ == ListenServer) {
                    auto connecting = transport_->Connect({.endpoint = connect_});
                    if (connecting.HasError())
                        return Result<void>::Failure(connecting.ErrorValue());
                }
                return Result<void>::Success();
            }

            Result<void> RunPhase(const Runtime::RuntimePhase phase) override {
                if (phase != Runtime::RuntimePhase::NetworkPoll)
                    return Result<void>::Success();
                if (auto polled = router_->RunNetworkPoll(++pollTick_); polled.HasError())
                    return Result<void>::Failure(polled.ErrorValue());
                (void)metrics_.Publish();
                const auto snapshot = metrics_.Snapshot();
                (void)debugger_->Publish(debugger_->Producer().Source(), &snapshot);
                return Result<void>::Success();
            }

            Result<void> RunFixedTick(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                if (router_ != nullptr)
                    router_->Shutdown();
                router_.reset();
                codecs_.reset();
                if (transport_ != nullptr) {
                    if (listener_.IsValid())
                        static_cast<void>(transport_->CloseListener(listener_));
                    transport_->Shutdown();
                    transport_.reset();
                }
            }

        private:
            std::shared_ptr<NetworkDebuggerService> debugger_;
            Net::NetworkMetrics metrics_;
            Net::NetworkProjectRole role_;
            Net::NetworkAddress bind_;
            Net::NetworkAddress connect_;
            std::unique_ptr<Net::INetworkTransport> transport_;
            std::optional<Net::MessageCodecRegistry> codecs_;
            std::unique_ptr<Net::InboundMessageDispatcher> router_;
            Net::ListenerHandle listener_{};
            std::uint64_t pollTick_{};
        };
#endif

        [[nodiscard]] Net::NetworkModeFactories ProductFactories(const Net::NetworkProjectRole role, const Net::NetworkAddress &bind,
                                                                 const Net::NetworkAddress &connect,
                                                                 std::shared_ptr<NetworkDebuggerService> debugger) {
            Net::NetworkModeFactories factories;
            for (const auto kind : {Net::NetworkModeServiceKind::Session, Net::NetworkModeServiceKind::LocalPlayer}) {
                factories.services[static_cast<std::size_t>(kind)] = [](const Net::NetworkModeServiceRequest &request) {
                    return Result<std::unique_ptr<Net::INetworkModeService>>::Success(
                        std::make_unique<SessionOrLocalPlayerService>(request));
                };
            }
#if HORO_PRODUCT_HAS_GNS
            factories.services[static_cast<std::size_t>(Net::NetworkModeServiceKind::Transport)] =
                [role, bind, connect, debugger](const Net::NetworkModeServiceRequest &) {
                return Result<std::unique_ptr<Net::INetworkModeService>>::Success(
                    std::make_unique<GnsProductTransport>(role, bind, connect, debugger));
            };
#else
            static_cast<void>(debugger);
            static_cast<void>(role);
            static_cast<void>(bind);
            static_cast<void>(connect);
#endif
            return factories;
        }

        /** @brief Chooses one explicit product input; synthetic mode remains unchanged when no Scene option is supplied. */
        Result<std::shared_ptr<const Runtime::RuntimeSceneDefinition>> ResolveProductScene(
            const ProductLaunch &launch, std::unique_ptr<HeadlessProjectSceneResources> &resources) {
            if (launch.sceneInput.scene.empty())
                return ProductScene(launch.gameplay);
            auto prepared = PrepareHeadlessProjectSceneResources(launch.sceneInput);
            if (prepared.HasError())
                return Result<std::shared_ptr<const Runtime::RuntimeSceneDefinition>>::Failure(prepared.ErrorValue());
            resources = std::move(prepared).Value();
            return Result<std::shared_ptr<const Runtime::RuntimeSceneDefinition>>::Success(resources->definition);
        }

        struct ProductWorlds final {
            std::array<Net::NetworkModeWorld, 2> values{};
            std::size_t count{1};
        };

        [[nodiscard]] ProductWorlds WorldsFor(const Net::NetworkProjectRole role) {
            ProductWorlds worlds;
            switch (role) {
                case Net::NetworkProjectRole::Standalone:
                    worlds.values[0] = {Net::NetworkModeWorldKind::Standalone, Runtime::SceneRuntimeId{1}, {}};
                    break;
                case Net::NetworkProjectRole::Client:
                    worlds.values[0] = {Net::NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{2}, {}};
                    break;
                case Net::NetworkProjectRole::ListenServer:
                    worlds.values[0] = {Net::NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{3},
                                        IdentityOf<Net::ReplicationAuthorityEpoch>(1)};
                    worlds.values[1] = {Net::NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{4}, {}};
                    worlds.count = 2;
                    break;
                case Net::NetworkProjectRole::DedicatedServer:
                    worlds.values[0] = {Net::NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{5},
                                        IdentityOf<Net::ReplicationAuthorityEpoch>(1)};
                    break;
                case Net::NetworkProjectRole::Count:
                    break;
            }
            return worlds;
        }

        [[nodiscard]] Result<std::unique_ptr<NetworkProductHost>> ComposeProduct(
            Clock &clock, const ProductLaunch &launch, const Net::NetworkProjectSettings &project,
            std::shared_ptr<const Runtime::RuntimeSceneDefinition> scene, const ProductWorlds &worlds,
            Net::NetworkTargetDiagnostic &diagnostic, HeadlessProjectSceneResources *resources = nullptr) {
            const auto product = BuiltProduct(project);
            const auto inventory = LinkedPackageInventory(project);
            const auto hostFacts = HeadlessHostFacts(project);
            const Net::NetworkTargetRequirements requirements{.requiredRoles = RoleSet(launch.role),
                                                              .requiredProvider = launch.role == Net::NetworkProjectRole::Standalone
                                                                                      ? EmptyProvider()
                                                                                      : GnsProvider()};
            Net::NetworkTargetSelection selection{.role = launch.role,
                                                  .expectedBuild = product.build,
                                                  .expectedProductRevision = product.revision,
                                                  .expectedHostRevision = hostFacts.revision,
                                                  .expectedProjectRevision = project.Revision()};
            if (launch.role != Net::NetworkProjectRole::Standalone) {
                selection.provider = GnsProvider();
                selection.protocolVersion = {1, 0};
            }
            const Net::NetworkModePresentation presentation{.localPlayer = launch.role == Net::NetworkProjectRole::Client ||
                                                                           launch.role == Net::NetworkProjectRole::ListenServer};
            auto debugger = std::make_shared<NetworkDebuggerService>();
            (void)debugger->Begin(worlds.values[0].scene.value, 1, launch.role != Net::NetworkProjectRole::Standalone,
                                  Net::NetworkDiagnosticProvider::Native);
            auto factories = ProductFactories(launch.role, launch.bind, launch.connect, debugger);
            if (resources) {
                auto configured = ComposeHeadlessSceneNetworkServices(std::move(scene),
                                                                      {launch.sceneInput.physicsMode, *resources->settings,
                                                                       resources->registry, *resources->loads, resources->load},
                                                                      std::move(factories), launch.gameplay);
                if (configured.HasError())
                    return Result<std::unique_ptr<NetworkProductHost>>::Failure(configured.ErrorValue());
                factories = std::move(configured).Value();
            } else {
                factories = ComposeHeadlessNetworkServices(std::move(scene), std::move(factories), launch.gameplay);
            }
            return NetworkProductHost::Create(clock,
                                              {project,
                                               product,
                                               inventory,
                                               hostFacts,
                                               requirements,
                                               selection,
                                               1,
                                               {worlds.values.data(), worlds.count},
                                               presentation},
                                              std::move(factories), &diagnostic, debugger);
        }

        [[nodiscard]] int RunProductFrames(NetworkProductHost &runtime, const ProductLaunch &launch, const ProductWorlds &worlds) {
            if (auto started = runtime.Startup(); started.HasError()) {
                std::cerr << "horo-engine: product startup failed: " << started.ErrorValue().message << '\n';
                return 3;
            }
            for (std::uint32_t frame = 0; frame < launch.frames; ++frame) {
                auto advanced = runtime.RunFrame();
                if (advanced.HasError()) {
                    std::cerr << "horo-engine: product frame failed: " << advanced.ErrorValue().message << '\n';
                    return 3;
                }
            }
            for (std::size_t index = 0; index < worlds.count; ++index) {
                if (runtime.Role(worlds.values[index].kind, worlds.values[index].scene).HasError()) {
                    std::cerr << "horo-engine: product Scene role was not published\n";
                    return 3;
                }
            }
            runtime.Shutdown();
            return 0;
        }
    }  // namespace

    int RunNetworkProduct(const std::span<char *> arguments) {
        auto launch = ParseNetworkProductLaunch(arguments);
        if (launch.HasError()) {
            std::cerr << "horo-engine: " << launch.ErrorValue().message << '\n';
            return 2;
        }
        auto project = ProjectSettings();
        std::unique_ptr<HeadlessProjectSceneResources> resources;
        auto scene = ResolveProductScene(launch.Value(), resources);
        if (project.HasError() || scene.HasError()) {
            const auto &failure = project.HasError() ? project.ErrorValue() : scene.ErrorValue();
            std::cerr << "horo-engine: product metadata/Scene failed (" << failure.code.Value() << "): " << failure.message << '\n';
            return 3;
        }
        const auto worlds = WorldsFor(launch.Value().role);
        SteadyClock clock;
        Net::NetworkTargetDiagnostic diagnostic;
        auto runtime =
            ComposeProduct(clock, launch.Value(), project.Value(), std::move(scene).Value(), worlds, diagnostic, resources.get());
        if (runtime.HasError()) {
            std::cerr << "horo-engine: network product admission failed (capability " << static_cast<int>(diagnostic.capability)
                      << ", reason " << static_cast<int>(diagnostic.reason) << ")\n";
            return 3;
        }
        auto outcome = RunProductFrames(*runtime.Value(), launch.Value(), worlds);
        runtime.Value()->Shutdown();
        if (launch.Value().gameplay && launch.Value().gameplay->moduleRetirements) {
            auto closed = launch.Value().gameplay->moduleRetirements->Close();
            if (closed.HasError()) {
                std::cerr << "horo-engine: gameplay shutdown incomplete: " << closed.ErrorValue().message << '\n';
                // This closed product composition exposes no external generation leases. An escaped owner violates final shutdown.
                std::terminate();
            }
        }
        if (outcome == 0)
            std::cout << "network product completed " << launch.Value().frames << " frames in mode " << arguments[0] << '\n';
        return outcome;
    }
}  // namespace Horo::Application::Internal
