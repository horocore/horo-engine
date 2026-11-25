#include "PackageUpdateReferences.h"

#include "Horo/Packages/PackageMigration.h"

#include <algorithm>
#include <array>
#include <map>

namespace Horo::Application::Internal {
    /** @brief Rejects changes to selected versions, source, policy, or exported contribution classes. */
    static bool CanPreserveAssetReferences(const Packages::PackageUpdateChange &change) {
        if (!change.current)
            return true;
        if (!change.candidate)
            return false;
        const auto &next = *change.candidate;
        auto compatible = *change.current;
        compatible.artifactDigest = next.artifactDigest;
        compatible.fileManifestDigest = next.fileManifestDigest;
        return compatible == next && std::ranges::equal(next.contributions, std::array<std::string_view, 1>{"assets"});
    }

    constexpr std::size_t MaximumUpdateDiagnostics = 64;

    /** @copydoc UpdateNeedsMigration */
    Error UpdateNeedsMigration(std::vector<Diagnostic> diagnostics) {
        std::string message{"Package update requires migration review."};
        if (!diagnostics.empty()) {
            const auto &first = diagnostics.front();
            const auto subject = first.path.empty() ? first.message : first.message + " / " + first.path;
            message += " The first blocker is " + subject + " (" + first.code.Value() + ").";
        }
        return Error{ErrorCode{"packages.host.update_requires_migration_review"}, ErrorDomainId{"packages.host"},
                     ErrorSeverity::Error, std::move(message), std::move(diagnostics)};
    }

    /** @brief Gives one canonical relative package path and its reason; keeps CLI diagnostics bounded. */
    static void ReportUpdateBlocker(std::vector<Diagnostic> &diagnostics, const char *code,
                                    const Packages::HoroPackageId &package, std::string_view path = {}) {
        if (diagnostics.size() >= MaximumUpdateDiagnostics)
            return;
        diagnostics.emplace_back(DiagnosticCode{code}, DiagnosticSeverity::Error, package.Value(), SourceLocation{"packages.host"},
                                 std::string{path});
    }

    /** @brief Computes the first reference-safety difference for one previous archive path. */
    static const char *AssetFileBlocker(const Packages::PackageFileEntry &oldFile, const Packages::PackageFileEntry *candidate) {
        if (!candidate)
            return "packages.host.update_asset_removed";
        if (oldFile.executable || candidate->executable)
            return "packages.host.update_archive_requires_owner";
        const auto contributionPath = [](const Packages::PackageFileEntry &file) -> std::string_view {
            return file.contributionRoot ? std::string_view{file.contributionRoot->Value()} : std::string_view{};
        };
        if (contributionPath(oldFile) != contributionPath(*candidate))
            return "packages.host.update_asset_owner_changed";
        if (oldFile.digest != candidate->digest || oldFile.size != candidate->size)
            return "packages.host.update_asset_content_changed";
        return nullptr;
    }

    /** @copydoc ReviewAssetFiles */
    void ReviewAssetFiles(const Packages::HoroPackageId &package, const Packages::ValidatedPackageArchive &previous,
                          const Packages::ValidatedPackageArchive &candidate, std::vector<Diagnostic> &diagnostics) {
        std::map<std::string_view, const Packages::PackageFileEntry *, std::less<>> byPath;
        for (const auto &file : candidate.Manifest().Entries()) {
            const std::string_view path = file.id.path.Value();
            if (path == Packages::PackageAssetRenameDeclarationPath && !file.executable)
                ReportUpdateBlocker(diagnostics, "packages.host.update_migration_execution_required", package, path);
            else if (file.executable || (path != "horo-package.toml" && !path.starts_with("assets/")))
                ReportUpdateBlocker(diagnostics, "packages.host.update_archive_requires_owner", package, path);
            byPath.emplace(path, &file);
        }
        for (const auto &oldFile : previous.Manifest().Entries()) {
            const auto &path = oldFile.id.path.Value();
            const auto next = byPath.find(path);
            if (const auto *blocked = AssetFileBlocker(oldFile, next == byPath.end() ? nullptr : next->second))
                ReportUpdateBlocker(diagnostics, blocked, package, path);
        }
    }

    /** @copydoc ReadUpdateArchive */
    Result<Packages::ValidatedPackageArchive> ReadUpdateArchive(const Packages::PackageCacheStore &cache,
                                                                const Packages::LockedPackage &locked) {
        using Archive = Result<Packages::ValidatedPackageArchive>;
        auto inspected = cache.InspectArchive(locked.artifactDigest);
        if (inspected.HasError())
            return Archive::Failure(inspected.ErrorValue());
        const auto &archive = inspected.Value();
        if (!archive || archive->PackageManifestDigest() != locked.manifestDigest ||
            archive->Manifest().Digest() != locked.fileManifestDigest)
            return Archive::Failure(Error{ErrorCode{"packages.host.update_archive_evidence_unavailable"}, ErrorDomainId{"packages.host"},
                                          ErrorSeverity::Error, locked.package.Value()});
        auto capture = std::move(inspected).Value();
        return Archive::Success(std::move(*capture));
    }

    /** @copydoc ReviewUpdateLock */
    std::vector<Diagnostic> ReviewUpdateLock(const Packages::PackageUpdatePreview &preview) {
        std::vector<Diagnostic> diagnostics;
        for (const auto &change : preview.changes) {
            if (CanPreserveAssetReferences(change))
                continue;
            const char *reason = "packages.host.update_lock_policy_changed";
            if (!change.candidate)
                reason = "packages.host.update_package_withdrawn";
            else if (change.current->version != change.candidate->version)
                reason = "packages.host.update_version_migration_required";
            ReportUpdateBlocker(diagnostics, reason, change.current->package);
        }
        return diagnostics;
    }

    /** @copydoc ReviewCachedChange */
    Result<void> ReviewCachedChange(const Packages::PackageUpdateChange &change, const Packages::PackageCacheStore &cache,
                                    std::vector<Diagnostic> &diagnostics) {
        if (!change.candidate)
            return Result<void>::Success();
        const auto candidate = ReadUpdateArchive(cache, *change.candidate);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        if (!change.current)
            return Result<void>::Success();
        const auto previous = ReadUpdateArchive(cache, *change.current);
        if (previous.HasError())
            return Result<void>::Failure(previous.ErrorValue());
        const auto renames = Packages::InspectPackageAssetRenames(*change.current, previous.Value(), *change.candidate, candidate.Value());
        if (renames.HasError())
            return Result<void>::Failure(renames.ErrorValue());
        for (const auto &rename : renames.Value()) {
            if (diagnostics.size() == MaximumUpdateDiagnostics)
                break;
            const auto *code = rename.preservesBytes ? "packages.host.update_declared_asset_rename"
                                                     : "packages.host.update_declared_asset_rename_changes_bytes";
            diagnostics.emplace_back(DiagnosticCode{code}, DiagnosticSeverity::Note,
                                     change.current->package.Value() + " -> " + rename.candidate.Value(), SourceLocation{"packages.host"},
                                     rename.previous.Value());
        }
        ReviewAssetFiles(change.current->package, previous.Value(), candidate.Value(), diagnostics);
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
