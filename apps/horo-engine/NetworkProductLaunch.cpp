#include "NetworkProductLaunch.h"

#include "HeadlessNetworkServices.h"
#include "HeadlessProjectScene.h"
#include "Horo/Network/NetworkAddress.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkTransport.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "NetworkProductHost.h"
#include "NetworkProductLaunchOptions.h"
#include "NetworkProductTransport.h"

#include <exception>
#if HORO_PRODUCT_HAS_GNS
#include "GnsTransportFactory.h"
#include "NetworkProductGnsSelection.h"
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
            using enum Net::TransportSupport;
            Net::TransportCapabilities capabilities;
            capabilities.revision = 1;
            capabilities.delivery.fill(Unsupported);
            capabilities.delivery[static_cast<std::size_t>(Net::DeliveryPolicy::UnreliableUnordered)] = Available;
            capabilities.delivery[static_cast<std::size_t>(Net::DeliveryPolicy::ReliableOrdered)] = Available;
            capabilities.maximumChannels = 3;
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
        /** @brief Bind explicitly selected native authorities into the same immutable product capture before service configuration. */
        Result<std::unique_ptr<Net::INetworkModeService>> CreateSecureProductService(
            const Net::NetworkProjectRole role, const Net::NetworkAddress &bind, const Net::NetworkAddress &connect,
            std::shared_ptr<NetworkDebuggerService> debugger, std::shared_ptr<ClientSessionOwner> sessions,
            const std::shared_ptr<const NetworkProductAdmission> &admission, const Net::GnsNativeSecurityOptions &options) {
            if (!admission)
                return Result<std::unique_ptr<Net::INetworkModeService>>::Failure(MakeError(Net::NetworkErrors::AdmissionPolicyInvalid));
            auto selected =
                SelectNetworkProductGns(admission->Policy(), admission->Providers(), admission->Codecs(), admission->Handlers(), options,
                                        admission->WorldBinding() ? std::optional{admission->WorldBinding()->policy} : std::nullopt);
            if (selected.HasError())
                return Result<std::unique_ptr<Net::INetworkModeService>>::Failure(selected.ErrorValue());
            auto composition = std::move(selected).Value();
            return CreateNetworkProductTransport(std::move(composition.transport), role, bind, connect, std::move(debugger),
                                                 std::move(sessions), std::move(composition.admission));
        }
