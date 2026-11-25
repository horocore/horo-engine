#include "Horo/Packages/PackageMigration.h"

#include "PackageJsonGuard.h"

#include <algorithm>
#include <array>
#include <set>

namespace Horo::Packages {
    namespace {
        using Json = nlohmann::json;
        constexpr std::size_t MaximumPlanBytes = 64U * 1024U;
        constexpr std::size_t MaximumTransitions = 32;
        constexpr std::size_t MaximumRenames = 128;

        /** @brief Identifies the exact package producing a migration diagnostic. */
        Error PlanError(const char *code, const LockedPackage &package) {
            return {ErrorCode{code}, ErrorDomainId{"packages.migration"}, ErrorSeverity::Error, package.package.Value()};
        }

        /** @brief Recognizes exactly the named fields of one versioned declaration object. */
        bool ExactFields(const Json &value, const std::span<const std::string_view> fields) {
            return value.is_object() && value.size() == fields.size() &&
                   std::ranges::all_of(fields, [&value](std::string_view field) { return value.contains(field); });
        }

        /** @brief Checks that the archive still represents the resolved package entry. */
        bool MatchesArchive(const LockedPackage &locked, const ValidatedPackageArchive &archive) {
            return locked.artifactDigest == archive.Digest() && locked.manifestDigest == archive.PackageManifestDigest() &&
                   locked.fileManifestDigest == archive.Manifest().Digest();
        }

        /** @brief Locates one exact, case-sensitive file identity in the verified inventory. */
        const PackageFileEntry *FindFile(const ValidatedPackageArchive &archive, const std::string_view path) {
            const auto files = archive.Manifest().Entries();
            const auto found = std::ranges::find(files, path, [](const auto &file) -> std::string_view {
                return file.id.path.Value();
            });
            return found == files.end() ? nullptr : &*found;
        }

        /** @brief Allows only existing non-executable source and target assets from the two sealed inventories. */
        std::optional<bool> VerifyAssetMove(const PackagePath &from, const PackagePath &to, const ValidatedPackageArchive &previous,
                                            const ValidatedPackageArchive &candidate) {
            const auto *oldFile = FindFile(previous, from.Value());
            const auto *newFile = FindFile(candidate, to.Value());
            if (!oldFile || !newFile || oldFile->executable || newFile->executable ||
                FindFile(candidate, from.Value()) || FindFile(previous, to.Value()))
                return std::nullopt;
            return oldFile->size == newFile->size && oldFile->digest == newFile->digest;
        }

        /** @brief Validates shape, path namespace and collision rules without reading project documents. */
        Result<std::pair<PackagePath, PackagePath>> DecodeRename(const Json &value, const LockedPackage &package,
                                                               std::set<std::string, std::less<>> &pathKeys) {
            using Rename = Result<std::pair<PackagePath, PackagePath>>;
            constexpr std::array<std::string_view, 2> fields{"from", "to"};
            if (!ExactFields(value, fields))
                return Rename::Failure(PlanError("packages.migration.invalid_declaration", package));
            const auto from = PackagePath::Parse(value.at("from").get_ref<const std::string &>());
            const auto to = PackagePath::Parse(value.at("to").get_ref<const std::string &>());
            if (from.HasError() || to.HasError())
                return Rename::Failure(PlanError("packages.migration.invalid_path", package));
            if (!from.Value().Value().starts_with("assets/") || !to.Value().Value().starts_with("assets/") ||
                !pathKeys.insert(from.Value().CollisionKey()).second || !pathKeys.insert(to.Value().CollisionKey()).second)
                return Rename::Failure(PlanError("packages.migration.invalid_path", package));
            return Rename::Success({from.Value(), to.Value()});
        }

        struct SourceIdentity final {
            PackageVersion version;
            Sha256Digest artifact;
        };

