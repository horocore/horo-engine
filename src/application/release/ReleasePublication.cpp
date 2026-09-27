#include "Horo/Release/ReleasePublication.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>

namespace Horo::Release {
    namespace {
        /** @brief Rejects ambiguous or unsafe project-defined channel names. */
        [[nodiscard]] bool ValidChannel(const ReleaseChannel &channel) {
            if (channel.kind != ReleaseChannelKind::ProjectDefined)
                return channel.customId.empty();
            if (channel.customId.empty() || channel.customId.size() > 64U)
                return false;
            return std::ranges::all_of(channel.customId, [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-';
            });
        }
    }  // namespace

    /** @copydoc PublishVerifiedReleaseCandidate */
    Result<ReleasePublicationReceipt> PublishVerifiedReleaseCandidate(const ReleasePublicationRequest &request,
                                                                      IReleasePublicationAdapter &adapter) {
        const auto &plan = request.plan.Request();
        if (!plan.publicationDestination || !ValidChannel(request.channel) || adapter.Destination() != *plan.publicationDestination ||
            request.verified.Candidate() != request.manifest.Data().candidate ||
            request.verified.ManifestDigest() != request.manifest.Digest() || request.verified.Root().empty() ||
            request.manifest.Data().product != plan.profile.Product() || request.manifest.Data().platform != plan.profile.Platform() ||
            request.manifest.Data().architecture != plan.architecture || request.manifest.Data().configuration != plan.configuration ||
            request.manifest.Data().frozen != request.plan.Identities())
            return Result<ReleasePublicationReceipt>::Failure(MakeError(ReleaseErrors::PipelineInputChanged));

        auto local = VerifyReleaseArtifactTree(request.verified.Root(), request.manifest);
        if (local.HasError())
            return Result<ReleasePublicationReceipt>::Failure(local.ErrorValue());

        auto uploaded = adapter.Upload(request);
        if (uploaded.HasError())
            return uploaded;
        const auto &receipt = uploaded.Value();
        if (receipt.destination != *plan.publicationDestination || receipt.candidate != request.verified.Candidate() ||
            receipt.manifestDigest != request.verified.ManifestDigest() || receipt.artifactCount != request.manifest.Artifacts().size() ||
            receipt.remoteIdentity.empty())
            return Result<ReleasePublicationReceipt>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));

        if (auto remote = adapter.VerifyRemote(request, receipt); remote.HasError())
            return Result<ReleasePublicationReceipt>::Failure(remote.ErrorValue());
        local = VerifyReleaseArtifactTree(request.verified.Root(), request.manifest);
        if (local.HasError())
            return Result<ReleasePublicationReceipt>::Failure(local.ErrorValue());

        if (auto committed = adapter.CommitChannel(request, receipt); committed.HasError())
            return Result<ReleasePublicationReceipt>::Failure(committed.ErrorValue());
        return uploaded;
    }
}  // namespace Horo::Release
