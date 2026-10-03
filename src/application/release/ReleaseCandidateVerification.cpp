#include "Horo/Release/ReleaseCandidateVerification.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Requires a nonempty set of distinct host smoke capabilities. */
        [[nodiscard]] bool ValidRequiredSmoke(const std::span<const ReleaseCandidateSmokeKind> requiredSmoke) {
            if (requiredSmoke.empty())
                return false;
            for (std::size_t index = 0; index < requiredSmoke.size(); ++index) {
                const auto prefixEnd = requiredSmoke.begin() + static_cast<std::ptrdiff_t>(index);
                if (std::ranges::find(requiredSmoke.begin(), prefixEnd, requiredSmoke[index]) != prefixEnd)
                    return false;
            }
            return true;
        }

        /** @brief Checks platform signature evidence only when the final manifest requires it. */
        [[nodiscard]] Result<void> VerifyCandidateSignature(const std::filesystem::path &root, const ReleaseArtifactManifest &manifest,
                                                            IReleaseCandidateSignatureVerifier *const verifier) {
            if (!manifest.Data().signing)
                return Result<void>::Success();
            if (verifier == nullptr)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return verifier->Verify(root, manifest);
        }

        /** @brief Selects exactly one installed probe for one required smoke capability. */
        [[nodiscard]] Result<void> CheckCandidateSmoke(const ReleaseCandidateSmokeKind kind, const std::filesystem::path &root,
                                                       const ReleaseArtifactManifest &manifest,
                                                       const std::span<IReleaseCandidateSmokeProbe *const> probes) {
            IReleaseCandidateSmokeProbe *selected = nullptr;
            for (IReleaseCandidateSmokeProbe *const probe : probes) {
                if (probe == nullptr)
                    return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
                if (probe->Kind() != kind)
                    continue;
                if (selected != nullptr)
                    return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
                selected = probe;
            }
            return selected == nullptr ? Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid))
                                       : selected->Check(root, manifest);
        }
    }  // namespace

    /** @copydoc VerifiedReleaseCandidate::VerifiedReleaseCandidate */
    VerifiedReleaseCandidate::VerifiedReleaseCandidate(const ReleaseCandidateId candidate, const Sha256Digest &manifestDigest,
                                                       std::filesystem::path root)
        : candidate_(candidate), manifestDigest_(manifestDigest), root_(std::move(root)) {}

    /** @copydoc VerifiedReleaseCandidate::Candidate */
    ReleaseCandidateId VerifiedReleaseCandidate::Candidate() const noexcept {
        return candidate_;
    }

    /** @copydoc VerifiedReleaseCandidate::ManifestDigest */
    const Sha256Digest &VerifiedReleaseCandidate::ManifestDigest() const noexcept {
        return manifestDigest_;
    }

    /** @copydoc VerifiedReleaseCandidate::Root */
    const std::filesystem::path &VerifiedReleaseCandidate::Root() const noexcept {
        return root_;
    }

    /** @copydoc VerifyReleaseCandidate */
    Result<VerifiedReleaseCandidate> VerifyReleaseCandidate(const std::filesystem::path &root, const ReleaseArtifactManifest &manifest,
                                                            const std::span<const ReleaseCandidateSmokeKind> requiredSmoke,
                                                            IReleaseCandidateSignatureVerifier *const signatureVerifier,
                                                            const std::span<IReleaseCandidateSmokeProbe *const> probes) {
        if (!ValidRequiredSmoke(requiredSmoke))
            return Result<VerifiedReleaseCandidate>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        if (auto bytes = VerifyReleaseArtifactTree(root, manifest); bytes.HasError())
            return Result<VerifiedReleaseCandidate>::Failure(bytes.ErrorValue());
        if (auto signature = VerifyCandidateSignature(root, manifest, signatureVerifier); signature.HasError())
            return Result<VerifiedReleaseCandidate>::Failure(signature.ErrorValue());
        for (const auto kind : requiredSmoke) {
            if (auto checked = CheckCandidateSmoke(kind, root, manifest, probes); checked.HasError())
                return Result<VerifiedReleaseCandidate>::Failure(checked.ErrorValue());
        }
        if (auto finalBytes = VerifyReleaseArtifactTree(root, manifest); finalBytes.HasError())
            return Result<VerifiedReleaseCandidate>::Failure(finalBytes.ErrorValue());
        return Result<VerifiedReleaseCandidate>::Success(VerifiedReleaseCandidate{manifest.Data().candidate, manifest.Digest(), root});
    }
}  // namespace Horo::Release
