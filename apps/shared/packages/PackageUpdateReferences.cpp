#include "PackageUpdateReferences.h"

#include "Horo/Packages/PackageMigration.h"
#include "PackageBehaviorReferences.h"

#include <algorithm>
#include <array>
#include <map>

namespace Horo::Application::Internal {
    /** @copydoc IsAssetOnly */
    bool IsAssetOnly(const Packages::LockedPackage &package) {
        return std::ranges::equal(package.contributions, std::array<std::string_view, 1>{"assets"});
    }

    /** @brief Binds inventory evidence to all three identities in the reviewed lock entry. */
    static bool MatchesArchive(const Packages::LockedPackage &package, const Packages::ValidatedPackageArchive &archive) {
        return package.artifactDigest == archive.Digest() && package.manifestDigest == archive.PackageManifestDigest() &&
               package.fileManifestDigest == archive.Manifest().Digest();
    }

    /** @brief Admits asset-only version or archive changes for exact inventory review. */
    static bool CanPreserveAssetReferences(const Packages::LockedPackage &current, const Packages::LockedPackage &next) {
        auto compatible = current;
        compatible.version = next.version;
        compatible.artifactDigest = next.artifactDigest;
        compatible.fileManifestDigest = next.fileManifestDigest;
        return compatible == next && IsAssetOnly(next);
    }

    /** @copydoc CanRestoreRetainedAsset */
    bool CanRestoreRetainedAsset(const Packages::LockedPackage &current, const Packages::LockedPackage &candidate) {
        return current.version == candidate.version && CanPreserveAssetReferences(current, candidate);
    }

