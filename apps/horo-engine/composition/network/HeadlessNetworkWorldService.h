#pragma once
/** @file HeadlessNetworkWorldService.h
 * @brief Target-private network service adapter over the exact generation-owned world.
 */
#include "HeadlessAnimationFixed.h"
#include "HeadlessCookedDecisionAdmission.h"
#include "HeadlessDecisionQueries.h"
#include "HeadlessNetworkWorldOwners.h"
#include "HeadlessPerceptionPolicy.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"

namespace Horo::Application::Internal::HeadlessNetworkDetail {
    class WorldService final : public Network::INetworkModeService {
    public:
        /** @copydoc Network::INetworkModeService::CaptureAuthoredScene */
        [[nodiscard]] Result<Network::NetworkAuthoredSceneProjection> CaptureAuthoredScene() const override;

        [[nodiscard]] std::shared_ptr<const Gameplay::GameplayPhysicsContext> PhysicsContext() const noexcept {
            return gameplay_ ? gameplay_->PhysicsContext() : nullptr;
        }

        std::shared_ptr<const Gameplay::GameplayNavigationContext> NavigationContext() const noexcept {
            if (std::this_thread::get_id() != owners_->owner || !gameplay_ || !record_ || record_->closeRequested ||
                !record_->navigationPath)
                return {};
            return record_->navigationPath->Context();
        }

        const Vfx::VfxExtractionOutcome *VfxFrame() const noexcept {
            if (std::this_thread::get_id() != owners_->owner || !record_ || record_->closeRequested || record_->closed || !record_->scenes)
                return nullptr;
            return &record_->scenes->VfxFrame();
        }

        const Runtime::SceneAnimationRuntime *AnimationOwner() const noexcept {
            return std::this_thread::get_id() == owners_->owner && record_ ? record_->animation.get() : nullptr;
        }

        std::shared_ptr<const AI::PerceptionSpatialSnapshot> PerceptionSnapshot() const noexcept {
            if (std::this_thread::get_id() != owners_->owner || !record_ || record_->closeRequested || record_->closed)
                return {};
            return record_->perception.Current();
        }

        /** @brief Reads one agent's owned native observations from the exact currently committed Scene. */
        Result<AI::PerceptionMemorySnapshot> PerceptionMemory(const Runtime::EntityRef recipient) const {
            const auto scene = ActiveScene();
            if (!scene || !record_->ai)
                return Result<AI::PerceptionMemorySnapshot>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            return record_->ai->ReadPerceptionAtSafePoint(*scene, recipient);
        }

        /** @brief Reads a copied Blackboard value from this exact live owner for product inspection. */
        Result<std::optional<AI::BlackboardValue>> ReadDecisionValue(AI::AgentId identity, AI::BlackboardKeyId key) const {
            using Value = Result<std::optional<AI::BlackboardValue>>;
            if (!ActiveScene() || !record_->ai)
                return Value::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            const auto capability = record_->ai->CapabilityAtSafePoint();
            const auto agent = capability.ResolveAgent(identity);
            if (agent.HasError())
                return Value::Failure(agent.ErrorValue());
            return capability.ReadBlackboard(agent.Value(), key);
        }

        /** @brief Copies the last completed runtime finding from the authoritative Scene on its owner lane. */
        Result<std::optional<AI::AiRuntimeDecisionDiagnostic>> ReadDecisionFailure() const {
            const auto scene = ActiveScene();
            if (!scene || !record_->ai)
                return Result<std::optional<AI::AiRuntimeDecisionDiagnostic>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            return record_->ai->ReadDecisionFailureAtSafePoint(*scene);
        }

        /** @brief Borrows the committed configured Scene on the current owner lane. */
        std::optional<Runtime::RuntimeSceneView> ActiveScene() const noexcept {
            if (std::this_thread::get_id() != owners_->owner || !record_ || record_->closeRequested || !record_->scenes)
                return {};
            return record_->scenes->ActiveScene();
        }

        /** @brief Returns one revocable exact mounted-world origin lease to the active Scene's consumers. */
        std::optional<WorldStreaming::OriginFrameLease> WorldOrigin() const {
            if (!ActiveScene() || request_.service != Network::NetworkModeServiceKind::Scene || !record_->mountedWorld)
                return {};
            auto lease = record_->mountedWorld->Origin().Lease();
            if (lease.HasError())
                return {};
            return std::move(lease).Value();
        }

        /** @brief Captures only geometry from the exact active mounted Scene service on this lane. */
        Result<MountedWorldRenderPacket> CaptureWorldRenderGeometry(const Runtime::SceneObjectId camera,
                                                                    std::span<const Runtime::SceneObjectId> meshes,
                                                                    Runtime::PrimitiveMeshCache &primitives,
                                                                    Render::RenderOriginSceneAdapter &renderer) const {
            const auto scene = ActiveScene();
            if (!scene || request_.service != Network::NetworkModeServiceKind::Scene || !record_->mountedWorld)
                return Result<MountedWorldRenderPacket>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
            return record_->mountedWorld->CaptureGeometry(*scene, camera, meshes, primitives, renderer);
        }

