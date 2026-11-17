#include "Horo/Application/ProjectAssetImportCommitter.h"

#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <format>
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
            bool staged{};
            bool published{};
        };

        /** @brief Portable single filename admission prevents display names from becoming paths. */
        bool Filename(std::string_view value) noexcept {
            if (value.empty() || value.size() > 200 || value == "." || value == "..")
                return false;
            for (const unsigned char c : value)
                if (c < 32 || c == 127 || std::string_view(R"(/\:*?"<>|)").find(static_cast<char>(c)) != std::string_view::npos)
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
            const auto append = [&](std::filesystem::path path, std::span<const std::byte> bytes) {
                if (bytes.size() > MaximumBatchBytes - total || !destinations.insert(path).second)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
                auto absent = Assets::CaptureAssetSource(batch.projectRoot, path, MaximumBatchBytes);
                if (absent.HasError())
                    return Result<void>::Failure(std::move(absent).ErrorValue());
                if (absent.Value().identity.exists)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
                Entry entry;
                entry.path = std::move(path);
                entry.stage = directory / std::format(".horo-import-{}-{}.tmp", stageKey, entries.size());
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
                if (auto payload = append(path, std::as_bytes(std::span(prepared.editorPayload))); payload.HasError())
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
            using enum Horo::Application::ProjectAssetImportStatus;
            for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
                try {
                    if (entry->published) {
                        auto current = Assets::CaptureAssetSource(root, entry->path, MaximumBatchBytes);
                        if (current.HasError() || current.Value().identity != entry->identity) {
                            receipt.status = ReconciliationRequired;
                        } else {
                            auto removed = files.RemoveDurable(entry->path);
                            if (removed.HasError())
                                receipt.status = ReconciliationRequired;
                            else
                                ++receipt.rolledBackFiles;
                        }
                    }
                    if (entry->staged && files.RemoveDurable(entry->stage).HasError())
                        receipt.status = ReconciliationRequired;
                } catch (...) {
                    receipt.status = ReconciliationRequired;
                }
            }
            if (receipt.status != ReconciliationRequired && receipt.promotedFiles)
                receipt.status = RolledBack;
        }

        /** @brief Stages every complete pair before making any asset destination visible. */
        Result<void> Stage(std::vector<Entry> &entries, const std::filesystem::path &root, std::string_view operation,
                           DurableFileSystem &files, const CancellationToken &cancellation) {
            for (auto &entry : entries) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Cancelled));
                auto lockPath = Assets::AssetSourcePublicationLockPath(root, entry.path);
                if (lockPath.HasError())
                    return Result<void>::Failure(std::move(lockPath).ErrorValue());
                auto lock = files.TryAcquireExclusive(lockPath.Value(), operation);
                if (lock.HasError())
                    return Result<void>::Failure(std::move(lock).ErrorValue());
                entry.lock.emplace(std::move(lock).Value());
                if (auto parent = files.CreateDirectoryPath(entry.path.parent_path(), DirectoryCreationMode::Parents); parent.HasError())
                    return Result<void>::Failure(std::move(parent).ErrorValue());
                if (auto staged = files.WritePrivateDurable(entry.stage, entry.bytes, entry.staged); staged.HasError())
                    return staged;
                auto verified = Assets::CaptureAssetSource(root, entry.stage, MaximumBatchBytes, cancellation);
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

    struct ProjectAssetImportCommitter::PreparedPublication final {
        const std::filesystem::path &root;
        const std::string &operation;
        std::vector<Entry> entries;
        std::vector<Assets::AssetRecord> expected;
    };

    namespace {
        /** @brief Revokes callback reentry through both initial publication entry points. */
        struct BusyScope final {
            bool &busy;

            BusyScope(const BusyScope &) = delete;
            BusyScope &operator=(const BusyScope &) = delete;

            explicit BusyScope(bool &value) noexcept : busy(value) {
                busy = true;
            }

            ~BusyScope() {
                busy = false;
            }
        };

        /** @brief Promote complete prevalidated sources while retaining irreversible receipts. */
        Result<void> Promote(std::vector<Entry> &entries, const std::filesystem::path &root, DurableFileSystem &files,
                             ProjectAssetImportReceipt &receipt, const CancellationToken &cancellation) {
            for (auto &entry : entries) {
                auto result = Assets::PublishAssetSource(root, entry.path, {}, entry.bytes, *entry.lock, files, cancellation);
                if (result.HasError())
                    return Result<void>::Failure(std::move(result).ErrorValue());
                entry.published = true;
                ++receipt.promotedFiles;
                if (result.Value().status != Assets::AssetSourcePublicationStatus::Durable) {
                    receipt.status = ProjectAssetImportStatus::ReconciliationRequired;
                    return Result<void>::Failure(*result.Value().durabilityError);
                }
            }
            return Result<void>::Success();
        }

        /** @brief Remove private copies before publishing registry references to the promoted batch. */
        Result<void> ClearStages(std::vector<Entry> &entries, DurableFileSystem &files) {
            for (auto &entry : entries) {
                if (auto removed = files.RemoveDurable(entry.stage); removed.HasError())
                    return removed;
                entry.staged = false;
            }
            return Result<void>::Success();
        }

        /** @brief A newly authored source is tracked only when its exact planned record is present in the complete candidate. */
        Result<void> PlannedRecords(const Assets::AssetRegistryCandidate &candidate, std::span<const Assets::AssetRecord> expected) {
            for (const auto &record : expected) {
                const auto found = std::ranges::find(candidate.records, record.id, &Assets::AssetRecord::id);
                if (found == candidate.records.end())
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
                if (found->type != record.type || found->sourcePath.String() != record.sourcePath.String() ||
                    found->metadataPath.String() != record.metadataPath.String())
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            return Result<void>::Success();
        }

        /** @brief Registry publication commits visibility; a later derived-index failure must not compensate sources. */
        Result<void> PublishRegistry(const std::filesystem::path &root, Assets::AssetRegistry *registry, ProjectAssetImportReceipt &receipt,
                                     bool &complete, std::span<const Assets::AssetRecord> expected) {
            if (!registry && expected.empty())
                return Result<void>::Success();
            auto candidate = Assets::PrepareAssetRegistryCandidate(root);
            if (candidate.HasError())
                return Result<void>::Failure(std::move(candidate).ErrorValue());
            if (candidate.Value().status == Assets::AssetRegistryBuildStatus::Failed)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            if (auto planned = PlannedRecords(candidate.Value(), expected); planned.HasError())
                return planned;
            if (!registry)
                return Result<void>::Success();
            if (const auto report = registry->Publish(std::move(candidate).Value());
                report.status == Assets::AssetRegistryBuildStatus::Failed)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            receipt.registryPublished = true;
            complete = true;
            receipt.status = ProjectAssetImportStatus::ReconciliationRequired;
            if (auto indexed = Assets::AssetIndexStore::SaveAtomically(root / std::filesystem::path{ProjectLayout::AssetIndexPath},
                                                                       registry->Snapshot());
                indexed.HasError())
                return Result<void>::Failure(WrapError(Assets::AssetSourceErrors::DurabilityUnconfirmed, std::move(indexed).ErrorValue()));
            return Result<void>::Success();
        }

        /** @brief Bound aggregate authored bytes and reserve only absent destinations before any native write. */
        class AuthoredPreparation final {
        public:
            explicit AuthoredPreparation(const PreparedAuthoredProjectAssetBatch &batch)
                : batch_(batch), key_(FormatSha256(ComputeSha256(std::as_bytes(std::span(batch.operationId)))).substr(7)) {}

            Result<void> Append(std::filesystem::path path, std::span<const std::byte> bytes) {
                if (bytes.size() > MaximumBatchBytes - total_ || !paths_.insert(path).second)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
                auto absent = Assets::CaptureAssetSource(batch_.projectRoot, path, MaximumBatchBytes);
                if (absent.HasError())
                    return Result<void>::Failure(std::move(absent).ErrorValue());
                if (absent.Value().identity.exists)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
                Entry entry;
                entry.path = std::move(path);
                entry.stage = entry.path.parent_path() / std::format(".horo-import-{}-{}.tmp", key_, entries.size());
                entry.bytes.assign(bytes.begin(), bytes.end());
                entry.identity = {true, bytes.size(), ComputeSha256(bytes)};
                total_ += bytes.size();
                entries.push_back(std::move(entry));
                return Result<void>::Success();
            }

            Result<void> Item(const PreparedAuthoredProjectAsset &item) {
                if (!item.asset.IsValid() || !ids_.insert(item.asset).second || item.type.Value().empty())
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
                const auto path = Foundation::Paths::Resolve(batch_.projectRoot, item.destination);
                // AssetTypeId is a validated canonical identifier, never arbitrary JSON text.
                const std::string metadata = std::format(R"({{"schemaVersion":1,"assetId":"{}","assetType":"{}"}})"
                                                         "\n",
                                                         item.asset.ToString(), item.type.Value());
                auto decoded = Assets::DecodeAssetIdentitySidecar(item.destination.String(), metadata);
                if (decoded.HasError())
                    return Result<void>::Failure(std::move(decoded).ErrorValue());
                expected.push_back(std::move(decoded).Value());
                if (auto payload = Append(path, item.payload); payload.HasError())
                    return payload;
                auto sidecar = path;
                sidecar += ".horo";
                return Append(std::move(sidecar), std::as_bytes(std::span(metadata)));
            }

            std::vector<Entry> entries;
            std::vector<Assets::AssetRecord> expected;

        private:
            const PreparedAuthoredProjectAssetBatch &batch_;
            std::string key_;
            std::set<std::filesystem::path> paths_;
            std::set<Assets::AssetId> ids_;
            std::size_t total_{};
        };

        struct AuthoredSources final {
            std::vector<Entry> entries;
            std::vector<Assets::AssetRecord> expected;
        };

        /** @brief Validate all authored items without synthesizing import provenance. */
        Result<AuthoredSources> PrepareAuthored(const PreparedAuthoredProjectAssetBatch &batch) {
            using Return = Result<AuthoredSources>;
            if (batch.items.empty() || batch.items.size() > MaximumBatchItems || batch.operationId.empty() ||
                batch.operationId.size() > 256)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            AuthoredPreparation preparation(batch);
            preparation.entries.reserve(batch.items.size() * 2);
            preparation.expected.reserve(batch.items.size());
            for (const auto &item : batch.items) {
                auto result = preparation.Item(item);
                if (result.HasError())
                    return Return::Failure(std::move(result).ErrorValue());
            }
            return Return::Success({std::move(preparation.entries), std::move(preparation.expected)});
        }
    }  // namespace

    /** @copydoc ProjectAssetImportCommitter::Commit */
    Result<void> ProjectAssetImportCommitter::Commit(const Assets::PreparedAssetImportBatch &batch, Assets::IAssetIdGenerator &ids,
                                                     const CancellationToken &cancellation) {
        if (busy_)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        BusyScope busy(busy_);
        receipt_ = {};
        try {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Cancelled));
            if (auto lease = mutations_.TryAcquire({batch.projectRoot, Editor::ProjectMutationOwner::Asset, batch.operationId});
                lease.HasValue()) {
                auto prepared = Prepare(batch, ids);
                if (prepared.HasError())
                    return Result<void>::Failure(std::move(prepared).ErrorValue());
                PreparedPublication publication{batch.projectRoot, batch.operationId, std::move(prepared).Value(), {}};
                return PublishPrepared(publication, cancellation);
            } else {
                return Result<void>::Failure(std::move(lease).ErrorValue());
            }
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        } catch (...) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }

    /** @copydoc ProjectAssetImportCommitter::CommitAuthored */
    Result<void> ProjectAssetImportCommitter::CommitAuthored(const PreparedAuthoredProjectAssetBatch &batch,
                                                             const CancellationToken &cancellation) {
        if (busy_)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        BusyScope busy(busy_);
        receipt_ = {};
        try {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Cancelled));
            if (auto lease = mutations_.TryAcquire({batch.projectRoot, Editor::ProjectMutationOwner::Asset, batch.operationId});
                lease.HasValue()) {
                auto prepared = PrepareAuthored(batch);
                if (prepared.HasError())
                    return Result<void>::Failure(std::move(prepared).ErrorValue());
                auto sources = std::move(prepared).Value();
                PreparedPublication publication{batch.projectRoot, batch.operationId, std::move(sources.entries),
                                                std::move(sources.expected)};
                return PublishPrepared(publication, cancellation);
            } else {
                return Result<void>::Failure(std::move(lease).ErrorValue());
            }
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        } catch (...) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }

    /** @brief Shared irreversible transition for imports and newly authored sources; leases outlive compensation. */
    Result<void> ProjectAssetImportCommitter::PublishPrepared(PreparedPublication &publication, const CancellationToken &cancellation) {
        bool complete{};

        struct Cleanup final {
            PreparedPublication &publication;
            DurableFileSystem &files;
            ProjectAssetImportReceipt &receipt;
            bool &complete;

            Cleanup(PreparedPublication &publication, DurableFileSystem &files, ProjectAssetImportReceipt &receipt, bool &complete) noexcept
                : publication(publication), files(files), receipt(receipt), complete(complete) {}

            Cleanup(const Cleanup &) = delete;
            Cleanup &operator=(const Cleanup &) = delete;

            ~Cleanup() {
                if (!complete)
                    Rollback(publication.entries, publication.root, files, receipt);
            }
        };

        Cleanup cleanup{publication, files_, receipt_, complete};

        auto uncertain = MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed);
        try {
            if (auto staged = Stage(publication.entries, publication.root, publication.operation, files_, cancellation); staged.HasError())
                return staged;
            if (auto promoted = Promote(publication.entries, publication.root, files_, receipt_, cancellation); promoted.HasError())
                return promoted;
            if (auto removed = ClearStages(publication.entries, files_); removed.HasError())
                return removed;
            if (auto registry = PublishRegistry(publication.root, registry_, receipt_, complete, publication.expected); registry.HasError())
                return registry;
            receipt_.status = ProjectAssetImportStatus::Published;
            complete = true;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            if (complete)
                return Result<void>::Failure(std::move(uncertain));
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        } catch (...) {
            if (complete)
                return Result<void>::Failure(std::move(uncertain));
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Authority));
        }
    }
}  // namespace Horo::Application
