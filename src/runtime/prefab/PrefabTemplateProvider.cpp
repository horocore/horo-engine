#include "Horo/Prefab/PrefabTemplateProvider.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabTemplateProviderInternal.h"

#include <algorithm>
#include <utility>

namespace Horo::Prefab {
    namespace {
        /** @brief Preserves cooked schema/occurrence authority while allowing fresh runtime behavior identities. */
        bool MatchesCookedMember(const CookedPrefabMember &source, const Runtime::RuntimeGroupMemberIdentity &projected) {
            if (const auto *raw = std::get_if<RawComponentPayload>(&source)) {
                const auto *type = std::get_if<Gameplay::ComponentTypeId>(&projected.type);
                return type && *type == raw->component.typeId && projected.instance == raw->instance.Value();
            }
            const auto *type = std::get_if<Gameplay::BehaviorTypeId>(&projected.type);
            return type && *type == std::get<Gameplay::BehaviorComponent>(source).typeId && projected.instance != 0;
        }

        /** @brief Checks portable target preservation; Scene still admits actual external entities at its final fence. */
        bool MatchesCookedReference(const CookedPrefabData &data, const CookedPrefabReference &source,
                                    const Runtime::RuntimeGroupReference &projected) {
            return std::visit([&data, &projected]<typename Target>(const Target &target) {
                if constexpr (std::is_same_v<Target, CookedPrefabEntitySlot>) {
                    const auto *value = std::get_if<Runtime::RuntimeGroupEntitySlot>(&projected.target);
                    return value && value->index == target.value;
                } else if constexpr (std::is_same_v<Target, CookedPrefabMemberSlot>) {
                    const auto *value = std::get_if<Runtime::RuntimeGroupMemberSlot>(&projected.target);
                    return value && value->entity.index == target.entity.value && value->member == target.member;
                } else if constexpr (std::is_same_v<Target, CookedPrefabAssetSlot>) {
                    const auto *value = std::get_if<Assets::AssetDependency>(&projected.target);
                    return value && value->id == data.dependencies[target.value].asset.id &&
                           value->expectedType == data.dependencies[target.value].asset.expectedType;
                } else {
                    const auto &binding = data.bindings[target.value];
                    if (std::holds_alternative<std::monostate>(projected.target))
                        return !binding.required;
                    const auto *value = std::get_if<Runtime::RuntimeGroupExternalReference>(&projected.target);
                    return value && value->component == binding.componentType;
                }
            }, source.target);
        }

        /** @brief Requires every cooked occurrence and reference before transferring anything to the Scene queue. */
        Result<void> ValidatePreparedInterfaces(const CookedPrefabData &data, const PrefabPreparedGroupOptions &options) {
            std::size_t reference{};
            for (std::size_t entity = 0; entity < data.entities.size(); ++entity) {
                const auto &members = data.entities[entity].members;
                const auto projected = options.members.empty()
                                           ? std::span<const Runtime::RuntimeGroupMemberIdentity>{}
                                           : std::span<const Runtime::RuntimeGroupMemberIdentity>{options.members[entity]};
                if (projected.size() != members.size())
                    return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Incomplete cooked member projection."));
                for (std::size_t member = 0; member < members.size(); ++member) {
                    if (!MatchesCookedMember(members[member], projected[member]))
                        return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected, "Cooked member identity changed."));
                }
                const auto references = options.references.empty()
                                            ? std::span<const Runtime::RuntimeGroupReference>{}
                                            : std::span<const Runtime::RuntimeGroupReference>{options.references[entity]};
                std::size_t index{};
                while (reference < data.references.size() && data.references[reference].owner.entity.value == entity) {
                    if (const auto &source = data.references[reference++];
                        index >= references.size() || references[index].ownerMember != source.owner.member ||
                        references[index].property != source.property.Value() || !MatchesCookedReference(data, source, references[index]))
                        return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
                    ++index;
                }
                if (index != references.size())
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceRewriteInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Transfers dense projected components and metadata while preserving cooked topology and ordering. */
        std::vector<Runtime::RuntimeEntityGroupEntry> MakePreparedEntries(const std::span<const CookedPrefabEntity> entities,
                                                                          std::vector<Runtime::RuntimeComponentSet> components,
                                                                          PrefabPreparedGroupOptions &options) {
            std::vector<Runtime::RuntimeEntityGroupEntry> entries;
            entries.reserve(entities.size());
            for (std::size_t index = 0; index < entities.size(); ++index) {
                Runtime::RuntimeEntityGroupEntry entry;
                entry.info.localTransform = entities[index].localTransform;
                entry.info.components = std::move(components[index]);
                if (!options.primitiveMeshes.empty())
                    entry.info.primitiveMesh = std::move(options.primitiveMeshes[index]);
                if (!options.physicsReferences.empty())
                    entry.physicsReferences = std::move(options.physicsReferences[index]);
                if (!options.members.empty())
                    entry.members = std::move(options.members[index]);
                if (!options.references.empty())
                    entry.references = std::move(options.references[index]);
                entry.spawnLineage = options.spawnLineage;
                if (entities[index].parent)
                    entry.parentInGroup = entities[index].parent->value;
                entries.push_back(std::move(entry));
            }
            if (options.rootPlacement)
                entries.front().info.localTransform = *options.rootPlacement;
            entries.front().info.parent = options.parent;
            return entries;
        }
    }  // namespace

