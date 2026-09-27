#include "Horo/Release/ReleaseCandidateVerification.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Release {
    /** @copydoc VerifiedReleaseCandidate::VerifiedReleaseCandidate */
    VerifiedReleaseCandidate::VerifiedReleaseCandidate(const ReleaseCandidateId candidate, Sha256Digest manifestDigest,
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
        if (requiredSmoke.empty())
            return Result<VerifiedReleaseCandidate>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));

        for (std::size_t index = 0; index < requiredSmoke.size(); ++index) {
            if (std::ranges::find(requiredSmoke.begin(), requiredSmoke.begin() + static_cast<std::ptrdiff_t>(index),
                                  requiredSmoke[index]) != requiredSmoke.begin() + static_cast<std::ptrdiff_t>(index))
                return Result<VerifiedReleaseCandidate>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        }

        auto bytes = VerifyReleaseArtifactTree(root, manifest);
        if (bytes.HasError())
            return Result<VerifiedReleaseCandidate>::Failure(bytes.ErrorValue());

        if (manifest.Data().signing) {
            if (signatureVerifier == nullptr)
                return Result<VerifiedReleaseCandidate>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            auto signature = signatureVerifier->Verify(root, manifest);
            if (signature.HasError())
                return Result<VerifiedReleaseCandidate>::Failure(signature.ErrorValue());
        }

        for (const auto kind : requiredSmoke) {
            IReleaseCandidateSmokeProbe *selected = nullptr;
            for (IReleaseCandidateSmokeProbe *const probe : probes) {
                if (probe == nullptr)
                    return Result<VerifiedReleaseCandidate>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
                if (probe->Kind() != kind)
                    continue;
                if (selected != nullptr)
                    return Result<VerifiedReleaseCandidate>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
                selected = probe;
            }
            if (selected == nullptr)
                return Result<VerifiedReleaseCandidate>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            auto checked = selected->Check(root, manifest);
            if (checked.HasError())
                return Result<VerifiedReleaseCandidate>::Failure(checked.ErrorValue());
        }
        auto finalBytes = VerifyReleaseArtifactTree(root, manifest);
        if (finalBytes.HasError())
            return Result<VerifiedReleaseCandidate>::Failure(finalBytes.ErrorValue());
        return Result<VerifiedReleaseCandidate>::Success(VerifiedReleaseCandidate{manifest.Data().candidate, manifest.Digest(), root});
    }
}  // namespace Horo::Release