        /** @brief Admits the exact source selectors and a bounded nonempty rename list. */
        Result<SourceIdentity> DecodeSource(const Json &transition, const LockedPackage &package) {
            using Source = Result<SourceIdentity>;
            constexpr std::array<std::string_view, 3> fields{"fromVersion", "fromArtifactSha256", "renames"};
            if (!ExactFields(transition, fields))
                return Source::Failure(PlanError("packages.migration.invalid_declaration", package));
            const auto version = PackageVersion::Parse(transition.at("fromVersion").get_ref<const std::string &>());
            const auto artifact = ParseSha256(transition.at("fromArtifactSha256").get_ref<const std::string &>());
            const auto &renames = transition.at("renames");
            if (version.HasError() || artifact.HasError() || !renames.is_array() || renames.empty() || renames.size() > MaximumRenames)
                return Source::Failure(PlanError("packages.migration.invalid_declaration", package));
            return Source::Success({version.Value(), artifact.Value()});
        }

        /** @brief Checks every scoped path, retaining results only for the exact source artifact. */
        Result<void> DecodeRenames(const Json &renames, const LockedPackage &package, const ValidatedPackageArchive &previous,
                                  const ValidatedPackageArchive &candidate, std::vector<PackageAssetRename> *selected) {
            std::set<std::string, std::less<>> pathKeys;
            for (const auto &value : renames) {
                auto rename = DecodeRename(value, package, pathKeys);
                if (rename.HasError())
                    return Result<void>::Failure(rename.ErrorValue());
                if (!selected)
                    continue;
                auto [from, to] = std::move(rename).Value();
                const auto preservesBytes = VerifyAssetMove(from, to, previous, candidate);
                if (!preservesBytes)
                    return Result<void>::Failure(PlanError("packages.migration.archive_paths_mismatch", package));
                selected->emplace_back(std::move(from), std::move(to), *preservesBytes);
            }
            return Result<void>::Success();
        }

        /** @brief Rejects ambiguous names and oversized values before gameplay interprets stable type identities. */
        Result<void> DecodeBehaviorRenames(const Json &renames, const LockedPackage &package, std::vector<PackageBehaviorRename> *selected) {
            constexpr std::array<std::string_view, 2> fields{"from", "to"};
            std::set<std::string_view> identities;
            for (const auto &value : renames) {
                if (!ExactFields(value, fields))
                    return Result<void>::Failure(PlanError("packages.migration.invalid_declaration", package));
                const auto &from = value.at("from").get_ref<const std::string &>();
                const auto &to = value.at("to").get_ref<const std::string &>();
                if (from.empty() || to.empty() || from.size() > 256 || to.size() > 256 ||
                    !identities.insert(from).second || !identities.insert(to).second)
                    return Result<void>::Failure(PlanError("packages.migration.invalid_behavior_rename", package));
                if (selected)
                    selected->emplace_back(from, to);
            }
            return Result<void>::Success();
        }

        /** @brief Validates the exact destination identity independently of the source transition. */
        bool MatchesDestination(const Json &document, const LockedPackage &candidate) {
            constexpr std::array<std::string_view, 4> fields{"schemaVersion", "packageId", "toVersion", "transitions"};
            return ExactFields(document, fields) &&
                   document.at("schemaVersion").is_number_unsigned() && document.at("schemaVersion") == 1 &&
                   document.at("packageId") == candidate.package.Value() && document.at("toVersion") == candidate.version.ToString() &&
                   document.at("transitions").is_array() && !document.at("transitions").empty() &&
                   document.at("transitions").size() <= MaximumTransitions;
        }

