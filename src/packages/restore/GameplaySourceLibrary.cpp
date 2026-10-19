#include "Horo/Packages/GameplaySourceLibrary.h"

#include "Horo/Packages/PackageInstallErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Packages {
    namespace {
        const ErrorDomainId Domain{"horo.packages.gameplay_source"};
        const ErrorCodeDescriptor CompilationDenied{Domain,
                                                    ErrorCode{"compilation_denied"},
                                                    ErrorSeverity::Error,
                                                    "Native source compilation is not approved for this package artifact.",
                                                    "Approve the exact restored artifact before building its native sources.",
                                                    false,
                                                    true};

        /** @brief Requires an explicit source inventory rather than executing package-provided build logic. */
        bool IsNativeSource(const PackagePath &path) {
            const auto &value = path.Value();
            return value.ends_with(".cpp") || value.ends_with(".cc") || value.ends_with(".cxx");
        }

        /** @brief Copies only one explicit contribution, retaining its independently verified inventory metadata. */
        Result<std::vector<PackageFileEntry>> SelectFiles(const ValidatedPackageArchive &archive, const PackagePath &root) {
            std::vector<PackageFileEntry> files;
            std::uint64_t bytes{};
            for (const auto &entry : archive.Manifest().Entries()) {
                if (!entry.contributionRoot || entry.contributionRoot->Value() != root.Value())
                    continue;
                if (!entry.id.path.Value().starts_with(root.Value() + '/') || files.size() >= 256U ||
                    entry.size > 64U * 1024U * 1024U - bytes || entry.id.path.Value().find_first_of(";\r\n") != std::string::npos)
                    return Result<std::vector<PackageFileEntry>>::Failure(MakeError(PackageInstallErrors::InvalidInput));
                bytes += entry.size;
                files.push_back(entry);
            }
            if (files.empty())
                return Result<std::vector<PackageFileEntry>>::Failure(MakeError(PackageInstallErrors::InvalidInput));
            std::ranges::sort(files, {}, [](const auto &entry) -> const std::string & {
                return entry.id.path.Value();
            });
            return Result<std::vector<PackageFileEntry>>::Success(std::move(files));
        }

        /** @brief Binds compiler inputs to package identity, exact archive and canonical source selection. */
        Sha256Digest BuildFingerprint(const PackageRestorePackage &package, const PackagePath &root,
                                      const std::span<const PackagePath> sources) {
            std::string bytes{"horo.gameplay.source.v1\n"};
            for (const auto &value :
                 {package.lock.package.Value(), package.lock.version.ToString(), FormatSha256(package.lock.artifactDigest), root.Value()})
                bytes.append(value).push_back('\0');
            for (const auto &source : sources)
                bytes.append(source.Value()).push_back('\0');
            return ComputeSha256(std::as_bytes(std::span{bytes}));
        }
    }  // namespace

    /** @copydoc GameplaySourceLibrary::Select */
    Result<GameplaySourceLibrary> GameplaySourceLibrary::Select(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                                const HoroPackageId &package, const PackagePath &root,
                                                                std::vector<PackagePath> sources,
                                                                const std::optional<Sha256Digest> &approvedArtifact) {
        if (!install || sources.empty() || sources.size() > 256U)
            return Result<GameplaySourceLibrary>::Failure(MakeError(PackageInstallErrors::InvalidInput));
        const auto &packages = install->Graph()->packages;
        const auto selected = std::ranges::find(packages, package, [](const auto &value) {
            return value.lock.package;
        });
        if (selected == packages.end())
            return Result<GameplaySourceLibrary>::Failure(MakeError(PackageInstallErrors::InvalidInput));
        if (!approvedArtifact || *approvedArtifact != selected->lock.artifactDigest)
            return Result<GameplaySourceLibrary>::Failure(MakeError(CompilationDenied));
        auto files = SelectFiles(*selected->archive, root);
        if (files.HasError())
            return Result<GameplaySourceLibrary>::Failure(files.ErrorValue());
        std::ranges::sort(sources, {}, &PackagePath::Value);
        std::string previous;
        for (const auto &source : sources) {
            if (!IsNativeSource(source) || source.Value() == previous ||
                std::ranges::find(files.Value(), source.Value(), [](const auto &entry) {
                return entry.id.path.Value();
            }) == files.Value().end())
                return Result<GameplaySourceLibrary>::Failure(MakeError(PackageInstallErrors::InvalidInput));
            previous = source.Value();
        }
        const auto fingerprint = BuildFingerprint(*selected, root, sources);
        const auto index = static_cast<std::size_t>(selected - packages.begin());
        return Result<GameplaySourceLibrary>::Success(
            GameplaySourceLibrary{std::move(install), index, root, std::move(sources), std::move(files).Value(), fingerprint});
    }

    /** @copydoc GameplaySourceLibrary::GameplaySourceLibrary */
    GameplaySourceLibrary::GameplaySourceLibrary(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                 const std::size_t packageIndex, PackagePath root, std::vector<PackagePath> sources,
                                                 std::vector<PackageFileEntry> files, const Sha256Digest &fingerprint)
        : install_(std::move(install)), packageIndex_(packageIndex), root_(std::move(root)), sources_(std::move(sources)),
          files_(std::move(files)), fingerprint_(fingerprint) {}

    /** @copydoc GameplaySourceLibrary::Install */
    const std::shared_ptr<const VerifiedPackageInstallRecord> &GameplaySourceLibrary::Install() const noexcept {
        return install_;
    }

    /** @copydoc GameplaySourceLibrary::Package */
    const PackageRestorePackage &GameplaySourceLibrary::Package() const noexcept {
        return install_->Graph()->packages[packageIndex_];
    }

    /** @copydoc GameplaySourceLibrary::Root */
    const PackagePath &GameplaySourceLibrary::Root() const noexcept {
        return root_;
    }

    /** @copydoc GameplaySourceLibrary::Sources */
    std::span<const PackagePath> GameplaySourceLibrary::Sources() const noexcept {
        return sources_;
    }

    /** @copydoc GameplaySourceLibrary::Files */
    std::span<const PackageFileEntry> GameplaySourceLibrary::Files() const noexcept {
        return files_;
    }

    /** @copydoc GameplaySourceLibrary::Fingerprint */
    const Sha256Digest &GameplaySourceLibrary::Fingerprint() const noexcept {
        return fingerprint_;
    }

    /** @copydoc GameplaySourceLibrarySet::GameplaySourceLibrarySet */
    GameplaySourceLibrarySet::GameplaySourceLibrarySet(std::vector<GameplaySourceLibrary> libraries) : libraries_(std::move(libraries)) {}

    /** @copydoc GameplaySourceLibrarySet::Libraries */
    std::span<const GameplaySourceLibrary> GameplaySourceLibrarySet::Libraries() const noexcept {
        return libraries_;
    }
}  // namespace Horo::Packages
