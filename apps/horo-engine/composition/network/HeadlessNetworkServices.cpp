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

        /** @brief Resolves one object against the committed Scene and the admitted live schema registry. */
        Result<Network::ReplicationAuthoredSceneObject> ResolveAuthoredObject(const Runtime::RuntimeSceneView &scene,
                                                                              const Network::NetworkObjectProvenance &binding,
                                                                              std::span<const Network::ReplicationSchemaId> admitted,
                                                                              const Network::ReplicationDescriptorSnapshot &installed) {
            using Row = Result<Network::ReplicationAuthoredSceneObject>;
            if (!binding.IsValid() || !binding.authoredObject)
                return Row::Failure(MakeError(Network::NetworkErrors::NetworkObjectMappingInvalid));
            const auto schema = installed.Find(binding.schema);
            if (schema.HasError() || schema.Value()->version != binding.schemaVersion ||
                std::ranges::find(admitted, binding.schema) == admitted.end())
                return Row::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
            const auto entity = scene.Find(*binding.authoredObject);
            if (!entity)
                return Row::Failure(MakeError(Network::NetworkErrors::ReplicationWorldStale));
            return Row::Success({*entity, binding});
        }

        /** @brief Copies the finite admitted projection without retaining a Scene borrow. */
        Result<Network::NetworkAuthoredSceneProjection> ProjectAuthoredObjects(const Runtime::RuntimeSceneView &scene,
                                                                               std::span<const Network::NetworkObjectProvenance> bindings,
                                                                               std::span<const Network::ReplicationSchemaId> admitted,
                                                                               const Network::ReplicationDescriptorSnapshot &installed,
                                                                               const CancellationToken &cancellation) {
            using Outcome = Result<Network::NetworkAuthoredSceneProjection>;
            if (bindings.size() > Network::ReplicationWorldLimits{}.maximumObjects)
                return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
            try {
                Network::NetworkAuthoredSceneProjection projection{.cancellation = cancellation};
                projection.objects.reserve(bindings.size());
                for (const auto &binding : bindings) {
                    auto row = ResolveAuthoredObject(scene, binding, admitted, installed);
                    if (row.HasError())
                        return Outcome::Failure(row.ErrorValue());
                    projection.objects.push_back(std::move(row).Value());
                }
                return Outcome::Success(std::move(projection));
            } catch (const std::bad_alloc &) {
                return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
            }
        }

        /** @brief Counts enabled persisted owners on an authored entity. Services have no authored occurrence. */
        std::size_t CountAuthoredOwners(const Runtime::RuntimeComponentSet &components, const Gameplay::GameplayReplicationOwner &owner) {
            if (const auto *behavior = std::get_if<Gameplay::BehaviorTypeId>(&owner))
                return static_cast<std::size_t>(std::ranges::count_if(components.behaviors, [&](const auto &binding) {
                    return binding.enabled && binding.typeId == *behavior;
                }));
            if (const auto *component = std::get_if<Gameplay::ComponentTypeId>(&owner))
                return static_cast<std::size_t>(
                    std::ranges::count(components.gameplayComponents, *component, &Gameplay::SerializedComponent::typeId));
            return 0;
        }

        struct AuthoredSchemaBinding final {
            const Gameplay::GameplayReplicationOwner *owner;
            const Network::ReplicationSchemaDescriptor *schema;
        };

        /** @brief Pins only registered and project-declared object schemas for this synchronous projection. */
        Result<std::vector<AuthoredSchemaBinding>> AdmitAuthoredSchemas(const Gameplay::GameplayReplicationLease &installed,
                                                                        std::span<const Network::ReplicationSchemaId> schemas) {
            using Bindings = Result<std::vector<AuthoredSchemaBinding>>;
            std::vector<AuthoredSchemaBinding> admitted;
            admitted.reserve(schemas.size());
            for (const auto &registration : installed.Registrations()) {
                if (std::holds_alternative<Gameplay::GameplayServiceId>(registration.owner))
                    continue;
                if (std::ranges::find(schemas, registration.schema) == schemas.end())
                    continue;
                auto schema = installed.Descriptors()->Find(registration.schema);
                if (schema.HasError())
                    return Bindings::Failure(schema.ErrorValue());
                // The registry was already checked by the role-specific project admission.
                admitted.push_back({&registration.owner, schema.Value()});
            }
            return Bindings::Success(std::move(admitted));
        }

        /** @brief Locates the sole admitted replication owner for one committed authored entity. */
        Result<const Network::ReplicationSchemaDescriptor *> SelectAuthoredSchema(const Runtime::RuntimeComponentSet &components,
                                                                                  std::span<const AuthoredSchemaBinding> bindings) {
            using Schema = Result<const Network::ReplicationSchemaDescriptor *>;
            const Network::ReplicationSchemaDescriptor *selected{};
            for (const auto &binding : bindings) {
                const auto count = CountAuthoredOwners(components, *binding.owner);
                if (count == 0)
                    continue;
                if (selected || count > 1)
                    return Schema::Failure(MakeError(Network::NetworkErrors::NetworkObjectMappingConflict,
                                                     "An authored network object must have one declared replication owner."));
                selected = binding.schema;
            }
            return Schema::Success(selected);
        }

        /** @brief Copies only authored Scene entities with a unique admitted replication owner. */
        Result<void> ProjectDiscoveredObjects(const Runtime::RuntimeSceneView &scene, std::span<const AuthoredSchemaBinding> bindings,
                                              Network::NetworkAuthoredSceneProjection &projection) {
            const auto slots = scene.SlotCount();
            for (std::size_t slot = 0; slot < slots; ++slot) {
                if (projection.cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCancelled));
                const auto entity = scene.EntityAt(slot);
                if (!entity || !entity->authoredObject)
                    continue;
                auto schema = SelectAuthoredSchema(*entity->components, bindings);
                if (schema.HasError())
                    return Result<void>::Failure(schema.ErrorValue());
                if (const auto *selected = schema.Value()) {
                    if (projection.objects.size() == Network::ReplicationWorldLimits{}.maximumObjects)
                        return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
                    projection.objects.push_back({entity->entity, {selected->id, selected->version, entity->authoredObject}});
                }
            }
            return Result<void>::Success();
        }

        /** @brief Resolves the admitted schema set before walking the committed Scene. */
        Result<Network::NetworkAuthoredSceneProjection> DiscoverAuthoredObjects(const Runtime::RuntimeSceneView &scene,
                                                                                const Gameplay::GameplayReplicationLease &installed,
                                                                                std::span<const Network::ReplicationSchemaId> schemas,
                                                                                const CancellationToken &cancellation) {
            using Outcome = Result<Network::NetworkAuthoredSceneProjection>;
            try {
                auto admitted = AdmitAuthoredSchemas(installed, schemas);
                if (admitted.HasError())
                    return Outcome::Failure(admitted.ErrorValue());
                Network::NetworkAuthoredSceneProjection projection{.cancellation = cancellation};
                if (!admitted.Value().empty()) {
                    if (auto copied = ProjectDiscoveredObjects(scene, admitted.Value(), projection); copied.HasError())
                        return Outcome::Failure(copied.ErrorValue());
                }
                return Outcome::Success(std::move(projection));
            } catch (const std::bad_alloc &) {
                return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCapacityExceeded));
            }
        }
    }  // namespace

    /** @copydoc HeadlessNetworkDetail::WorldService::CaptureAuthoredScene */
    Result<Network::NetworkAuthoredSceneProjection> WorldService::CaptureAuthoredScene() const {
        using Outcome = Result<Network::NetworkAuthoredSceneProjection>;
        if (!owners_->configured)
            return Outcome::Success({});
        const auto scene = ActiveScene();
        if (!scene || request_.service != Network::NetworkModeServiceKind::Scene ||
            request_.world != Network::NetworkModeWorldKind::AuthorityServer)
            return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldUnavailable));
        if (record_->sceneCancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationWorldCancelled));
        const auto &explicitBindings = owners_->configured->replicatedSceneObjects;
        const auto *installed = record_->activeGameplay ? record_->activeGameplay->Replication() : nullptr;
        if (!installed)
            return explicitBindings.empty() ? Outcome::Success({})
                                            : Outcome::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
        const auto schemas = record_->activeGameplay->AdmittedReplicationSchemas();
        if (explicitBindings.empty())
            return DiscoverAuthoredObjects(*scene, *installed, schemas, record_->sceneCancellation);
        return ProjectAuthoredObjects(*scene, explicitBindings, schemas, *installed->Descriptors(), record_->sceneCancellation);
    }

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
        NetworkProductComposition composition{InstallWorldFactories(owners, std::move(selectedFactories)),
                                              {},
                                              [owners](std::uint32_t maximumRecords) {
            return owners->PollRetirement(maximumRecords);
        }};
        if (owners->configured->animationIdentities || (owners->gameplay && owners->gameplay->vfxScene)) {
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
