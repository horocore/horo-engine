#include "PackageUpdateReferences.h"

#include "Horo/Packages/PackageMigration.h"
#include "PackageBehaviorReferences.h"

#include <algorithm>
#include <array>
#include <map>

namespace Horo::Application::Internal {
    /** @brief Checks the contribution policy admitted by offline package updates. */
    static bool IsAssetOnly(const Packages::LockedPackage &package) {
        return std::ranges::equal(package.contributions, std::array<std::string_view, 1>{"assets"});
    }

    /** @brief Rejects changes to selected versions, source, policy, or exported contribution classes. */
    static bool CanPreserveAssetReferences(const Packages::LockedPackage &current, const Packages::LockedPackage &next) {
        auto compatible = current;
        compatible.artifactDigest = next.artifactDigest;
        compatible.fileManifestDigest = next.fileManifestDigest;
        return compatible == next && IsAssetOnly(next);
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
    void ReviewAddedArchive(const Packages::LockedPackage &candidate, const Packages::ValidatedPackageArchive &archive,
                            std::vector<Diagnostic> &diagnostics) {
        for (const auto &file : archive.Manifest().Entries())
            if (!Packages::IsPackageAssetRenameMetadata(file))
                ReviewCandidateFile(candidate.package, file, diagnostics);
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
        if (renames.HasError())
            return Result<void>::Failure(renames.ErrorValue());
        if (!renames.Value().empty())
            ReportUpdateBlocker(diagnostics, "packages.host.update_migration_execution_required", previous.package,
                                Packages::PackageAssetRenameDeclarationPath);
        ReviewInventory(previous.package, previousArchive, candidateArchive, diagnostics);
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
        for (const auto &name : names) {
            auto from = Gameplay::BehaviorTypeId::Parse(name.previous);
            auto to = Gameplay::BehaviorTypeId::Parse(name.candidate);
            if (from.HasError() || to.HasError())
                return Result<void>::Failure(Error{ErrorCode{"packages.migration.invalid_behavior_rename"},
                                                   ErrorDomainId{"packages.migration"}, ErrorSeverity::Error, previous.package.Value()});
            claims.emplace_back(previous.package, std::move(from).Value(), std::move(to).Value());
        }
        return Result<void>::Success();
    }

    /** @copydoc ReviewCachedChange */
    Result<void> ReviewCachedChange(const Packages::PackageUpdateChange &change, const Packages::PackageCacheStore &cache,
                                    std::vector<Diagnostic> &diagnostics, std::vector<PackageBehaviorMigration> &claims) {
        if (!change.candidate)
            return Result<void>::Success();
        const auto candidate = ReadUpdateArchive(cache, *change.candidate);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        if (!change.current) {
            ReviewAddedArchive(*change.candidate, candidate.Value(), diagnostics);
            return Result<void>::Success();
        }
        const auto previous = ReadUpdateArchive(cache, *change.current);
        if (previous.HasError())
            return Result<void>::Failure(previous.ErrorValue());
        const auto behavior = Packages::InspectPackageBehaviorRenames(*change.current, previous.Value(), *change.candidate, candidate.Value());
        if (behavior.HasError())
            return Result<void>::Failure(behavior.ErrorValue());
        if (auto typed = AppendBehaviorClaims(*change.current, behavior.Value(), claims); typed.HasError())
            return typed;
        return ReviewAssetFiles(*change.current, previous.Value(), *change.candidate, candidate.Value(), diagnostics);
    }
}  // namespace Horo::Application::Internal
