#include "ProjectSceneNavigationComposition.h"

#include "Horo/Application/NavigationProjectSettings.h"
#include "Horo/Assets/NavMeshAssetType.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationDefinitionSerialization.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>

namespace Horo::Application::Internal {
    namespace Nav = Navigation;

    namespace {
        /** @brief The currently qualified native provider admits one authored surface/profile without dropping content. */
        [[nodiscard]] Result<const Runtime::NavigationSurfaceComponent *> SelectedSurface(const Runtime::RuntimeSceneDefinition &scene) {
            using Surface = Result<const Runtime::NavigationSurfaceComponent *>;
            const Runtime::NavigationSurfaceComponent *selected = nullptr;
            for (const auto &entity : scene.Entities()) {
                if (entity.components.navigationLink && entity.components.navigationLink->enabled)
                    return Surface::Failure(MakeError(Nav::NavigationErrors::OperationUnsupported,
                                                      "The selected Detour provider cannot activate authored off-mesh links."));
                const auto &surface = entity.components.navigationSurface;
                if (!surface || !surface->enabled)
                    continue;
                if (selected || surface->profiles.size() != 1)
                    return Surface::Failure(MakeError(Nav::NavigationErrors::OperationUnsupported,
                                                      "The selected Detour provider requires one enabled surface and one profile."));
                selected = std::addressof(*surface);
            }
            if (!selected)
                return Surface::Failure(MakeError(Nav::NavigationErrors::SceneSurfaceMissing));
            return Surface::Success(selected);
        }

        /** @brief Reads one registered definition through canonical project file and source codecs. */
        [[nodiscard]] Result<Nav::NavigationDefinition> ReadCatalog(const Assets::AssetId id,
                                                                    const ProjectSceneNavigationSource &workspace) {
            using Catalog = Result<Nav::NavigationDefinition>;
            const auto *record = workspace.registry.Find(id);
            if (!record || record->type.Value() != Assets::NavMeshAssetTypeName)
                return Catalog::Failure(MakeError(Nav::NavigationErrors::NoNavigationData));
            const auto source = workspace.files.SourceText(record->sourcePath.String(), workspace.context, {});
            if (source.HasError())
                return Catalog::Failure(source.ErrorValue());
            const std::array support{Nav::NavigationDefinitionRecordSupport()};
            const auto records =
                Nav::DeserializeNavigationSourceRecords(std::as_bytes(std::span{*source.Value().text}), {.supportedRecords = support});
            if (records.HasError())
                return Catalog::Failure(records.ErrorValue());
            if (records.Value().Records().size() != 1 || !records.Value().GeneratedPayloads().empty() ||
                records.Value().HasQuarantinedGeneratedPayloads())
                return Catalog::Failure(MakeError(Nav::NavigationErrors::NavMeshArtifactInvalid));
            return Nav::DecodeNavigationDefinitionRecord(records.Value().Records().front());
        }

