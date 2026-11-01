#include "../AssetErrors.h"
#include "AssetImportFileFacts.h"
#include "AssetReimportPublication.h"
#include "Horo/Foundation/PathUtils.h"

#include <atomic>

namespace Horo::Assets::Detail {
    namespace {
        std::atomic_uint64_t temporarySequence{};

        /** @brief Preserve the existing bounded transaction sibling naming; private creation never overwrites a stale sibling. */
        std::filesystem::path BackupPath(const std::filesystem::path &target) {
            std::filesystem::path name{"."};
            name += target.filename().native();
            name += ".horo-reimport-" + std::to_string(++temporarySequence) + "-original";
            return target.parent_path() / name;
        }

        Result<std::filesystem::path> TargetPath(const AssetReimportRequest &request, const std::filesystem::path &root) {
            using Return = Result<std::filesystem::path>;
            const auto path = NormalizeAssetImportPath(request.absoluteAssetPath);
            const auto assetRoot = NormalizeAssetImportPath(ProjectLayout::AssetRoot(root));
            if (!request.absoluteAssetPath.is_absolute() || !Foundation::Paths::HasPathPrefix(root, path) ||
                !Foundation::Paths::HasPathPrefix(assetRoot, path))
                return Return::Failure(MakeError(AssetErrors::SourceMissing));
            return Return::Success(path);
        }

        Result<ReimportEntry> Entry(const std::filesystem::path &root, std::filesystem::path path, DurableFileSystem &files) {
            using Return = Result<ReimportEntry>;
            auto lockPath = AssetSourcePublicationLockPath(root, path);
            if (lockPath.HasError())
                return Return::Failure(std::move(lockPath).ErrorValue());
            auto lock = files.TryAcquireExclusive(lockPath.Value(), "asset.reimport");
            if (lock.HasError())
                return Return::Failure(std::move(lock).ErrorValue());
            ReimportEntry entry;
            entry.backup = BackupPath(path);
            entry.target = std::move(path);
            entry.lease.emplace(std::move(lock).Value());
            return Return::Success(std::move(entry));
        }

        Result<void> AcquireTargets(ReimportAdmission &admission, const AssetReimportRequest &request) {
            admission.root = NormalizeAssetImportPath(request.absoluteProjectRoot);
            auto target = TargetPath(request, admission.root);
            if (target.HasError())
                return Result<void>::Failure(std::move(target).ErrorValue());
            admission.entries.reserve(2);
            auto payload = Entry(admission.root, target.Value(), *admission.files);
            if (payload.HasError())
                return Result<void>::Failure(std::move(payload).ErrorValue());
            auto metadataPath = target.Value();
            metadataPath += ".horo";
            admission.entries.push_back(std::move(payload).Value());
            auto metadata = Entry(admission.root, std::move(metadataPath), *admission.files);
            if (metadata.HasError())
                return Result<void>::Failure(std::move(metadata).ErrorValue());
            admission.entries.push_back(std::move(metadata).Value());
            return Result<void>::Success();
        }

        Result<const AssetImporterContribution *> Importer(const AssetImporterCatalogSnapshot &catalog,
                                                           const AssetImportMetadata &metadata) {
            using Return = Result<const AssetImporterContribution *>;
            if (metadata.absoluteSourcePath.empty() || !metadata.absoluteSourcePath.is_absolute() ||
                metadata.importerContributionId.empty())
                return Return::Failure(MakeError(AssetErrors::SourceMissing));
            const auto *contribution = catalog.FindById(metadata.importerContributionId);
            if (!contribution || !contribution->strategy || !contribution->HandlesExtension(metadata.sourceExtension))
                return Return::Failure(MakeError(ImportErrors::NoImporter));
            return Return::Success(contribution);
        }