#endif

        [[nodiscard]] Net::NetworkModeFactories ProductFactories(const Net::NetworkProjectRole role, const Net::NetworkAddress &bind,
                                                                 const Net::NetworkAddress &connect,
                                                                 std::shared_ptr<NetworkDebuggerService> debugger,
                                                                 std::shared_ptr<ClientSessionOwner> sessions,
                                                                 std::shared_ptr<const NetworkProductAdmission> admission,
                                                                 std::shared_ptr<const Net::GnsNativeSecurityOptions> nativeSecurity) {
            Net::NetworkModeFactories factories;
            for (const auto kind : {Net::NetworkModeServiceKind::Session, Net::NetworkModeServiceKind::LocalPlayer}) {
                factories.services[static_cast<std::size_t>(kind)] = [](const Net::NetworkModeServiceRequest &request) {
                    return Result<std::unique_ptr<Net::INetworkModeService>>::Success(
                        std::make_unique<SessionOrLocalPlayerService>(request));
                };
            }
#if HORO_PRODUCT_HAS_GNS
            factories.services[static_cast<std::size_t>(Net::NetworkModeServiceKind::Transport)] =
                [role, bind, connect, debugger, sessions, admission, nativeSecurity](const Net::NetworkModeServiceRequest &) {
                if (nativeSecurity)
                    return CreateSecureProductService(role, bind, connect, debugger, sessions, admission, *nativeSecurity);
                auto transport = Net::CreateGnsTransport();
                if (transport.HasError())
                    return Result<std::unique_ptr<Net::INetworkModeService>>::Failure(transport.ErrorValue());
                return CreateNetworkProductTransport(std::move(transport).Value(), role, bind, connect, debugger, sessions, admission);
            };
#else
            static_cast<void>(debugger);
            static_cast<void>(sessions);
            static_cast<void>(admission);
            static_cast<void>(nativeSecurity);
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
                    worlds.values[0] = {Net::NetworkModeWorldKind::Standalone, Runtime::SceneRuntimeId{1}};
                    break;
                case Net::NetworkProjectRole::Client:
                    worlds.values[0] = {Net::NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{2}};
                    break;
                case Net::NetworkProjectRole::ListenServer:
                    worlds.values[0] = {Net::NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{3}};
                    worlds.values[1] = {Net::NetworkModeWorldKind::Client, Runtime::SceneRuntimeId{4}};
                    worlds.count = 2;
                    break;
                case Net::NetworkProjectRole::DedicatedServer:
                    worlds.values[0] = {Net::NetworkModeWorldKind::AuthorityServer, Runtime::SceneRuntimeId{5}};
                    break;
                case Net::NetworkProjectRole::Count:
                    break;
            }
            return worlds;
        }

        /** @brief Owns complete project admission policy while binding the explicitly selected world services. */
        [[nodiscard]] Result<Net::NetworkModeFactories> ComposeProductWorldServices(
            std::shared_ptr<const Runtime::RuntimeSceneDefinition> scene, const ProductLaunch &launch,
            const Net::NetworkProjectSettings &project, Net::NetworkModeFactories factories, HeadlessProjectSceneResources *resources) {
            std::optional<Gameplay::GameplayReplicationAdmissionPolicy> replicationAdmission;
            if (launch.role != Net::NetworkProjectRole::Standalone)
                replicationAdmission.emplace(Gameplay::GameplayReplicationAdmissionPolicy{.inventory = project.ReplicationInventory()});
            if (!resources)
                return Result<Net::NetworkModeFactories>::Success(
                    ComposeHeadlessNetworkServices(std::move(scene), std::move(factories), launch.gameplay, replicationAdmission));
            HeadlessSceneActivationInput input{.mode = launch.sceneInput.physicsMode,
                                               .settings = *resources->settings,
                                               .registry = resources->registry,
                                               .loads = *resources->loads,
                                               .load = resources->load,
                                               .prefabLoads = resources->prefabLoads.get()};
            return ComposeHeadlessSceneNetworkServices(std::move(scene), std::move(input), std::move(factories), launch.gameplay,
                                                       replicationAdmission);
        }

        /** @brief Select an explicit millisecond owner clock; standalone constructs no session capability. */
        std::shared_ptr<ClientSessionOwner> CreateProductSessions(const std::shared_ptr<Clock> &clock, const Net::NetworkProjectRole role) {
            if (role == Net::NetworkProjectRole::Standalone)
                return nullptr;
            auto domain = std::make_shared<NetworkProductSessionClock>(clock, Duration::FromMilliseconds(1), clock->MonotonicNow());
            return std::make_shared<ClientSessionOwner>(std::move(domain));
        }

        /** @brief Explicit optional runtime owners supplied by the executable composition root. */
        struct ProductRuntimeCapabilities final {
            HeadlessProjectSceneResources *resources{};
            std::shared_ptr<const NetworkProductAdmission> admission;
            std::shared_ptr<const Net::GnsNativeSecurityOptions> nativeSecurity;
        };

        /** @brief Reject requested network gameplay before composition when no explicit world authority was supplied. */
        [[nodiscard]] Result<void> ValidateGameplayBinding(const ProductLaunch &launch,
                                                           const std::optional<Net::NetworkWorldBindingSetup> &binding) {
            if (launch.role != Net::NetworkProjectRole::Standalone && launch.purpose == Net::NetworkWorldCompositionPurpose::Gameplay &&
                !binding)
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::AdmissionPolicyInvalid,
                              "Network gameplay requires explicit host-injected world binding and security authorities."));
            return Result<void>::Success();
        }

        /** @brief Create the optional diagnostic owner from the actual host-selected world and product mode. */
        std::shared_ptr<NetworkDebuggerService> CreateProductDebugger(const ProductWorlds &worlds, const Net::NetworkProjectRole role) {
            auto debugger = std::make_shared<NetworkDebuggerService>();
            (void)debugger->Begin(worlds.values[0].scene.value, 1, role != Net::NetworkProjectRole::Standalone,
                                  Net::NetworkDiagnosticProvider::Native);
            return debugger;
        }

        [[nodiscard]] Result<std::unique_ptr<NetworkProductHost>> ComposeProduct(
            std::shared_ptr<Clock> clock, const ProductLaunch &launch, const Net::NetworkProjectSettings &project,
            std::shared_ptr<const Runtime::RuntimeSceneDefinition> scene, const ProductWorlds &worlds,
            Net::NetworkTargetDiagnostic &diagnostic, ProductRuntimeCapabilities capabilities) {
            const auto binding = capabilities.admission ? capabilities.admission->WorldBinding() : std::nullopt;
            if (auto valid = ValidateGameplayBinding(launch, binding); valid.HasError())
                return Result<std::unique_ptr<NetworkProductHost>>::Failure(valid.ErrorValue());
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
            auto debugger = CreateProductDebugger(worlds, launch.role);
            auto sessions = CreateProductSessions(clock, launch.role);
            auto configured =
                ComposeProductWorldServices(std::move(scene), launch, project,
                                            ProductFactories(launch.role, launch.bind, launch.connect, debugger, sessions,
                                                             std::move(capabilities.admission), std::move(capabilities.nativeSecurity)),
                                            capabilities.resources);
            if (configured.HasError())
                return Result<std::unique_ptr<NetworkProductHost>>::Failure(configured.ErrorValue());
            return NetworkProductHost::Create(*clock,
                                              {project,
                                               product,
                                               inventory,
                                               hostFacts,
                                               requirements,
                                               selection,
                                               1,
                                               {worlds.values.data(), worlds.count},
                                               presentation,
                                               std::move(sessions),
                                               launch.purpose,
                                               binding},
                                              std::move(configured).Value(), &diagnostic, debugger);
        }

        /** @brief Enforce the selected purpose without manufacturing a gameplay role for transport qualification. */
        [[nodiscard]] Result<void> ValidateProductRolePurpose(const ProductLaunch &launch, const Result<Net::NetworkModeRoleView> &role) {
            if (launch.role != Net::NetworkProjectRole::Standalone &&
                launch.purpose == Net::NetworkWorldCompositionPurpose::TransportQualification) {
                if (role.HasValue())
                    return Result<void>::Failure(MakeError(Net::NetworkErrors::NetworkModeInvalid,
                                                           "Transport qualification unexpectedly published a gameplay role."));
                const auto &error = role.ErrorValue();
                if (error.domain.Value() == Net::NetworkErrors::NetworkModeUnavailable.domain.Value() &&
                    error.code.Value() == Net::NetworkErrors::NetworkModeUnavailable.code.Value())
                    return Result<void>::Success();
                return Result<void>::Failure(error);
            }
            if (role.HasError())
                return Result<void>::Failure(role.ErrorValue());
            if (launch.role != Net::NetworkProjectRole::Standalone && !role.Value().worldSession.has_value())
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeUnavailable, "Network gameplay has no canonical published world occurrence."));
            return Result<void>::Success();
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
                const auto role = runtime.Role(worlds.values[index].kind, worlds.values[index].scene);
                if (auto valid = ValidateProductRolePurpose(launch, role); valid.HasError()) {
                    std::cerr << "horo-engine: product world purpose failed (" << valid.ErrorValue().code.Value()
                              << "): " << valid.ErrorValue().message << '\n';
                    return 3;
                }
            }
            runtime.Shutdown();
            return 0;
        }
    }  // namespace

    int RunNetworkProduct(const std::span<char *> arguments, std::shared_ptr<const NetworkProductAdmission> admission,
                          std::shared_ptr<const Net::GnsNativeSecurityOptions> nativeSecurity) {
        auto launch = ParseNetworkProductLaunch(arguments);
        if (launch.HasError()) {
            std::cerr << "horo-engine: " << launch.ErrorValue().message << '\n';
            return 2;
        }
        if (nativeSecurity && launch.Value().role == Net::NetworkProjectRole::Standalone) {
            std::cerr << "horo-engine: native security requires an explicit network role\n";
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
        auto clock = std::make_shared<SteadyClock>();
        Net::NetworkTargetDiagnostic diagnostic;
        auto runtime = ComposeProduct(clock, launch.Value(), project.Value(), std::move(scene).Value(), worlds, diagnostic,
                                      {resources.get(), std::move(admission), std::move(nativeSecurity)});
        if (runtime.HasError()) {
            std::cerr << "horo-engine: network product admission failed (" << runtime.ErrorValue().code.Value() << ", capability "
                      << static_cast<int>(diagnostic.capability) << ", reason " << static_cast<int>(diagnostic.reason)
                      << "): " << runtime.ErrorValue().message << '\n';
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
