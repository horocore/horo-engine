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
#include "Horo/AI/AINavigationIntentDispatch.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"

namespace Horo::Application::Internal::HeadlessNetworkDetail {
    class WorldService final : public Network::INetworkModeService {
    public:
        /** @copydoc Network::INetworkModeService::CaptureAuthoredScene */
        [[nodiscard]] Result<Network::NetworkAuthoredSceneProjection> CaptureAuthoredScene() const override;

        /** @copydoc AcquireHeadlessPublicTransforms */
        std::shared_ptr<const Network::SceneReplicationCommitSource> PublicTransforms() const noexcept {
            if (std::this_thread::get_id() != owners_->owner || !record_ || record_->closeRequested ||
                request_.world != Network::NetworkModeWorldKind::AuthorityServer)
                return {};
            return record_->publicTransforms;
        }

        [[nodiscard]] std::shared_ptr<const Gameplay::GameplayPhysicsContext> PhysicsContext() const noexcept {
            return gameplay_ ? gameplay_->PhysicsContext() : nullptr;
        }

        std::shared_ptr<const Gameplay::GameplayNavigationContext> NavigationContext() const noexcept {
            if (std::this_thread::get_id() != owners_->owner || !gameplay_ || !record_ || record_->closeRequested ||
                !record_->navigationPath)
                return {};
            return record_->navigationPath->Context();
        }

