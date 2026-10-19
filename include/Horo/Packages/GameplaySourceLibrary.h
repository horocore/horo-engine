#pragma once

/** @file GameplaySourceLibrary.h
 * @brief Explicit source-library selection over an installed, immutable package graph.
 */

#include "Horo/Packages/PackageInstall.h"

namespace Horo::Packages {
    /**
     * @brief Sealed source inputs retained from one exact restored and installed package.
     *
     * Selection is a tooling operation, not activation. It does not execute package build scripts,
     * register descriptors or load a binary. The application must obtain compilation approval for
     * the exact artifact independently of restore/install and retain this value through the build.
     */
    class GameplaySourceLibrary final {
    public:
        /**
         * @brief Selects bounded native sources and their declared contribution inventory.
         * @param install Sealed complete install record, mandatory before a build can consume packages.
         * @param package Exact package identity in that record.
         * @param root Explicit contribution root recorded in the verified file inventory.
         * @param sources Nonempty package-relative .cpp/.cc/.cxx files inside that contribution.
         * @param approvedArtifact Exact artifact approved by the host for native source compilation; absence denies selection.
         * @return Owned immutable input lease, or typed missing/install/approval/inventory failure.
         * @details Sources sort by canonical path. At most 256 files and 64 MiB of contribution content are admitted.
         */
        [[nodiscard]] static Result<GameplaySourceLibrary> Select(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                                  const HoroPackageId &package, const PackagePath &root,
                                                                  std::vector<PackagePath> sources,
                                                                  const std::optional<Sha256Digest> &approvedArtifact = std::nullopt);

        /** @brief Returns the sealed graph that owns the exact bytes and dependency versions. @return Retained install lease. */
        [[nodiscard]] const std::shared_ptr<const VerifiedPackageInstallRecord> &Install() const noexcept;
        /** @brief Returns the exact selected package. @return Immutable locked identity and verified archive. */
        [[nodiscard]] const PackageRestorePackage &Package() const noexcept;
        /** @brief Returns the selected contribution root. @return Immutable package-relative root. */
        [[nodiscard]] const PackagePath &Root() const noexcept;
        /** @brief Returns canonical source order. @return Borrowed source paths owned by this value. */
        [[nodiscard]] std::span<const PackagePath> Sources() const noexcept;
        /** @brief Returns the complete selected contribution inventory. @return Borrowed verified file metadata. */
        [[nodiscard]] std::span<const PackageFileEntry> Files() const noexcept;
        /** @brief Returns a path-independent build identity including source selection and archive digest. @return SHA-256 identity. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept;

    private:
        /** @brief Retains only factory-validated source selection and its exact installed graph lease. */
        GameplaySourceLibrary(std::shared_ptr<const VerifiedPackageInstallRecord> install, std::size_t packageIndex, PackagePath root,
                              std::vector<PackagePath> sources, std::vector<PackageFileEntry> files, const Sha256Digest &fingerprint);
        std::shared_ptr<const VerifiedPackageInstallRecord> install_;
        std::size_t packageIndex_{};
        PackagePath root_;
        std::vector<PackagePath> sources_;
        std::vector<PackageFileEntry> files_;
        Sha256Digest fingerprint_;
    };

    /** @brief Immutable owned source selections; application admission enforces aggregate graph and work limits. */
    class GameplaySourceLibrarySet final {
    public:
        /** @brief Freezes sealed selections without copying inventories at subsequent build-request boundaries.
         * @param libraries Owned selections, each independently approved for exact installed artifact bytes.
         * @details Creation performs no build, install, activation or implicit approval. The immutable vector
         * exposes no mutation or borrowed caller storage; application composition retains a shared const owner.
         */
        explicit GameplaySourceLibrarySet(std::vector<GameplaySourceLibrary> libraries);
        ~GameplaySourceLibrarySet() = default;
        GameplaySourceLibrarySet(const GameplaySourceLibrarySet &) = delete;
        GameplaySourceLibrarySet &operator=(const GameplaySourceLibrarySet &) = delete;
        GameplaySourceLibrarySet(GameplaySourceLibrarySet &&) = delete;
        GameplaySourceLibrarySet &operator=(GameplaySourceLibrarySet &&) = delete;
        /** @brief Returns immutable inputs while this owner is retained. @return Borrowed selected library span. */
        [[nodiscard]] std::span<const GameplaySourceLibrary> Libraries() const noexcept;

    private:
        std::vector<GameplaySourceLibrary> libraries_;
    };
}  // namespace Horo::Packages