    /** @copydoc PrefabTemplateLease::PrefabTemplateLease */
    PrefabTemplateLease::PrefabTemplateLease(std::shared_ptr<const Detail::PrefabTemplateAllocation> allocation,
                                             Runtime::SceneRuntimeId scene) noexcept
        : allocation_(std::move(allocation)), scene_(scene) {}

    /** @copydoc PrefabTemplateLease::Template */
    const CookedPrefab *PrefabTemplateLease::Template() const noexcept {
        return allocation_ ? &allocation_->value : nullptr;
    }

    /** @copydoc PrefabTemplateLease::RegistryRevision */
    Assets::AssetRegistryRevision PrefabTemplateLease::RegistryRevision() const noexcept {
        return allocation_ ? allocation_->registry.Revision() : Assets::AssetRegistryRevision{};
    }

    /** @copydoc PrefabTemplateLease::Scene */
    Runtime::SceneRuntimeId PrefabTemplateLease::Scene() const noexcept {
        return scene_;
    }

    /** @copydoc PrefabTemplateLease::Artifact */
    Assets::AssetPayloadLease PrefabTemplateLease::Artifact() const noexcept {
        return allocation_ ? allocation_->root : Assets::AssetPayloadLease{};
    }

    /** @copydoc PrefabTemplateLease::Dependencies */
    std::span<const PrefabTemplateDependencyLease> PrefabTemplateLease::Dependencies() const noexcept {
        return allocation_ ? std::span<const PrefabTemplateDependencyLease>{allocation_->dependencies}
                           : std::span<const PrefabTemplateDependencyLease>{};
    }

    /** @copydoc PrefabTemplateProviderLimits::IsValid */
    bool PrefabTemplateProviderLimits::IsValid() const noexcept {
        return maximumOutstanding > 0 && maximumOutstanding <= 1024 && maximumConcurrentLoads > 0 &&
               maximumConcurrentLoads <= maximumOutstanding && maximumTemplates > 0 && maximumTemplates <= 128 &&
               maximumArtifactBytes > 0 && maximumArtifactBytes <= 256U * 1024U * 1024U && maximumRetainedBytes >= maximumArtifactBytes;
    }

    /** @copydoc PrefabTemplateLoadHandle::PrefabTemplateLoadHandle */
    PrefabTemplateLoadHandle::PrefabTemplateLoadHandle(std::shared_ptr<Detail::PrefabTemplateRequest> request) noexcept
        : request_(std::move(request)) {}