        Result<void> StageNavigationMotion(AI::AiTaskContinuation task, Navigation::NavigationMovementRequest request,
                                           const std::uint64_t tick) const {
            if (std::this_thread::get_id() != owners_->owner || !record_ || record_->closeRequested || !record_->decisionMotion ||
                !record_->ai || tick == 0 || tick != record_->motionEvaluationTick)
                return Result<void>::Failure(MakeError(AI::AIErrors::SchedulerPhaseInvalid));
            if (const auto resolved = record_->ai->CapabilityAtSafePoint().ResolveAgentOwner(task.Operation().agent); resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            return record_->decisionMotion->Stage(std::move(task), tick, std::move(request));
        }

        const Vfx::VfxExtractionOutcome *VfxFrame() const noexcept {
            if (std::this_thread::get_id() != owners_->owner || !record_ || record_->closeRequested || record_->closed || !record_->scenes)
                return nullptr;
            return &record_->scenes->VfxFrame();
        }

        const Runtime::SceneAnimationRuntime *AnimationOwner() const noexcept {
            return std::this_thread::get_id() == owners_->owner && record_ ? record_->animation.get() : nullptr;
        }

        /** @brief Admits an action only through the active authoritative Scene facade. */
        Animation::GraphActionCommandOutcome QueueAnimationAction(Runtime::EntityRef entity, const Animation::GraphActionRequest &request) {
            if (request_.service != Network::NetworkModeServiceKind::Scene || !IsAuthoritativeWorld() || !ActiveScene() ||
                !record_->animation)
                return {Animation::GraphActionFailure::Closed};
            return record_->animation->QueueAction(entity, request);
        }

        std::shared_ptr<const AI::PerceptionSpatialSnapshot> PerceptionSnapshot() const noexcept {
            if (!IsAuthoritativeWorld() || std::this_thread::get_id() != owners_->owner || !record_ || record_->closeRequested ||
                record_->closed)
                return {};
            return record_->perception.Current();
        }

        /** @brief Reads one agent's owned native observations from the exact currently committed Scene. */
        Result<AI::PerceptionMemorySnapshot> PerceptionMemory(const Runtime::EntityRef recipient) const {
            if (!IsAuthoritativeWorld())
                return Result<AI::PerceptionMemorySnapshot>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            const auto scene = ActiveScene();
            if (!scene || !record_->ai)
                return Result<AI::PerceptionMemorySnapshot>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            return record_->ai->ReadPerceptionAtSafePoint(*scene, recipient);
        }

        /** @brief Reads a copied Blackboard value from this exact live owner for product inspection. */
        Result<std::optional<AI::BlackboardValue>> ReadDecisionValue(AI::AgentId identity, AI::BlackboardKeyId key) const {
            using Value = Result<std::optional<AI::BlackboardValue>>;
            if (!IsAuthoritativeWorld() || !ActiveScene() || !record_->ai)
                return Value::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            const auto capability = record_->ai->CapabilityAtSafePoint();
            const auto agent = capability.ResolveAgent(identity);
            if (agent.HasError())
                return Value::Failure(agent.ErrorValue());
            return capability.ReadBlackboard(agent.Value(), key);
        }

        /** @brief Copies the last completed runtime finding from the authoritative Scene on its owner lane. */
        Result<std::optional<AI::AiRuntimeDecisionDiagnostic>> ReadDecisionFailure() const {
            if (!IsAuthoritativeWorld())
                return Result<std::optional<AI::AiRuntimeDecisionDiagnostic>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
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

        /** @brief Requests mounted work through the exact live Scene facade and its existing demand owner. */
        Result<void> RequestMountedCell(const CommittedWorldInitialCell &selected, const CancellationToken &cancellation) const {
            auto *mount = LiveMountedScene();
            return mount ? mount->RequestLiveCell(selected, cancellation)
                         : Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        }

        /** @brief Withdraws mounted work only from the exact live Scene facade and its existing demand owner. */
        Result<void> WithdrawMountedCell(const std::optional<Runtime::SceneInstanceId> occurrence = {}) const {
            auto *mount = LiveMountedScene();
            if (!mount)
                return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
            return occurrence ? mount->WithdrawLiveCell(*occurrence) : mount->WithdrawLiveCell();
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
                if (phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges && SelectsLiveMount())
                    if (auto advanced = AdvanceLiveMount(); advanced.HasError())
                        return advanced;
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
            RetireAi();
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
        /** @brief Checks immutable policy before any live scope lookup or Scene phase forwarding. */
        bool SelectsLiveMount() const noexcept {
            return owners_->configured && owners_->configured->mountedWorld && owners_->configured->mountedWorld->liveCell.has_value();
        }

        /** @brief Resolves only the exact active Scene facade. Physics and other roles cannot issue demand. */
        MountedWorld *LiveMountedScene() const noexcept {
            return SelectsLiveMount() && ActiveScene() && request_.service == Network::NetworkModeServiceKind::Scene
                       ? record_->mountedWorld.get()
                       : nullptr;
        }

        /** @brief The existing configured Scene/Physics pair owns this one cutoff when no gameplay owner is installed. */
        Result<void> AdvanceLiveMount() const {
            if (!record_->mountedWorld || !owners_->fixed)
                return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
            constexpr auto cutoff = Runtime::RuntimePhase::CommitDeferredLifecycleChanges;
            const auto &evidence = owners_->fixed->SceneDispatch();
            Runtime::RuntimeDispatchFacts facts;
            if (evidence.Read(owners_->fixed->Source(), cutoff, facts) != Runtime::RuntimeDispatchStatus::Valid)
                return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
            const auto advanced = record_->mountedWorld->AdvanceLiveCell(evidence);
            if (advanced.HasError() && advanced.ErrorValue().code.Value() != WorldStreaming::WorldArtifactErrors::Cancelled.code.Value())
                return Result<void>::Failure(advanced.ErrorValue());
            const Runtime::FrameContext frame{.frameNumber = facts.frame,
                                              .cancellation = record_->sceneCancellation,
                                              .dispatchEvidence = evidence};
            return record_->scenes->OnPhase(cutoff, frame);
        }

        bool IsSceneOwner() const noexcept {
            return record_ && owners_->configured && request_.service == Network::NetworkModeServiceKind::Scene;
        }

        void RevokeGameplayContext() const noexcept {
            if (record_)
                if (const auto context = record_->gameplay.lock())
                    context->Revoke();
        }

        /** @brief Cancels scheduled work and retires Scene-owned AI callbacks before the gameplay teardown. */
        void RetireAi() const noexcept {
            if (record_ && record_->ai)
                record_->ai->BeginShutdown();
        }

        void RetireGameplay() noexcept {
            if (record_ && record_->prefabReplica)
                record_->prefabReplica->Cancel();
            if (IsSceneOwner() && record_->activeGameplay) {
                record_->initialTransformCapture.reset();
                record_->activeGameplay->Shutdown();
                record_->activeGameplay = nullptr;
            }
            if (record_ && gameplay_ && record_->activeGameplay == gameplay_.get()) {
                record_->initialTransformCapture.reset();
                record_->activeGameplay = nullptr;
            }
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
            if (input.navigationPath && !IsAuthoritativeWorld())
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "The best-effort Navigation path scope requires world authority."));
            if (!input.navigationActors.empty() && (!input.navigationPath || !input.navigation || !owners_->gameplay))
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::CapabilityUnavailable));
            const auto &boundary = input.load;
            record_->sceneCancellation = boundary.cancellation;
            record_->scenes = std::make_shared<Runtime::RuntimeSceneService>(input.registry, input.loads,
                                                                             Runtime::RuntimeSceneAssetLimits{}, request_.scene);
            if (auto bound = BindReplicaSceneTransforms(); bound.HasError())
                return bound;
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
            if (input.decisionReplay && request_.world == Network::NetworkModeWorldKind::Standalone) {
                if (auto bound = HeadlessDecisionReplay::BindAuthoredScene(*record_, *owners_->definition); bound.HasError())
                    return bound;
            }
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
            if (const auto &live = input.mountedWorld->liveCell) {
                if (!owners_->fixed || !input.worldJobs)
                    return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Invalid));
                if (auto bound =
                        world.Value()->BindLiveCell(owners_->fixed->Source(), *input.worldJobs, live->limits, input.registry.Snapshot());
                    bound.HasError())
                    return bound;
                if (live->firstDemand)
                    if (auto requested = world.Value()->RequestLiveCell(*live->firstDemand, record_->sceneCancellation);
                        requested.HasError())
                        return requested;
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
            // Live CoreEcs admits no gameplay module. Scene itself rejects any component that needs an absent owner.
            if (SelectsLiveMount())
                return Result<void>::Success();
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
            if (auto motion = PrepareDecisionMotion(settings); motion.HasError())
                return motion;
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

