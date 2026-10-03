#pragma once

/**
 * @file UpdateManifest.h
 * @brief Canonical signed update metadata and fail-closed admission contract.
 */

#include "Horo/Release/DistributionModel.h"
#include "Horo/Security/ArtifactSignature.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Release {
    class UpdateTrustRootSnapshot;

    /** @brief One exact downloadable package named by authenticated update metadata. */
    struct UpdatePackageRecord final {
        DistributionPackageSelection selection;
        std::string url;
        std::uint64_t size{};
        Sha256Digest digest;
        Security::DetachedSignatureEnvelope signature;
    };

    /** @brief Signed delta artifact bound to one full package and exact base, patch, and target file inventories. */
    struct UpdateDeltaPackageRecord final {
        UpdatePackageRecord package;
        DistributionPackageId fullPackage;
        Sha256Digest baseInventoryDigest;
        Sha256Digest deltaInventoryDigest;
        Sha256Digest targetInventoryDigest;
    };

    /** @brief Complete unsigned manifest payload; signature is kept outside these canonical bytes. */
    struct UpdateManifestData final {
        DistributionProductIdentity product;
        ReleaseProductVersion version;
        DistributionBuildId build;
        std::string channel;
        std::uint64_t sequence{};
        std::uint64_t publishedAt{}; /**< Unix seconds in UTC. */
        std::uint64_t expiresAt{};   /**< Unix seconds in UTC. */
        std::uint32_t minimumUpdaterVersion{};
        std::uint64_t minimumRootRevision{};
        std::optional<ReleaseProductVersion> minimumAllowedVersion;
        std::string releaseNotes;                      /**< Optional signed plain-text notes, bounded to 32 KiB. */
        std::vector<std::string> compatibilityImpacts; /**< Optional signed user-visible impact summaries. */
        std::vector<UpdatePackageRecord> packages;
        std::vector<UpdateDeltaPackageRecord> deltas; /**< Empty for schema-v1 full-package-only manifests. */
    };

    /**
     * @brief Canonicalizes validated unsigned metadata for a host-owned signer.
     * @param data Complete typed metadata including package signature evidence.
     * @return Exact payload bytes to hash and sign, or a typed invalid-metadata error.
     */
    [[nodiscard]] Result<std::string> BuildCanonicalUpdatePayload(const UpdateManifestData &data);

    /** @brief Exact canonical signed document parsed under fixed schema and resource bounds. */
    class SignedUpdateManifest final {
    public:
        /**
         * @brief Validates typed data and builds canonical signed document bytes.
         * @param data Complete unsigned payload.
         * @param signature Detached signature over canonical unsigned payload bytes.
         * @return Validated document or a typed malformed-metadata error.
         */
        [[nodiscard]] static Result<SignedUpdateManifest> Create(UpdateManifestData data, Security::DetachedSignatureEnvelope signature);

        /**
         * @brief Parses exact canonical schema-v1 or schema-v2 JSON, rejecting duplicate keys and excess resources.
         * @param bytes Complete signed document bytes.
         * @return Validated document or a typed malformed-metadata error.
         */
        [[nodiscard]] static Result<SignedUpdateManifest> ParseCanonical(std::string_view bytes);

        /** @brief Returns immutable typed payload. @return Borrowed payload. */
        [[nodiscard]] const UpdateManifestData &Data() const noexcept;
        /** @brief Returns exact authenticated payload bytes. @return Borrowed canonical JSON. */
        [[nodiscard]] const std::string &CanonicalPayload() const noexcept;
        /** @brief Returns exact signed document bytes. @return Borrowed canonical JSON. */
        [[nodiscard]] const std::string &CanonicalDocument() const noexcept;
        /** @brief Returns detached metadata signature. @return Borrowed envelope. */
        [[nodiscard]] const Security::DetachedSignatureEnvelope &Signature() const noexcept;

    private:
        SignedUpdateManifest(UpdateManifestData data, Security::DetachedSignatureEnvelope signature, std::string payload,
                             std::string document);

        UpdateManifestData data_;
        Security::DetachedSignatureEnvelope signature_;
        std::string payload_;
        std::string document_;
    };

    /** @brief Host-owned state against which an update is admitted. */
    struct UpdateAdmissionContext final {
        DistributionProductIdentity installedProduct;
        ReleaseProductVersion installedVersion;
        std::string channel;
        DistributionPlatform platform{DistributionPlatform::Linux};
        DistributionArchitecture architecture{DistributionArchitecture::X64};
        std::uint32_t updaterVersion{};
        std::uint64_t minimumAcceptedSequence{};
        std::uint64_t now{}; /**< Unix seconds in UTC from a trusted host clock. */
        bool authorizedDowngrade{};
    };

    /** @brief Explicit administrator exception for offline manifest expiry; trust roots never receive an expiry exception. */
    struct UpdateMetadataFreshnessPolicy final {
        std::uint64_t maximumExpiredManifestSeconds{}; /**< Zero requires fresh metadata. */
    };

    /**
     * @brief Authenticates metadata before applying freshness, product, platform and rollback policy.
     * @param manifest Canonical signed document.
     * @param context Installed product and monotonic trusted state.
     * @param roots Installed versioned trust roots for this product.
     * @param provider Host-composed signature provider; null fails closed.
     * @param freshness Explicit bounded offline expiry exception; zero keeps the default strict policy.
     * @return Success only for authenticated, policy-fresh, matching and admitted metadata.
     */
    [[nodiscard]] Result<void> VerifyUpdateManifest(const SignedUpdateManifest &manifest, const UpdateAdmissionContext &context,
                                                    const UpdateTrustRootSnapshot &roots,
                                                    std::shared_ptr<const Security::SignatureProvider> provider,
                                                    UpdateMetadataFreshnessPolicy freshness = {});

    /** @brief Authenticated full package with an optional applicable delta optimization. */
    struct UpdatePackageCandidates final {
        UpdatePackageRecord full;
        std::optional<UpdateDeltaPackageRecord> delta;
    };

    /** @brief Bounded package-attempt progression after authenticated candidate selection. */
    enum class UpdatePackageAttempt : std::uint8_t {
        Initial,
        AfterDeltaFailure,
        AfterFullFailure
    };

    /**
     * @brief Prefers an applicable delta, then falls back once to its signed allowed full package.
     * @param candidates Previously authenticated full and optional delta candidates.
     * @param attempt Initial selection or outcome of the immediately preceding failed attempt.
     * @return Next package to download or stage; empty after a full-package failure.
     * @note The host does not call this after cancellation or success. It still owns transfer and staging execution.
     */
    [[nodiscard]] std::optional<UpdatePackageRecord> PlanUpdatePackageAttempt(const UpdatePackageCandidates &candidates,
                                                                              UpdatePackageAttempt attempt);

    /**
     * @brief Selects a signed full package and only a delta bound to the verified base inventory.
     * @param manifest Canonical signed update metadata.
     * @param context Installed product, target, freshness, and rollback admission state.
     * @param roots Installed versioned trust roots.
     * @param provider Host-composed signature provider.
     * @param fullPackage ID of the allowed full package for this installation.
     * @param baseInventoryDigest Canonical inventory digest of the verified installed base; zero selects full only.
     * @return Authenticated full candidate and optional applicable delta, or an admission failure.
     */
    [[nodiscard]] Result<UpdatePackageCandidates> SelectUpdatePackageCandidates(const SignedUpdateManifest &manifest,
                                                                                const UpdateAdmissionContext &context,
                                                                                const UpdateTrustRootSnapshot &roots,
                                                                                std::shared_ptr<const Security::SignatureProvider> provider,
                                                                                const DistributionPackageId &fullPackage,
                                                                                const Sha256Digest &baseInventoryDigest);

    /**
     * @brief Verifies exact downloaded bytes against the selected signed package record.
     * @param package Manifest-declared package and signature.
     * @param bytes Complete downloaded bytes.
     * @param verifier Host-composed signature verifier and trust roots.
     * @return Success only when size, hash and publisher signature all match.
     */
    [[nodiscard]] Result<void> VerifyUpdatePackage(const UpdatePackageRecord &package, std::span<const std::byte> bytes,
                                                   const Security::ArtifactVerifier &verifier);
}  // namespace Horo::Release