    /** @copydoc PrefabTemplateLoadHandle::~PrefabTemplateLoadHandle */
    PrefabTemplateLoadHandle::~PrefabTemplateLoadHandle() {
        RequestCancel();
    }

    /** @copydoc PrefabTemplateLoadHandle::operator= */
    PrefabTemplateLoadHandle &PrefabTemplateLoadHandle::operator=(PrefabTemplateLoadHandle &&other) noexcept {
        if (this != &other) {
            RequestCancel();
            request_ = std::move(other.request_);
        }
        return *this;
    }

    /** @copydoc PrefabTemplateLoadHandle::State */
    PrefabTemplateLoadState PrefabTemplateLoadHandle::State() const noexcept {
        return request_ ? request_->status : PrefabTemplateLoadState::Failed;
    }

    /** @copydoc PrefabTemplateLoadHandle::RequestCancel */
    void PrefabTemplateLoadHandle::RequestCancel() const noexcept {
        if (request_)
            request_->cancellation.RequestCancellation();
    }

    /** @copydoc PrefabTemplateProvider::PrefabTemplateProvider */
    PrefabTemplateProvider::PrefabTemplateProvider(Assets::AssetRegistry &registry, Assets::AssetLoadService &loads,
                                                   Runtime::RuntimeSceneService &scenes, PrefabLimitProfile profile,
                                                   PrefabTemplateProviderLimits limits, PrefabMetricHandles metrics)
        : state_(std::make_unique<Detail::PrefabTemplateProviderState>(registry, loads, scenes, std::move(profile), limits,
                                                                       std::move(metrics))) {}

    /** @copydoc PrefabTemplateProvider::LimitProfile */
    const PrefabLimitProfile &PrefabTemplateProvider::LimitProfile() const noexcept {
        return state_->profile;
    }

    /** @copydoc PrefabTemplateProvider::Metrics */
    const PrefabMetricHandles &PrefabTemplateProvider::Metrics() const noexcept {
        return state_->metrics;
    }

    /** @copydoc PrefabTemplateProvider::~PrefabTemplateProvider */
    PrefabTemplateProvider::~PrefabTemplateProvider() {
        Shutdown();
    }

