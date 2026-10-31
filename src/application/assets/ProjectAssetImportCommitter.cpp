#include "Horo/Application/ProjectAssetImportCommitter.h"

#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <new>
#include <set>

namespace Horo::Application {
    namespace {
        constexpr std::size_t MaximumBatchBytes = 64 * 1024 * 1024;
        constexpr std::size_t MaximumBatchItems = 64;
        const ErrorCodeDescriptor
            TerrainSidecarRequired{ErrorDomainId{"horo.application.asset_import"}, ErrorCode{"asset.import.terrain.sidecar_required"},
                                   ErrorSeverity::Error,
                                   "Persistent terrain imports require their identity and coordinate settings sidecar.",
                                   "Enable identity sidecar creation for this terrain import."};

        /** @brief A publication entry owns one exclusive destination lease and only its freshly created private stage. */
        struct Entry final {
            std::filesystem::path path;
            std::filesystem::path stage;
            std::vector<std::byte> bytes;
            Assets::AssetSourceIdentity identity;
            std::optional<ExclusiveFileLock> lock;
            bool staged{}, published{};
        };

        /** @brief Portable single filename admission prevents display names from becoming paths. */
        bool Filename(std::string_view value) noexcept {
            if (value.empty() || value.size() > 200 || value == "." || value == "..")
                return false;
            for (const unsigned char c : value)
                if (c < 32 || c == 127 || std::string_view("/\\:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos)
                    return false;
            return value.back() != '.' && value.back() != ' ';
        }

        /** @brief Reserves complete pair bytes and identities before entering a native publication transition. */
        Result<std::vector<Entry>> Prepare(const Assets::PreparedAssetImportBatch &batch, Assets::IAssetIdGenerator &ids) {
            using Return = Result<std::vector<Entry>>;
            if (batch.items.empty() || batch.items.size() > MaximumBatchItems || batch.operationId.empty() ||
                batch.operationId.size() > 256)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            const auto directory = Foundation::Paths::Resolve(batch.projectRoot, batch.destinationFolder);
            const auto operationBytes = std::as_bytes(std::span(batch.operationId));
            const auto stageKey = FormatSha256(ComputeSha256(operationBytes)).substr(7);
            std::vector<Entry> entries;
            entries.reserve(batch.items.size() * 2);
            std::set<std::filesystem::path> destinations;
            std::size_t total{};
            const auto append = [&](std::filesystem::path path, std::span<const std::byte> bytes) -> Result<void> {
                if (bytes.size() > MaximumBatchBytes - total || !destinations.insert(path).second)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
                auto absent = Assets::CaptureAssetSource(batch.projectRoot, path, MaximumBatchBytes);
                if (absent.HasError())
                    return Result<void>::Failure(std::move(absent).ErrorValue());
                if (absent.Value().identity.exists)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
                Entry entry;
                entry.path = std::move(path);
                entry.stage = directory / (".horo-import-" + stageKey + "-" + std::to_string(entries.size()) + ".tmp");
                entry.bytes.assign(bytes.begin(), bytes.end());
                entry.identity = {true, bytes.size(), ComputeSha256(bytes)};
                total += bytes.size();
                entries.push_back(std::move(entry));
                return Result<void>::Success();
            };
            for (const auto &item : batch.items) {
                if (!item.result || !Filename(item.displayName) || item.targetExtension.empty() || item.targetExtension.front() != '.' ||
                    !Filename(item.displayName + item.targetExtension))
                    return Return::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
                const auto &prepared = *item.result;
                if (prepared.type.Value() == "core.terrain" && (!item.createMetaSidecar || !item.supportsMetaSidecar))
                    return Return::Failure(MakeError(TerrainSidecarRequired));
                const auto path = directory / (item.displayName + item.targetExtension);
                auto payload = append(path, std::as_bytes(std::span(prepared.editorPayload)));
                if (payload.HasError())
                    return Return::Failure(std::move(payload).ErrorValue());
                if (item.createMetaSidecar && item.supportsMetaSidecar) {
                    const auto id = item.preservedAssetId ? *item.preservedAssetId : ids.Generate();
                    if (!id.IsValid())
                        return Return::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
                    const Assets::AssetImportMetadata metadata{.assetId = id,
                                                               .assetType = prepared.type,
                                                               .importerContributionId = item.importerContributionId,
                                                               .importerVersion = item.importerVersion,
                                                               .importerPackageId = item.importerPackageId,
                                                               .importerModuleId = item.importerModuleId,
                                                               .importerModuleVersion = item.importerModuleVersion,
                                                               .absoluteSourcePath = item.absoluteSourcePath,
                                                               .sourceExtension = item.sourceExtension,
                                                               .sourceHash = item.sourceHash,
                                                               .sourceByteSize = item.sourceByteSize,
                                                               .sourceLastWriteTime = item.sourceLastWriteTime,
                                                               .importSettings = item.settings,
                                                               .dependencies = prepared.dependencies,
                                                               .lastImportReasons =
                                                                   item.importReasons.empty()
                                                                       ? std::vector{Assets::AssetImportReason::InitialImport}
                                                                       : item.importReasons,
                                                               .importedAtUtc = Assets::CurrentImportTimestampUtc()};
                    auto serialized = Assets::SerializeAssetImportMetadata(metadata);
                    if (serialized.HasError())
                        return Return::Failure(std::move(serialized).ErrorValue());
                    auto sidecar = path;
                    sidecar += ".horo";
                    auto appended = append(std::move(sidecar), std::as_bytes(std::span(serialized.Value())));
                    if (appended.HasError())
                        return Return::Failure(std::move(appended).ErrorValue());
                }
            }
            return Return::Success(std::move(entries));
        }

        /** @brief Compensates only proven owned destinations while all cooperating writer leases remain held. */
        void Rollback(std::vector<Entry> &entries, const std::filesystem::path &root, DurableFileSystem &files,
                      ProjectAssetImportReceipt &receipt) noexcept {
            for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
                try {
                    if (entry->published) {
                        auto current = Assets::CaptureAssetSource(root, entry->path, MaximumBatchBytes);
                        if (current.HasError() || current.Value().identity != entry->identity) {
                            receipt.status = ProjectAssetImportStatus::ReconciliationRequired;
                        } else {
                            auto removed = files.RemoveDurable(entry->path);
                            if (removed.HasError())
                                receipt.status = ProjectAssetImportStatus::ReconciliationRequired;
                            else
                                ++receipt.rolledBackFiles;
                        }
                    }
                    if (entry->staged && files.RemoveDurable(entry->stage).HasError())
                        receipt.status = ProjectAssetImportStatus::ReconciliationRequired;
                } catch (...) {
                    receipt.status = ProjectAssetImportStatus::ReconciliationRequired;
                }
            }
            if (receipt.status != ProjectAssetImportStatus::ReconciliationRequired && receipt.promotedFiles)
                receipt.status = ProjectAssetImportStatus::RolledBack;
        }

        /** @brief Stages every complete pair before making any asset destination visible. */
        Result<void> Stage(std::vector<Entry> &entries, const Assets::PreparedAssetImportBatch &batch, DurableFileSystem &files,
                           const CancellationToken &cancellation) {
            for (auto &entry : entries) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Cancelled));
                auto lockPath = Assets::AssetSourcePublicationLockPath(batch.projectRoot, entry.path);
                if (lockPath.HasError())
                    return Result<void>::Failure(std::move(lockPath).ErrorValue());
                auto lock = files.TryAcquireExclusive(lockPath.Value(), batch.operationId);
                if (lock.HasError())
                    return Result<void>::Failure(std::move(lock).ErrorValue());
                entry.lock.emplace(std::move(lock).Value());
                auto parent = files.CreateDirectoryPath(entry.path.parent_path(), DirectoryCreationMode::Parents);
                if (parent.HasError())
                    return Result<void>::Failure(std::move(parent).ErrorValue());
                auto staged = files.WritePrivateDurable(entry.stage, entry.bytes, entry.staged);
                if (staged.HasError())
                    return staged;
                auto verified = Assets::CaptureAssetSource(batch.projectRoot, entry.stage, MaximumBatchBytes, cancellation);
                if (verified.HasError())
                    return Result<void>::Failure(std::move(verified).ErrorValue());
                if (verified.Value().identity != entry.identity)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ProjectAssetImportCommitter::ProjectAssetImportCommitter */
    ProjectAssetImportCommitter::ProjectAssetImportCommitter(Editor::ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                             Assets::AssetRegistry *registry) noexcept
        : mutations_(mutations), files_(files), registry_(registry) {}

