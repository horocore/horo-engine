#pragma once
#include "Horo/Application/WorldBakeApplication.h"

#include <map>

namespace Horo::Application::WorldBakeDetail {
    namespace W = WorldStreaming;

    struct PlacedEntity final {
        W::WorldAuthoringObjectAddress address;
        Runtime::RuntimeEntityDefinition entity;
        std::vector<W::StreamingCellId> cells;
        std::vector<Runtime::TriggerVolumeMigrationEvidence> triggers;
        std::size_t sceneIndex{};
    };

    struct PreparedWorld final {
        WorldBakeOutput output;
        std::vector<std::optional<DiagnosticSourceLocation>> sources;
        std::vector<PlacedEntity> objects;
        std::vector<W::WorldPartitionCellDescriptor> topology;
    };

    struct CookedCells final {
        std::vector<Runtime::WorldBuildCell> expected;
        std::vector<std::shared_ptr<const Runtime::RuntimeSceneCellPayload>> payloads;
    };

    [[nodiscard]] Result<CookedCells> CookCells(const WorldBakeRequest &request, const W::WorldPartitionDescriptor &partition,
                                                const PreparedWorld &prepared, const CancellationToken &cancellation);
    [[nodiscard]] Result<std::vector<Runtime::WorldBuildReference>> CaptureReferences(
        std::span<const Runtime::WorldBuildCell> expected,
        std::span<const std::shared_ptr<const Runtime::RuntimeSceneCellPayload>> payloads,
        const Runtime::WorldBuildValidationLimits &limits);
    [[nodiscard]] Result<void> Encode(const WorldBakeRequest &request, WorldBakeOutput &output, const CancellationToken &cancellation,
                                      const std::function<void(float)> &progress);
    [[nodiscard]] Result<std::optional<Assets::AssetCookGeneration>> ResolveDependencies(WorldBakeRequest &request,
                                                                                         const CancellationToken &cancellation);
    [[nodiscard]] Result<void> VerifyDependencies(const WorldBakeRequest &request,
                                                  const std::optional<Assets::AssetCookGeneration> &generation,
                                                  const CancellationToken &cancellation);
    [[nodiscard]] std::uint64_t ObjectId(Assets::AssetId page, std::uint64_t object);
    [[nodiscard]] Assets::AssetId ChunkId(const W::WorldPartitionId &world, W::StreamingCellId cell);
    void Remap(Runtime::RuntimeEntityDefinition &entity, Assets::AssetId page);
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertPage(const SceneSource::SceneSourceView &source,
                                                                      const W::WorldAuthoringPageDescriptor &page,
                                                                      const WorldBakeRequest &request);
    [[nodiscard]] Result<std::vector<W::StreamingCellId>> Placement(const Editor::PartitionedWorldObject &object,
                                                                    const WorldBakeRequest &request);
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> CapturePage(const WorldBakeRequest &request,
                                                                      const Editor::PartitionedWorldPage &page,
                                                                      const CancellationToken &cancellation, std::size_t &sourceBytes,
                                                                      WorldBakeOutput &output);
    [[nodiscard]] Error AtSource(Error error, const std::optional<DiagnosticSourceLocation> &source);
    [[nodiscard]] Result<void> Admit(const WorldBakeRequest &request);
    [[nodiscard]] Result<PreparedWorld> Convert(const WorldBakeRequest &request, const CancellationToken &cancellation);
    [[nodiscard]] std::optional<DiagnosticSourceLocation> SourceFor(const WorldBakeRequest &request, Assets::AssetId page);
}  // namespace Horo::Application::WorldBakeDetail
