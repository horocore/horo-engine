#include "WorldBakeInternal.h"

#include <algorithm>
#include <array>

namespace Horo::Application::WorldBakeDetail {
    namespace {
        /** @brief Uses the same explicit inventory/byte ceilings for capture and final pointer verification. */
        Assets::AssetCookLimits Limits(const WorldBakeRequest &request) {
            return {.maximumSourceBytes = request.limits.maximumSourceBytes,
                    .maximumArtifactBytes = request.limits.maximumSourceBytes,
                    .maximumAssets = request.limits.cache.maximumDependencies,
                    .maximumConcurrentCooks = 1};
        }

        /** @brief Binds all verified generation members conservatively when provider-private dependency edges are opaque. */
        Sha256Digest Settings(const Sha256Digest &settings, const Sha256Digest &manifest) {
            constexpr std::string_view domain = "Horo.WorldBake.ProjectAssets.v1";
            const std::array<std::span<const std::byte>, 3> parts{std::as_bytes(std::span{domain.data(), domain.size()}),
                                                                  std::as_bytes(std::span{settings.bytes}),
                                                                  std::as_bytes(std::span{manifest.bytes})};
            return ComputeSha256Fragments(parts);
        }
    }  // namespace

    /** @brief Captures real committed envelopes from the exact host-selected target without provider/backend discovery. */
    Result<std::optional<Assets::AssetCookGeneration>> ResolveDependencies(WorldBakeRequest &request,
                                                                           const CancellationToken &cancellation) {
        if (!request.project.dependencySource)
            return Result<std::optional<Assets::AssetCookGeneration>>::Success(std::nullopt);
        if (cancellation.IsCancellationRequested())
            return Result<std::optional<Assets::AssetCookGeneration>>::Failure(MakeError(WorldBakeErrors::Cancelled));
        const auto &selection = *request.project.dependencySource;
        auto captured = Assets::ResolveCurrentCookGeneration(*selection.files, selection.targetRoot, Limits(request));
        if (captured.HasError())
            return Result<std::optional<Assets::AssetCookGeneration>>::Failure(captured.ErrorValue());
        auto generation = std::move(captured).Value();
        if (generation.target != selection.target)
            return Result<std::optional<Assets::AssetCookGeneration>>::Failure(MakeError(WorldBakeErrors::Dependency));
        auto evidence = CaptureWorldBakeDependencies(*selection.files, generation, request.project.fence.revision,
                                                     request.limits.maximumSourceBytes, request.limits.cache.maximumDependencies);
        if (evidence.HasError())
            return Result<std::optional<Assets::AssetCookGeneration>>::Failure(evidence.ErrorValue());
        for (const auto &dependency : evidence.Value()) {
            const auto *record = request.registry.Find(dependency.asset.id);
            if (record && record->type != dependency.asset.expectedType)
                return Result<std::optional<Assets::AssetCookGeneration>>::Failure(MakeError(WorldBakeErrors::Dependency));
        }
        request.project.dependencies = std::move(evidence).Value();
        request.project.settings = Settings(request.project.settings, generation.manifestDigest);
        return Result<std::optional<Assets::AssetCookGeneration>>::Success(std::move(generation));
    }

    /** @brief Rejects replacement of the selected committed generation before an offline result leaves its worker. */
    Result<void> VerifyDependencies(const WorldBakeRequest &request, const std::optional<Assets::AssetCookGeneration> &generation,
                                    const CancellationToken &cancellation) {
        if (!generation)
            return Result<void>::Success();
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(WorldBakeErrors::Cancelled));
        const auto &selection = *request.project.dependencySource;
        auto current = Assets::ResolveCurrentCookGeneration(*selection.files, selection.targetRoot, Limits(request));
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value().target != generation->target || current.Value().manifestDigest != generation->manifestDigest ||
            current.Value().generationRoot != generation->generationRoot)
            return Result<void>::Failure(MakeError(WorldBakeErrors::Stale));
        return Result<void>::Success();
    }
}  // namespace Horo::Application::WorldBakeDetail