    /** @copydoc UpdateNeedsMigration */
    Error UpdateNeedsMigration(std::vector<Diagnostic> diagnostics) {
        std::string message{"Package update requires migration review."};
        const auto first = std::ranges::find(diagnostics, DiagnosticSeverity::Error, &Diagnostic::severity);
        if (first != diagnostics.end()) {
            const auto subject = first->path.empty() ? first->message : first->message + " / " + first->path;
            message += " The first blocker is " + subject + " (" + first->code.Value() + ").";
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

    /** @brief Records which side of a change lacks archive evidence without ending the review. */
    static void ReportMissingUpdateArchive(std::vector<Diagnostic> &diagnostics, const Packages::LockedPackage &locked,
                                           std::string_view side, const Error &failure, std::optional<Error> &firstFailure) {
        if (!firstFailure)
            firstFailure.emplace(failure);
        if (diagnostics.size() >= MaximumUpdateDiagnostics)
            return;
        diagnostics.emplace_back(DiagnosticCode{"packages.host.update_archive_evidence_unavailable"}, DiagnosticSeverity::Error,
                                 std::string{side} + " archive: " + failure.code.Value(), SourceLocation{"packages.lock"},
                                 locked.package.Value());
    }

    /** @brief Records the rejected declaration without suppressing independent evidence. */
    static void ReportMigrationFailure(std::vector<Diagnostic> &diagnostics, const Packages::LockedPackage &candidate,
                                       std::string_view declaration, Error failure, std::optional<Error> &firstFailure) {
        if (diagnostics.size() < MaximumUpdateDiagnostics)
            diagnostics.emplace_back(DiagnosticCode{failure.code.Value()}, DiagnosticSeverity::Error, candidate.package.Value(),
                                     SourceLocation{"packages.host"}, std::string{declaration});
        if (!firstFailure)
            firstFailure.emplace(std::move(failure));
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

    /** @brief Identifies candidate files whose contribution needs a live package owner. */
    static void ReviewCandidateFile(const Packages::HoroPackageId &package, const Packages::PackageFileEntry &file,
                                    std::vector<Diagnostic> &diagnostics) {
        const auto &path = file.id.path.Value();
        if (file.executable || (path != "horo-package.toml" && !path.starts_with("assets/")))
            ReportUpdateBlocker(diagnostics, "packages.host.update_archive_requires_owner", package, path);
    }

    /** @copydoc ReviewAddedArchive */
    Result<void> ReviewAddedArchive(const Packages::LockedPackage &candidate, const Packages::ValidatedPackageArchive &archive,
                                    std::vector<Diagnostic> &diagnostics) {
        auto metadata = Packages::ValidatePackageAssetRenameDeclaration(candidate, archive);
        if (metadata.HasValue() || MatchesArchive(candidate, archive)) {
            for (const auto &file : archive.Manifest().Entries())
                if (!Packages::IsPackageAssetRenameMetadata(file))
                    ReviewCandidateFile(candidate.package, file, diagnostics);
        }
        return metadata;
    }

    /** @brief Checks the paths that could be visible to existing package consumers. */
    static void ReviewInventory(const Packages::HoroPackageId &package, const Packages::ValidatedPackageArchive &previous,
                                const Packages::ValidatedPackageArchive &candidate, std::vector<Diagnostic> &diagnostics) {
        std::map<std::string_view, const Packages::PackageFileEntry *, std::less<>> byPath;
        for (const auto &file : candidate.Manifest().Entries()) {
            const std::string_view path = file.id.path.Value();
            if (Packages::IsPackageAssetRenameMetadata(file))
                continue;
            ReviewCandidateFile(package, file, diagnostics);
            byPath.emplace(path, &file);
        }
        for (const auto &oldFile : previous.Manifest().Entries()) {
            if (Packages::IsPackageAssetRenameMetadata(oldFile))
                continue;
            const auto &path = oldFile.id.path.Value();
            const auto next = byPath.find(path);
            if (const auto *blocked = AssetFileBlocker(oldFile, next == byPath.end() ? nullptr : next->second))
                ReportUpdateBlocker(diagnostics, blocked, package, path);
        }
    }

    /** @copydoc ReviewAssetFiles */
    Result<void> ReviewAssetFiles(const Packages::LockedPackage &previous, const Packages::ValidatedPackageArchive &previousArchive,
                                  const Packages::LockedPackage &candidate, const Packages::ValidatedPackageArchive &candidateArchive,
                                  std::vector<Diagnostic> &diagnostics) {
        const auto renames = Packages::InspectPackageAssetRenames(previous, previousArchive, candidate, candidateArchive);
        if (renames.HasValue() && !renames.Value().empty())
            ReportUpdateBlocker(diagnostics, "packages.host.update_migration_execution_required", previous.package,
                                Packages::PackageAssetRenameDeclarationPath);
        if (renames.HasValue() || (previous.package == candidate.package && MatchesArchive(previous, previousArchive) &&
                                  MatchesArchive(candidate, candidateArchive)))
            ReviewInventory(previous.package, previousArchive, candidateArchive, diagnostics);
        if (renames.HasError())
            return Result<void>::Failure(renames.ErrorValue());
        for (const auto &rename : renames.Value()) {
            if (diagnostics.size() >= MaximumUpdateDiagnostics)
                break;
            const auto *code = rename.preservesBytes ? "packages.host.update_declared_asset_rename"
                                                     : "packages.host.update_declared_asset_rename_changes_bytes";
            diagnostics.emplace_back(DiagnosticCode{code}, DiagnosticSeverity::Note,
                                     previous.package.Value() + " -> " + rename.candidate.Value(), SourceLocation{"packages.host"},
                                     rename.previous.Value());
        }
        return Result<void>::Success();
    }

    /** @copydoc ReadUpdateArchive */
    Result<Packages::ValidatedPackageArchive> ReadUpdateArchive(const Packages::PackageCacheStore &cache,
                                                                const Packages::LockedPackage &locked) {
        using Archive = Result<Packages::ValidatedPackageArchive>;
        auto inspected = cache.InspectArchive(locked.artifactDigest);
        if (inspected.HasError())
            return Archive::Failure(inspected.ErrorValue());
        const auto &archive = inspected.Value();
        if (!archive || !MatchesArchive(locked, *archive))
            return Archive::Failure(Error{ErrorCode{"packages.host.update_archive_evidence_unavailable"}, ErrorDomainId{"packages.host"},
                                          ErrorSeverity::Error, locked.package.Value()});
        auto capture = std::move(inspected).Value();
        return Archive::Success(std::move(*capture));
    }

    /** @copydoc ReviewUpdateLock */
    std::vector<Diagnostic> ReviewUpdateLock(const Packages::PackageUpdatePreview &preview) {
        std::vector<Diagnostic> diagnostics;
        for (const auto &change : preview.changes) {
            if (!change.current) {
                if (change.candidate && !IsAssetOnly(*change.candidate))
                    ReportUpdateBlocker(diagnostics, "packages.host.update_archive_requires_owner", change.candidate->package);
                continue;
            }
            if (change.candidate && CanPreserveAssetReferences(*change.current, *change.candidate))
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

    /** @brief Binds names from the archive to Gameplay type identities and bounds the project-wide scan. */
    static Result<void> AppendBehaviorClaims(const Packages::LockedPackage &previous,
                                             std::span<const Packages::PackageBehaviorRename> names,
                                             std::vector<PackageBehaviorMigration> &claims) {
        constexpr std::size_t maximumClaims = 128;
        if (names.size() > maximumClaims - claims.size())
            return Result<void>::Failure(Error{ErrorCode{"packages.migration.behavior_rename_limit"}, ErrorDomainId{"packages.migration"}});
        const auto originalCount = std::ranges::ssize(claims);
        for (const auto &name : names) {
            auto from = Gameplay::BehaviorTypeId::Parse(name.previous);
            auto to = Gameplay::BehaviorTypeId::Parse(name.candidate);
            if (from.HasError() || to.HasError()) {
                claims.erase(claims.begin() + originalCount, claims.end());
                return Result<void>::Failure(Error{ErrorCode{"packages.migration.invalid_behavior_rename"},
                                                   ErrorDomainId{"packages.migration"}, ErrorSeverity::Error, previous.package.Value()});
            }
            claims.emplace_back(previous.package, std::move(from).Value(), std::move(to).Value());
        }
        return Result<void>::Success();
    }

    /** @copydoc ReviewCachedChange */
    void ReviewCachedChange(const Packages::PackageUpdateChange &change, const Packages::PackageCacheStore &cache,
                            std::vector<Diagnostic> &diagnostics, std::vector<PackageBehaviorMigration> &claims,
                            std::optional<Error> &firstFailure) {
        if (!change.candidate)
            return;
        const auto candidate = ReadUpdateArchive(cache, *change.candidate);
        if (candidate.HasError())
            ReportMissingUpdateArchive(diagnostics, *change.candidate, "Candidate", candidate.ErrorValue(), firstFailure);
        const auto reportAssets = [&](Result<void> reviewed) {
            if (reviewed.HasError())
                ReportMigrationFailure(diagnostics, *change.candidate, Packages::PackageAssetRenameDeclarationPath,
                                       std::move(reviewed).ErrorValue(), firstFailure);
        };
        if (!change.current) {
            if (candidate.HasValue())
                reportAssets(ReviewAddedArchive(*change.candidate, candidate.Value(), diagnostics));
            return;
        }
        const auto previous = ReadUpdateArchive(cache, *change.current);
        if (previous.HasError())
            ReportMissingUpdateArchive(diagnostics, *change.current, "Current", previous.ErrorValue(), firstFailure);
        if (previous.HasError() || candidate.HasError())
            return;
        const auto behavior = Packages::InspectPackageBehaviorRenames(*change.current, previous.Value(), *change.candidate, candidate.Value());
        auto reviewed = behavior.HasError() ? Result<void>::Failure(behavior.ErrorValue())
                                            : AppendBehaviorClaims(*change.current, behavior.Value(), claims);
        if (reviewed.HasError())
            ReportMigrationFailure(diagnostics, *change.candidate, Packages::PackageBehaviorRenameDeclarationPath,
                                   std::move(reviewed).ErrorValue(), firstFailure);
        reportAssets(ReviewAssetFiles(*change.current, previous.Value(), *change.candidate, candidate.Value(), diagnostics));
    }
}  // namespace Horo::Application::Internal
