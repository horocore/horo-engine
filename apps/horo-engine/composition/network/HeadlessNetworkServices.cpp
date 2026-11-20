#include "HeadlessInitialStateAuthority.h"
#include "HeadlessNetworkWorldService.h"
#include "HeadlessPerceptionPolicy.h"
#include "Horo/Gameplay/GameplayVfxScene.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "PhysicsStateHostLifecycle.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using ReplayEntity = Result<AI::AiSavedEntityReference>;

        /** @brief Resolves the exact current root authored entity; raw runtime references never enter the encoded record. */
        ReplayEntity ProjectReplayEntity(const HeadlessNetworkDetail::ReplayAuthoredScene &authored, const Runtime::RuntimeSceneView &scene,
                                         const AI::BlackboardStoredEntityReference &stored) {
            const Runtime::EntityRef reference{{stored.sceneIncarnation}, {stored.slot, stored.generation}};
            auto entity = scene.Get(reference);
            if (entity.HasError())
                return ReplayEntity::Failure(std::move(entity).ErrorValue());
            if (!scene.SameIncarnation(authored.initialScene) || stored.slot >= authored.slots.size() ||
                authored.slots[stored.slot] != reference.entity || !entity.Value().authoredObject)
                return ReplayEntity::Failure(
                    MakeError(AI::AIErrors::CanonicalStateInvalid, "AI replay requires an original root authored entity reference."));

            // The original authored incarnation is independent of transient slot generations and addresses.
            AI::AiSavedEntityReference projected{.generation = 1};
            constexpr auto width = sizeof(std::uint64_t);
            for (std::size_t byte = 0; byte < width; ++byte) {
                const auto shift = (width - 1 - byte) * 8;
                projected.identity[byte] = static_cast<std::uint8_t>(scene.DefinitionId().value >> shift);
                projected.identity[width + byte] = static_cast<std::uint8_t>(entity.Value().authoredObject->value >> shift);
            }
            return ReplayEntity::Success(projected);
        }

        /** @brief Detaches canonical reference bytes while the fixed owner still holds the exact Scene borrow. */
        HeadlessDecisionReplay::Capture EncodeReplayCapture(AI::AiSceneDecisionReplayCapture evidence,
                                                            const HeadlessNetworkDetail::ReplayAuthoredScene &authored,
                                                            const Runtime::RuntimeSceneView &scene) {
            auto encoded = AI::EncodeAiCanonicalStateRecord(evidence.canonical, HeadlessDecisionReplay::CanonicalBudget,
                                                            [&](const AI::BlackboardStoredEntityReference &stored) {
                return ProjectReplayEntity(authored, scene, stored);
            });
            if (encoded.HasError())
                return HeadlessDecisionReplay::Capture::Failure(std::move(encoded).ErrorValue());
            return HeadlessDecisionReplay::Capture::Success({std::move(evidence), std::move(encoded).Value()});
        }
    }  // namespace

    /** @copydoc HeadlessDecisionReplay::BindAuthoredScene */
    Result<void> HeadlessDecisionReplay::BindAuthoredScene(HeadlessNetworkDetail::WorldRecord &world) {
        const auto scene = world.scenes->ActiveScene();
        if (!scene || scene->RuntimeId() != world.request.scene || !scene->DefinitionId().IsValid())
            return Result<void>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        const auto slots = scene->SlotCount();
        if (slots > CanonicalBudget / sizeof(Runtime::EntityId))
            return Result<void>::Failure(
                MakeError(AI::AIErrors::CanonicalStateInvalid, "Root Scene exceeds the replay identity inventory limit."));
        try {
            HeadlessNetworkDetail::ReplayAuthoredScene authored{*scene, std::vector<Runtime::EntityId>(slots)};
            for (std::size_t slot = 0; slot < authored.slots.size(); ++slot) {
                const auto entity = scene->EntityAt(slot);
                if (entity && entity->authoredObject)
                    authored.slots[slot] = entity->entity.entity;
            }
            world.replayAuthoredScene.emplace(std::move(authored));
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc HeadlessDecisionReplay::TakeCommitted */
    std::optional<HeadlessDecisionReplay::Capture> HeadlessDecisionReplay::TakeCommitted() noexcept {
        return owner_ == std::this_thread::get_id() ? std::exchange(committed_, std::nullopt) : std::nullopt;
    }

    void HeadlessDecisionReplay::Arm(HeadlessNetworkDetail::WorldRecord &world, const Runtime::FixedStepContext &context) {
        if (owner_ != std::this_thread::get_id() || published_ || attemptWorld_ || context.simulationTick != selection_.tick || !world.ai)
            return;
        attemptWorld_ = &world;
        attempt_ = context.attemptNumber;
        const auto agent = world.ai->CapabilityAtSafePoint().ResolveAgent(selection_.agent);
        if (agent.HasError()) {
            pending_ = Capture::Failure(agent.ErrorValue());
            return;
        }
        const auto armed = world.ai->ArmDecisionReplayAtSafePoint(agent.Value(), selection_.tick);
        if (armed.HasError())
            pending_ = Capture::Failure(armed.ErrorValue());
        else
            armedAgent_ = agent.Value();
    }

    void HeadlessDecisionReplay::Prepare(const HeadlessNetworkDetail::WorldRecord &world, const Runtime::FixedStepContext &context) {
        if (owner_ != std::this_thread::get_id() || pending_ || attemptWorld_ != &world || attempt_ != context.attemptNumber ||
            selection_.tick != context.simulationTick)
            return;
        const auto scene = world.closeRequested ? std::nullopt : world.scenes->ActiveScene();
        auto evidence = scene && world.replayAuthoredScene
                            ? world.ai->CaptureDecisionReplayAtSafePoint(*scene, selection_.tick, CanonicalBudget)
                            : Result<AI::AiSceneDecisionReplayCapture>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        if (evidence.HasError()) {
            pending_ = Capture::Failure(std::move(evidence).ErrorValue());
            DiscardArm();
            return;
        }
        armedAgent_ = {};
        pending_ = EncodeReplayCapture(std::move(evidence).Value(), *world.replayAuthoredScene, *scene);
    }

    void HeadlessDecisionReplay::Commit(const Runtime::FixedStepContext &context) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<Capture>);
        if (owner_ != std::this_thread::get_id() || !pending_ || attempt_ != context.attemptNumber ||
            selection_.tick != context.simulationTick)
            return;
        committed_.emplace(std::move(*pending_));
        published_ = true;
        pending_.reset();
        attemptWorld_ = nullptr;
    }

    void HeadlessDecisionReplay::Abort(const Runtime::FixedStepContext &context) noexcept {
        if (owner_ == std::this_thread::get_id() && attemptWorld_ && attempt_ == context.attemptNumber &&
            selection_.tick == context.simulationTick) {
            pending_.reset();
            DiscardArm();
            attemptWorld_ = nullptr;
        }
    }

    /** @copydoc HeadlessDecisionReplay::DiscardArm */
    void HeadlessDecisionReplay::DiscardArm() noexcept {
        if (armedAgent_.IsValid())
            attemptWorld_->ai->DiscardDecisionReplayAtSafePoint(armedAgent_, selection_.tick);
        armedAgent_ = {};
    }

    namespace {
        using HeadlessNetworkDetail::HeadlessFixedParticipant;
        using HeadlessNetworkDetail::WorldOwners;
        using HeadlessNetworkDetail::WorldRecord;
        using HeadlessNetworkDetail::WorldService;

        /** @brief Allocate one non-reused Scene activation epoch with a bounded unpaired-factory slot. */
        Result<std::shared_ptr<WorldRecord>> CreateRecord(WorldOwners &owners, const Network::NetworkModeServiceRequest &request) {
            const auto free = std::ranges::find_if(owners.unpaired, [](const auto &record) {
                return record.expired();
            });
            if (free == owners.unpaired.end() || owners.nextWorldGeneration == 0)
                return Result<std::shared_ptr<WorldRecord>>::Failure(MakeError(Physics::PhysicsErrors::GenerationExhausted));
            std::weak_ptr<WorldRecord> *knownSlot{};
            if (owners.configured) {
                if (!request.scene.IsValid())
                    return Result<std::shared_ptr<WorldRecord>>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
                const auto &world = owners.configured->mountedWorld;
                for (auto &known : owners.known) {
                    const auto retained = known.lock();
                    if (world && world->liveCell && retained && retained->mountedWorld && !retained->closeRequested)
                        return Result<std::shared_ptr<WorldRecord>>::Failure(
                            MakeError(WorldStreaming::WorldArtifactErrors::Unsupported,
                                      "Replacing a live mounted World requires a cold shutdown."));
                    if (retained && retained->request.scene == request.scene)
                        return Result<std::shared_ptr<WorldRecord>>::Failure(
                            MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                      "Scene identity is already reserved by a current, pending or retained world record."));
                    if (!retained && !knownSlot)
                        knownSlot = &known;
                }
                if (!knownSlot)
                    return Result<std::shared_ptr<WorldRecord>>::Failure(
                        MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                  "The bounded current/replacement role records have not retired."));
            }
            auto record = std::make_shared<WorldRecord>(request);
            record->request = request;
            record->generation = owners.nextWorldGeneration++;
            *free = record;
            if (knownSlot)
                *knownSlot = record;
            return Result<std::shared_ptr<WorldRecord>>::Success(std::move(record));
        }

        /** @brief Match the complete world-instance request, not merely its reusable Scene ID. */
        bool MatchesRequest(const std::shared_ptr<WorldRecord> &record, const Network::NetworkModeServiceRequest &request) {
            return record && record->request.world == request.world && record->request.scene == request.scene &&
                   record->request.mode == request.mode;
        }

        /** @brief Host factories borrow only their actual owner lane and cannot reopen retired native admission. */
        Result<void> ValidateFactoryOwner(const WorldOwners &owners) {
            if (std::this_thread::get_id() != owners.owner)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
            if (owners.retirementStarted)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
            return Result<void>::Success();
        }

        /** @brief Pair factories before preparation, preserving exact candidate ownership even for repeated Scene IDs. */
        [[nodiscard]] Result<std::shared_ptr<WorldRecord>> SelectRecord(WorldOwners &owners,
                                                                        const Network::NetworkModeServiceRequest &request) {
            if (auto valid = ValidateFactoryOwner(owners); valid.HasError())
                return Result<std::shared_ptr<WorldRecord>>::Failure(valid.ErrorValue());
            using Kind = Network::NetworkModeServiceKind;
            if (request.service != Kind::Scene && request.service != Kind::Physics)
                return Result<std::shared_ptr<WorldRecord>>::Success({});
            if (request.service == Kind::Scene)
                return CreateRecord(owners, request);
            std::shared_ptr<WorldRecord> selected;
            std::weak_ptr<WorldRecord> *slot{};
            for (auto &candidate : owners.unpaired) {
                auto record = candidate.lock();
                if (!MatchesRequest(record, request))
                    continue;
                if (selected)
                    return Result<std::shared_ptr<WorldRecord>>::Failure(
                        MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                  "Ambiguous unpaired Scene candidates; construct each Scene/Physics pair together."));
                selected = std::move(record);
                slot = &candidate;
            }
            if (!selected)
                return Result<std::shared_ptr<WorldRecord>>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Physics has no exact unpaired Scene candidate."));
            slot->reset();
            return Result<std::shared_ptr<WorldRecord>>::Success(std::move(selected));
        }

    }  // namespace

    namespace {
        /** @brief Registers the same exact pair factories for synthetic and explicit project compositions. */
        Network::NetworkModeFactories InstallWorldFactories(const std::shared_ptr<WorldOwners> &owners,
                                                            Network::NetworkModeFactories selectedFactories) {
            using enum Network::NetworkModeServiceKind;
            for (const auto service : {Scene, Physics, Replication}) {
                selectedFactories.services[static_cast<std::size_t>(service)] =
                    [owners](const Network::NetworkModeServiceRequest &request) {
                    auto record = SelectRecord(*owners, request);
                    if (record.HasError())
                        return Result<std::unique_ptr<Network::INetworkModeService>>::Failure(record.ErrorValue());
                    return Result<std::unique_ptr<Network::INetworkModeService>>::Success(
                        std::make_unique<WorldService>(owners, request, std::move(record).Value()));
                };
            }
            return selectedFactories;
        }
    }  // namespace

    Network::NetworkModeFactories ComposeHeadlessNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> sceneDefinition, Network::NetworkModeFactories selectedFactories,
        const std::optional<GameplayWorldSelection> &gameplay,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission, NetworkNativeRetirementPoll *retirement) {
        const auto owners = std::make_shared<WorldOwners>(std::move(sceneDefinition), gameplay, replicationAdmission);
        if (retirement)
            *retirement = [owners](std::uint32_t maximumRecords) {
                return owners->PollRetirement(maximumRecords);
            };
        return InstallWorldFactories(owners, std::move(selectedFactories));
    }

    namespace {
        Result<void> ValidateConfiguredInput(const std::shared_ptr<const Runtime::RuntimeSceneDefinition> &definition,
                                             const HeadlessSceneActivationInput &input,
                                             const std::optional<GameplayWorldSelection> &gameplay) {
            if (const auto policy = Prefab::PrefabLimitProfile::Create(input.prefabPolicy); policy.HasError())
                return Result<void>::Failure(policy.ErrorValue());
            if (!definition || input.load.deadline == std::chrono::steady_clock::time_point{} ||
                input.load.deadline == std::chrono::steady_clock::time_point::max())
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if (input.perceptionExecution && (!input.perceptionPositions || !input.perceptionExecution->generation))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "Native Perception requires a selected producer and Scene positions."));
            if (input.publicTransforms && (!gameplay || !gameplay->enabled))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
            const auto &decisions = input.cookedDecisions;
            if (input.decisionReplay && (!input.decisionReplay->Selection().IsValid() || !decisions || !decisions->execution))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "AI replay requires an exact agent, tick and configured cooked execution."));
            if (decisions && decisions->provider == HeadlessDecisionProvider::SelectedNativeGameplay &&
                (!decisions->execution || decisions->environment.nodes || !gameplay || !gameplay->enabled ||
                 gameplay->nativeArtifact.empty() || !gameplay->moduleRetirements))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "Selected native AI requires scoped native gameplay and execution."));
            const bool needsPrefab = decisions && std::ranges::any_of(decisions->bindings, [](const auto &binding) {
                return binding.prefab.has_value();
            });
            if (needsPrefab && (!gameplay || gameplay->prefabPermission != GameplayPrefabPermission::Granted))
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "AI prefab selection requires the actual host's prefab grant."));
            if (input.navigationPath && (!input.navigationPath->IsValid() || !input.navigation || !gameplay || !gameplay->enabled ||
                                         gameplay->nativeArtifact.empty() || (decisions && decisions->execution)))
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid,
                              "Best-effort paths require cooked Navigation and native gameplay without deterministic AI dispatch."));
            if (gameplay && gameplay->prefabPermission == GameplayPrefabPermission::Granted && !input.prefabLoads)
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Prefab grant requires an explicit committed artifact loader."));
            return Result<void>::Success();
        }

        /** @brief Denies unsupported live composition before creating Scene and native owners. */
        Result<void> ValidateLiveWorldSelection(const HeadlessSceneActivationInput &input,
                                                const std::optional<GameplayWorldSelection> &gameplay) {
            if (!input.mountedWorld || !input.mountedWorld->liveCell)
                return Result<void>::Success();
            if (!input.worldJobs || input.mountedWorld->initialCell || gameplay)
                return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Unsupported,
                                                       "Live mounted CoreEcs requires the standalone Scene structural owner."));
            return Result<void>::Success();
        }

        Result<void> ValidateAnimationSelection(const HeadlessSceneActivationInput &input) {
            if (input.animationTarget.IsValid() != static_cast<bool>(input.animationIdentities))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if ((input.animationInputs || input.replicaAnimationInputs) && !input.animationIdentities)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            return Result<void>::Success();
        }
    }  // namespace

    namespace {
        /** @brief Binds a declared origin to this exact committed definition before installing either callback. */
        Result<void> AdmitPerceptionProject(const std::shared_ptr<const Runtime::RuntimeSceneDefinition> &definition,
                                            HeadlessSceneActivationInput &input) {
            if (!input.perceptionProject)
                return Result<void>::Success();
            const auto &[policy, registry, provider] = *input.perceptionProject;
            if (!definition || !policy || input.perceptionPositions || input.perceptionGroupPositions)
                return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing,
                                                       "Project Perception needs one complete position and descriptor authority."));
            switch (provider) {
                case HeadlessPerceptionProvider::CapturedRegistry:
                    if (!registry)
                        return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
                    break;
                case HeadlessPerceptionProvider::SelectedNativeGameplay:
                    if (registry || input.perceptionExecution || !policy->native || !input.cookedDecisions ||
                        input.cookedDecisions->provider != HeadlessDecisionProvider::SelectedNativeGameplay)
                        return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing,
                                                               "Native Perception requires schema two and a selected prepared module."));
                    break;
            }
            if (definition->Id() != policy->definition || definition->Revision() != policy->revision)
                return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionSpatialStale));
            auto sources = MakeHeadlessPerceptionPositionSources(policy);
            input.perceptionPositions = std::move(sources.scene);
            input.perceptionGroupPositions = std::move(sources.group);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ComposeHeadlessSceneNetworkServices */
    Result<NetworkProductComposition> ComposeHeadlessSceneNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition, HeadlessSceneActivationInput input,
        Network::NetworkModeFactories selectedFactories, const std::optional<GameplayWorldSelection> &gameplay,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission) {
        if (auto admitted = AdmitPerceptionProject(definition, input); admitted.HasError())
            return Result<NetworkProductComposition>::Failure(admitted.ErrorValue());
        if (auto valid = ValidateConfiguredInput(definition, input, gameplay); valid.HasError())
            return Result<NetworkProductComposition>::Failure(std::move(valid).ErrorValue());
        if (auto valid = ValidateAnimationSelection(input); valid.HasError())
            return Result<NetworkProductComposition>::Failure(std::move(valid).ErrorValue());
        if (auto valid = ValidateLiveWorldSelection(input, gameplay); valid.HasError())
            return Result<NetworkProductComposition>::Failure(std::move(valid).ErrorValue());
        auto owners = std::make_shared<WorldOwners>(std::move(definition), gameplay, replicationAdmission);
        if (auto prepared = owners->PreparePhysicsRuntime(input.mode); prepared.HasError())
            return Result<NetworkProductComposition>::Failure(prepared.ErrorValue());
        owners->configured.emplace(std::move(input));
        if (auto &configured = *owners->configured; configured.initialSceneState) {
            if (!configured.publicTransforms)
                return Result<NetworkProductComposition>::Failure(MakeError(Network::NetworkErrors::AdmissionPolicyInvalid));
            if (auto bound = configured.initialSceneState->BindWorlds(owners); bound.HasError())
                return Result<NetworkProductComposition>::Failure(std::move(bound).ErrorValue());
        }
        NetworkProductComposition composition{InstallWorldFactories(owners, std::move(selectedFactories)),
                                              {},
                                              [owners](std::uint32_t maximumRecords) {
            return owners->PollRetirement(maximumRecords);
        }};
        const auto &mount = owners->configured->mountedWorld;
        if (owners->configured->decisionReplay || owners->configured->animationIdentities || owners->configured->publicTransforms ||
            (mount && mount->liveCell) || (owners->gameplay && owners->gameplay->vfxScene)) {
            if (owners->configured->animationIdentities && !owners->configured->animationTarget.IsValid())
                return Result<NetworkProductComposition>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            composition.fixedParticipant = [owners](Runtime::RuntimeDispatchSource source) {
                return HeadlessFixedParticipant::Create(owners, std::move(source));
            };
        }
        return Result<NetworkProductComposition>::Success(std::move(composition));
    }

    /** @copydoc InspectHeadlessVfxFrame */
    const Vfx::VfxExtractionOutcome *InspectHeadlessVfxFrame(const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->VfxFrame() : nullptr;
    }

    const Runtime::SceneAnimationRuntime *InspectHeadlessAnimation(const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->AnimationOwner() : nullptr;
    }

    /** @copydoc AcquireHeadlessPublicTransforms */
    std::shared_ptr<const Network::SceneReplicationCommitSource> AcquireHeadlessPublicTransforms(
        const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->PublicTransforms() : nullptr;
    }

    /** @copydoc InspectHeadlessPerception */
    std::shared_ptr<const AI::PerceptionSpatialSnapshot> InspectHeadlessPerception(const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->PerceptionSnapshot() : nullptr;
    }

    /** @copydoc InspectHeadlessPerceptionMemory */
    Result<AI::PerceptionMemorySnapshot> InspectHeadlessPerceptionMemory(const Network::INetworkModeService &service,
                                                                         const Runtime::EntityRef recipient) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        if (!world)
            return Result<AI::PerceptionMemorySnapshot>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        return world->PerceptionMemory(recipient);
    }

    /** @copydoc InspectHeadlessDecisionValue */
    Result<std::optional<AI::BlackboardValue>> InspectHeadlessDecisionValue(const Network::INetworkModeService &service, AI::AgentId agent,
                                                                            AI::BlackboardKeyId key) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        if (!world)
            return Result<std::optional<AI::BlackboardValue>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        return world->ReadDecisionValue(agent, key);
    }

    /** @copydoc InspectHeadlessDecisionFailure */
    Result<std::optional<AI::AiRuntimeDecisionDiagnostic>> InspectHeadlessDecisionFailure(const Network::INetworkModeService &service) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        if (!world)
            return Result<std::optional<AI::AiRuntimeDecisionDiagnostic>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        return world->ReadDecisionFailure();
    }

    /** @copydoc InspectHeadlessScene */
    std::optional<Runtime::RuntimeSceneView> InspectHeadlessScene(const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->ActiveScene() : std::nullopt;
    }

    /** @copydoc InspectHeadlessWorldOrigin */
    std::optional<WorldStreaming::OriginFrameLease> InspectHeadlessWorldOrigin(const Network::INetworkModeService &service) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->WorldOrigin() : std::nullopt;
    }

    /** @copydoc RequestHeadlessWorldCell */
    Result<void> RequestHeadlessWorldCell(const Network::INetworkModeService &service, const CommittedWorldInitialCell &selected,
                                          const CancellationToken &cancellation) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->RequestMountedCell(selected, cancellation)
                     : Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
    }

    /** @copydoc WithdrawHeadlessWorldCell(const Network::INetworkModeService &) */
    Result<void> WithdrawHeadlessWorldCell(const Network::INetworkModeService &service) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->WithdrawMountedCell() : Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
    }

    /** @copydoc WithdrawHeadlessWorldCell(const Network::INetworkModeService &, Runtime::SceneInstanceId) */
    Result<void> WithdrawHeadlessWorldCell(const Network::INetworkModeService &service, const Runtime::SceneInstanceId occurrence) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->WithdrawMountedCell(occurrence)
                     : Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
    }

    /** @copydoc CaptureHeadlessWorldRenderGeometry */
    Result<MountedWorldRenderPacket> CaptureHeadlessWorldRenderGeometry(const Network::INetworkModeService &service,
                                                                        const Runtime::SceneObjectId camera,
                                                                        std::span<const Runtime::SceneObjectId> meshes,
                                                                        Runtime::PrimitiveMeshCache &primitives,
                                                                        Render::RenderOriginSceneAdapter &renderer) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        if (!world)
            return Result<MountedWorldRenderPacket>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        return world->CaptureWorldRenderGeometry(camera, meshes, primitives, renderer);
    }

    std::optional<Runtime::EntityRef> InspectHeadlessAnimationEntity(const Network::INetworkModeService &service,
                                                                     Runtime::SceneObjectId object) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->AnimationEntity(object) : std::nullopt;
    }

    /** @copydoc InspectHeadlessNavigationContext */
    std::shared_ptr<const Gameplay::GameplayNavigationContext> InspectHeadlessNavigationContext(
        const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->NavigationContext() : nullptr;
    }

    /** @copydoc InspectHeadlessGameplayContext */
    std::shared_ptr<const Gameplay::GameplayPhysicsContext> InspectHeadlessGameplayContext(
        const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->PhysicsContext() : nullptr;
    }
}  // namespace Horo::Application::Internal
