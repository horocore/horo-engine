#include "WorldBakeInternal.h"

#include <algorithm>
#include <array>
#include <set>

namespace Horo::Application {
    namespace W = WorldStreaming;
    namespace S = Runtime;

    namespace {
        /** @brief Require all dependencies from one actual source page before cell publication. */
        Result<void> RequirePageAssets(S::SceneDefinitionBuilder &builder, const S::RuntimeSceneDefinition &scene) {
            for (const auto &dependency : scene.AssetDependencies())
                if (auto added = builder.RequireAsset(dependency); added.HasError())
                    return added;
            return Result<void>::Success();
        }

        /** @brief Builds one complete owned cell scene, retaining local references for canonical Scene validation. */
        Result<S::RuntimeSceneDefinition> CellScene(const WorldBakeDetail::PreparedWorld &prepared, const W::StreamingCellId cell,
                                                    const S::SceneCellPayloadIdentity &identity) {
            S::SceneDefinitionBuilder builder{identity.scene, identity.revision};
            std::set<std::size_t> includedPages;
            for (const auto &object : prepared.objects) {
                if (std::ranges::find(object.cells, cell) == object.cells.end())
                    continue;
                builder.Add(object.entity);
                if (includedPages.insert(object.sceneIndex).second) {
                    if (auto added = RequirePageAssets(builder, prepared.output.sourceScenes[object.sceneIndex]); added.HasError())
                        return Result<S::RuntimeSceneDefinition>::Failure(added.ErrorValue());
                }
                for (const auto &trigger : object.triggers)
                    if (auto added = builder.RequireTriggerVolume(trigger); added.HasError())
                        return Result<S::RuntimeSceneDefinition>::Failure(added.ErrorValue());
            }
            return std::move(builder).Build();
        }

        /** @brief Derives one nonzero logical Scene identity from the same persistent chunk identity on every cook. */
        S::SceneDefinitionId SceneId(const Assets::AssetId &chunk) {
            std::uint64_t value{};
            for (std::size_t index = 0; index < 8; ++index)
                value |= static_cast<std::uint64_t>(chunk.Bytes()[index]) << (8U * index);
            return {value == 0 ? 1 : value};
        }

        /** @brief Builds one complete Scene and releases its mutable preparation storage before starting another cell. */
        Result<std::shared_ptr<const S::RuntimeSceneCellPayload>> CookOneCell(const WorldBakeRequest &request,
                                                                              const W::WorldPartitionDescriptor &partition,
                                                                              const WorldBakeDetail::PreparedWorld &prepared,
                                                                              const S::SceneCellPayloadIdentity &identity,
                                                                              S::IncrementalSceneCellCook &cache, std::size_t &sourceBytes,
                                                                              const CancellationToken &cancellation) {
            const auto placed = std::ranges::find_if(prepared.objects, [&](const auto &object) {
                return std::ranges::find(object.cells, identity.cell) != object.cells.end();
            });
            const auto *origin = placed == prepared.objects.end() ? nullptr : &prepared.sources[placed->sceneIndex];
            if (origin && *origin) {
                if ((*origin)->absolutePath.size() > request.limits.validation.maximumSourceBytes - sourceBytes)
                    return Result<std::shared_ptr<const S::RuntimeSceneCellPayload>>::Failure(MakeError(WorldBakeErrors::Capacity));
                sourceBytes += (*origin)->absolutePath.size();
            }
            auto sceneResult = CellScene(prepared, identity.cell, identity);
            if (sceneResult.HasError())
                return Result<std::shared_ptr<const S::RuntimeSceneCellPayload>>::Failure(
                    WorldBakeDetail::AtSource(sceneResult.ErrorValue(), origin ? *origin : std::nullopt));
            auto scene = std::move(sceneResult).Value();
            const S::SceneCellCookInput input{{identity, scene.Entities(), scene.AssetDependencies(), request.project.components,
                                               request.project.behaviors, scene.TriggerVolumeEvidence()},
                                              identity};
            std::vector<S::SceneCellCookDependency> dependencies;
            for (const auto &dependency : request.project.dependencies)
                dependencies.emplace_back(dependency.asset.id, dependency.digest, dependency.revision, dependency.dependencies);
            const std::array<std::span<const std::byte>, 2> settings{std::as_bytes(std::span{request.project.settings.bytes}),
                                                                     std::as_bytes(std::span{request.project.fence.replication.bytes})};
            auto cooked = cache.Cook(partition, std::span{&input, 1}, dependencies, ComputeSha256Fragments(settings), cache.Revision(),
                                     request.limits.codec.semantic, cancellation);
            if (cooked.HasError())
                return Result<std::shared_ptr<const S::RuntimeSceneCellPayload>>::Failure(
                    WorldBakeDetail::AtSource(cooked.ErrorValue(), origin ? *origin : std::nullopt));
            return Result<std::shared_ptr<const S::RuntimeSceneCellPayload>>::Success(cooked.Value().cells.front().payload);
        }
    }  // namespace

    /** @brief Builds and cooks one bounded cell at a time; only admitted immutable payloads survive the next cell. */
    Result<WorldBakeDetail::CookedCells> WorldBakeDetail::CookCells(const WorldBakeRequest &request,
                                                                    const W::WorldPartitionDescriptor &partition,
                                                                    const PreparedWorld &prepared, const CancellationToken &cancellation) {
        CookedCells output;
        S::IncrementalSceneCellCook cache{request.limits.cache};
        std::set<S::SceneDefinitionId> identities;
        std::size_t sourceBytes{};
        std::size_t retainedBytes{};
        for (const auto &cell : partition.Cells()) {
            if (cancellation.IsCancellationRequested())
                return Result<CookedCells>::Failure(MakeError(WorldBakeErrors::Cancelled));
            const S::SceneCellPayloadIdentity identity{partition.Partition(),
                                                       cell.id,
                                                       SceneId(cell.package.chunkAsset),
                                                       {request.document->fence.revision.Value()}};
            if (!identities.insert(identity.scene).second)
                return Result<CookedCells>::Failure(MakeError(WorldBakeErrors::Conflict));
            auto cooked = CookOneCell(request, partition, prepared, identity, cache, sourceBytes, cancellation);
            if (cooked.HasError())
                return Result<CookedCells>::Failure(cooked.ErrorValue());
            auto payload = std::move(cooked).Value();
            if (payload->RetainedBytes() > request.limits.cache.maximumRetainedBytes - retainedBytes)
                return Result<CookedCells>::Failure(MakeError(WorldBakeErrors::Capacity));
            retainedBytes += payload->RetainedBytes();
            const auto placed = std::ranges::find_if(prepared.objects, [&](const auto &object) {
                return std::ranges::find(object.cells, cell.id) != object.cells.end();
            });
            output.expected.emplace_back(identity, placed == prepared.objects.end() ? std::nullopt : prepared.sources[placed->sceneIndex]);
            output.payloads.push_back(payload);
        }
        return Result<CookedCells>::Success(std::move(output));
    }
}  // namespace Horo::Application
