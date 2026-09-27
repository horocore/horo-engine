#include "Horo/Release/ReleaseCandidateVerification.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>

namespace Horo::Release {
    /** @copydoc VerifyReleaseCandidate */
    Result<void> VerifyReleaseCandidate(const std::filesystem::path &root, const ReleaseArtifactManifest &manifest,
                                        const std::span<const ReleaseCandidateSmokeKind> requiredSmoke,
                                        IReleaseCandidateSignatureVerifier *const signatureVerifier,
                                        const std::span<IReleaseCandidateSmokeProbe *const> probes) {
        if (requiredSmoke.empty())
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));

        for (std::size_t index = 0; index < requiredSmoke.size(); ++index) {
            if (std::ranges::find(requiredSmoke.begin(), requiredSmoke.begin() + static_cast<std::ptrdiff_t>(index),
                                  requiredSmoke[index]) != requiredSmoke.begin() + static_cast<std::ptrdiff_t>(index))
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        }

        auto bytes = VerifyReleaseArtifactTree(root, manifest);
        if (bytes.HasError())
            return bytes;

        if (manifest.Data().signing) {
            if (signatureVerifier == nullptr)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            auto signature = signatureVerifier->Verify(root, manifest);
            if (signature.HasError())
                return signature;
        }

        for (const auto kind : requiredSmoke) {
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
            if (selected == nullptr)
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            auto checked = selected->Check(root, manifest);
            if (checked.HasError())
                return checked;
        }
        return VerifyReleaseArtifactTree(root, manifest);
    }
}  // namespace Horo::Release