        std::optional<Runtime::EntityRef> AnimationEntity(Runtime::SceneObjectId object) const noexcept {
            const auto scene = ActiveScene();
            return scene ? scene->Find(object) : std::nullopt;
        }

        WorldService(std::shared_ptr<WorldOwners> owners, const Network::NetworkModeServiceRequest &request,
                     std::shared_ptr<WorldRecord> record)
            : owners_(std::move(owners)), request_(request), record_(std::move(record)) {}

        ~WorldService() override {
            Shutdown();
        }

        Result<void> Prepare() override {
            if (const auto lane = CheckOwnerLane(); lane.HasError())
                return lane;
            switch (request_.service) {
                case Network::NetworkModeServiceKind::Scene: {
                    if (owners_->definition == nullptr)
                        return Result<void>::Failure(
                            MakeError(Network::NetworkErrors::NetworkModeUnavailable, "No cooked runtime Scene definition was supplied."));
                    if (owners_->configured)
                        return PrepareConfiguredScene();
                    auto scene = Runtime::RuntimeScene::Create(*owners_->definition, request_.scene);
                    if (scene.HasError())
                        return Result<void>::Failure(scene.ErrorValue());
                    record_->scene = std::move(scene).Value();
                    return Result<void>::Success();
                }
                case Network::NetworkModeServiceKind::Physics: {
                    if (owners_->configured) {
                        if (!record_ || !record_->scenes || !record_->scenes->ActiveScene() ||
                            record_->scenes->ActiveScene()->RuntimeId() != request_.scene)
                            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
                        auto borrowed = record_->native.Borrow();
                        return borrowed.HasError() ? Result<void>::Failure(borrowed.ErrorValue()) : Result<void>::Success();
                    }
                    return PreparePhysics();
                }
                case Network::NetworkModeServiceKind::Replication:
                    // NetworkModeComposition owns the canonical Stage/Commit/Acquire lifecycle.
                    // This process-scoped facade never creates a competing empty world owner.
                    return Result<void>::Success();
                default:
                    return Result<void>::Failure(
                        MakeError(Network::NetworkErrors::NetworkModeInvalid, "Headless world factory received a non-world service."));
            }
        }

        Result<void> Activate() override {
            if (const auto lane = CheckOwnerLane(); lane.HasError())
                return lane;
            if (request_.service != Network::NetworkModeServiceKind::Physics)
                return Result<void>::Success();
            if (auto admitted = PrepareGameplaySelection(); admitted.HasError())
                return admitted;
            if (owners_->configured) {
                if (configuredActivated_)
                    return Result<void>::Failure(
                        MakeError(Network::NetworkErrors::NetworkModeInvalid, "This paired Physics service was already activated."));
                auto activated = ActivateConfiguredGameplay();
                configuredActivated_ = activated.HasValue();
                return activated;
            }
            auto identity = owners_->physics->IssueWorldIdentity();
            if (identity.HasError())
                return Result<void>::Failure(identity.ErrorValue());
            if (auto activated = physicsWorld_->Activate(identity.Value()); activated.HasError())
                return activated;
            if (selectedGameplay_) {
                auto gameplay =
                    GameplayWorldComposition::Create(*record_->scene, physicsWorld_.get(), record_->generation, *selectedGameplay_);
                if (gameplay.HasError())
                    return Result<void>::Failure(gameplay.ErrorValue());
                gameplay_ = std::move(gameplay).Value();
                record_->gameplay = gameplay_->PhysicsContext();
            }
            return Result<void>::Success();
        }

        Result<void> RunPhase(const Runtime::RuntimePhase phase) override {
            if (const auto lane = CheckOwnerLane(); lane.HasError())
                return lane;
            if (owners_->gameplay && owners_->gameplay->moduleRetirements)
                (void)owners_->gameplay->moduleRetirements->Drain();
            if (phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges && IsSceneOwner() && record_->prefabReplica)
                return AdvanceReplicaPrefabs();
            if (owners_->configured && request_.service == Network::NetworkModeServiceKind::Physics && configuredActivated_) {
                const auto previous = record_->perception.Current();
                if (gameplay_) {
                    const Runtime::FrameContext frame{.frameNumber = ++phaseFrame_, .cancellation = record_->sceneCancellation};
                    if (auto advanced = gameplay_->OnPhase(phase, frame); advanced.HasError())
                        return advanced;
                }
                if (phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges && previous == record_->perception.Current())
                    return PublishConfiguredPerception();
            }
            return Result<void>::Success();
        }