        /** @brief Parses exact scoped transitions using the same strict bounds as package intent and lock files. */
        template <typename Rename, typename Decode>
        Result<std::vector<Rename>> DecodePlan(const std::span<const std::byte> bytes, const LockedPackage &previous,
                                               const LockedPackage &candidate, Decode decode) {
            using Renames = Result<std::vector<Rename>>;
            try {
                Detail::PackageJsonGuard guard;
                const auto *data = reinterpret_cast<const char *>(bytes.data());
                const auto document = Json::parse(data, data + bytes.size(), std::ref(guard), false);
                if (!guard.IsValid() || !MatchesDestination(document, candidate))
                    return Renames::Failure(PlanError("packages.migration.invalid_declaration", candidate));
                std::set<std::pair<std::string, std::string>> selectors;
                std::vector<Rename> renamed;
                for (const auto &transition : document.at("transitions")) {
                    const auto source = DecodeSource(transition, candidate);
                    if (source.HasError())
                        return Renames::Failure(source.ErrorValue());
                    const auto &identity = source.Value();
                    if (!selectors.emplace(identity.version.ToString(), FormatSha256(identity.artifact)).second)
                        return Renames::Failure(PlanError("packages.migration.duplicate_transition", candidate));
                    const bool match = identity.version == previous.version && identity.artifact == previous.artifactDigest;
                    if (auto decoded = decode(transition.at("renames"), candidate, match ? &renamed : nullptr);
                        decoded.HasError())
                        return Renames::Failure(decoded.ErrorValue());
                }
                return Renames::Success(std::move(renamed));
            } catch (const Json::exception &) {
                return Renames::Failure(PlanError("packages.migration.invalid_declaration", candidate));
            }
        }

        /** @brief Admits a bounded inert declaration from the two exact locked archives. */
        template <typename Rename, typename Decode>
        Result<std::vector<Rename>> ReadDeclaration(const std::string_view path, const LockedPackage &previous,
                                                     const ValidatedPackageArchive &previousArchive, const LockedPackage &candidate,
                                                     const ValidatedPackageArchive &candidateArchive, Decode decode) {
            using Renames = Result<std::vector<Rename>>;
            if (previous.package != candidate.package || !MatchesArchive(previous, previousArchive) ||
                !MatchesArchive(candidate, candidateArchive))
                return Renames::Failure(PlanError("packages.migration.archive_evidence_invalid", candidate));
            const auto *declared = FindFile(candidateArchive, path);
            if (!declared)
                return Renames::Success({});
            if (declared->executable || declared->contributionRoot || declared->size == 0)
                return Renames::Failure(PlanError("packages.migration.archive_evidence_invalid", candidate));
            const auto bytes = candidateArchive.ReadDeclaredFile(declared->id.path, MaximumPlanBytes);
            if (bytes.HasError())
                return Renames::Failure(WithCause(PlanError("packages.migration.declaration_unreadable", candidate), bytes.ErrorValue()));
            return DecodePlan<Rename>(bytes.Value(), previous, candidate, decode);
        }
    }  // namespace

    /** @copydoc IsPackageAssetRenameMetadata */
    bool IsPackageAssetRenameMetadata(const PackageFileEntry &file) {
        return file.id.path.Value() == PackageAssetRenameDeclarationPath && !file.executable && !file.contributionRoot;
    }

    /** @copydoc InspectPackageAssetRenames */
    Result<std::vector<PackageAssetRename>> InspectPackageAssetRenames(const LockedPackage &previous,
                                                                      const ValidatedPackageArchive &previousArchive,
                                                                      const LockedPackage &candidate,
                                                                      const ValidatedPackageArchive &candidateArchive) {
        return ReadDeclaration<PackageAssetRename>(
            PackageAssetRenameDeclarationPath, previous, previousArchive, candidate, candidateArchive,
            [&](const Json &renames, const LockedPackage &package, std::vector<PackageAssetRename> *selected) {
                return DecodeRenames(renames, package, previousArchive, candidateArchive, selected);
            });
    }

    /** @copydoc InspectPackageBehaviorRenames */
    Result<std::vector<PackageBehaviorRename>> InspectPackageBehaviorRenames(const LockedPackage &previous,
                                                                            const ValidatedPackageArchive &previousArchive,
                                                                            const LockedPackage &candidate,
                                                                            const ValidatedPackageArchive &candidateArchive) {
        return ReadDeclaration<PackageBehaviorRename>(PackageBehaviorRenameDeclarationPath, previous, previousArchive, candidate,
                                                     candidateArchive, DecodeBehaviorRenames);
    }
}  // namespace Horo::Packages
