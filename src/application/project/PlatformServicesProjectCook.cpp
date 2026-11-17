#include "Horo/Application/PlatformServicesProjectCook.h"

#include "PlatformServicesPublicationFiles.h"

#include <type_traits>
#include <utility>

namespace Horo::Application {
    namespace {
        /** @brief Prepares and synchronizes the contained derived-directory chain before staging a paired output. */
        Result<void> PrepareDestination(DurableFileSystem &files, const std::filesystem::path &root,
                                        const std::filesystem::path &destination) {
            auto created = files.CreateDirectoryPath(destination.parent_path(), DirectoryCreationMode::Parents);
            if (created.HasError())
                return Result<void>::Failure(std::move(created).ErrorValue());
            for (auto directory = destination.parent_path(); directory != root; directory = directory.parent_path()) {
                auto synced = files.SyncDirectory(directory);
                if (synced.HasError())
                    return synced;
            }
            return files.SyncDirectory(root);
        }

        /** @brief Rechecks actual source, private mapping and lifecycle ownership without selecting or activating a provider. */
        Result<void> CheckCurrent(const PlatformServicesProjectCookRequest &request, const CancellationToken cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PlatformServices::PlatformProviderManifestCookErrors::Cancelled));
            auto current = RevalidatePlatformServicesProjectSources(request.sources, cancellation);
            if (current.HasError())
                return current;
            if (!request.sources.Configuration().UsesNullProvider() && !request.validateMappingRevision)
                return Result<void>::Failure(MakeError(PlatformServices::PlatformProviderManifestCookErrors::StaleGeneration));
            if (request.validateMappingRevision) {
                auto mapping = request.validateMappingRevision();
                if (mapping.HasError())
                    return mapping;
            }
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PlatformServices::PlatformProviderManifestCookErrors::Cancelled));
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc CookAndPublishPlatformServicesProject */
    Result<PlatformServicesProjectCookPublication> CookAndPublishPlatformServicesProject(const PlatformServicesProjectCookRequest &request,
                                                                                         const Assets::AssetSourceIdentity &expected,
                                                                                         const ExclusiveFileLock &lease,
                                                                                         DurableFileSystem &files,
                                                                                         const CancellationToken cancellation) {
        using Return = Result<PlatformServicesProjectCookPublication>;
        const auto &sources = request.sources;
        if (sources.Configuration().Profile() != PlatformServices::PlatformServicesHostProfile::Cook)
            return Return::Failure(MakeError(PlatformServices::PlatformProviderManifestCookErrors::InvalidInput));
        const auto destination = sources.ProjectRoot() / PlatformServicesProjectCookPath;
        auto lockPath = Assets::AssetSourcePublicationLockPath(sources.ProjectRoot(), destination);
        if (lockPath.HasError())
            return Return::Failure(std::move(lockPath).ErrorValue());
        if (!lease.ProtectsPath(lockPath.Value()))
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        if (auto current = CheckCurrent(request, cancellation); current.HasError())
            return Return::Failure(std::move(current).ErrorValue());
        auto cooked = PlatformServices::CookPlatformProviderManifest({.stableIds = sources.StableIds(),
                                                                      .achievements = sources.Achievements(),
                                                                      .leaderboards = sources.Leaderboards(),
                                                                      .stats = sources.Stats(),
                                                                      .presence = sources.Presence(),
                                                                      .configuration = sources.Configuration(),
                                                                      .mappingPolicy = request.mappingPolicy,
                                                                      .mappingRevision = request.mappingRevision,
                                                                      .mappings = request.mappings,
                                                                      .hostCancellation = cancellation});
        if (cooked.HasError())
            return Return::Failure(std::move(cooked).ErrorValue());
        auto bytes = PlatformServices::EncodePlatformProviderManifestCookGeneration(cooked.Value());
        if (bytes.HasError())
            return Return::Failure(std::move(bytes).ErrorValue());
        if (auto prepared = PrepareDestination(files, sources.ProjectRoot(), destination); prepared.HasError())
            return Return::Failure(std::move(prepared).ErrorValue());
        PlatformServicesDetail::PublicationFiles fenced{files, [&request, cancellation] {
            return CheckCurrent(request, cancellation);
        }};
        auto published =
            Assets::PublishAssetSource(sources.ProjectRoot(), destination, expected, bytes.Value(), lease, fenced, cancellation);
        if (published.HasError())
            return Return::Failure(std::move(published).ErrorValue());
        static_assert(std::is_nothrow_move_constructible_v<PlatformServicesProjectCookPublication>);
        return Return::Success({cooked.Value().generationFingerprint, std::move(published).Value()});
    }
}  // namespace Horo::Application