        Result<void> RunFixedTick(const Runtime::FixedStepContext &context) override {
            if (const auto lane = CheckOwnerLane(); lane.HasError())
                return lane;
            if (request_.service != Network::NetworkModeServiceKind::Physics)
                return Result<void>::Success();
            if (owners_->configured)
                return RunConfiguredTick(context);
            if (gameplay_) {
                if (auto advanced = gameplay_->FixedTick(context); advanced.HasError())
                    return advanced;
            }
            // Gameplay submits against the previous completed publication. Execute queued
            // queries on this owner-thread safe point before the next tick replaces it.
            if (auto processed = physicsWorld_->ProcessQueryBatch(); processed.HasError()) {
                if (gameplay_)
                    gameplay_->Shutdown();
                return processed;
            }
            const auto stepped = physicsWorld_->AdvanceFixedTick(
                {.simulationTick = ++localTick_, .sceneGeneration = record_->generation, .fixedDelta = context.fixedDelta});
            if (stepped.HasError() && gameplay_)
                gameplay_->Shutdown();
            return stepped;
        }

        void Shutdown() noexcept override {
            RevokeGameplayContext();
            RetireGameplay();
            if (IsSceneOwner())
                CloseRecord(*record_);
            DrainModuleRetirements();
            if (physicsWorld_)
                physicsWorld_->Shutdown();
            physicsWorld_.reset();
            record_.reset();
        }

    private:
        bool IsSceneOwner() const noexcept {
            return record_ && owners_->configured && request_.service == Network::NetworkModeServiceKind::Scene;
        }

        void RevokeGameplayContext() const noexcept {
            if (record_)
                if (const auto context = record_->gameplay.lock())
                    context->Revoke();
        }

        void RetireGameplay() noexcept {
            if (record_ && record_->prefabReplica)
                record_->prefabReplica->Cancel();
            if (IsSceneOwner() && record_->activeGameplay) {
                record_->activeGameplay->Shutdown();
                record_->activeGameplay = nullptr;
            }
            if (record_ && record_->activeGameplay == gameplay_.get())
                record_->activeGameplay = nullptr;
            if (gameplay_)
                gameplay_->Shutdown();
            gameplay_.reset();
        }

        void DrainModuleRetirements() const noexcept {
            if (owners_->gameplay && owners_->gameplay->moduleRetirements)
                (void)owners_->gameplay->moduleRetirements->Drain();
        }

        /** @brief Resolves admission only from the actual world role, retaining the complete owned project inventory. */
        Result<void> PrepareGameplaySelection() {
            selectedGameplay_ = owners_->gameplay;
            if ((request_.mode == Network::NetworkProjectRole::Standalone) != (request_.world == Network::NetworkModeWorldKind::Standalone))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if (request_.world == Network::NetworkModeWorldKind::Standalone) {
                if (selectedGameplay_)
                    selectedGameplay_->replicationAdmission.reset();
                return Result<void>::Success();
            }
            if (!owners_->replicationAdmission ||
                owners_->replicationAdmission->inventory.completeness != Network::NetworkReplicationInventoryCompleteness::Complete)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
            const auto use = request_.world == Network::NetworkModeWorldKind::AuthorityServer
                                 ? Network::ReplicationDeclarationUse::ServerActivation
                                 : Network::ReplicationDeclarationUse::ClientActivation;
            if (request_.world != Network::NetworkModeWorldKind::AuthorityServer && request_.world != Network::NetworkModeWorldKind::Client)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            auto policy = *owners_->replicationAdmission;
            if (policy.use != Network::ReplicationDeclarationUse::Count && policy.use != use)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
            policy.use = use;
            if (!selectedGameplay_)
                return policy.RequireAdmission({}, {});
            selectedGameplay_->replicationAdmission = std::move(policy);
            return Result<void>::Success();
        }

        /** @brief Rejects foreign configured-host access before reading mutable record/native state. */
        Result<void> CheckOwnerLane() const {
            if (owners_->configured && std::this_thread::get_id() != owners_->owner)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
            return Result<void>::Success();
        }