        Result<void> Backup(ReimportAdmission &admission, ReimportEntry &entry, std::span<const std::byte> original,
                            const CancellationToken &cancellation) {
            auto written = admission.files->WritePrivateDurable(entry.backup, original, entry.backupCreated);
            if (written.HasError())
                return written;
            auto verified = InspectReimportAssetSource(admission.root, entry.backup, cancellation);
            if (verified.HasError())
                return Result<void>::Failure(std::move(verified).ErrorValue());
            if (verified.Value() != entry.original)
                return Result<void>::Failure(MakeError(AssetSourceErrors::Conflict));
            return Result<void>::Success();
        }

        /** @brief Backups are written from the admitted captures, never copied from a later potentially replaced source. */
        Result<void> CaptureOriginals(ReimportAdmission &admission, const AssetImporterCatalogSnapshot &catalog,
                                      const CancellationToken &cancellation) {
            auto payload = CaptureReimportAssetSource(admission.root, admission.entries[0].target, cancellation);
            if (payload.HasError())
                return Result<void>::Failure(std::move(payload).ErrorValue());
            auto metadata = CaptureAssetSource(admission.root, admission.entries[1].target, 1024 * 1024, cancellation);
            if (metadata.HasError())
                return Result<void>::Failure(std::move(metadata).ErrorValue());
            if (!payload.Value().identity.exists || !metadata.Value().identity.exists)
                return Result<void>::Failure(MakeError(AssetErrors::SourceMissing));
            admission.entries[0].original = payload.Value().identity;
            admission.entries[1].original = metadata.Value().identity;
            const auto &bytes = metadata.Value().bytes;
            auto parsed = DecodeAssetImportMetadata(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
            if (parsed.HasError())
                return Result<void>::Failure(std::move(parsed).ErrorValue());
            admission.metadata = std::move(parsed).Value();
            auto contribution = Importer(catalog, admission.metadata);
            if (contribution.HasError())
                return Result<void>::Failure(std::move(contribution).ErrorValue());
            admission.contribution = contribution.Value();
            if (auto compatible = admission.contribution->strategy->ValidateReimportDestination(payload.Value().bytes, cancellation);
                compatible.HasError())
                return compatible;
            if (auto saved = Backup(admission, admission.entries[0], payload.Value().bytes, cancellation); saved.HasError())
                return saved;
            return Backup(admission, admission.entries[1], metadata.Value().bytes, cancellation);
        }

        Result<void> StillOriginal(const ReimportAdmission &admission, const CancellationToken &cancellation) {
            for (const auto &entry : admission.entries) {
                auto current = InspectReimportAssetSource(admission.root, entry.target, cancellation);
                if (current.HasError())
                    return Result<void>::Failure(std::move(current).ErrorValue());
                if (current.Value() != entry.original)
                    return Result<void>::Failure(MakeError(AssetSourceErrors::Conflict));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ReimportAdmission::~ReimportAdmission */
    ReimportAdmission::~ReimportAdmission() {
        for (auto &entry : entries) {
            if (!entry.backupCreated)
                continue;
            try {
                static_cast<void>(files->RemoveDurable(entry.backup));
            } catch (...) { /* Private owned cleanup cannot hide an original publication outcome. */
            }
        }
    }

    /** @copydoc AdmitReimport */
    Result<ReimportAdmission> AdmitReimport(const AssetReimportRequest &request, const CancellationToken &cancellation) {
        using Return = Result<ReimportAdmission>;
        if (!request.importerCatalog || !request.registry || !request.files)
            return Return::Failure(MakeError(AssetErrors::IndexIo));
        ReimportAdmission admission;
        admission.files = request.files;
        if (auto targets = AcquireTargets(admission, request); targets.HasError())
            return Return::Failure(std::move(targets).ErrorValue());
        if (auto captured = CaptureOriginals(admission, *request.importerCatalog, cancellation); captured.HasError())
            return Return::Failure(std::move(captured).ErrorValue());
        if (auto current = StillOriginal(admission, cancellation); current.HasError())
            return Return::Failure(std::move(current).ErrorValue());
        return Return::Success(std::move(admission));
    }
}  // namespace Horo::Assets::Detail
