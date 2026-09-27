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

    /**
     * @brief Verifies exact final file bytes and rejects undeclared files or symbolic links in a private staging tree.
     * @param root Private, quiescent staging directory containing manifest.json and every declared artifact.
     * @param manifest Expected immutable final manifest.
     * @return Success only when all recorded sizes, hashes, and canonical manifest bytes match the tree.
     */
    [[nodiscard]] Result<void> VerifyReleaseArtifactTree(const std::filesystem::path &root, const ReleaseArtifactManifest &manifest);
}  // namespace Horo::Release