    /** @copydoc PrefabTemplateProvider::LoadAsync */
    Result<PrefabTemplateLoadHandle> PrefabTemplateProvider::LoadAsync(PrefabTemplateLoadRequest input,
                                                                       const CancellationToken &cancellation) {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return Result<PrefabTemplateLoadHandle>::Failure(owner.ErrorValue());
        const auto snapshot = state_->registry.Snapshot();
        const auto scene = state_->scenes.ActiveScene();
        if (!scene)
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::SceneUnavailable));
        if (auto admission = state_->CheckAdmission(snapshot, scene->RuntimeId()); admission.HasError())
            return Result<PrefabTemplateLoadHandle>::Failure(admission.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::Cancelled));
        if (!input.asset.IsValid() || !input.target.IsValid() || input.artifactDigest == Sha256Digest{})
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::IdentityInvalid));
        const auto *record = snapshot.Find(input.asset);
        if (!record)
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::AssetNotFound));
        if (record->type.Value() != "core.prefab")
            return Result<PrefabTemplateLoadHandle>::Failure(MakeError(PrefabErrors::DependencyTypeMismatch));
        if (state_->requests.size() >= state_->limits.maximumOutstanding)
            return Result<PrefabTemplateLoadHandle>::Failure(
                MakeError(PrefabErrors::AdmissionRejected, "Prefab request capacity exhausted."));

        auto request =
            std::make_shared<Detail::PrefabTemplateRequest>(std::move(input), snapshot, scene->RuntimeId(), cancellation, state_->identity);
        for (const auto &entry : state_->cache) {
            if (const auto allocation = entry.resident; allocation && allocation->value.Data().assetId == request->root.asset &&
                                                        allocation->registry.Revision() == snapshot.Revision() &&
                                                        allocation->root.Digest() == request->root.artifactDigest &&
                                                        allocation->target == request->root.target) {
                request->result = allocation;
                request->status = PrefabTemplateLoadState::Succeeded;
                return Result<PrefabTemplateLoadHandle>::Success(PrefabTemplateLoadHandle(std::move(request)));
            }
        }
        // Publication precedes submission; a vector allocation failure cannot abandon accepted worker work.
        state_->requests.push_back(request);
        return Result<PrefabTemplateLoadHandle>::Success(PrefabTemplateLoadHandle(std::move(request)));
    }

    /** @copydoc PrefabTemplateProvider::Advance */
    Result<void> PrefabTemplateProvider::Advance() {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return owner;
        return state_->Pump();
    }

    /** @copydoc PrefabTemplateProvider::TakeResult */
    Result<PrefabTemplateLease> PrefabTemplateProvider::TakeResult(const PrefabTemplateLoadHandle &handle) const {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return Result<PrefabTemplateLease>::Failure(owner.ErrorValue());
        const auto &request = handle.request_;
        if (!request || request->identity != state_->identity || request->consumed)
            return Result<PrefabTemplateLease>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        if (request->error)
            return Result<PrefabTemplateLease>::Failure(*request->error);
        if (request->status != PrefabTemplateLoadState::Succeeded)
            return Result<PrefabTemplateLease>::Failure(MakeError(PrefabErrors::AssetNotLoaded));
        if (request->cancellation.Token().IsCancellationRequested())
            return Result<PrefabTemplateLease>::Failure(MakeError(PrefabErrors::Cancelled));
        PrefabTemplateLease lease(request->result, request->scene);
        if (auto admission = ValidateAdmission(lease); admission.HasError())
            return Result<PrefabTemplateLease>::Failure(admission.ErrorValue());
        request->consumed = true;
        request->result.reset();
        return Result<PrefabTemplateLease>::Success(std::move(lease));
    }

    /** @copydoc PrefabTemplateProvider::ValidateAdmission */
    Result<void> PrefabTemplateProvider::ValidateAdmission(const PrefabTemplateLease &lease) const {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return owner;
        if (!lease.allocation_ || lease.allocation_->identity != state_->identity)
            return Result<void>::Failure(MakeError(PrefabErrors::AdmissionRejected));
        return state_->CheckAdmission(lease.allocation_->registry, lease.scene_);
    }

    /** @copydoc PrefabTemplateProvider::UsesSceneService */
    bool PrefabTemplateProvider::UsesSceneService(const Runtime::RuntimeSceneService &scenes) const noexcept {
        return &state_->scenes == &scenes;
    }

    /** @copydoc PrefabTemplateProvider::QueuePreparedGroup */
    Result<std::vector<Runtime::DeferredEntity>> PrefabTemplateProvider::QueuePreparedGroup(
        const PrefabTemplateLease &lease, std::vector<Runtime::RuntimeComponentSet> components, const CancellationToken &cancellation,
        PrefabPreparedGroupOptions options) {
        using Tokens = std::vector<Runtime::DeferredEntity>;
        if (auto admission = ValidateAdmission(lease); admission.HasError())
            return Result<Tokens>::Failure(admission.ErrorValue());
        if (cancellation.IsCancellationRequested() || options.scopeCancellation.IsCancellationRequested())
            return Result<Tokens>::Failure(MakeError(PrefabErrors::Cancelled));
        if (options.spawnLineage.size() > state_->profile.Policy().maximumRuntimeSpawnDepth)
            return Result<Tokens>::Failure(MakeError(PrefabErrors::SpawnDepthExceeded));
        const auto &entities = lease.Template()->Data().entities;
        if (options.rootPlacement && options.rootPlacement->TryToMatrix().HasError())
            return Result<Tokens>::Failure(MakeError(PrefabErrors::InvalidPlacement));
        if (options.parent) {
            const auto active = state_->scenes.ActiveScene();
            if (!active || active->Get(*options.parent).HasError())
                return Result<Tokens>::Failure(MakeError(PrefabErrors::InvalidParent));
        }
        if (components.size() != entities.size() ||
            (!options.physicsReferences.empty() && options.physicsReferences.size() != entities.size()) ||
            (!options.members.empty() && options.members.size() != entities.size()) ||
            (!options.references.empty() && options.references.size() != entities.size()) ||
            (!options.primitiveMeshes.empty() && options.primitiveMeshes.size() != entities.size()))
            return Result<Tokens>::Failure(MakeError(PrefabErrors::ComponentAllocationFailed));
        if (auto projected = ValidatePreparedInterfaces(lease.Template()->Data(), options); projected.HasError())
            return Result<Tokens>::Failure(projected.ErrorValue());
        auto entries = MakePreparedEntries(entities, std::move(components), options);
        std::vector<Runtime::RuntimeGroupAssetLease> resources;
        resources.reserve(lease.Dependencies().size() + 1);
        resources.emplace_back(Assets::AssetDependency{lease.Template()->Data().assetId, Assets::AssetTypeId::Parse("core.prefab").Value()},
                               lease.Artifact());
        for (const auto &dependency : lease.Dependencies())
            resources.emplace_back(dependency.metadata, dependency.artifact);
        Runtime::SceneCommandBuffer commands;
        auto tokens = commands.CreateGroup(std::move(entries), std::move(resources),
                                           {lease.Scene(), lease.RegistryRevision(), cancellation, state_->retirement.Token(),
                                            options.scopeCancellation});
        if (tokens.HasError())
            return tokens;
        if (options.receipt) {
            auto queued = state_->scenes.QueueTrackedStructuralCommands(std::move(commands));
            if (queued.HasError())
                return Result<Tokens>::Failure(queued.ErrorValue());
            *options.receipt = std::move(queued).Value();
        } else if (auto queued = state_->scenes.QueueStructuralCommands(std::move(commands)); queued.HasError())
            return Result<Tokens>::Failure(queued.ErrorValue());
        return tokens;
    }

    /** @copydoc PrefabTemplateProvider::Evict */
    void PrefabTemplateProvider::Evict(Assets::AssetId asset) noexcept {
        for (auto &entry : state_->cache) {
            if (entry.resident && entry.resident->value.Data().assetId == asset) {
                state_->EvictAllocation(*entry.resident);
                entry.resident.reset();
            }
        }
    }

    /** @copydoc PrefabTemplateProvider::PayloadSnapshot */
    Assets::AssetPayloadCacheSnapshot PrefabTemplateProvider::PayloadSnapshot() const noexcept {
        return state_->payloads ? state_->payloads->Snapshot() : Assets::AssetPayloadCacheSnapshot{};
    }

    /** @copydoc PrefabTemplateProvider::Startup */
    Result<void> PrefabTemplateProvider::Startup(const CancellationToken &cancellation) {
        if (auto owner = state_->CheckOwner(); owner.HasError())
            return owner;
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
        if (!state_->payloads || state_->closed)
            return Result<void>::Failure(MakeError(PrefabErrors::LimitProfileInvalid));
        return Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProvider::OnPhase */
    Result<void> PrefabTemplateProvider::OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &) {
        return phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges ? Advance() : Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProvider::OnFixedUpdate */
    Result<void> PrefabTemplateProvider::OnFixedUpdate(const Runtime::FixedStepContext &) {
        return Result<void>::Success();
    }

    /** @copydoc PrefabTemplateProvider::Shutdown */
    void PrefabTemplateProvider::Shutdown() noexcept {
        state_->closed = true;
        state_->retirement.RequestCancellation();
        for (const auto &request : state_->requests) {
            request->cancellation.RequestCancellation();
            request->status = PrefabTemplateLoadState::Cancelled;
            state_->ReleasePins(*request);
        }
        state_->requests.clear();
        state_->DropResidency();
        if (state_->payloads)
            state_->payloads->Shutdown();
    }
}  // namespace Horo::Prefab
