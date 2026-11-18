#pragma once

/**
 * @file ReleaseArtifactManifest.h
 * @brief Canonical final candidate inventory for exact post-sign release bytes.
 */

#include "Horo/Release/DistributionModel.h"
#include "Horo/Release/ReleaseJobTracker.h"
#include "Horo/Release/ReleasePreflight.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Release {
    /** @brief Runtime-safe artifact classes admitted to a final candidate. */
    enum class ReleaseArtifactRole : std::uint8_t {
        Binary,
        AssetArchive,
        NativePackage,
        Notice,
        Symbols,
        DiagnosticLog
    };

    /** @brief One normalized relative file path with exact final byte evidence. */
    struct ReleaseArtifactRecord final {
        std::string path;
        ReleaseArtifactRole role{ReleaseArtifactRole::Binary};
        std::uint64_t size{};
        Sha256Digest digest;
        bool operator==(const ReleaseArtifactRecord &) const noexcept = default;
    };

    /**
     * @brief Validates the canonical portable path grammar shared by release metadata files.
     * @param path Candidate relative artifact path.
     * @return True only for a bounded, portable path that cannot name manifest.json.
     */
    [[nodiscard]] bool IsValidReleaseArtifactPath(std::string_view path);

    /** @brief Final, non-secret signing evidence for the already signed artifact set. */
    struct ReleaseManifestSigning final {
        std::string algorithm;
        std::string publisher;
        std::string keyId;
        Sha256Digest signedPayloadDigest;
        bool operator==(const ReleaseManifestSigning &) const noexcept = default;
    };

    /** @brief Opaque inert optional extension with an explicit namespace and version. */
    struct ReleaseManifestExtension final {
        std::string name;
        std::uint32_t version{};
        std::string canonicalJson;
        bool operator==(const ReleaseManifestExtension &) const noexcept = default;
    };

    /** @brief Exact post-sign identity and inventory; no filesystem or credential handles are serialized. */
    struct ReleaseArtifactManifestData final {
        ReleaseCandidateId candidate;
        DistributionProductIdentity product;
        ReleaseProductVersion version;
        ReleaseSourceRevision sourceRevision;
        DistributionPlatform platform{DistributionPlatform::Linux};
        DistributionArchitecture architecture{DistributionArchitecture::X64};
        ReleaseBuildConfiguration configuration{ReleaseBuildConfiguration::Shipping};
        DistributionBuildId build;
        std::string toolchainId;
        ReleaseFrozenIdentities frozen;
        std::vector<std::string> runtimeFeatures;
        std::uint32_t assetArchiveFormatVersion{1U};
        std::optional<ReleaseManifestSigning> signing;
        std::vector<ReleaseArtifactRecord> artifacts;
        std::vector<ReleaseManifestExtension> extensions;
    };

    /** @brief Canonical unsigned file inventory captured before byte-changing signing work. */
    class ReleasePreSignInventory final {
    public:
        /**
         * @brief Validates and freezes exact unsigned artifact records separately from final metadata.
         * @param candidate Nonzero candidate identity that owns the private stage.
         * @param artifacts Complete unsigned file inventory.
         * @return Canonical pre-sign inventory or a typed invalid-output failure.
         */
        [[nodiscard]] static Result<ReleasePreSignInventory> Create(ReleaseCandidateId candidate,
                                                                    std::vector<ReleaseArtifactRecord> artifacts);

        /**
         * @brief Parses only exact canonical pre-sign inventory bytes.
         * @param json Complete inventory bytes.
         * @return Validated inventory or a typed invalid-output failure.
         */
        [[nodiscard]] static Result<ReleasePreSignInventory> ParseCanonical(std::string_view json);

        /** @brief Returns the owning candidate. @return Candidate identity. */
        [[nodiscard]] ReleaseCandidateId Candidate() const noexcept;
        /** @brief Returns exact canonical pre-sign bytes. @return Immutable JSON bytes. */
        [[nodiscard]] const std::string &CanonicalJson() const noexcept;
        /** @brief Returns SHA-256 of the canonical inventory. @return Pre-sign inventory digest. */
        [[nodiscard]] const Sha256Digest &Digest() const noexcept;
        /** @brief Returns validated unsigned file records. @return Borrowed immutable inventory. */
        [[nodiscard]] std::span<const ReleaseArtifactRecord> Artifacts() const noexcept;

    private:
        ReleasePreSignInventory(ReleaseCandidateId candidate, std::vector<ReleaseArtifactRecord> artifacts, std::string json,
                                const Sha256Digest &digest);

        ReleaseCandidateId candidate_;
        std::vector<ReleaseArtifactRecord> artifacts_;
        std::string json_;
        Sha256Digest digest_;
    };

    /** @brief Validated canonical schema-v1 final manifest, frozen after byte-changing operations. */
    class ReleaseArtifactManifest final {
    public:
        /**
         * @brief Validates all identities, paths, inventory ordering, and finite bounds before canonicalization.
         * @param data Typed final candidate evidence.
         * @return Immutable manifest or a typed invalid-output failure.
         */
        [[nodiscard]] static Result<ReleaseArtifactManifest> Create(ReleaseArtifactManifestData data);

        /**
         * @brief Parses only exact canonical schema-v1 bytes; unknown required fields fail closed.
         * @param json Complete manifest bytes.
         * @return Validated manifest or a typed invalid-output failure.
         */
        [[nodiscard]] static Result<ReleaseArtifactManifest> ParseCanonical(std::string_view json);

        /** @brief Returns exact canonical bytes. @return Immutable JSON bytes. */
        [[nodiscard]] const std::string &CanonicalJson() const noexcept;
        /** @brief Returns SHA-256 of exact canonical bytes. @return Final manifest digest. */
        [[nodiscard]] const Sha256Digest &Digest() const noexcept;
        /** @brief Returns validated file evidence. @return Borrowed immutable inventory. */
        [[nodiscard]] std::span<const ReleaseArtifactRecord> Artifacts() const noexcept;
        /** @brief Returns all typed final candidate evidence. @return Borrowed immutable evidence. */
        [[nodiscard]] const ReleaseArtifactManifestData &Data() const noexcept;

    private:
        ReleaseArtifactManifest(ReleaseArtifactManifestData data, std::string json, const Sha256Digest &digest);

        ReleaseArtifactManifestData data_;
        std::string json_;
        Sha256Digest digest_;
    };

    /** @brief Reserved root filename for the exact canonical release inventory. */
    inline constexpr std::string_view ReleaseManifestFilename = "manifest.json";
    /** @brief Reserved root filename for the detached release publisher envelope. */
    inline constexpr std::string_view ReleaseManifestSignatureFilename = "manifest.signature.json";
    /** @brief Upper bound for the detached signature document before parsing. */
    inline constexpr std::size_t MaximumReleaseManifestSignatureBytes = 1U << 20U;

    /** @brief Host-supplied cryptographic admission for the canonical manifest and its exact detached envelope bytes. */
    class IReleaseManifestSignatureVerifier {
    public:
        virtual ~IReleaseManifestSignatureVerifier() = default;

        /** @brief Verifies one exact manifest against independently supplied publisher trust.
         * @param canonicalManifest Expected canonical final manifest bytes.
         * @param detachedEnvelope Bounded exact candidate envelope document.
         * @return Success only for an authentic authorized publisher signature over these manifest bytes.
         */
        [[nodiscard]] virtual Result<void> Verify(std::span<const std::byte> canonicalManifest,
                                                  std::span<const std::byte> detachedEnvelope) const = 0;
    };

    /** @brief Identity of the separately authenticated envelope, outside the manifest's artifact list. */
    struct ReleaseManifestSignatureIdentity final {
        std::uint64_t size{};
        Sha256Digest digest;
        bool operator==(const ReleaseManifestSignatureIdentity &) const noexcept = default;
    };

    /**
     * @brief Verifies exact final file bytes and rejects undeclared files or symbolic links in a private staging tree.
     * @param root Private, quiescent staging directory containing manifest.json and every declared artifact.
     * @param manifest Expected immutable final manifest.
     * @details Use this entry point when the host does not require a detached manifest signature. A detached envelope is undeclared here.
     * @return Success only when all recorded sizes, hashes, and canonical manifest bytes match the tree.
     */
    [[nodiscard]] Result<void> VerifyReleaseArtifactTree(const std::filesystem::path &root, const ReleaseArtifactManifest &manifest);

    /** @brief Verifies one signed final tree with its sole detached metadata role.
     * @param root Quiescent final stage with the canonical manifest, detached envelope and every declared artifact.
     * @param manifest Exact caller-selected canonical manifest.
     * @param verifier Host trust boundary whose publisher roots are established independently of the candidate.
     * @return Immutable envelope byte identity after authenticating the manifest and verifying the complete tree.
     */
    [[nodiscard]] Result<ReleaseManifestSignatureIdentity> VerifySignedReleaseArtifactTree(
        const std::filesystem::path &root, const ReleaseArtifactManifest &manifest, const IReleaseManifestSignatureVerifier &verifier);

    /**
     * @brief Verifies every unsigned staged file and rejects undeclared files and symbolic links.
     * @param root Quiescent private stage before signing, without final manifest.json.
     * @param inventory Expected canonical unsigned inventory.
     * @return Success only when the complete tree matches exact recorded sizes and hashes.
     */
    [[nodiscard]] Result<void> VerifyReleaseStagedTree(const std::filesystem::path &root, const ReleasePreSignInventory &inventory);
}  // namespace Horo::Release
