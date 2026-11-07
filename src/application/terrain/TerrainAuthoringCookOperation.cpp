#include "TerrainAuthoringCookInternal.h"

namespace Horo::Application::TerrainCookDetail {
    namespace {
        /** @brief The feature admits a finite envelope before native captures; canonical domain validators retain semantic authority. */
        bool CaptureLimits(const TerrainAuthoringCookProfile &profile) noexcept {
            constexpr std::uint64_t MiB = 1024 * 1024;
            return profile.maximumCapturedBytes > 0 && profile.maximumCapturedBytes <= 128 * MiB && profile.assets.maximumAssets > 0 &&
                   profile.assets.maximumAssets <= 4096 && profile.assets.maximumSourceBytes > 0 &&
                   profile.assets.maximumSourceBytes <= 64 * MiB;
        }

        bool OutputLimits(const TerrainAuthoringCookProfile &profile) noexcept {
            constexpr std::uint64_t MiB = 1024 * 1024;
            return profile.assets.maximumArtifactBytes >= 256 && profile.assets.maximumArtifactBytes <= 256 * MiB &&
                   profile.assets.maximumConcurrentCooks > 0 && profile.assets.maximumConcurrentCooks <= 8 &&
                   profile.source.maximumBytes > 0 && profile.source.maximumBytes <= 64 * MiB;
        }

        Result<Assets::AssetRegistrySnapshot> RootRecord(const Capture &capture) {
            const auto *record = capture.inputs->Registry().Find(capture.source.source.sourceAsset);
            if (!record)
                return Result<Assets::AssetRegistrySnapshot>::Failure(MakeError(TerrainAuthoringCookErrors::Invalid));
            Assets::AssetRegistry selected;
            if (selected.Publish(std::vector<Assets::AssetRecord>{*record}).status != Assets::AssetRegistryBuildStatus::Complete)
                return Result<Assets::AssetRegistrySnapshot>::Failure(MakeError(TerrainAuthoringCookErrors::Invalid));
            return Result<Assets::AssetRegistrySnapshot>::Success(selected.Snapshot());
        }

        /** @brief The sole produced root keys every real source read; leaf sources are not fake compiled outputs. */
        Result<std::shared_ptr<const Assets::AssetCookKeyProjection>> Projection(const Capture &capture,
                                                                                 const TerrainAuthoringCookRequest &request) {
            auto inputs = capture.dependencies;
            inputs.push_back(request.asset);
            return Assets::AssetCookKeyProjection::Create(*capture.inputs,
                                                          {{request.asset, std::move(inputs), ProfileDigest(request.profile)}},
                                                          request.profile.assets.maximumAssets);
        }

        Result<Assets::AssetCookReport> Publish(const TerrainAuthoringCookEnvironment &environment,
                                                const TerrainAuthoringCookRequest &request, std::shared_ptr<const Capture> capture,
                                                const CancellationToken &cancellation) {
            using Return = Result<Assets::AssetCookReport>;
            auto catalog = MakeCatalog(capture, request, cancellation);
            if (catalog.HasError())
                return Return::Failure(std::move(catalog).ErrorValue());
            auto selected = RootRecord(*capture);
            if (selected.HasError())
                return Return::Failure(std::move(selected).ErrorValue());
            auto projection = Projection(*capture, request);
            if (projection.HasError())
                return Return::Failure(std::move(projection).ErrorValue());
            Assets::AssetCookRequest cook{.sourceRoot = request.sourceRoot,
                                          .cacheFiles = environment.cacheFiles,
                                          .cookedRoot = request.cookedRoot,
                                          .registry = std::move(selected).Value(),
                                          .target = request.profile.target,
                                          .limits = request.profile.assets,
                                          .buildOutputStore = environment.buildOutput,
                                          .operationStore = environment.operations,
                                          .publicationFiles = environment.publicationFiles,
                                          .newPublicationOperationId = environment.newPublicationOperationId,
                                          .pinnedInputs = capture->inputs,
                                          .keyProjection = std::move(projection).Value(),
                                          .validateHostInputs = [&environment, &request, capture, cancellation] {
                return VerifySources(environment, request, *capture, cancellation);
            }};
            Assets::AssetCookService service(*environment.jobs, std::move(catalog).Value());
            return service.Cook(cook, cancellation);
        }
    }  // namespace

    /** @copydoc CookAttempt */
    Result<Assets::AssetCookReport> CookAttempt(const TerrainAuthoringCookEnvironment &environment,
                                                const TerrainAuthoringCookRequest &request, const CancellationToken &cancellation) {
        using Return = Result<Assets::AssetCookReport>;
        if (cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(TerrainAuthoringCookErrors::Cancelled));
        if (!CaptureLimits(request.profile) || !OutputLimits(request.profile))
            return Return::Failure(MakeError(Terrain::TerrainSourceErrors::LimitExceeded));
        auto lease = environment.mutations->TryAcquire({request.sourceRoot, Editor::ProjectMutationOwner::Asset, "terrain.authoring.cook"});
        if (lease.HasError())
            return Return::Failure(std::move(lease).ErrorValue());
        auto captured = CaptureSources(environment, request, cancellation);
        if (captured.HasError())
            return Return::Failure(std::move(captured).ErrorValue());
        auto capture = std::make_shared<const Capture>(std::move(captured).Value());
        return Publish(environment, request, std::move(capture), cancellation);
    }
}  // namespace Horo::Application::TerrainCookDetail