        /** @brief Creates one service at the exact request identity, before any derived gameplay activation. */
        Result<void> PrepareConfiguredScene() {
            if (record_->scenes)
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Each world record admits exactly one Scene candidate."));
            const auto &input = *owners_->configured;
            if (input.navigationPath && request_.world != Network::NetworkModeWorldKind::Standalone)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "The best-effort Navigation path scope requires standalone authority."));
            if (SelectsNativePerception() && request_.world != Network::NetworkModeWorldKind::Standalone)
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Native project Perception requires standalone authority."));
            const auto &boundary = input.load;
            record_->sceneCancellation = boundary.cancellation;
            record_->scenes = std::make_unique<Runtime::RuntimeSceneService>(input.registry, input.loads,
                                                                             Runtime::RuntimeSceneAssetLimits{}, request_.scene);
            if (auto native = PrepareConfiguredNative(); native.HasError())
                return native;
            if (auto ai = PrepareConfiguredAi(); ai.HasError())
                return ai;
            if (auto navigation = PrepareConfiguredNavigation(); navigation.HasError())
                return navigation;
            if (auto animation = PrepareConfiguredAnimation(*owners_, *record_); animation.HasError())
                return animation;
            if (auto vfx = PrepareConfiguredVfx(); vfx.HasError())
                return vfx;
            if (auto prepared = PrepareHeadlessSceneAtLoadBoundary(*record_->scenes, *owners_->definition, boundary); prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            if (auto templates = PrepareConfiguredTemplates(); templates.HasError())
                return templates;
            if (auto replica = BindReplicaPrefabs(); replica.HasError())
                return replica;
            return PrepareConfiguredWorldMount();
        }

        /** @brief Attaches verified World authority only to the exact live standalone Scene candidate. */
        Result<void> PrepareConfiguredWorldMount() const {
            const auto &input = *owners_->configured;
            if (!input.mountedWorld)
                return Result<void>::Success();
            if (request_.world != Network::NetworkModeWorldKind::Standalone)
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Committed World mounting requires standalone authority."));
            if (std::chrono::steady_clock::now() >= input.load.deadline)
                return Result<void>::Failure(MakeError(HeadlessSceneLoadErrors::Deadline));
            auto world = MountedWorld::Prepare(NativeDurableFileSystem{}, *input.mountedWorld, record_->generation, *record_->scenes,
                                               record_->sceneCancellation);
            if (world.HasError())
                return Result<void>::Failure(world.ErrorValue());
            if (std::chrono::steady_clock::now() >= input.load.deadline)
                return Result<void>::Failure(MakeError(HeadlessSceneLoadErrors::Deadline));
            if (input.mountedWorld->initialCell) {
                const auto activated =
                    world.Value()->ActivateInitialCell(*input.mountedWorld->initialCell, {input.load.cancellation, input.load.deadline});
                if (activated.HasError())
                    return activated;
            }
            record_->mountedWorld = std::move(world).Value();
            return Result<void>::Success();
        }

        Result<void> PrepareConfiguredNative() const {
            const auto &input = *owners_->configured;
            auto participant = record_->native.MakeParticipant(*owners_->physics, owners_->authority, input.settings);
            if (participant.HasError())
                return Result<void>::Failure(participant.ErrorValue());
            if (auto added = record_->scenes->AddActivationParticipant(std::move(participant).Value()); added.HasError())
                return added;
            auto structural = record_->native.MakeStructuralParticipant();
            if (structural.HasError())
                return Result<void>::Failure(structural.ErrorValue());
            if (auto added = record_->scenes->AddStructuralParticipant(std::move(structural).Value()); added.HasError())
                return added;
            if (auto added = record_->scenes->AddStructuralParticipant(MakeGameplayStructuralParticipant(record_->activeGameplay));
                added.HasError())
                return added;
            return Result<void>::Success();
        }

        /** @brief Reserves only inert Scene identity on a client, even inside a listen-server process. */
        Result<void> PrepareConfiguredAi() const {
            const auto &input = *owners_->configured;
            auto settings = input.aiSettings;
            if (!IsAuthoritativeWorld()) {
                settings.availableCapabilities = {};
                settings.missingController = AI::AiMissingControllerPolicy::PreserveInactive;
                return InstallAiSceneOwner(settings, {});
            }
            settings.requirePerceptionEndpoints = true;
            if (!OwnsPerception())
                settings.availableCapabilities.bits &= ~AI::AiCapabilitySet::Of(AI::AiCapability::Perception).bits;

            std::span<const AI::AiControllerDescriptor> catalog = input.aiControllers;
            if (input.cookedDecisions) {
                HeadlessQueryPlanCatalog queries;
                auto prepared = PrepareCookedControllers(catalog, *input.cookedDecisions, queries);
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                record_->admittedControllers = std::move(prepared).Value();
                catalog = record_->admittedControllers;
                if (OwnsCookedDecisionExecution() && !queries.empty()) {
                    auto driver = HeadlessDecisionQueries::Create(std::move(queries), input.cookedDecisions->querySchema,
                                                                  input.cookedDecisions->execution->scheduler);
                    if (driver.HasError())
                        return Result<void>::Failure(driver.ErrorValue());
                    record_->decisionQueries = std::move(driver).Value();
                }
            }
            if (OwnsPerception() && input.perceptionProject) {
                auto prepared = PreparePerceptionControllers(catalog, *input.perceptionProject);
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                record_->admittedControllers = std::move(prepared).Value();
                catalog = record_->admittedControllers;
            }
            return InstallAiSceneOwner(settings, catalog);
        }

