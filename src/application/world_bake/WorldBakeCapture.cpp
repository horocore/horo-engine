#include "WorldBakeInternal.h"

#include <algorithm>
#include <set>

namespace Horo::Application::WorldBakeDetail {
    namespace {
        /** @brief Detached collision and placement budget for one synchronous conversion. */
        struct ProjectionBudget final {
            std::size_t assignments{};
            std::set<std::uint64_t> objects;
            std::set<W::StreamingCellId, W::StreamingCellCanonicalLess> cells;
        };

        /** @brief Adds one exact page-qualified entity and its authored placement to a detached candidate. */
        Result<void> AppendEntity(const WorldBakeRequest &request, const Editor::PartitionedWorldPage &page,
                                  const Runtime::RuntimeSceneDefinition &scene, const Runtime::RuntimeEntityDefinition &entity,
                                  const std::size_t sceneIndex, PreparedWorld &prepared, ProjectionBudget &budget) {
            const auto source = std::ranges::find(page.objects, entity.object.value, [](const auto &value) {
                return value.descriptor.address.object;
            });
            if (source == page.objects.end())
                return Result<void>::Failure(MakeError(WorldBakeErrors::Conflict));
            if (prepared.objects.size() >= request.limits.maximumObjects)
                return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
            auto placement = Placement(*source, request);
            if (placement.HasError())
                return Result<void>::Failure(placement.ErrorValue());
            auto placed = std::move(placement).Value();
            if (placed.size() > request.limits.maximumAssignments - budget.assignments)
                return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
            budget.assignments += placed.size();
            auto projected = entity;
            Remap(projected, page.head.sourceAsset);
            if (!budget.objects.insert(projected.object.value).second)
                return Result<void>::Failure(MakeError(WorldBakeErrors::Conflict, "Stable world object identity collision."));
            PlacedEntity output{source->descriptor.address, std::move(projected), std::move(placed), {}, sceneIndex};
            for (const auto &evidence : scene.TriggerVolumeEvidence())
                if (evidence.object == entity.object) {
                    auto remapped = evidence;
                    remapped.object = output.entity.object;
                    output.triggers.push_back(std::move(remapped));
                }
            for (const auto cell : output.cells) {
                budget.cells.insert(cell);
                if (budget.cells.size() > request.limits.maximumCells)
                    return Result<void>::Failure(MakeError(WorldBakeErrors::Capacity));
            }
            prepared.objects.push_back(std::move(output));
            return Result<void>::Success();
        }

        /** @brief Verifies required provider bytes once per converted page, before entity projection allocates output. */
        Result<void> CheckDependencies(const Runtime::RuntimeSceneDefinition &scene, const WorldBakeRequest &request) {
            for (const auto &dependency : scene.AssetDependencies())
                if (std::ranges::find(request.project.dependencies, dependency, &WorldBakeDependency::asset) ==
                    request.project.dependencies.end())
                    return Result<void>::Failure(MakeError(WorldBakeErrors::Dependency, dependency.id.ToString()));
            return Result<void>::Success();
        }

        /** @brief Converts and projects one complete source page before reading another publication. */
        Result<void> AppendPage(const WorldBakeRequest &request, const Editor::PartitionedWorldPage &page,
                                const CancellationToken &cancellation, PreparedWorld &prepared, ProjectionBudget &budget,
                                std::size_t &sourceBytes) {
            const auto fail = [&](Error error) {
                return Result<void>::Failure(AtSource(std::move(error), SourceFor(request, page.head.sourceAsset)));
            };
            if (cancellation.IsCancellationRequested())
                return fail(MakeError(WorldBakeErrors::Cancelled));
            if (page.objects.size() > request.limits.maximumObjects - prepared.objects.size())
                return fail(MakeError(WorldBakeErrors::Capacity));
            auto captured = CapturePage(request, page, cancellation, sourceBytes, prepared.output);
            if (captured.HasError())
                return fail(captured.ErrorValue());
            if (auto verified = CheckDependencies(captured.Value(), request); verified.HasError())
                return fail(verified.ErrorValue());
            const auto sceneIndex = prepared.output.sourceScenes.size();
            prepared.sources.push_back(SourceFor(request, page.head.sourceAsset));
            prepared.output.sourceScenes.push_back(std::move(captured).Value());
            const auto &scene = prepared.output.sourceScenes.back();
            for (const auto &entity : scene.Entities()) {
                if (cancellation.IsCancellationRequested())
                    return fail(MakeError(WorldBakeErrors::Cancelled));
                auto appended = AppendEntity(request, page, scene, entity, sceneIndex, prepared, budget);
                if (appended.HasError())
                    return fail(appended.ErrorValue());
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Captures a complete source generation with explicit bounded projection state and collision rejection. */
    Result<PreparedWorld> Convert(const WorldBakeRequest &request, const CancellationToken &cancellation) {
        PreparedWorld prepared{
            .output = {.document = request.document->fence, .project = request.project.fence, .source = request.document}};
        ProjectionBudget budget;
        std::size_t sourceBytes{};
        for (const auto &page : request.document->document.pages)
            if (auto appended = AppendPage(request, page, cancellation, prepared, budget, sourceBytes); appended.HasError())
                return Result<PreparedWorld>::Failure(appended.ErrorValue());
        std::set<Assets::AssetId> chunks;
        for (const auto cell : budget.cells) {
            const auto chunk = ChunkId(request.document->document.settings.partition, cell);
            if (!chunk.IsValid() || !chunks.insert(chunk).second)
                return Result<PreparedWorld>::Failure(MakeError(WorldBakeErrors::Conflict));
            prepared.topology.push_back({cell, {chunk}});
        }
        return Result<PreparedWorld>::Success(std::move(prepared));
    }
}  // namespace Horo::Application::WorldBakeDetail
