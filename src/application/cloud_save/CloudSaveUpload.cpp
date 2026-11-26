#include "Horo/Application/CloudSaveUpload.h"
#include "Horo/Application/CloudSaveTransport.h"

#include <algorithm>
#include <utility>

namespace Horo::Application {
    /** @copydoc StageCloudSaveUpload */
    Result<StagedCloudSaveUpload> StageCloudSaveUpload(
        Runtime::SaveSlotLifecycle &lifecycle, const CloudSaveMutationJournal &journal,
        const Runtime::SaveSlotLifecycleTarget &target, const Runtime::SaveCloudMutationId mutation,
        std::optional<PlatformServices::ProviderObjectRevision> expectedRevision, const std::size_t maximumBytes,
        const CancellationToken &cancellation) {
        using Response = Result<StagedCloudSaveUpload>;
        if (!journal.MatchesAccess(target.address.namespaceAccess))
            return Response::Failure(MakeError(Runtime::SaveErrors::NamespaceStale));
        if (!mutation.IsValid() || (expectedRevision && !expectedRevision->IsValid()))
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataInvalid));

        const auto limit = std::min(maximumBytes, static_cast<std::size_t>(PlatformServices::MaximumCloudObjectPayloadBytes));
        const auto exported = lifecycle.ExportVerified(target, limit, cancellation);
        if (exported.HasError())
            return Response::Failure(exported.ErrorValue());
        auto bytes = PlatformServices::CloudBlobOwnedBytes::Copy(*exported.Value().archive.bytes);
        if (bytes.HasError())
            return Response::Failure(bytes.ErrorValue());
        const auto &publication = exported.Value().entry.publication;
        const auto payload = bytes.Value().Bytes();
        auto durable = journal.Store({.mutation = mutation,
                                      .slot = publication.slot,
                                      .generation = publication.generation,
                                      .parent = exported.Value().parent,
                                      .archive = publication.archiveContent,
                                      .upload = CloudSaveUploadEvidence{ComputeSha256(payload), payload.size()},
                                      .revision = std::move(expectedRevision)},
                                     cancellation);
        if (durable.HasError())
            return Response::Failure(durable.ErrorValue());
        return Response::Success({.mutation = std::move(durable).Value(), .bytes = std::move(bytes).Value()});
    }

    /** @copydoc CloudSaveTransport::PublishUploadEvidence */
    Result<void> CloudSaveTransport::PublishUploadEvidence(Runtime::SaveSlotLifecycle &lifecycle,
                                                               const Runtime::SaveNamespaceAccessRequest &access,
                                                               const CloudSaveDurableMutation &mutation,
                                                               const PlatformServices::CloudMutationResult &completion) const {
        if (!OwnsAccess(access))
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::NamespaceStale));
        const auto current = lifecycle.ReadCloudRevision(access, scope_);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto candidate = ConfirmUpload(mutation, completion, current.Value());
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        return lifecycle.PublishCloudRevision(access, current.Value(), candidate.Value());
    }
}