        /** @brief Shares Scene lifecycle ownership without constructing any client controller or Blackboard. */
        Result<void> InstallAiSceneOwner(const AI::AiSceneRuntimeSettings settings,
                                         const std::span<const AI::AiControllerDescriptor> catalog) const {
            const auto &input = *owners_->configured;
            auto ai = OwnsCookedDecisionExecution() && !record_->preparedGameplay
                          ? AI::AiSceneRuntime::Create(settings, ConfiguredDecisionExecution())
                          : AI::AiSceneRuntime::Create(settings);
            if (ai.HasError())
                return Result<void>::Failure(ai.ErrorValue());
            record_->ai = std::make_unique<AI::AiSceneRuntime>(std::move(ai).Value());
            std::optional<AI::AiScenePerceptionActivationInput> activation;
            std::optional<AI::AiScenePerceptionGroupInput> structural;
            if (OwnsPerception()) {
                activation.emplace(AI::AiScenePerceptionActivationInput{record_->perception, input.perceptionFiltering,
                                                                        input.perceptionPositions, input.perceptionExecution});
                structural.emplace(
                    AI::AiScenePerceptionGroupInput{record_->perception, input.perceptionFiltering, input.perceptionGroupPositions});
            }
            if (auto added = record_->scenes->AddActivationParticipant(
                    std::make_unique<AI::AiSceneActivationParticipant>(*record_->ai, catalog, std::move(activation)));
                added.HasError())
                return added;
            if (auto added =
                    record_->scenes->AddStructuralParticipant(record_->ai->MakeStructuralParticipant(catalog, std::move(structural)));
                added.HasError())
                return added;
            return Result<void>::Success();
        }

        /** @brief Adds this world's explicit query owner to the selected immutable scheduler policy. */
        AI::AiSceneCookedDecisionExecution ConfiguredDecisionExecution() const {
            auto execution = *owners_->configured->cookedDecisions->execution;
            if (record_->decisionQueries)
                execution.queryTasks = record_->decisionQueries;
            return execution;
        }

        /** @brief Resolves authored endpoints using either the explicit registry or the same prepared native image. */
        Result<std::vector<AI::AiControllerDescriptor>> PreparePerceptionControllers(
            std::span<const AI::AiControllerDescriptor> catalog, const HeadlessPerceptionProjectAdmission &project) const {
            using Output = Result<std::vector<AI::AiControllerDescriptor>>;
            if (!SelectsNativePerception())
                return BindHeadlessPerceptionEndpoints(*project.policy, project.registry, catalog);
            if (!record_->preparedGameplay)
                return Output::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
            const auto contracts = record_->preparedGameplay->DescribePerceptionSenses();
            if (contracts.HasError())
                return Output::Failure(contracts.ErrorValue());
            auto registry = FreezeHeadlessNativePerceptionRegistry(*project.policy->native, contracts.Value());
            if (registry.HasError())
                return Output::Failure(registry.ErrorValue());
            return BindHeadlessPerceptionEndpoints(*project.policy, std::move(registry).Value(), catalog);
        }

        /** @brief Captures exact native provider metadata before any platform scope or game callback is exposed. */
        Result<std::vector<AI::AiControllerDescriptor>> PrepareCookedControllers(std::span<const AI::AiControllerDescriptor> catalog,
                                                                                 const HeadlessCookedDecisionInput &selection,
                                                                                 HeadlessQueryPlanCatalog &queries) const {
            auto limits = Prefab::PrefabLimitProfile::Create(owners_->configured->prefabPolicy);
            if (limits.HasError())
                return Result<std::vector<AI::AiControllerDescriptor>>::Failure(limits.ErrorValue());
            if (selection.provider == HeadlessDecisionProvider::Captured)
                return AdmitHeadlessCookedControllers(catalog, *owners_->definition, selection, limits.Value(), record_->sceneCancellation,
                                                      &queries);
            const auto &gameplay = *owners_->gameplay;
            auto image = Gameplay::GameModuleHost::PrepareImage(gameplay.nativeArtifact,
                                                                {gameplay.moduleId, Gameplay::CurrentGameplayBuildFingerprint(),
                                                                 gameplay.descriptorRevision},
                                                                record_->sceneCancellation);
            if (image.HasError())
                return Result<std::vector<AI::AiControllerDescriptor>>::Failure(image.ErrorValue());
            record_->preparedGameplay = std::move(image).Value();
            auto nodes = record_->preparedGameplay->AcquireDecisionNodes();
            if (nodes.HasError())
                return Result<std::vector<AI::AiControllerDescriptor>>::Failure(nodes.ErrorValue());
            auto configured = selection;
            configured.environment.nodes = std::move(nodes).Value();
            return AdmitHeadlessCookedControllers(catalog, *owners_->definition, configured, limits.Value(), record_->sceneCancellation,
                                                  &queries);
        }

