#include "HeadlessWorldMount.h"

#include "Horo/Assets/AssetProvider.h"
#include "Horo/WorldStreaming/StreamingCellInspection.h"
#include "Horo/WorldStreaming/StreamingCellState.h"
#include "Horo/WorldStreaming/StreamingSourcePlanner.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "Horo/WorldStreaming/WorldDependencyArtifact.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace Horo::Application::Internal {
    namespace WS = WorldStreaming;

    namespace {
        constexpr std::uint32_t MaximumCells = 4096;
        constexpr std::size_t MaximumBytes = 256U * 1024U * 1024U;
        constexpr std::size_t MaximumCellBytes = 32U * 1024U * 1024U;
        constexpr Assets::AssetCookLimits CookLimits{MaximumBytes, 40U * 1024U * 1024U, MaximumCells + 2U, 1};
        constexpr WS::WorldIndexArtifactLimits IndexLimits{8U * 1024U * 1024U,
                                                           65536,
                                                           1024,
                                                           {256, MaximumCells, 65536},
                                                           {MaximumCells, MaximumCells, 65536, MaximumCellBytes, MaximumCellBytes}};
        constexpr WS::WorldDependencyArtifactLimits
            DependencyLimits{32U * 1024U * 1024U, 64U * 1024U * 1024U, 65536, MaximumCells, 65536, 65536, 65536, 65536, 65536, 1'048'576};
        constexpr WS::StreamingCellInspectionLimits CellLimits{32, MaximumCellBytes, MaximumCellBytes};

        template <typename Id> [[nodiscard]] Id Identity(const std::uint64_t value) {
            return Id::Create(value).Value();
        }

        /** @brief Reads only members of the selected immutable inventory, preserving exact envelope type and target. */
        Result<Assets::AssetCookArtifact> ReadArtifact(const Assets::AssetCookGenerationContents &contents,
                                                       const Assets::AssetCookTargetId &target, const Assets::AssetId id,
                                                       const std::string_view type) {
            const auto entry = std::ranges::lower_bound(contents.entries, id, {}, &Assets::AssetCookManifestEntry::assetId);
            if (entry == contents.entries.end() || entry->assetId != id || entry->assetType.Value() != type)
                return Result<Assets::AssetCookArtifact>::Failure(
                    MakeError(WS::WorldArtifactErrors::Invalid,
                              "The selected World generation is missing an exact generated artifact role."));
            auto artifact =
                Assets::DecodeCookedArtifact(contents.artifacts[static_cast<std::size_t>(entry - contents.entries.begin())], CookLimits);
            if (artifact.HasError())
                return artifact;
            if (artifact.Value().id != id || artifact.Value().target != target || artifact.Value().type != entry->assetType)
                return Result<Assets::AssetCookArtifact>::Failure(
                    MakeError(WS::WorldArtifactErrors::Invalid, "The World artifact envelope does not match its selected manifest entry."));
            return artifact;
        }

        /** @brief Prepares the actual source-planning owner for one standalone mounted registry. */
        Result<std::unique_ptr<WS::StreamingSourcePlanner>> MakePlanner(const WS::WorldPartitionRegistrySnapshot &partition) {
            const auto priority = WS::StreamingPriorityPolicy::Create(
                {.id = Identity<WS::StreamingPriorityPolicyId>(1), .revision = Identity<WS::StreamingPriorityPolicyRevision>(1)});
            const auto stability =
                WS::StreamingCellStabilityPolicy::Create({.id = Identity<WS::StreamingCellStabilityPolicyId>(1),
                                                          .revision = Identity<WS::StreamingCellStabilityPolicyRevision>(1),
                                                          .maximumTrackedCells = MaximumCells});
            const auto prefetch = WS::StreamingPrefetchPolicy::Create(
                {.id = Identity<WS::StreamingPrefetchPolicyId>(1), .revision = Identity<WS::StreamingPrefetchPolicyRevision>(1)});
            using Output = Result<std::unique_ptr<WS::StreamingSourcePlanner>>;
            if (priority.HasError())
                return Output::Failure(priority.ErrorValue());
            if (stability.HasError())
                return Output::Failure(stability.ErrorValue());
            if (prefetch.HasError())
                return Output::Failure(prefetch.ErrorValue());
            std::vector<WS::StreamingLayerId> enabled;
            enabled.reserve(partition.Layers().size());
            for (const auto &layer : partition.Layers())
                if ((static_cast<std::uint32_t>(layer.flags) & (static_cast<std::uint32_t>(WS::WorldLayerFlags::ClientOnly) |
                                                                static_cast<std::uint32_t>(WS::WorldLayerFlags::ServerOnly))) == 0)
                    enabled.push_back(layer.id);
            return WS::StreamingSourcePlanner::Create({.limits = {1024, MaximumCells, 16384, 32, 128, 256, 256, 60000},
                                                       .shapeSupport = {true, true, true, true},
                                                       .priority = priority.Value(),
                                                       .stability = stability.Value(),
                                                       .prefetch = prefetch.Value()},
                                                      partition, {1, {1}, 1, 0, enabled});
        }

        /** @brief Verifies actual cell bytes against every corresponding index row before publishing a provider. */
        Result<void> LoadCells(Assets::MemoryAssetProvider &provider, const Assets::AssetCookGenerationContents &contents,
                               const Assets::AssetCookTargetId &target, const WS::DecodedWorldIndex &index,
                               const CancellationToken &cancellation) {
            for (const auto &cell : index.descriptor.Cells()) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));
                const auto expected = std::ranges::lower_bound(index.cells, cell.id, WS::StreamingCellCanonicalLess{},
                                                               &WS::CookedWorldCellManifestEntry::cell);
                if (expected == index.cells.end() || expected->cell != cell.id)
                    return Result<void>::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
                auto artifact = ReadArtifact(contents, target, cell.package.chunkAsset, "world.cell");
                if (artifact.HasError())
                    return Result<void>::Failure(artifact.ErrorValue());
                auto inspected = WS::InspectStreamingCellArtifact(std::as_bytes(std::span{artifact.Value().payload}),
                                                                  expected->artifactHash, CellLimits, cancellation);
                if (inspected.HasError())
                    return Result<void>::Failure(inspected.ErrorValue());
                const auto actual = inspected.Value().Header();
                if (actual.cell != cell.id || actual.compressedSize != expected->compressedSize ||
                    actual.uncompressedSize != expected->uncompressedSize || actual.payloadCrc32 != expected->payloadCrc32)
                    return Result<void>::Failure(
                        MakeError(WS::WorldArtifactErrors::Invalid,
                                  "A World cell does not match the exact identity and integrity advertised by its index."));
                provider.Insert(cell.package.chunkAsset, std::move(artifact).Value().payload);
            }
            return Result<void>::Success();
        }
    }  // namespace

    struct HeadlessWorldMount::State final {
        Assets::MemoryAssetProvider cells;
        std::unique_ptr<WS::WorldPartitionRegistry> registry;
        std::unique_ptr<WS::StreamingSourcePlanner> planner;
        std::unique_ptr<WS::StreamingCellStateLedger> residency;
        std::optional<WS::DecodedWorldDependencies> dependencies;
        std::optional<WS::WorldStreamingRuntimeComposition> composition;
    };

    /** @copydoc HeadlessWorldMount::Prepare */
    Result<std::unique_ptr<HeadlessWorldMount>> HeadlessWorldMount::Prepare(const DurableFileSystem &files,
                                                                            const HeadlessWorldSelection &selection,
                                                                            const std::uint64_t generation,
                                                                            Runtime::RuntimeSceneService &scene,
                                                                            const CancellationToken &cancellation) {
        using Output = Result<std::unique_ptr<HeadlessWorldMount>>;
        if (generation == 0 || !selection.targetRoot.is_absolute() || !selection.target.IsValid() || !selection.index.IsValid() ||
            !selection.dependencies.IsValid() || selection.index == selection.dependencies)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Invalid));
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));

        const auto published = Assets::ResolveCurrentCookGeneration(files, selection.targetRoot, CookLimits);
        if (published.HasError())
            return Output::Failure(published.ErrorValue());
        if (published.Value().target != selection.target || published.Value().manifestDigest != selection.manifest)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Stale, "The selected committed World manifest or target changed."));
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));
        const auto contents = Assets::ReadCookGenerationContents(files, published.Value(), MaximumBytes, CookLimits);
        if (contents.HasError())
            return Output::Failure(contents.ErrorValue());

        const auto indexArtifact = ReadArtifact(contents.Value(), selection.target, selection.index, "world.index");
        if (indexArtifact.HasError())
            return Output::Failure(indexArtifact.ErrorValue());
        auto index = WS::DecodeWorldIndexArtifact(std::as_bytes(std::span{indexArtifact.Value().payload}), IndexLimits, cancellation);
        if (index.HasError())
            return Output::Failure(index.ErrorValue());
        if (index.Value().coordinates != WS::WorldIndexCoordinateSystem::RightHandedYUp)
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Unsupported, "Standalone World mounting requires Horo world axes."));
        const auto bundle = ReadArtifact(contents.Value(), selection.target, selection.dependencies, "world.dependencies");
        if (bundle.HasError())
            return Output::Failure(bundle.ErrorValue());
        auto dependencies = WS::DecodeWorldDependencyArtifact(std::as_bytes(std::span{bundle.Value().payload}), index.Value(),
                                                              DependencyLimits, cancellation);
        if (dependencies.HasError())
            return Output::Failure(dependencies.ErrorValue());

        auto state = std::make_unique<State>();
        if (auto loaded = LoadCells(state->cells, contents.Value(), selection.target, index.Value(), cancellation); loaded.HasError())
            return Output::Failure(loaded.ErrorValue());
        state->dependencies.emplace(std::move(dependencies).Value());
        const WS::StreamingRuntimeOwnerToken owner{index.Value().descriptor.Partition(), Identity<WS::PartitionEpoch>(generation),
                                                   Identity<WS::StreamingRuntimeOwnerId>(generation)};
        auto registry =
            WS::WorldPartitionRegistry::Create(Identity<WS::WorldPartitionRegistryId>(generation), owner, {MaximumCells, MaximumCells});
        if (registry.HasError())
            return Output::Failure(registry.ErrorValue());
        state->registry = std::move(registry).Value();
        auto described = state->registry->Publish(std::move(index).Value().descriptor, Identity<WS::WorldPartitionRegistryRevision>(1));
        if (described.HasError())
            return Output::Failure(described.ErrorValue());
        auto partition = state->registry->Snapshot();
        if (partition.HasError())
            return Output::Failure(partition.ErrorValue());
        auto planner = MakePlanner(partition.Value());
        if (planner.HasError())
            return Output::Failure(planner.ErrorValue());
        state->planner = std::move(planner).Value();
        auto residency = WS::StreamingCellStateLedger::Create({owner, WS::StreamingCellStateLedgerConfig::MaximumTrackedAttempts});
        if (residency.HasError())
            return Output::Failure(residency.ErrorValue());
        state->residency = std::make_unique<WS::StreamingCellStateLedger>(std::move(residency).Value());

        constexpr auto standalone = WS::WorldPartitionProjectProfile::Standalone;
        const WS::WorldStreamingRuntimeCompositionConfig
            config{.owner = owner,
                   .revision = Identity<WS::StreamingRuntimeCompositionRevision>(1),
                   .schedulerOwner = Identity<WS::StreamingSchedulerLedgerId>(generation),
                   .schedulerLimits = {.concurrentOperations = 4,
                                       .capacityUnits = 4,
                                       .concurrency = {standalone, Identity<WS::StreamingConcurrencyRevision>(1), 2, 2, 2}},
                   .maximumFeatureAdapters = 1};
        const auto binding = [](const WS::StreamingRuntimeServiceRole role, const void *service) {
            return WS::StreamingRuntimeServiceBinding{Identity<WS::StreamingRuntimeServiceId>(static_cast<std::uint64_t>(role) + 1),
                                                      Identity<WS::StreamingRuntimeServiceRevision>(1), role, service};
        };
        using enum WS::StreamingRuntimeServiceRole;
        const std::array services{binding(Planner, state->planner.get()), binding(AssetProvider, &state->cells),
                                  binding(SceneRuntime, &scene), binding(FeatureAdapter, state->residency.get())};
        auto origin = WS::OriginFrame::Create({Identity<WS::OriginFrameId>(generation), Identity<WS::OriginFrameRevision>(1),
                                               Identity<WS::OriginGeneration>(1)},
                                              selection.origin);
        if (origin.HasError())
            return Output::Failure(origin.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(WS::WorldArtifactErrors::Cancelled));
        auto composition =
            WS::WorldStreamingRuntimeComposition::CreateWithOrigin(config, services, *state->registry, std::move(origin).Value());
        if (composition.HasError())
            return Output::Failure(composition.ErrorValue());
        state->composition.emplace(std::move(composition).Value());
        return Output::Success(std::unique_ptr<HeadlessWorldMount>{new HeadlessWorldMount{std::move(state)}});
    }

    HeadlessWorldMount::HeadlessWorldMount(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc HeadlessWorldMount::Origin */
    const WS::OriginFrameOwner &HeadlessWorldMount::Origin() const noexcept {
        return *state_->composition->Origin();
    }

    /** @copydoc HeadlessWorldMount::~HeadlessWorldMount */
    HeadlessWorldMount::~HeadlessWorldMount() {
        static_cast<void>(state_->composition->BeginShutdown(state_->composition->Owner()));
        static_cast<void>(state_->residency->BeginShutdown(state_->composition->Owner()));
        static_cast<void>(state_->planner->Shutdown());
        state_->registry->Shutdown();
    }
}  // namespace Horo::Application::Internal
