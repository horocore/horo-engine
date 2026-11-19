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
#include <optional>
#include <utility>

namespace Horo::Application::Internal {
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
                for (auto &known : owners.known) {
                    const auto retained = known.lock();
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

        Result<void> ValidateAnimationSelection(const HeadlessSceneActivationInput &input) {
            if (input.animationTarget.IsValid() != static_cast<bool>(input.animationIdentities))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if (input.animationInputs && !input.animationIdentities)
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
        if (owners->configured->animationIdentities || owners->configured->publicTransforms ||
            (owners->gameplay && owners->gameplay->vfxScene)) {
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
