#pragma once

/**
 * @file ReleaseCandidateVerification.h
 * @brief Exact final candidate verification before host-specific package smoke checks.
 */

#include "Horo/Release/ReleaseArtifactManifest.h"

#include <cstdint>
#include <filesystem>
#include <span>

namespace Horo::Release {
    enum class ReleaseCandidateSmokeKind : std::uint8_t;
    class IReleaseCandidateSignatureVerifier;
    class IReleaseCandidateSmokeProbe;

    /** @brief Candidate identity issued only after complete final verification. */
    class VerifiedReleaseCandidate final {
    public:
        /** @brief Returns the verified candidate identity. @return Service candidate ID. */
        [[nodiscard]] ReleaseCandidateId Candidate() const noexcept;
        /** @brief Returns the canonical manifest digest. @return Exact finalized metadata identity. */
        [[nodiscard]] const Sha256Digest &ManifestDigest() const noexcept;
        /** @brief Returns the checked directory. @return Borrowed immutable path. */
        [[nodiscard]] const std::filesystem::path &Root() const noexcept;

    private:
        friend Result<VerifiedReleaseCandidate> VerifyReleaseCandidate(const std::filesystem::path &, const ReleaseArtifactManifest &,
                                                                       std::span<const ReleaseCandidateSmokeKind>,
                                                                       IReleaseCandidateSignatureVerifier *,
                                                                       std::span<IReleaseCandidateSmokeProbe *const>);
        /** @brief Issued only by the complete final verifier. */
        VerifiedReleaseCandidate(ReleaseCandidateId candidate, const Sha256Digest &manifestDigest, std::filesystem::path root);

        ReleaseCandidateId candidate_;
        Sha256Digest manifestDigest_;
        std::filesystem::path root_;
    };

    /** @brief Host-specific smoke checks selected by release policy. */
    enum class ReleaseCandidateSmokeKind : std::uint8_t {
        ArchiveReadable,
        RuntimeAssets,
        InstallAndLaunch,
        Compatibility
    };

    /** @brief Verifies a declared final signature using a host-owned platform trust boundary. */
    class IReleaseCandidateSignatureVerifier {
    public:
        virtual ~IReleaseCandidateSignatureVerifier() = default;

        /**
         * @brief Checks the final packaged bytes against declared platform signature evidence.
         * @param root Quiescent candidate directory.
         * @param manifest Final canonical manifest.
         * @return Success only when the platform trust result matches the exact candidate.
         */
        [[nodiscard]] virtual Result<void> Verify(const std::filesystem::path &root, const ReleaseArtifactManifest &manifest) = 0;
    };

    /** @brief One host-installed package smoke checker with a single declared capability. */
    class IReleaseCandidateSmokeProbe {
    public:
        virtual ~IReleaseCandidateSmokeProbe() = default;

        /** @brief Reports the owned smoke kind. @return Exact capability. */
        [[nodiscard]] virtual ReleaseCandidateSmokeKind Kind() const noexcept = 0;

        /**
         * @brief Checks the already frozen candidate without changing it.
         * @param root Quiescent candidate directory.
         * @param manifest Final canonical manifest.
         * @return Success or a typed verification failure.
         */
        [[nodiscard]] virtual Result<void> Check(const std::filesystem::path &root, const ReleaseArtifactManifest &manifest) = 0;
    };

    /**
     * @brief Verifies exact final bytes, required platform signature, and every selected smoke check.
     * @param root Quiescent final candidate directory.
     * @param manifest Final post-sign manifest selected for publication.
     * @param requiredSmoke Policy-selected required smoke capabilities; duplicates fail closed.
     * @param signatureVerifier Required when the manifest declares signing.
     * @param probes Host-installed smoke adapters; missing or duplicate required kinds fail closed.
     * @return Success only after every required check passes on the exact candidate.
     */
    [[nodiscard]] Result<VerifiedReleaseCandidate> VerifyReleaseCandidate(const std::filesystem::path &root,
                                                                          const ReleaseArtifactManifest &manifest,
                                                                          std::span<const ReleaseCandidateSmokeKind> requiredSmoke,
                                                                          IReleaseCandidateSignatureVerifier *signatureVerifier,
                                                                          std::span<IReleaseCandidateSmokeProbe *const> probes);
}  // namespace Horo::Release
