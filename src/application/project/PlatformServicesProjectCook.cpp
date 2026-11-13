#include "Horo/Application/PlatformServicesProjectCook.h"

#include <type_traits>
#include <utility>

namespace Horo::Application {
    namespace {
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

        /** @brief Adds the domain freshness fence to the existing tracked single-file publication; all authority remains injected. */
        class CookCommitFiles final : public DurableFileSystem {
        public:
            CookCommitFiles(DurableFileSystem &files, const PlatformServicesProjectCookRequest &request, CancellationToken cancellation)
                : files_(files), request_(request), cancellation_(std::move(cancellation)) {}

            Result<bool> CreateDirectoryPath(const std::filesystem::path &path, DirectoryCreationMode mode) override {
                return files_.CreateDirectoryPath(path, mode);
            }

            Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path, std::string_view owner) override {
                return files_.TryAcquireExclusive(path, owner);
            }

            Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
                return files_.AvailableBytes(path);
            }

            Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
                return files_.WriteDurable(path, bytes);
            }

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms permissions) override {
                return files_.WritePrivateDurable(path, bytes, created, permissions);
            }

            Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
                return files_.CopyDurable(source, destination);
            }

            Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
                return files_.AtomicReplace(prepared, destination);
            }

            Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                              AtomicFileReplacementReceipt &receipt) override {
                auto current = CheckCurrent(request_, cancellation_);
                if (current.HasError())
                    return current;
                return files_.AtomicReplaceTracked(prepared, destination, receipt);
            }

            Result<void> RemoveDurable(const std::filesystem::path &path) override {
                return files_.RemoveDurable(path);
            }

            Result<void> SyncDirectory(const std::filesystem::path &path) override {
                return files_.SyncDirectory(path);
            }

        private:
            DurableFileSystem &files_;
            const PlatformServicesProjectCookRequest &request_;
            CancellationToken cancellation_;
        };
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
        auto created = files.CreateDirectoryPath(destination.parent_path(), DirectoryCreationMode::Parents);
        if (created.HasError())
            return Return::Failure(std::move(created).ErrorValue());
        for (auto directory = destination.parent_path(); directory != sources.ProjectRoot(); directory = directory.parent_path()) {
            auto synced = files.SyncDirectory(directory);
            if (synced.HasError())
                return Return::Failure(std::move(synced).ErrorValue());
        }
        auto syncedRoot = files.SyncDirectory(sources.ProjectRoot());
        if (syncedRoot.HasError())
            return Return::Failure(std::move(syncedRoot).ErrorValue());
        CookCommitFiles fenced{files, request, cancellation};
        auto published =
            Assets::PublishAssetSource(sources.ProjectRoot(), destination, expected, bytes.Value(), lease, fenced, cancellation);
        if (published.HasError())
            return Return::Failure(std::move(published).ErrorValue());
        static_assert(std::is_nothrow_move_constructible_v<PlatformServicesProjectCookPublication>);
        return Return::Success({cooked.Value().generationFingerprint, std::move(published).Value()});
    }
}  // namespace Horo::Application