        /** @brief Installs one authoritative navigation aggregate; paired client worlds stay presentation-only. */
        Result<void> PrepareConfiguredNavigation() const {
            const auto &selection = owners_->configured->navigation;
            if (!selection || !IsAuthoritativeWorld())
                return Result<void>::Success();
            auto world = CreateProjectSceneNavigationWorld(*selection);
            if (world.HasError())
                return Result<void>::Failure(world.ErrorValue());
            record_->navigation = std::move(world).Value();
            return RegisterProjectSceneNavigation(*record_->scenes, *record_->navigation, selection);
        }

        /** @brief Identifies the local world that owns authoritative simulation. */
        bool IsAuthoritativeWorld() const noexcept {
            const auto world = request_.world;
            return world == Network::NetworkModeWorldKind::Standalone || world == Network::NetworkModeWorldKind::AuthorityServer;
        }

        /** @brief The runtime routes AI phases only for explicit authoritative execution. */
        bool OwnsCookedDecisionExecution() const noexcept {
            const auto &selection = owners_->configured->cookedDecisions;
            return IsAuthoritativeWorld() && selection && selection->execution;
        }

        /** @brief Identifies the selected native mode without constructing registry or provider state. */
        bool SelectsNativePerception() const noexcept {
            const auto &project = owners_->configured->perceptionProject;
            return project && project->provider == HeadlessPerceptionProvider::SelectedNativeGameplay;
        }

        /** @brief Only an authoritative host with explicit global positions owns a Perception publication. */
        bool OwnsPerception() const noexcept {
            return IsAuthoritativeWorld() && owners_->configured->perceptionPositions;
        }