        /** @brief Binds the selected policy to this generation's exact decision dispatch. */
        Result<void> PrepareDecisionMotion(const AI::AiSceneRuntimeSettings &settings) const {
            const auto &input = *owners_->configured;
            const auto &policy = input.navigationMotion;
            if (!policy)
                return Result<void>::Success();
            if (!OwnsCookedDecisionExecution() || !policy->solver || !policy->capture || input.navigationActors.empty() ||
                input.cookedDecisions->execution->intents)
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::CapabilityDescriptorInvalid));
            auto motion =
                AI::AiNavigationIntentDispatch::Create(settings.maximumAgents,
                                                       [weak = std::weak_ptr<WorldRecord>{record_},
                                                        policy](const AI::AiTaskContinuation &task,
                                                                Navigation::NavigationMovementRequest request, const std::uint64_t tick) {
                const auto world = weak.lock();
                if (!world || world->closeRequested || !world->navigationPath || !world->ai || !world->scenes)
                    return Result<void>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
                const auto scene = world->scenes->ActiveScene();
                return scene ? world->navigationPath->AdmitMotion(*scene, *policy, world->ai->CapabilityAtSafePoint(), task,
                                                                  std::move(request), tick, world->animation.get())
                             : Result<void>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
            });
            if (motion.HasError())
                return Result<void>::Failure(std::move(motion).ErrorValue());
            record_->decisionMotion = std::move(motion).Value();
            return Result<void>::Success();
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
            if (record_->decisionMotion) {
                execution.intents = record_->decisionMotion;
                execution.timing = AI::AiCookedDecisionTiming::BestEffortExternalFeedback;
            }
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
            selected.animation = request_.world == Network::NetworkModeWorldKind::Client ? std::weak_ptr<Runtime::SceneAnimationRuntime>{}
                                                                                         : record_->animation;
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
            return BindAuthoritySceneTransforms();
        }

        /** @brief Installs the cooked Prefab mapping adapter for this exact authority Scene only. */
        [[nodiscard]] Result<void> BindAuthorityPrefabs(GameplayWorldSelection &selected) const;

        /** @brief Installs an explicitly configured public transform source after exact schema admission. */
        [[nodiscard]] Result<void> BindAuthoritySceneTransforms() const;
        [[nodiscard]] Result<void> BindReplicaSceneTransforms() const;

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
            const auto scene = record_->scenes->ActiveScene();
            if (!scene)
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::StaleSnapshot));
            if (auto actors = paths.Value()->PrepareActors(record_->native, *scene, owners_->configured->navigationActors);
                actors.HasError())
                return actors;
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
            const auto &replay = owners_->configured->decisionReplay;
            auto *observed = replay ? replay->ObserveGameplay(*record_, context) : nullptr;
            return gameplay_->FixedTick(context, {}, observed);
        }

        /** @brief Executes the opted-in canonical AI phases before configured gameplay on the same fixed clock. */
        Result<void> RunConfiguredDecisions(const Runtime::FixedStepContext &context) const {
            if (!OwnsCookedDecisionExecution())
                return Result<void>::Success();
            if (context.cancellation.IsCancellationRequested())
                return JobCancelled();
            if (record_->decisionTick != 0 && record_->decisionTick == context.simulationTick)
                return Result<void>::Success();

            struct ResetMotionPhase {
                std::uint64_t &tick;

                ~ResetMotionPhase() {
                    tick = 0;
                }
            } reset{record_->motionEvaluationTick};

            record_->motionEvaluationTick = context.simulationTick;
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

        /** @brief Publishes path results and consumes the preceding Character result before decision evaluation. */
        Result<void> PrepareNavigationTick(const Runtime::FixedStepContext &context) {
            auto *paths = record_->navigationPath.get();
            if (!paths)
                return Result<void>::Success();
            if (context.cancellation.IsCancellationRequested())
                return JobCancelled();
            const auto &replay = owners_->configured->decisionReplay;
            auto *observed = replay ? replay->ObserveNavigation(*record_, context) : nullptr;
            auto committed = paths->CommitAtFixedTick(context.simulationTick, observed);
            if (committed.HasError() || !record_->decisionMotion)
                return committed;
            if (context.simulationTick == 0)
                return Result<void>::Failure(MakeError(AI::AIErrors::SchedulerPhaseInvalid));
            paths->ObserveMotion(context.simulationTick - 1);
            paths->BeginMotionTick(context.simulationTick, static_cast<float>(context.fixedDelta.ToNanoseconds()) / 1'000'000'000.0F);
            return Result<void>::Success();
        }

        /** @brief Submits the selected actors' safe motion after behavior and Animation evaluation. */
        Result<void> SubmitConfiguredMotion() {
            if (!record_->decisionMotion)
                return Result<void>::Success();
            const auto scene = record_->scenes->ActiveScene();
            return scene ? record_->navigationPath->SubmitMotion(*scene, *owners_->configured->navigationMotion)
                         : Result<void>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        }

        /** @brief Uses ordinary fixed stages only after readiness; load progress never dispatches a tick. */
        Result<void> RunConfiguredTick(const Runtime::FixedStepContext &context) {
            if (!configuredActivated_ || !record_ || record_->closeRequested)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if (OwnsCookedDecisionExecution() && owners_->configured->decisionReplay && owners_->fixed &&
                request_.world == Network::NetworkModeWorldKind::Standalone)
                owners_->fixed->ArmDecisionReplay(*record_, context);
            if (auto navigation = PrepareNavigationTick(context); navigation.HasError())
                return navigation;
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
            if (auto motion = SubmitConfiguredMotion(); motion.HasError())
                return motion;
            auto advanced = AdvanceConfiguredNative(context);
            if (advanced.HasError()) {
                if (record_->decisionMotion)
                    record_->navigationPath->DiscardMotionFeedback();
                RetireAi();
                if (gameplay_)
                    gameplay_->Shutdown();
                return advanced;
            }
            return Result<void>::Success();
        }

        /** @brief Keeps authoritative Character candidates under the same scheduler fence as their Animation owner. */
        Result<void> AdvanceConfiguredNative(const Runtime::FixedStepContext &context) {
            if (record_->animation && record_->request.world != Network::NetworkModeWorldKind::Client) {
                const auto scene = record_->scenes->ActiveScene();
                if (!scene)
                    return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
                return record_->native.StageAnimationFixedTick(*scene, *record_->animation, context);
            }
            auto advanced = record_->native.AdvanceFixedTick(context.fixedDelta);
            if (advanced.HasValue() && record_->navigationPath)
                record_->navigationPath->DispatchAtFixedTick(context.simulationTick);
            return advanced;
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