    /** @copydoc ProjectAssetImportCommitter::Commit */
    Result<void> ProjectAssetImportCommitter::Commit(const Assets::PreparedAssetImportBatch &batch, Assets::IAssetIdGenerator &ids,
                                                     const CancellationToken &cancellation) {
        if (busy_)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        busy_ = true;

        struct Reset final {
            bool &busy;

            ~Reset() {
                busy = false;
            }
        } reset{busy_};

        receipt_ = {};
        std::vector<Entry> entries;
        std::optional<Editor::ProjectMutationLease> lease;
        bool complete{};
        std::optional<Error> publishedFailure;

        struct Cleanup final {
            std::vector<Entry> &entries;
            const std::filesystem::path &root;
            DurableFileSystem &files;
            ProjectAssetImportReceipt &receipt;
            bool &complete;

            ~Cleanup() {
                if (!complete)
                    Rollback(entries, root, files, receipt);
                else
                    for (auto &entry : entries) {
                        try {
                            if (entry.staged)
                                static_cast<void>(files.RemoveDurable(entry.stage));
                        } catch (...) {
                        }
                    }
            }
        } cleanup{entries, batch.projectRoot, files_, receipt_, complete};

        try {
            publishedFailure.emplace(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Cancelled));
            auto acquired = mutations_.TryAcquire({batch.projectRoot, Editor::ProjectMutationOwner::Asset, batch.operationId});
            if (acquired.HasError())
                return Result<void>::Failure(std::move(acquired).ErrorValue());
            lease.emplace(std::move(acquired).Value());
            auto prepared = Prepare(batch, ids);
            if (prepared.HasError())
                return Result<void>::Failure(std::move(prepared).ErrorValue());
            entries = std::move(prepared).Value();
            auto staged = Stage(entries, batch, files_, cancellation);
            if (staged.HasError())
                return staged;
            for (auto &entry : entries) {
                auto published =
                    Assets::PublishAssetSource(batch.projectRoot, entry.path, {}, entry.bytes, *entry.lock, files_, cancellation);
                if (published.HasError())
                    return Result<void>::Failure(std::move(published).ErrorValue());
                entry.published = true;
                ++receipt_.promotedFiles;
                if (published.Value().status != Assets::AssetSourcePublicationStatus::Durable) {
                    receipt_.status = ProjectAssetImportStatus::ReconciliationRequired;
                    return Result<void>::Failure(std::move(*published.Value().durabilityError));
                }
            }
            for (auto &entry : entries) {
                auto removed = files_.RemoveDurable(entry.stage);
                if (removed.HasError())
                    return Result<void>::Failure(std::move(removed).ErrorValue());
                entry.staged = false;
            }
            if (registry_) {
                auto candidate = Assets::PrepareAssetRegistryCandidate(batch.projectRoot);
                if (candidate.HasError())
                    return Result<void>::Failure(std::move(candidate).ErrorValue());
                if (candidate.Value().status == Assets::AssetRegistryBuildStatus::Failed)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
                const auto report = registry_->Publish(std::move(candidate).Value());
                if (report.status == Assets::AssetRegistryBuildStatus::Failed)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
                receipt_.registryPublished = true;
                // Publish performs every allocation before its atomic state replacement. From here onward the registry
                // owns visible references to these complete pairs: a later derived-index failure cannot remove them.
                complete = true;
                receipt_.status = ProjectAssetImportStatus::ReconciliationRequired;
                auto indexed =
                    Assets::AssetIndexStore::SaveAtomically(batch.projectRoot / std::filesystem::path{ProjectLayout::AssetIndexPath},
                                                            registry_->Snapshot());
                if (indexed.HasError())
                    return Result<void>::Failure(
                        WrapError(Assets::AssetSourceErrors::DurabilityUnconfirmed, std::move(indexed).ErrorValue()));
            }
            receipt_.status = ProjectAssetImportStatus::Published;
            complete = true;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            if (complete && publishedFailure)
                return Result<void>::Failure(std::move(*publishedFailure));
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        } catch (...) {
            if (complete && publishedFailure)
                return Result<void>::Failure(std::move(*publishedFailure));
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }
}  // namespace Horo::Application