        /** @brief Matches the real Scene's publication, preserving the prior spatial capture on dependency failure. */
        Result<void> PublishConfiguredPerception() const {
            if (!OwnsPerception())
                return Result<void>::Success();
            const auto scene = record_->scenes->ActiveScene();
            if (!scene)
                return Result<void>::Success();
            const auto previous = record_->perception.Current();
            const auto revision = previous && previous->Scene() == scene->RuntimeId() ? previous->Revision() : 0;
            if (revision == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionSpatialLimitExceeded));
            auto positions = owners_->configured->perceptionPositions(*scene);
            if (positions.HasError())
                return Result<void>::Failure(positions.ErrorValue());
            const auto published = record_->ai->PublishPerceptionAtSafePoint(*scene, revision + 1, positions.Value(), record_->perception,
                                                                             owners_->configured->perceptionFiltering);
            if (published.HasError())
                return Result<void>::Failure(published.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Each genuine current/replacement World gets an independent source owner copied from explicit policy. */
        Result<void> PrepareConfiguredVfx() const {
            if (!owners_->gameplay || !owners_->gameplay->vfxScene)
                return Result<void>::Success();
            if (!owners_->fixed)
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Configured VFX requires the actual fixed attempt participant."));
            auto created = owners_->gameplay->vfxScene->CreateSibling();
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            auto source = std::move(created).Value();
            if (auto registered = record_->scenes->AddActivationParticipant(source->MakeActivationParticipant()); registered.HasError())
                return registered;
            record_->vfxScene = std::move(source);
            return Result<void>::Success();
        }

        Result<void> PrepareConfiguredTemplates() const {
            const auto &input = *owners_->configured;
            const auto &boundary = input.load;
            if (owners_->gameplay && owners_->gameplay->prefabPermission == GameplayPrefabPermission::Granted) {
                auto limits = Prefab::PrefabLimitProfile::Create(input.prefabPolicy);
                if (limits.HasError())
                    return Result<void>::Failure(limits.ErrorValue());
                record_->templates =
                    std::make_unique<Prefab::PrefabTemplateProvider>(input.registry, *input.prefabLoads, *record_->scenes,
                                                                     std::move(limits).Value(), Prefab::PrefabTemplateProviderLimits{},
                                                                     input.prefabMetrics);
                return record_->templates->Startup(boundary.cancellation);
            }
            return Result<void>::Success();
        }

        /** @brief Admits gameplay from the already published private aggregate, without reactivating either native owner. */
        Result<void> ActivateConfiguredGameplay() {
            auto scope = record_->native.Borrow();
            if (scope.HasError())
                return Result<void>::Failure(scope.ErrorValue());
            if (!selectedGameplay_)
                return Result<void>::Success();
            auto selected = *selectedGameplay_;
            selected.vfxScene = record_->vfxScene;
            if (record_->prefabReplica)
                selected.prefabPermission = GameplayPrefabPermission::ReplicaOnly;
            if (auto bound = BindAuthorityPrefabs(selected); bound.HasError())
                return bound;
            if (auto bound = BindNavigationPath(selected); bound.HasError())
                return bound;
            auto gameplay = ComposeConfiguredGameplay(selected, scope.Value().physics, scope.Value().character);
            if (gameplay.HasError())
                return Result<void>::Failure(gameplay.ErrorValue());
            gameplay_ = std::move(gameplay).Value();
            record_->activeGameplay = gameplay_.get();
            record_->gameplay = gameplay_->PhysicsContext();
            return Result<void>::Success();
        }

        /** @brief Installs the cooked Prefab mapping adapter for this exact authority Scene only. */
        [[nodiscard]] Result<void> BindAuthorityPrefabs(GameplayWorldSelection &selected) const;

        /** @brief Installs protected Prefab consumption on the exact client Scene, with explicit cooked asset authority. */
        [[nodiscard]] Result<void> BindReplicaPrefabs();

        /** @brief Submits only fully reconstructed records before the paired Gameplay structural phase. */
        [[nodiscard]] Result<void> AdvanceReplicaPrefabs();

        /** @brief Binds only the module's selected registered path scope to the already activated immutable provider. */
        Result<void> BindNavigationPath(GameplayWorldSelection &selected) {
            const auto &policy = owners_->configured->navigationPath;
            if (!policy)
                return Result<void>::Success();
            auto paths = HeadlessNavigationPathScope::Create(*record_->navigation, *owners_->configured->navigation, *policy,
                                                             request_.scene, selected.moduleId);
            if (paths.HasError())
                return Result<void>::Failure(paths.ErrorValue());
            if (auto retained = owners_->HoldNavigationRetirement(record_); retained.HasError())
                return retained;
            record_->navigationPath = std::move(paths).Value();
            selected.navigationContext = record_->navigationPath->Context();
            return Result<void>::Success();
        }

        /** @brief Activates the same prepared image or the host's ordinary gameplay route. */
        Result<std::unique_ptr<GameplayWorldComposition>> ComposeConfiguredGameplay(const GameplayWorldSelection &selected,
                                                                                    Physics::PhysicsWorld &world,
                                                                                    Character::CharacterWorld &character) const {
            if (record_->preparedGameplay) {
                const auto &input = *owners_->configured;
                std::optional<GameplayPreparedPerception> perception;
                if (SelectsNativePerception())
                    perception.emplace(GameplayPreparedPerception{record_->perception, *input.perceptionProject->policy->native});
                const auto execution = ConfiguredDecisionExecution();
                const GameplayPreparedDecisions prepared{*record_->preparedGameplay, record_->sceneCancellation, *record_->ai, execution,
                                                         perception ? &*perception : nullptr};
                return GameplayWorldComposition::CreatePrepared(*record_->scenes, record_->templates.get(), world, request_.scene.value,
                                                                selected, character, prepared);
            }
            return record_->templates
                       ? GameplayWorldComposition::Create(*record_->scenes, *record_->templates, &world, request_.scene.value, selected,
                                                          nullptr, &character)
                       : GameplayWorldComposition::Create(*record_->scenes, &world, request_.scene.value, selected, &character);
        }

        /** @brief Records actual participation before a fallible gameplay source callback can partially stage work. */
        Result<void> RunConfiguredGameplay(const Runtime::FixedStepContext &context) {
            if (!gameplay_)
                return Result<void>::Success();
            if (owners_->fixed) {
                if (const auto tracked = owners_->fixed->TrackGameplay(record_, context); tracked.HasError())
                    return tracked;
            }
            return gameplay_->FixedTick(context);
        }

        /** @brief Executes the opted-in canonical AI phases before configured gameplay on the same fixed clock. */
        Result<void> RunConfiguredDecisions(const Runtime::FixedStepContext &context) const {
            if (!OwnsCookedDecisionExecution())
                return Result<void>::Success();
            if (context.cancellation.IsCancellationRequested())
                return JobCancelled();
            if (record_->decisionTick != 0 && record_->decisionTick == context.simulationTick)
                return Result<void>::Success();
            auto result = record_->decisionQueries ? RunConfiguredDecisionQueries(context.simulationTick)
                                                   : EvaluateConfiguredDecisions(context.simulationTick);
            if (result.HasValue())
                record_->decisionTick = context.simulationTick;
            return result;
        }

        /** @brief Drives the exact Scene and its current origin through all deterministic query phases. */
        Result<void> RunConfiguredDecisionQueries(std::uint64_t tick) const {
            const auto scene = record_->scenes->ActiveScene();
            if (!scene)
                return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryContextStale));
            Math::WorldCoordinate64 origin;
            if (record_->mountedWorld) {
                const auto lease = record_->mountedWorld->Origin().Lease();
                if (lease.HasError())
                    return Result<void>::Failure(lease.ErrorValue());
                const auto frame = lease.Value().Get();
                if (frame.HasError())
                    return Result<void>::Failure(frame.ErrorValue());
                origin = frame.Value().Origin();
            }
            return record_->decisionQueries->RunAtFixedTick(*record_->ai, *scene, origin, tick);
        }

        /** @brief Executes the registered decision phases for worlds without authored query tasks. */
        Result<void> EvaluateConfiguredDecisions(std::uint64_t tick) const {
            const auto scheduler = record_->ai->TaskSchedulerAtSafePoint();
            if (scheduler.HasError())
                return Result<void>::Failure(scheduler.ErrorValue());
            if (const auto evaluated = scheduler.Value()->EvaluateAtDecision(tick); evaluated.HasError())
                return Result<void>::Failure(evaluated.ErrorValue());
            const auto committed = scheduler.Value()->CommitAtIntentDispatch(tick);
            return committed.HasError() ? Result<void>::Failure(committed.ErrorValue()) : Result<void>::Success();
        }

        /** @brief Polls native observations once on the configured authority's exact fixed tick and Scene. */
        Result<void> RunConfiguredPerception(const Runtime::FixedStepContext &context) const {
            if (!OwnsPerception() || (!owners_->configured->perceptionExecution && !SelectsNativePerception()))
                return Result<void>::Success();
            if (context.cancellation.IsCancellationRequested())
                return JobCancelled();
            const auto scene = record_->scenes->ActiveScene();
            if (!scene)
                return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionSpatialStale));
            return record_->ai->PollPerceptionAtSensePoll(*scene, record_->perception, context.simulationTick, context.fixedDelta);
        }

        /** @brief Uses ordinary fixed stages only after readiness; load progress never dispatches a tick. */
        Result<void> RunConfiguredTick(const Runtime::FixedStepContext &context) {
            if (!configuredActivated_ || !record_ || record_->closeRequested)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if (record_->navigationPath) {
                if (context.cancellation.IsCancellationRequested())
                    return JobCancelled();
                if (const auto committed = record_->navigationPath->CommitAtFixedTick(context.simulationTick); committed.HasError())
                    return committed;
            }
            if (const auto perception = RunConfiguredPerception(context); perception.HasError())
                return perception;
            if (const auto decisions = RunConfiguredDecisions(context); decisions.HasError())
                return decisions;
            if (const auto gameplay = RunConfiguredGameplay(context); gameplay.HasError())
                return gameplay;
            if (record_->animation) {
                auto animation = owners_->fixed->Evaluate(record_, context);
                if (animation.HasError())
                    return animation;
            }
            auto advanced = record_->native.AdvanceFixedTick(context.fixedDelta);
            if (advanced.HasError()) {
                if (gameplay_)
                    gameplay_->Shutdown();
                return advanced;
            }
            if (record_->navigationPath)
                record_->navigationPath->DispatchAtFixedTick(context.simulationTick);
            return Result<void>::Success();
        }

        /** @brief Prepare only canonical Physics for this exact paired Scene; no fallback or ambient lookup. */
        Result<void> PreparePhysics() {
            if (auto prepared = owners_->PreparePhysicsRuntime(Physics::PhysicsRuntimeMode::Canonical); prepared.HasError())
                return prepared;
            Physics::PhysicsWorldSettingsDescriptor descriptor;
            descriptor.world.capacity = {16, 32, 16, 4096};
            descriptor.budgets.maximumContactPairs = 32;
            descriptor.budgets.maximumContactConstraints = 16;
            descriptor.budgets.maximumInFlightPairs = 8;
            descriptor.budgets.scratchBytes = 1024 * 1024;
            auto settings = Physics::PhysicsWorldSettings::Capture(descriptor);
            if (settings.HasError())
                return Result<void>::Failure(settings.ErrorValue());
            auto world = owners_->physics->PrepareWorld(settings.Value());
            if (world.HasError())
                return Result<void>::Failure(world.ErrorValue());
            physicsWorld_ = std::move(world).Value();
            if (!record_ || !record_->scene)
                return Result<void>::Failure(MakeError(Gameplay::GameplayErrors::PhysicsUnavailable));
            return Result<void>::Success();
        }

        std::shared_ptr<WorldOwners> owners_;
        std::optional<GameplayWorldSelection> selectedGameplay_;
        Network::NetworkModeServiceRequest request_;
        std::shared_ptr<WorldRecord> record_;
        std::unique_ptr<Physics::PhysicsWorld> physicsWorld_;
        std::unique_ptr<GameplayWorldComposition> gameplay_;
        std::uint64_t localTick_{};
        std::uint64_t phaseFrame_{};
        bool configuredActivated_{};
    };

}  // namespace Horo::Application::Internal::HeadlessNetworkDetail