        /** @brief Uses the one immutable catalog to admit resident agents and later prefab additions. */
        [[nodiscard]] Result<void> AdmitAgent(const ProjectSceneNavigationSelection &selection,
                                              const Nav::NavigationAgentDescriptor &agent) {
            if (agent.profile != selection.profile)
                return Result<void>::Failure(
                    MakeError(Nav::NavigationErrors::AgentDescriptorInvalid, "Agent profile has no admitted Detour surface."));
            const auto filter = selection.catalog->Registry().ResolveFilter(agent.filter);
            if (filter.HasError())
                return Result<void>::Failure(filter.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Materializes only the selected exact decoded partition through the production native factory. */
        [[nodiscard]] Result<Nav::NavigationPreparedAssetBackend> PrepareBackend(const ProjectSceneNavigationSelection &selection,
                                                                                 const Nav::NavigationWorldActivationDescriptor &world,
                                                                                 std::span<const Nav::NavigationLoadedSurface> surfaces,
                                                                                 const std::size_t budget) {
            using Prepared = Result<Nav::NavigationPreparedAssetBackend>;
            if (surfaces.size() != 1 || !surfaces.front().partition)
                return Prepared::Failure(MakeError(Nav::NavigationErrors::OperationUnsupported));
            const auto &partition = *surfaces.front().partition;
            if (partition.tiles.size() > selection.policy.Capacities().maximumResidentTiles)
                return Prepared::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
            const auto &geometry = partition.descriptor.geometry;
            const float horizontal = std::max(geometry.radiusMeters, geometry.cellSizeMeters);
            const auto providerBudget = std::min(budget, Nav::RecastDetourProviderHardLimits::OwnedBytes);
            auto backend = Nav::PrepareRecastDetourCookedNavigationQueryBackend(
                {.world = world.world,
                 .topology = world.topology,
                 .surface = selection.surface,
                 .profile = selection.profile,
                 .definition = *selection.catalog,
                 .tiles = partition.tiles,
                 .nearestPointHalfExtents = {horizontal, geometry.heightMeters, horizontal},
                 .queryLimits = selection.policy.MaximumQuery().limits,
                 .maximumConcurrentQueries = selection.policy.Capacities().maximumConcurrentQueries,
                 .maximumOwnedBytes = providerBudget,
                 .capabilityRevision = selection.policy.Revision().Value()});
            if (backend.HasError())
                return Prepared::Failure(backend.ErrorValue());
            auto prepared = std::move(backend).Value();
            return Prepared::Success({std::move(prepared.backend), prepared.reservedBytes});
        }

        Result<std::unique_ptr<Assets::AssetPayloadCache>> CreateNavigationTileCache(const Nav::NavigationProjectProfile &policy) {
            using Cache = Result<std::unique_ptr<Assets::AssetPayloadCache>>;
            const auto &capacity = policy.Capacities();
            if (capacity.maximumConcurrentQueries > Nav::RecastDetourProviderHardLimits::ConcurrentQueries ||
                capacity.maximumResidentMemoryBytes > std::numeric_limits<std::size_t>::max() ||
                capacity.maximumBytesPerResidentTile >= capacity.maximumResidentMemoryBytes)
                return Cache::Failure(MakeError(Nav::NavigationErrors::CapacityExceeded));
            return Assets::AssetPayloadCache::Create(capacity.maximumResidentTiles,
                                                     static_cast<std::size_t>(capacity.maximumBytesPerResidentTile));
        }

        /** @brief Bounds queued work from the same project capacities as provider admission. */
        [[nodiscard]] Nav::NavigationPathBatchLimits CoordinatorLimits(const Nav::NavigationProjectProfile &profile) noexcept {
            const auto &capacity = profile.Capacities();
            const auto slots = std::bit_ceil(std::max(2U, capacity.maximumConcurrentQueries));
            return {.requestSlots = slots,
                    .maximumJobs = capacity.maximumConcurrentQueries,
                    .requestsPerJob = 1,
                    .requestsPerTick = capacity.maximumConcurrentQueries,
                    .requestsPerCaller = 1,
                    .maximumPendingPerCaller = 1,
                    .nodeExpansionsPerTick = capacity.maximumWorkUnitsPerTick,
                    .nodeExpansionsPerCaller = profile.MaximumQuery().limits.maximumNodeExpansions,
                    .selectionProbesPerTick = slots * 2,
                    .maximumOwnedBytes = static_cast<std::size_t>(capacity.maximumResidentMemoryBytes)};
        }
    }  // namespace

    /** @copydoc CaptureProjectSceneNavigation */
    Result<std::shared_ptr<const ProjectSceneNavigationSelection>> CaptureProjectSceneNavigation(
        const ProjectSceneNavigationSource &workspace) {
        using Selection = Result<std::shared_ptr<const ProjectSceneNavigationSelection>>;
        const auto *selected = workspace.registry.Find(workspace.scene);
        if (!selected || selected->type.Value() != "core.scene" || !workspace.target.IsValid())
            return Selection::Failure(MakeError(Nav::NavigationErrors::NoNavigationData));
        const auto surface = SelectedSurface(workspace.definition);
        if (surface.HasError())
            return Selection::Failure(surface.ErrorValue());
        const auto source = workspace.files.SourceText(".horo/navigation.json", workspace.context, {.maximumFileBytes = 64U * 1024U});
        if (source.HasError())
            return Selection::Failure(source.ErrorValue());
        const auto settings = DecodeNavigationProjectSettings(*source.Value().text);
        if (settings.HasError())
            return Selection::Failure(settings.ErrorValue());
        const auto &input = settings.Value().Input();
        if (input.defaultProvider != "recast_detour" || input.definition != surface.Value()->definition)
            return Selection::Failure(MakeError(Nav::NavigationErrors::OperationUnsupported,
                                                "Selected Scene surface must use the explicitly configured Recast definition."));
        auto catalog = ReadCatalog(input.definition, workspace);
        if (catalog.HasError())
            return Selection::Failure(catalog.ErrorValue());
        const auto scope = Nav::NavigationBakeScopeId::ForScene(workspace.scene.Bytes());
        if (scope.HasError())
            return Selection::Failure(scope.ErrorValue());
        return Selection::Success(std::make_shared<const ProjectSceneNavigationSelection>(
            ProjectSceneNavigationSelection{{surface.Value()->id, scope.Value()},
                                            surface.Value()->definition,
                                            surface.Value()->profiles.front(),
                                            workspace.target,
                                            settings.Value().RuntimeProfile(),
                                            std::make_shared<const Nav::NavigationDefinition>(std::move(catalog).Value()),
                                            workspace.jobs,
                                            surface.Value()->generation}));
    }

    /** @copydoc PrepareProjectNavigationPreviewBackend */
    Result<std::unique_ptr<Nav::INavigationQueryBackend>> PrepareProjectNavigationPreviewBackend(
        const ProjectSceneNavigationSelection &selection, const Nav::NavMeshAssetSource &assets,
        const Nav::NavigationWorldActivationDescriptor &identity, const CancellationToken &cancellation) {
        using Backend = Result<std::unique_ptr<Nav::INavigationQueryBackend>>;
        const auto &capacity = selection.policy.Capacities();
        const auto tileLimit = static_cast<std::size_t>(capacity.maximumBytesPerResidentTile);
        auto cache = CreateNavigationTileCache(selection.policy);
        if (cache.HasError())
            return Backend::Failure(cache.ErrorValue());
        const auto loaded = Nav::LoadNavMeshAsset(assets, selection.definition, selection.target, *cache.Value(), cancellation,
                                                  {.limits = {.maximumBytesPerTile = tileLimit}, .scope = selection.surface.scope});
        if (loaded.HasError())
            return Backend::Failure(loaded.ErrorValue());
        const auto &provenance = loaded.Value().contentProvenance;
        if (!provenance || !provenance->projectProfile || !provenance->projectProfile->MatchesAuthority(selection.policy))
            return Backend::Failure(MakeError(Nav::NavigationErrors::ProjectProfileStale));
        const auto &partitions = loaded.Value().partitions;
        const auto partition = std::ranges::find_if(partitions, [&](const auto &candidate) {
            return candidate.surface == selection.surface.surface && candidate.profile == selection.profile;
        });
        if (partition == partitions.end())
            return Backend::Failure(MakeError(Nav::NavigationErrors::StaleSnapshot));
        const std::array surfaces{Nav::NavigationLoadedSurface{selection.definition, 0, std::to_address(partition)}};
        auto prepared =
            PrepareBackend(selection, identity, surfaces, static_cast<std::size_t>(capacity.maximumResidentMemoryBytes) - tileLimit);
        if (prepared.HasError())
            return Backend::Failure(prepared.ErrorValue());
        auto result = std::move(prepared).Value();
        return Backend::Success(std::move(result.backend));
    }

    /** @copydoc CreateProjectSceneNavigationWorld */
    Result<std::unique_ptr<ProjectSceneNavigationWorld>> CreateProjectSceneNavigationWorld(const ProjectSceneNavigationSelection &selection,
                                                                                           const Nav::NavigationPathExecution execution) {
        using World = Result<std::unique_ptr<ProjectSceneNavigationWorld>>;
        const auto &capacity = selection.policy.Capacities();
        auto cache = CreateNavigationTileCache(selection.policy);
        if (cache.HasError())
            return World::Failure(cache.ErrorValue());
        auto agents = Nav::NavigationAgentRegistry::Create({capacity.maximumAgents});
        if (agents.HasError())
            return World::Failure(agents.ErrorValue());
        auto limits = CoordinatorLimits(selection.policy);
        limits.execution = execution;
        auto coordinator = Nav::NavigationCoordinator::Create(selection.jobs, limits);
        if (coordinator.HasError())
            return World::Failure(coordinator.ErrorValue());
        return World::Success(std::make_unique<ProjectSceneNavigationWorld>(
            ProjectSceneNavigationWorld{std::move(cache).Value(), std::move(agents).Value(), std::move(coordinator).Value(),
                                        limits.maximumPendingPerCaller}));
    }

    /** @copydoc RegisterProjectSceneNavigation */
    Result<void> RegisterProjectSceneNavigation(Runtime::RuntimeSceneService &scene, ProjectSceneNavigationWorld &world,
                                                std::shared_ptr<const ProjectSceneNavigationSelection> selection, bool streamCoverage) {
        const auto &capacity = selection->policy.Capacities();
        const auto tileLimit = static_cast<std::size_t>(capacity.maximumBytesPerResidentTile);
        const Nav::NavigationAssetSceneLimits limits{.maximumReservedProviderBytes =
                                                         static_cast<std::size_t>(capacity.maximumResidentMemoryBytes) - tileLimit,
                                                     .assets = {.maximumBytesPerTile = tileLimit}};
        auto assets =
            std::make_unique<Nav::NavigationAssetSceneActivationParticipant>(*world.cache, selection->target,
                                                                             [selection](const auto &descriptor, const auto surfaces,
                                                                                         const auto budget) {
            return PrepareBackend(*selection, descriptor, surfaces, budget);
        },
                                                                             limits, std::nullopt,
                                                                             Nav::NavigationAssetSceneSelection{selection->surface.scope,
                                                                                                                selection->policy,
                                                                                                                streamCoverage});
        auto *assetOwner = assets.get();
        const auto admission = [selection](const Nav::NavigationAgentDescriptor &agent) {
            return AdmitAgent(*selection, agent);
        };
        auto agents = std::make_unique<Nav::NavigationSceneActivationParticipant>(world.agents, admission);
        auto groups = agents->MakeStructuralParticipant([assetOwner] {
            return assetOwner->ActiveWorld();
        }, world.coordinator, admission);
        if (auto added = scene.AddActivationParticipant(std::move(assets)); added.HasError())
            return added;
        if (auto added = scene.AddActivationParticipant(std::move(agents)); added.HasError())
            return added;
        if (auto added = scene.AddStructuralParticipant(std::move(groups)); added.HasError())
            return added;
        world.assets = assetOwner;
        world.streamCoverage = streamCoverage;
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
