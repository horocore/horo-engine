#pragma once

/**
 * @file PackageLockfile.h
 * @brief Deterministic bounded package lockfile generation and restore validation.
 */

#include "Horo/Packages/PackageDependencyResolver.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Packages {
    /** @brief Resource ceilings applied before admitting untrusted lockfile content. */
    struct PackageLockfileLimits {
        std::size_t documentBytes{4U * 1024U * 1024U};
        std::size_t packages{512};
        std::size_t roots{512};
        std::size_t dependenciesPerPackage{128};
        std::size_t platformsPerPackage{64};
        std::size_t contributionsPerPackage{256};
    };

    /** @brief Exact artifact and manifest evidence bound to one resolved package. */
    struct PackageLockArtifact {
        HoroPackageId package;
        PackageVersion version;
        HoroPackageSourceId source;
        Sha256Digest artifactDigest;
        Sha256Digest manifestDigest;
        Sha256Digest fileManifestDigest;
        std::uint32_t packageFormatVersion{1};
        std::vector<PackagePlatform> platforms;
        std::vector<std::string> contributions;
    };

    /** @brief Exact package/version edge stored in the immutable resolved graph. */
    struct LockedPackageReference {
        HoroPackageId package;
        PackageVersion version;
        [[nodiscard]] bool operator==(const LockedPackageReference &) const noexcept = default;
    };

    /** @brief Immutable lock entry with portable source and complete integrity evidence. */
    struct LockedPackage {
        HoroPackageId package;
        PackageVersion version;
        HoroPackageSourceId source;
        Sha256Digest artifactDigest;
        Sha256Digest manifestDigest;
        Sha256Digest fileManifestDigest;
        std::uint32_t packageFormatVersion{1};
        std::vector<PackagePlatform> platforms;
        std::vector<LockedPackageReference> dependencies;
        std::vector<std::string> contributions;
    };

    /** @brief Exact effects of withdrawing one root from the locked dependency graph, before checking project and runtime references. */
    struct PackageRemovalGraphPreview final {
        bool directRoot{};
        std::vector<LockedPackageReference> removed;    /**< Packages unreachable from every remaining root, in package-ID order. */
        std::vector<LockedPackageReference> requiredBy; /**< Retained packages with a direct edge to the requested package. */
    };

    /** @brief Strict schema-v1 package lockfile detached from cache, trust and transport state. */
    class ValidatedPackageLockfileV1 final {
    public:
        /**
         * @brief Generates a canonical lockfile from one exact resolver plan and artifact evidence snapshot.
         * @param plan Exact resolver output.
         * @param roots Package identities requested directly by portable project metadata.
         * @param requestHash Digest of the canonical portable dependency request.
         * @param artifacts Complete verified artifact evidence, one item per resolved package.
         * @param limits Caller-owned resource policy.
         * @return Immutable canonical lockfile or a typed failure; no partial value escapes.
         */
        [[nodiscard]] static Result<ValidatedPackageLockfileV1> Generate(const PackageResolutionPlan &plan,
                                                                         std::span<const HoroPackageId> roots,
                                                                         const Sha256Digest &requestHash,
                                                                         std::span<const PackageLockArtifact> artifacts,
                                                                         const PackageLockfileLimits &limits = {});

        /**
         * @brief Parses strict canonical JSON with duplicate-key, exact-shape and resource validation.
         * @param json Complete package lockfile bytes.
         * @param limits Caller-owned resource policy.
         * @return Immutable lockfile or a typed failure; parsing performs no I/O or mutation.
         */
        [[nodiscard]] static Result<ValidatedPackageLockfileV1> Parse(std::string_view json, const PackageLockfileLimits &limits = {});

        /**
         * @brief Validates this frozen graph for one restore request before any cache or project mutation.
         * @param expectedRequestHash Current canonical dependency-request digest.
         * @param platform Exact restore host tuple.
         * @param supportedPackageFormatVersion Package archive format understood by the restoring host.
         * @return Success when the lock is current and every package supports the host, otherwise a typed failure.
         */
        [[nodiscard]] Result<void> ValidateForRestore(const Sha256Digest &expectedRequestHash, const PackagePlatform &platform,
                                                      std::uint32_t supportedPackageFormatVersion = 1U) const;

        /** @brief Serializes the unique canonical JSON representation. @return Stable UTF-8 bytes ending in a newline. */
        [[nodiscard]] std::string SerializeCanonical() const;
        /** @brief Returns the dependency-request digest. @return Immutable request digest. */
        [[nodiscard]] const Sha256Digest &RequestHash() const noexcept;
        /** @brief Returns direct project roots in package-ID order. @return Borrowed immutable root references. */
        [[nodiscard]] std::span<const LockedPackageReference> Roots() const noexcept;
        /** @brief Returns the exact graph in package-ID order. @return Borrowed immutable lock entries. */
        [[nodiscard]] std::span<const LockedPackage> Packages() const noexcept;
        /** @brief Computes what would be pruned if this direct root were withdrawn from the same exact locked graph.
         * @param package Identity present in this lockfile. A transitive-only target cannot be withdrawn directly.
         * @return Deterministic package removals and remaining dependents, or a typed failure for a missing package.
         * @post Does not resolve new versions, change metadata, or authorize removal. */
        [[nodiscard]] Result<PackageRemovalGraphPreview> PreviewRemoval(const HoroPackageId &package) const;

    private:
        friend struct PackageRemovalPlan;

        /** @brief Projects retained lock entries after withdrawing a direct request without resolving or fetching new artifacts.
         * @param package Withdrawn request identity. A missing locked root leaves the graph intact.
         * @param requestHash Canonical digest of the amended portable request.
         * @param removed Exact removals from the current graph preview in package-ID order.
         * @return Exact retained lock graph. Callers must bind it to the amended request before publication. */
        [[nodiscard]] ValidatedPackageLockfileV1 WithRootWithdrawn(const HoroPackageId &package, const Sha256Digest &requestHash,
                                                                   std::span<const LockedPackageReference> removed) const;

        ValidatedPackageLockfileV1(const Sha256Digest &requestHash, std::vector<LockedPackageReference> roots,
                                   std::vector<LockedPackage> packages);

        Sha256Digest m_requestHash;
        std::vector<LockedPackageReference> m_roots;
        std::vector<LockedPackage> m_packages;
    };
}  // namespace Horo::Packages
