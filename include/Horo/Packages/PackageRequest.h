#pragma once

/** @file PackageRequest.h @brief Bounded inert portable package intent and current-request-to-lock validation. */
#include "Horo/Packages/PackageLockfile.h"

#include <optional>

namespace Horo::Packages {
    /** @brief Portable authority categories; local overrides and transport/trust credentials are not project intent. */
    enum class PortablePackageSourceKind : std::uint8_t {
        PublicRegistry,
        PrivateRegistry,
        StaticIndex,
        DirectArtifact,
        VendoredArtifact
    };

    /** @brief One validated explicit direct dependency, detached from source transport and lifecycle state. */
    struct PortablePackageDependency final {
        PackageDependencyRequest request;
        HoroPackageSourceId source;
        std::optional<Sha256Digest> artifactDigest;
        std::vector<std::string> contributions;
    };

    /** @brief Immutable schema-v1 intent; decoding performs no restore, resolution, trust or activation. */
    class ValidatedPackageRequest final {
    public:
        /** @brief Decodes up to 1 MiB of credential-free portable intent, with at most 128 sources and 512 roots.
         * @param bytes Actual `.horo/packages.json` document. Unknown/duplicate/legacy fields and invalid pins fail.
         * @return Canonically ordered typed intent, or a typed malformed/bounds diagnostic.
         * @details Canonical source declarations, assignments, ranges, features and contribution requests all enter the digest.
         * Optional schemaVersion is accepted only as 1; its omission normalizes to schema 1. */
        [[nodiscard]] static Result<ValidatedPackageRequest> Parse(std::string_view bytes);
        /** @brief Returns the versioned canonical intent digest used by lock generation and freshness checks. @return Owned identity. */
        [[nodiscard]] const Sha256Digest &Digest() const noexcept;
        /** @brief Returns canonical credential-free schema-v1 JSON. @return Borrowed bytes owned by this value. */
        [[nodiscard]] const std::string &SerializeCanonical() const noexcept;
        /** @brief Returns explicitly assigned direct roots in ascending package-ID order. @return Borrowed immutable roots. */
        [[nodiscard]] std::span<const PortablePackageDependency> Dependencies() const noexcept;
        /** @brief Checks current intent digest, exact root membership, version ranges, sources and explicit artifact pins.
         * @param lock Actual validated lockfile; no graph may be inferred from caller-supplied hashes.
         * @return Success or typed stale/incompatible intent diagnostic. Platform/archive verification remains restore-owned. */
        [[nodiscard]] Result<void> ValidateLock(const ValidatedPackageLockfileV1 &lock) const;

    private:
        /** @brief Owns canonical semantic bytes and their immutable digest after successful bounded validation. */
        ValidatedPackageRequest(std::vector<PortablePackageDependency> dependencies, std::string canonical);
        std::vector<PortablePackageDependency> dependencies_;
        std::string canonical_;
        Sha256Digest digest_;
    };
}  // namespace Horo::Packages
