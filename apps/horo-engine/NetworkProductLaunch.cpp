#include "NetworkProductLaunch.h"

#include "HeadlessInitialStateAuthority.h"
#include "HeadlessNetworkServices.h"
#include "HeadlessProjectScene.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/Foundation/Sha256.h"
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

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

        [[nodiscard]] Result<Net::NetworkProjectSettings> ProjectSettings(HeadlessProjectSceneResources *resources) {
            if (resources && resources->networkSettings)
                return Result<Net::NetworkProjectSettings>::Success(std::move(*resources->networkSettings));
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
            builder.Add(entity);
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
                    return CreateSecureNetworkProductTransport(role, bind, connect, debugger, sessions, admission, *nativeSecurity);
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
            auto selection = launch.sceneInput;
            selection.captureNetworkSettings =
                launch.role != Net::NetworkProjectRole::Standalone && launch.purpose == Net::NetworkWorldCompositionPurpose::Gameplay;
            if (launch.role == Net::NetworkProjectRole::DedicatedServer) {
                constexpr unsigned maximumCpuWorkers = 4;
                selection.cpuWorkerCount = std::clamp(std::thread::hardware_concurrency(), 1U, maximumCpuWorkers);
                auto &assets = selection.simulationAssets.emplace();
                assets.runtimePrefabs = launch.gameplay && launch.gameplay->prefabPermission == GameplayPrefabPermission::Granted;
                for (const auto &binding : launch.cookedDecisionBindings) {
                    assets.decisions.push_back(binding.artifact);
                    if (binding.prefab)
                        assets.decisions.push_back(*binding.prefab);
                    if (binding.localPolicy)
                        assets.decisions.push_back(binding.localPolicy->artifact);
                }
            }
            auto prepared = PrepareHeadlessProjectSceneResources(selection);
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

        /** @brief Admits the project scheduler and the authored Perception wake policy together. */
        [[nodiscard]] Result<AI::AiSceneCookedDecisionExecution> ResolveProductDecisionExecution(HeadlessProjectSceneResources &resources) {
            using Execution = Result<AI::AiSceneCookedDecisionExecution>;
            const bool reactive = resources.perceptionPolicy && SelectsReactiveDecisions(*resources.perceptionPolicy);
            if (reactive && resources.decisionSchedulerModeAuthored && resources.decisionScheduler.mode != AI::AiSchedulingMode::BestEffort)
                return Execution::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid,
                                                    "Headless AI scheduler mode must match the authored Perception watches."));
            AI::AiSceneCookedDecisionExecution execution{resources.jobs, resources.decisionScheduler, {}};
            if (resources.localPolicy)
                execution.agent.workUnitsPerTick = resources.decisionScheduler.workUnitsPerTick;
            if (reactive) {
                execution.scheduler.mode = AI::AiSchedulingMode::BestEffort;
                execution.timing = AI::AiCookedDecisionTiming::BestEffortReactive;
            }
            return Execution::Success(std::move(execution));
        }

        /** @brief Admits the project's requested Decision and replay services before composing a live world. */
        Result<void> PrepareProductDecisions(HeadlessSceneActivationInput &input, const ProductLaunch &launch,
                                             HeadlessProjectSceneResources &resources) {
            if (launch.decisionReplay && !resources.developmentReplayAllowed)
                return Result<void>::Failure(
                    MakeError(AI::AIErrors::CapabilityUnavailable, "Project AI policy does not grant development replay capture."));
            input.decisionReplay = launch.decisionReplay;
            if (launch.cookedDecisionBindings.empty())
                return Result<void>::Success();
            if (!resources.committed)
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "AI decisions require the selected committed Scene generation."));
            auto execution = ResolveProductDecisionExecution(resources);
            if (execution.HasError())
                return Result<void>::Failure(execution.ErrorValue());
            auto querySchema = AI::CaptureBuiltinQuerySchema();
            if (querySchema.HasError())
                return Result<void>::Failure(querySchema.ErrorValue());
            const auto target = AssetCookTargetId::Parse(launch.sceneInput.cookTarget);
            if (target.HasError())
                return Result<void>::Failure(target.ErrorValue());
            auto bindings = ConfigureSceneLocalPolicies(launch.cookedDecisionBindings, resources.localPolicy.get(), target.Value());
            if (bindings.HasError())
                return Result<void>::Failure(bindings.ErrorValue());
            input.cookedDecisions = std::make_shared<const HeadlessCookedDecisionInput>(
                HeadlessCookedDecisionInput{.committed = *resources.committed,
                                            .bindings = std::move(bindings).Value(),
                                            .execution = std::move(execution).Value(),
                                            .provider = HeadlessDecisionProvider::SelectedNativeGameplay,
                                            .querySchema = std::make_shared<const AI::QuerySchemaRegistry>(std::move(querySchema).Value()),
                                            .queryWorkUnitsLimit = resources.queryWorkUnitsLimit,
                                            .queryCacheLimits = resources.queryCacheLimits});
            return Result<void>::Success();
        }

        /** @brief Owns complete project admission policy while binding the explicitly selected world services. */
        [[nodiscard]] Result<NetworkProductComposition> ComposeProductWorldServices(
            std::shared_ptr<const Runtime::RuntimeSceneDefinition> scene, const ProductLaunch &launch,
            const Net::NetworkProjectSettings &project, Net::NetworkModeFactories factories, HeadlessProjectSceneResources *resources,
            std::shared_ptr<AnimationRuntimeIdentitySource> animationIdentities, const HeadlessInitialStateSelection *initialState,
            std::shared_ptr<HeadlessInitialStateHost> initialHost) {
            std::optional<Gameplay::GameplayReplicationAdmissionPolicy> replicationAdmission;
            if (launch.role != Net::NetworkProjectRole::Standalone)
                replicationAdmission.emplace(Gameplay::GameplayReplicationAdmissionPolicy{.inventory = project.ReplicationInventory()});
            if (!resources) {
                NetworkProductComposition composition;
                composition.factories = ComposeHeadlessNetworkServices(std::move(scene), std::move(factories), launch.gameplay,
                                                                       replicationAdmission, &composition.pollRetirement);
                return Result<NetworkProductComposition>::Success(std::move(composition));
            }
            AssetCookTargetId animationTarget;
            if (!launch.sceneInput.cookTarget.empty()) {
                auto selected = AssetCookTargetId::Parse(launch.sceneInput.cookTarget);
                if (selected.HasError())
                    return Result<NetworkProductComposition>::Failure(std::move(selected).ErrorValue());
                animationTarget = std::move(selected).Value();
            }
            HeadlessSceneActivationInput input{.mode = launch.sceneInput.physicsMode,
                                               .settings = *resources->settings,
                                               .registry = resources->registry,
                                               .loads = *resources->loads,
                                               .load = resources->load,
                                               .aiSettings = {.missingController = launch.aiMissingController.value_or(
                                                                  AI::AiMissingControllerPolicy::Reject)},
                                               .prefabLoads = resources->prefabLoads.get(),
                                               .prefabMetrics = resources->prefabMetrics,
                                               .prefabPolicy = resources->prefabPolicy,
                                               .prefabTarget = animationTarget,
                                               .animationTarget = animationTarget,
                                               .animationIdentities = animationTarget.IsValid() ? std::move(animationIdentities) : nullptr,
                                               .navigation = resources->navigation,
                                               .navigationPath = launch.navigationPath,
                                               .mountedWorld = launch.sceneInput.world ? std::make_shared<const CommittedWorldSelection>(
                                                                                             *launch.sceneInput.world)
                                                                                       : nullptr,
                                               .worldJobs = &resources->jobs};
            if (const auto decisions = PrepareProductDecisions(input, launch, *resources); decisions.HasError())
                return Result<NetworkProductComposition>::Failure(decisions.ErrorValue());
            if (launch.role == Net::NetworkProjectRole::DedicatedServer)
                input.animationParallel.emplace(resources->jobs, Duration::FromMilliseconds(50));
            if (initialState) {
                input.replicatedSceneObjects = initialState->authoredObjects;
                input.publicTransforms = initialState->transforms;
                input.initialSceneState = std::move(initialHost);
            }
            if (resources->perceptionPolicy)
                input.perceptionProject =
                    HeadlessPerceptionProjectAdmission{.policy = resources->perceptionPolicy,
                                                       .provider = HeadlessPerceptionProvider::SelectedNativeGameplay};
            if (input.navigation && launch.role == Net::NetworkProjectRole::Client)
                return Result<NetworkProductComposition>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeInvalid, "Navigation requires an authoritative world."));
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
            std::shared_ptr<AnimationRuntimeIdentitySource> animationIdentities;
            std::shared_ptr<const HeadlessInitialStateSelection> initialState;
        };

        /** @brief Selects only the explicit initial-state lanes supported by this product role. */
        [[nodiscard]] bool SupportsInitialSceneRoles(Net::NetworkProjectRole role, const HeadlessInitialStateSelection &scene) noexcept {
            switch (role) {
                case Net::NetworkProjectRole::DedicatedServer:
                    return scene.authority && !scene.replica;
                case Net::NetworkProjectRole::Client:
                    return scene.replica && !scene.authority;
                case Net::NetworkProjectRole::ListenServer:
                    return scene.authority || scene.replica;
                default:
                    return false;
            }
        }

        /** @brief Qualifies the committed Scene and secure role before opening any initial-state authority. */
        [[nodiscard]] Result<void> ValidateInitialSceneSelection(const ProductLaunch &launch,
                                                                 const ProductRuntimeCapabilities &capabilities,
                                                                 const std::optional<Net::NetworkWorldBindingSetup> &binding) {
            const auto &scene = *capabilities.initialState;
            const bool secureGameplay = capabilities.nativeSecurity && launch.purpose == Net::NetworkWorldCompositionPurpose::Gameplay;
            const bool committedScene = capabilities.resources && capabilities.resources->committed && scene.scene.IsValid() &&
                                        scene.scene == capabilities.resources->sceneAsset;
            if (!secureGameplay || !committedScene || !binding || !binding->policy.initialState || !binding->initialStateHost)
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::AdmissionPolicyInvalid,
                              "Initial Scene state requires secure gameplay, a committed Scene and explicit grants."));
            if (!SupportsInitialSceneRoles(launch.role, scene))
                return Result<void>::Failure(MakeError(Net::NetworkErrors::AdmissionPolicyInvalid));
            return Result<void>::Success();
        }

        /** @brief Retain the same adapter in the world binding and selected secure transport factory. */
        [[nodiscard]] Result<std::shared_ptr<HeadlessInitialStateHost>> SelectSceneInitialState(const ProductLaunch &launch,
                                                                                                const Net::NetworkProjectProfileV1 &profile,
                                                                                                ProductRuntimeCapabilities &capabilities) {
            using Outcome = Result<std::shared_ptr<HeadlessInitialStateHost>>;
            if (!capabilities.initialState)
                return Outcome::Success(nullptr);
            const auto &admission = capabilities.admission;
            const auto binding = admission ? admission->WorldBinding() : std::nullopt;
            const auto &scene = *capabilities.initialState;
            if (const auto valid = ValidateInitialSceneSelection(launch, capabilities, binding); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            try {
                std::optional<HeadlessAuthorityUpdateLimits> limits;
                if (binding->policy.stateUpdates && scene.authority)
                    limits = HeadlessAuthorityUpdateLimits{profile.maxCapturedObjectsPerTick.value,
                                                           profile.maxCandidatesPerConnectionPerTick.value,
                                                           profile.maxSerializedFieldsPerConnectionPerTick.value};
                auto adapter = std::make_shared<HeadlessInitialStateHost>(scene.authority, binding->initialStateHost, scene.replica,
                                                                          binding->stateUpdateHost, limits);
                auto updates = binding->stateUpdateHost;
                if (updates && (scene.replica || limits))
                    updates = adapter;
                auto configured = NetworkProductAdmission::Capture(admission->Policy(), admission->Providers(), admission->Codecs(),
                                                                   admission->Handlers(), binding->policy, adapter, std::move(updates));
                if (configured.HasError())
                    return Outcome::Failure(std::move(configured).ErrorValue());
                capabilities.admission = std::move(configured).Value();
                return Outcome::Success(std::move(adapter));
            } catch (const std::bad_alloc &) {
                return Outcome::Failure(MakeError(Net::NetworkErrors::ReplicationWorldCapacityExceeded));
            }
        }

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

        /** @brief Selects the roles and transport provider required by the admitted product role. */
        [[nodiscard]] Net::NetworkTargetRequirements ProductRequirements(const Net::NetworkProjectRole role) {
            return {.requiredRoles = RoleSet(role),
                    .requiredProvider = role == Net::NetworkProjectRole::Standalone ? EmptyProvider() : GnsProvider()};
        }

        /** @brief Chooses an admitted project version without changing the injected handshake and schema policy. */
        [[nodiscard]] Result<Net::ProtocolVersion> SelectProductProtocol(const Net::NetworkProjectProtocolPolicy &project,
                                                                         const NetworkProductAdmission *admission) {
            using Version = Result<Net::ProtocolVersion>;
            if (!admission)
                return Version::Success(project.supportedVersions.minimum);
            const auto &local = admission->Policy().local.compatibility;
            const auto selected = std::min(project.supportedVersions.maximum, local.versions.maximum);
            if (local.protocol != project.protocol || local.schemaFingerprint != project.schemaFingerprint ||
                !local.versions.Contains(selected) || !project.supportedVersions.Contains(selected))
                return Version::Failure(MakeError(Net::NetworkErrors::AdmissionPolicyInvalid,
                                                  "The project protocol does not match the admitted handshake policy."));
            return Version::Success(selected);
        }

        /** @brief Pins the exact revisions and protocol selected for target admission. */
        [[nodiscard]] Result<Net::NetworkTargetSelection> SelectProductTarget(const Net::NetworkProjectRole role,
                                                                              const Net::NetworkProjectSettings &project,
                                                                              const Net::NetworkProductCapabilityManifest &product,
                                                                              const Net::NetworkTargetHostFacts &host,
                                                                              const ProductRuntimeCapabilities &capabilities) {
            using Selection = Result<Net::NetworkTargetSelection>;
            Net::NetworkTargetSelection selection{.role = role,
                                                  .expectedBuild = product.build,
                                                  .expectedProductRevision = product.revision,
                                                  .expectedHostRevision = host.revision,
                                                  .expectedProjectRevision = project.Revision()};
            if (role == Net::NetworkProjectRole::Standalone)
                return Selection::Success(selection);
            const auto *admission =
                capabilities.resources && capabilities.resources->networkSettings ? capabilities.admission.get() : nullptr;
            auto version = SelectProductProtocol(project.Protocol(), admission);
            if (version.HasError())
                return Selection::Failure(version.ErrorValue());
            selection.provider = GnsProvider();
            selection.protocolVersion = version.Value();
            return Selection::Success(selection);
        }

        [[nodiscard]] Result<std::unique_ptr<NetworkProductHost>> ComposeProduct(
            std::shared_ptr<Clock> clock, const ProductLaunch &launch, const Net::NetworkProjectSettings &project,
            std::shared_ptr<const Runtime::RuntimeSceneDefinition> scene, const ProductWorlds &worlds,
            Net::NetworkTargetDiagnostic &diagnostic, ProductRuntimeCapabilities capabilities) {
            auto initialHost = SelectSceneInitialState(launch, project.Profile(), capabilities);
            if (initialHost.HasError())
                return Result<std::unique_ptr<NetworkProductHost>>::Failure(std::move(initialHost).ErrorValue());
            const auto binding = capabilities.admission ? capabilities.admission->WorldBinding() : std::nullopt;
            if (auto valid = ValidateGameplayBinding(launch, binding); valid.HasError())
                return Result<std::unique_ptr<NetworkProductHost>>::Failure(valid.ErrorValue());
            const auto product = BuiltProduct(project);
            const auto inventory = LinkedPackageInventory(project);
            const auto hostFacts = HeadlessHostFacts(project);
            const auto requirements = ProductRequirements(launch.role);
            auto selection = SelectProductTarget(launch.role, project, product, hostFacts, capabilities);
            if (selection.HasError())
                return Result<std::unique_ptr<NetworkProductHost>>::Failure(selection.ErrorValue());
            const Net::NetworkModePresentation presentation{.localPlayer = launch.role == Net::NetworkProjectRole::Client ||
                                                                           launch.role == Net::NetworkProjectRole::ListenServer};
            auto debugger = CreateProductDebugger(worlds, launch.role);
            auto sessions = CreateProductSessions(clock, launch.role);
            auto configured =
                ComposeProductWorldServices(std::move(scene), launch, project,
                                            ProductFactories(launch.role, launch.bind, launch.connect, debugger, sessions,
                                                             std::move(capabilities.admission), std::move(capabilities.nativeSecurity)),
                                            capabilities.resources, std::move(capabilities.animationIdentities),
                                            capabilities.initialState.get(), std::move(initialHost).Value());
            if (configured.HasError())
                return Result<std::unique_ptr<NetworkProductHost>>::Failure(configured.ErrorValue());
            std::optional<HeadlessSceneTravelLoad> travelLoad;
            if (capabilities.resources)
                travelLoad.emplace(capabilities.resources->load, launch.sceneInput.loadTimeout);
            return NetworkProductHost::Create(*clock,
                                              {project,
                                               product,
                                               inventory,
                                               hostFacts,
                                               requirements,
                                               selection.Value(),
                                               1,
                                               {worlds.values.data(), worlds.count},
                                               presentation,
                                               std::move(sessions),
                                               launch.purpose,
                                               binding,
                                               configured.Value().fixedParticipant,
                                               configured.Value().pollRetirement,
                                               travelLoad,
                                               configured.Value().aiHealth},
                                              std::move(configured).Value().factories, &diagnostic, debugger);
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

        /** @brief Publishes an entirely committed replay after the frame loop and reports its original trace identities. */
        [[nodiscard]] Result<void> PrintDecisionReplay(HeadlessDecisionReplay &replay, const std::optional<HeadlessReplayOutput> &output) {
            auto captured = replay.TakeCommittedWindow();
            if (!captured)
                return Result<void>::Failure(
                    MakeError(Net::NetworkErrors::NetworkModeUnavailable, "Selected AI replay window did not fully commit."));
            if (captured->HasError())
                return Result<void>::Failure(std::move(*captured).ErrorValue());
            if (output) {
                if (auto published = PublishHeadlessReplayWindow(*output, captured->Value()); published.HasError())
                    return published;
            }
            for (const auto &frame : captured->Value()) {
                const auto &capture = frame.evidence;
                const auto hash = ComputeSha256(std::as_bytes(std::span{frame.canonicalRecord}));
                std::cout << "AI replay scene " << capture.scene.value << " tick " << capture.tick << " agent " << capture.subject.Value()
                          << " blackboard-revision " << capture.blackboardRevision << " plan "
                          << FormatSha256(capture.trace->Header().planDigest) << " canonical-ai-state " << FormatSha256(hash) << '\n';
            }
            return Result<void>::Success();
        }

        /** @brief Prints one fixed-size health line per selected and published authority world. */
        void PrintProductAiHealth(const NetworkProductHost &runtime, const ProductWorlds &worlds) {
            for (std::size_t index = 0; index < worlds.count; ++index) {
                const auto health = runtime.AiHealth(worlds.values[index]);
                if (!health)
                    continue;
                const auto status = health->degraded ? "degraded" : "ready";
                std::cout << "horo-engine: AI health " << status << " decision_tick=" << health->decisionTick
                          << " evaluated=" << health->evaluated << " work_units=" << health->workUnits
                          << " worker_submissions=" << health->workersSubmitted << " deferred=" << health->deferred
                          << " exhausted=" << health->exhausted << " failed=" << health->failed
                          << " perception_listeners=" << health->perceptionListeners << '\n';
            }
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
            if (launch.decisionReplay) {
                if (auto replay = PrintDecisionReplay(*launch.decisionReplay, launch.decisionReplayOutput); replay.HasError()) {
                    std::cerr << "horo-engine: AI replay failed (" << replay.ErrorValue().code.Value()
                              << "): " << replay.ErrorValue().message << '\n';
                    return 3;
                }
            }
            PrintProductAiHealth(runtime, worlds);
            runtime.Shutdown();
            return 0;
        }

        /** @brief Cancels and joins actual project workers before native/code owner retirement. */
        void SettleProductSceneWorkers(HeadlessProjectSceneResources *resources) {
            if (!resources)
                return;
            resources->cancellation.RequestCancellation();
            if (resources->prefabLoads)
                resources->prefabLoads->Shutdown();
            if (resources->loads)
                resources->loads->Shutdown();
            resources->jobs.Shutdown(ShutdownPolicy::Cancel);
        }

        /** @brief Finite owner-only collection after real shutdown/join; pending never drops native owners. */
        void CloseProductPhysicsOwners(NetworkProductHost &host, HeadlessProjectSceneResources *resources) {
            host.Shutdown();
            SettleProductSceneWorkers(resources);
            constexpr std::uint32_t MaximumRetirementPolls = 8;
            for (std::uint32_t poll = 0; poll < MaximumRetirementPolls; ++poll) {
                auto retired = host.PollShutdown(64);
                if (retired.HasError()) {
                    std::cerr << "horo-engine: native retirement failed (" << retired.ErrorValue().code.Value()
                              << "): " << retired.ErrorValue().message << '\n';
                    std::terminate();
                }
                if (retired.Value())
                    return;
            }
            std::cerr << "horo-engine: native retirement remains pending after owned workers settled; retaining owners.\n";
            std::terminate();
        }

        void CloseProductModules(const ProductLaunch &launch) {
            if (launch.gameplay && launch.gameplay->moduleRetirements) {
                auto closed = launch.gameplay->moduleRetirements->Close();
                if (closed.HasError()) {
                    std::cerr << "horo-engine: gameplay shutdown incomplete: " << closed.ErrorValue().message << '\n';
                    // This closed product composition exposes no external generation leases. An escaped owner violates final shutdown.
                    std::terminate();
                }
            }
        }
    }  // namespace

    int RunNetworkProduct(const std::span<char *> arguments, std::shared_ptr<const NetworkProductAdmission> admission,
                          std::shared_ptr<const Net::GnsNativeSecurityOptions> nativeSecurity, const Prefab::PrefabMetricHandles &metrics,
                          std::shared_ptr<AnimationRuntimeIdentitySource> animationIdentities,
                          std::shared_ptr<const HeadlessInitialStateSelection> initialState) {
        auto launch = ParseNetworkProductLaunch(arguments);
        if (launch.HasError()) {
            std::cerr << "horo-engine: " << launch.ErrorValue().message << '\n';
            return 2;
        }
        if (HORO_NETWORK_PRODUCT_PACKAGED && launch.Value().decisionReplay) {
            std::cerr << "horo-engine: AI replay capture is unavailable in the packaged network product\n";
            return 3;
        }
        if (nativeSecurity && launch.Value().role == Net::NetworkProjectRole::Standalone) {
            std::cerr << "horo-engine: native security requires an explicit network role\n";
            return 2;
        }
        std::unique_ptr<HeadlessProjectSceneResources> resources;
        auto scene = ResolveProductScene(launch.Value(), resources);
        auto project = ProjectSettings(resources.get());
        if (project.HasError() || scene.HasError()) {
            const auto &failure = project.HasError() ? project.ErrorValue() : scene.ErrorValue();
            std::cerr << "horo-engine: product metadata/Scene failed (" << failure.code.Value() << "): " << failure.message << '\n';
            return 3;
        }
        if (resources)
            resources->prefabMetrics = metrics;
        const auto worlds = WorldsFor(launch.Value().role);
        auto clock = std::make_shared<SteadyClock>();
        Net::NetworkTargetDiagnostic diagnostic;
        auto runtime = ComposeProduct(clock, launch.Value(), project.Value(), std::move(scene).Value(), worlds, diagnostic,
                                      {resources.get(), std::move(admission), std::move(nativeSecurity), std::move(animationIdentities),
                                       std::move(initialState)});
        if (runtime.HasError()) {
            std::cerr << "horo-engine: network product admission failed (" << runtime.ErrorValue().code.Value() << ", capability "
                      << static_cast<int>(diagnostic.capability) << ", reason " << static_cast<int>(diagnostic.reason)
                      << "): " << runtime.ErrorValue().message << '\n';
            return 3;
        }
        auto outcome = RunProductFrames(*runtime.Value(), launch.Value(), worlds);
        CloseProductPhysicsOwners(*runtime.Value(), resources.get());
        CloseProductModules(launch.Value());
        if (outcome == 0)
            std::cout << "network product completed " << launch.Value().frames << " frames in mode " << arguments[0] << '\n';
        return outcome;
    }
}  // namespace Horo::Application::Internal
