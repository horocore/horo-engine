#include "Horo/Application/CloudSaveTransport.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

namespace Horo::Application {
    namespace {
        using namespace PlatformServices;

        /** @brief Tags the durable partition encoding independently from the slot suffix. */
        constexpr std::string_view partitionDomain = "horo.cloud-save.partition.v1";
        constexpr std::string_view slotDomain = "horo.cloud-save.slot.v1";
        constexpr std::string_view journalDomain = "horo.cloud-save.journal.v1";

        /** @brief Hashes portable partition identities while excluding the installation-local user ID. */
        Sha256Digest HashPartition(const Runtime::SaveCloudMetadataScope &scope, const Runtime::GameProfileId &profile) {
            const std::array<std::span<const std::byte>, 6> parts{
                std::as_bytes(std::span{partitionDomain}), std::as_bytes(std::span{scope.localNamespace.product.Bytes()}),
                std::as_bytes(std::span{scope.localNamespace.environment.Bytes()}), std::as_bytes(std::span{profile.Bytes()}),
                std::as_bytes(std::span{scope.provider.Bytes()}), std::as_bytes(std::span{scope.account.Bytes()})};
            return ComputeSha256Fragments(parts);
        }

        /** @brief Separates durable local ownership from portable remote addressing. */
        Sha256Digest HashJournalPartition(const Runtime::CanonicalSaveNamespaceKey &local, const Sha256Digest &remote) {
            const std::array<std::span<const std::byte>, 3> parts{
                std::as_bytes(std::span{journalDomain}), local.Bytes(), std::as_bytes(std::span{remote.bytes})};
            return ComputeSha256Fragments(parts);
        }

        /** @brief Projects a durable Save mutation UUID onto the exact opaque transfer bytes. */
        CloudMutationId MutationId(const Runtime::SaveCloudMutationId &mutation) {
            CloudMutationId result;
            std::ranges::copy(std::as_bytes(std::span{mutation.Bytes()}), result.bytes.begin());
            return result;
        }

        /** @brief Admits only operations with an application owner and preserves the frontend result. */
        template <typename T, typename Submit>
        Result<CloudSaveRequest<T>> Admit(const OperationId operation, Submit submit) {
            using Response = Result<CloudSaveRequest<T>>;
            if (operation == 0)
                return Response::Failure(MakeError(Runtime::SaveErrors::OperationInvalid));
            auto request = submit();
            if (request.HasError())
                return Response::Failure(request.ErrorValue());
            return Response::Success({.operation = operation, .handle = std::move(request).Value()});
        }

        /** @brief Checks exact session, object and bytes before accepting a commit for Save. */
        Result<void> CheckUploadCompletion(const CloudMutationResult &result, const CloudSaveMutationIntent &intent,
                                           const PlatformSubjectHandle &subject, const CloudSaveObjectKey &key) {
            if (result.subject != subject || result.sessionGeneration != subject.SessionGeneration() || result.key != key ||
                result.mutation != MutationId(intent.mutation))
                return Result<void>::Failure(MakeError(CloudObjectErrors::StaleSession));
            const auto &head = result.committedObject;
            if (!head || head->key != key || !head->revision.IsValid() || head->sizeBytes != intent.upload->size ||
                head->transportDigest != intent.upload->digest || head->revision == intent.revision)
                return Result<void>::Failure(MakeError(CloudObjectErrors::InvalidProviderResponse));
            return Result<void>::Success();
        }

        /** @brief Requires the sidecar to retain the exact revision precondition used on the wire. */
        bool MatchesPrecondition(const std::optional<Runtime::SaveCloudObjectRef> &previous, const CloudSaveObjectKey &key,
                                 const std::optional<ProviderObjectRevision> &expected) {
            if (!previous)
                return !expected;
            if (!std::ranges::equal(previous->key, key.Bytes()) || previous->revision.has_value() != expected.has_value())
                return false;
            return !expected || std::ranges::equal(*previous->revision, expected->Bytes());
        }

        /** @brief Checks whether the selected local record can still apply the original remote precondition. */
        bool CanApplyUpload(const Runtime::SaveCloudGenerationRecord &selected, const CloudSaveObjectKey &key,
                            const std::optional<ProviderObjectRevision> &expected) {
            using enum Runtime::SaveCloudGenerationState;
            return selected.state != Deleted && selected.state != Conflicted && selected.state != Downloading &&
                   MatchesPrecondition(selected.object, key, expected);
        }

        /** @brief Restages immutable archive bytes only when the exact original intent can still describe them. */
        Result<StagedCloudSaveUpload> RestageUpload(Runtime::SaveSlotLifecycle &lifecycle,
                                                   const Runtime::SaveSlotLifecycleTarget &target,
                                                   const CloudSaveDurableMutation &mutation, const std::uint64_t maximumBytes,
                                                   const CancellationToken &cancellation) {
            using Response = Result<StagedCloudSaveUpload>;
            const auto exported = lifecycle.ExportVerified(target, static_cast<std::size_t>(maximumBytes), cancellation);
            if (exported.HasError())
                return Response::Failure(exported.ErrorValue());
            const auto &intent = mutation.Intent();
            const auto &archive = exported.Value();
            if (archive.parent != intent.parent || archive.entry.publication.archiveContent != intent.archive)
                return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));
            const auto &bytes = *archive.archive.bytes;
            if (bytes.size() != intent.upload->size || ComputeSha256(bytes) != intent.upload->digest)
                return Response::Failure(MakeError(CloudObjectErrors::IntegrityMismatch));
            auto owned = CloudBlobOwnedBytes::Copy(bytes);
            if (owned.HasError())
                return Response::Failure(owned.ErrorValue());
            return Response::Success({.mutation = mutation, .bytes = std::move(owned).Value()});
        }

        /** @brief Requires a terminal observation from the exact admitted read. */
        Result<void> CheckReadObservation(const PlatformRequestHandle<CloudBlobReadResult> &handle,
                                          const PlatformRequestSnapshot<CloudBlobReadResult> &completion) {
            if (!handle.IsValid() || completion.id != handle.Id() || completion.generation != handle.Generation())
                return Result<void>::Failure(MakeError(RequestErrors::Stale));
            if (!completion.terminal || !IsTerminal(completion.state))
                return Result<void>::Failure(MakeError(CloudObjectErrors::InvalidRequest));
            return Result<void>::Success();
        }

        /** @brief Checks a complete download against the exact revision and address approved for import. */
        Result<const CloudBlobReadResult *> CheckImportRead(
            const CloudSaveRequest<CloudBlobReadResult> &admission, const PlatformRequestSnapshot<CloudBlobReadResult> &completion,
            const CloudBlobReadRequest &request, const ProviderObjectRevision &revision) {
            using Response = Result<const CloudBlobReadResult *>;
            if (const auto observed = CheckReadObservation(admission.handle, completion); observed.HasError())
                return Response::Failure(observed.ErrorValue());
            if (const auto *error = completion.terminal->ErrorValue())
                return Response::Failure(*error);
            const auto *read = completion.terminal->Value();
            if (!read || completion.state != PlatformRequestState::Succeeded)
                return Response::Failure(MakeError(CloudObjectErrors::InvalidProviderResponse));
            if (read->head.revision != revision)
                return Response::Failure(MakeError(CloudObjectErrors::RevisionChanged));
            if (const auto valid = ValidateCloudBlobReadCompletion(*read, request, request.subject); valid.HasError())
                return Response::Failure(valid.ErrorValue());
            return Response::Success(read);
        }
    }

    CloudSaveDownload::CloudSaveDownload(const Sha256Digest partition, const Runtime::SaveGameSlotId slot,
                                         ProviderObjectRevision revision, CloudBlobReadRequest read,
                                         CloudSaveRequest<CloudBlobReadResult> admission)
        : partition_(partition), slot_(slot), revision_(std::move(revision)), read_(std::move(read)), admission_(std::move(admission)) {}

    CloudSaveUploadProbe::CloudSaveUploadProbe(CloudSaveDurableMutation mutation, const PlatformSubjectHandle subject,
                                               const std::uint64_t maximumBytes, CloudSaveRequest<CloudBlobReadResult> request)
        : mutation_(std::move(mutation)), subject_(subject), maximumBytes_(maximumBytes), request_(std::move(request)) {}

    CloudSaveTransport::CloudSaveTransport(const PlatformServicesFrontend &frontend, const PlatformSubjectHandle subject,
                                           CloudSaveObjectPrefix prefix, const Sha256Digest journalPartition,
                                           const Runtime::SaveCloudMetadataScope scope) noexcept
        : frontend_(&frontend), subject_(subject), prefix_(std::move(prefix)), journalPartition_(journalPartition),
          scope_(scope) {}

    /** @copydoc CloudSaveTransport::Create */
    Result<CloudSaveTransport> CloudSaveTransport::Create(PlatformServicesFrontend &frontend,
                                                         const Runtime::SaveCloudMetadataScope &scope,
                                                         const PlatformSubjectHandle subject) {
        const auto *owner = std::get_if<Runtime::UserProfileOwner>(&scope.localNamespace.owner);
        if (!owner || !scope.IsValid() || !subject.IsValid())
            return Result<CloudSaveTransport>::Failure(MakeError(Runtime::SaveErrors::CloudMetadataInvalid));
        auto local = Runtime::EncodeSaveNamespaceKey(scope.localNamespace);
        if (local.HasError())
            return Result<CloudSaveTransport>::Failure(local.ErrorValue());
        const auto digest = HashPartition(scope, owner->profile);
        auto prefix = CloudSaveObjectPrefix::Copy(std::as_bytes(std::span{digest.bytes}));
        if (prefix.HasError())
            return Result<CloudSaveTransport>::Failure(prefix.ErrorValue());
        return Result<CloudSaveTransport>::Success(
            CloudSaveTransport{frontend, subject, std::move(prefix).Value(), HashJournalPartition(local.Value(), digest),
                               scope});
    }

    /** @copydoc CloudSaveTransport::Key */
    Result<CloudSaveObjectKey> CloudSaveTransport::Key(const Runtime::SaveGameSlotId slot) const {
        if (!slot.IsValid())
            return Result<CloudSaveObjectKey>::Failure(MakeError(Runtime::SaveErrors::CloudMetadataInvalid));
        const std::array<std::span<const std::byte>, 3> parts{
            std::as_bytes(std::span{slotDomain}), prefix_.Bytes(), std::as_bytes(std::span{slot.Bytes()})};
        const auto suffix = ComputeSha256Fragments(parts);
        constexpr auto digestBytes = Sha256Digest{}.bytes.size();
        std::array<std::byte, 2 * digestBytes> key{};
        const auto end = std::ranges::copy(prefix_.Bytes(), key.begin()).out;
        std::ranges::copy(std::as_bytes(std::span{suffix.bytes}), end);
        return CloudSaveObjectKey::Copy(key);
    }

    /** @copydoc CloudSaveTransport::List */
    Result<CloudSaveRequest<CloudObjectPage>> CloudSaveTransport::List(const OperationId operation, const std::uint32_t pageSize,
                                                                      std::optional<CloudListCursor> cursor) const {
        return Admit<CloudObjectPage>(operation, [&] {
            return frontend_->ListCloudObjects({.subject = subject_, .prefix = prefix_, .cursor = std::move(cursor), .pageSize = pageSize});
        });
    }

    /** @copydoc CloudSaveTransport::Read */
    Result<CloudSaveRequest<CloudBlobReadResult>> CloudSaveTransport::Read(const OperationId operation,
                                                                         const Runtime::SaveGameSlotId slot,
                                                                         const std::uint64_t maximumBytes) const {
        return Admit<CloudBlobReadResult>(operation, [&]() -> Result<PlatformRequestHandle<CloudBlobReadResult>> {
            auto key = Key(slot);
            if (key.HasError())
                return Result<PlatformRequestHandle<CloudBlobReadResult>>::Failure(key.ErrorValue());
            return frontend_->ReadCloudObject(
                CloudBlobReadRequest{.subject = subject_, .key = std::move(key).Value(), .maximumBytes = maximumBytes});
        });
    }

    /** @copydoc CloudSaveTransport::DownloadForImport */
    Result<CloudSaveDownload> CloudSaveTransport::DownloadForImport(const OperationId operation,
                                                                    const Runtime::SaveGameSlotId slot,
                                                                    ProviderObjectRevision revision,
                                                                    const std::uint64_t maximumBytes) const {
        using Response = Result<CloudSaveDownload>;
        if (!revision.IsValid())
            return Response::Failure(MakeError(CloudObjectErrors::InvalidRequest));
        auto key = Key(slot);
        if (key.HasError())
            return Response::Failure(key.ErrorValue());
        CloudBlobReadRequest read{subject_, std::move(key).Value(), maximumBytes};
        auto admission = Admit<CloudBlobReadResult>(operation, [&] { return frontend_->ReadCloudObject(read); });
        if (admission.HasError())
            return Response::Failure(admission.ErrorValue());
        return Response::Success(
            CloudSaveDownload{journalPartition_, slot, std::move(revision), std::move(read), std::move(admission).Value()});
    }

    /** @copydoc CloudSaveTransport::ImportDownload */
    Result<Runtime::SaveSlotLifecycleResult> CloudSaveTransport::ImportDownload(
        Runtime::SaveSlotLifecycle &lifecycle, const CloudSaveDownload &download,
        const PlatformRequestSnapshot<CloudBlobReadResult> &completion, const CloudSaveImportTarget &target,
        const CancellationToken &cancellation) const {
        using Response = Result<Runtime::SaveSlotLifecycleResult>;
        const auto &destination = target.destination;
        const auto &access = destination.address.namespaceAccess;
        if (!frontend_->IsOpen() || download.partition_ != journalPartition_ || download.read_.subject != subject_ ||
            access.expected != scope_.localNamespace)
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));
        if (destination.generation || destination.address.slot == download.slot_)
            return Response::Failure(MakeError(Runtime::SaveErrors::StoragePermissionDenied));
        if (cancellation.IsCancellationRequested())
            return Response::Failure(MakeError(Runtime::SaveErrors::OperationCancelled));
        const auto read = CheckImportRead(download.Request(), completion, download.read_, download.revision_);
        if (read.HasError())
            return Response::Failure(read.ErrorValue());
        const auto current = lifecycle.ReadCloudRevision(access, scope_);
        if (current.HasError())
            return Response::Failure(current.ErrorValue());
        if (current.Value().Index().revision != destination.catalogRevision)
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));

        try {
            const auto bytes = read.Value()->bytes.Bytes();
            const auto staged = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
            return lifecycle.ImportVerified({.kind = Runtime::SaveSlotLifecycleKind::Import,
                                             .source = destination,
                                             .imported = {staged},
                                             .importSource = target.sourceScope,
                                             .display = target.display},
                                            download.slot_, cancellation);
        } catch (const std::bad_alloc &) {
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataAllocationFailed));
        }
    }

    /** @copydoc CloudSaveTransport::DispatchMutation */
    Result<CloudSaveRequest<CloudMutationResult>> CloudSaveTransport::DispatchMutation(
        const OperationId operation, const CloudSaveDurableMutation &mutation, std::optional<CloudBlobOwnedBytes> bytes) const {
        return Admit<CloudMutationResult>(operation, [&]() -> Result<PlatformRequestHandle<CloudMutationResult>> {
            using Response = Result<PlatformRequestHandle<CloudMutationResult>>;
            if (mutation.partition_ != journalPartition_)
                return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));
            const auto &intent = mutation.intent_;
            if (intent.upload.has_value() != bytes.has_value())
                return Response::Failure(MakeError(CloudObjectErrors::InvalidRequest));
            if (!intent.upload)
                return frontend_->DeleteCloudObject({.subject = subject_,
                                                     .key = mutation.key_,
                                                     .expectedRevision = *intent.revision,
                                                     .mutation = MutationId(intent.mutation)});
            if (bytes->Bytes().size() != intent.upload->size)
                return Response::Failure(MakeError(CloudObjectErrors::IntegrityMismatch));

            CloudWritePrecondition condition = CloudCreateIfAbsent{};
            if (intent.revision)
                condition = CloudMatchProviderRevision{*intent.revision};
            return frontend_->WriteCloudObject({.subject = subject_,
                                                .key = mutation.key_,
                                                .bytes = std::move(*bytes),
                                                .expectedDigest = intent.upload->digest,
                                                .precondition = std::move(condition),
                                                .mutation = MutationId(intent.mutation)});
        });
    }

    /** @copydoc CloudSaveTransport::ConfirmUpload */
    Result<Runtime::SaveCloudRevisionSnapshot> CloudSaveTransport::ConfirmUpload(const CloudSaveDurableMutation &mutation,
                                                                                 const CloudMutationResult &completion,
                                                                                 const Runtime::SaveCloudRevisionSnapshot &current) const {
        using Response = Result<Runtime::SaveCloudRevisionSnapshot>;
        const auto *selected = FindUploadSelection(mutation, current);
        if (!selected)
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));
        const auto &intent = mutation.intent_;
        if (auto checked = CheckUploadCompletion(completion, intent, subject_, mutation.key_); checked.HasError())
            return Response::Failure(checked.ErrorValue());

        const auto &currentMetadata = current.Metadata();
        const auto &head = *completion.committedObject;
        if (selected->lastConfirmed == intent.mutation && selected->state == Runtime::SaveCloudGenerationState::Clean &&
            MatchesPrecondition(selected->object, mutation.key_, head.revision))
            return Response::Success(current);
        if (!CanApplyUpload(*selected, mutation.key_, intent.revision) ||
            currentMetadata.revision == std::numeric_limits<std::uint64_t>::max())
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));

        try {
            auto candidate = currentMetadata;
            ++candidate.revision;
            auto &record = candidate.records[static_cast<std::size_t>(selected - currentMetadata.records.data())];
            const auto key = mutation.key_.Bytes();
            const auto revision = head.revision.Bytes();
            record.object = {.key = {key.begin(), key.end()}, .revision = std::vector<std::byte>{revision.begin(), revision.end()}};
            record.state = Runtime::SaveCloudGenerationState::Clean;
            record.lastConfirmed = intent.mutation;
            return Runtime::SaveCloudRevisionSnapshot::Create(current.Index(), scope_, std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataAllocationFailed));
        }
    }

    /** @copydoc CloudSaveTransport::FindUploadSelection */
    const Runtime::SaveCloudGenerationRecord *CloudSaveTransport::FindUploadSelection(
        const CloudSaveDurableMutation &mutation, const Runtime::SaveCloudRevisionSnapshot &current) const {
        const auto &metadata = current.Metadata();
        const auto &intent = mutation.intent_;
        if (!frontend_->IsOpen() || mutation.partition_ != journalPartition_ || metadata.scope != scope_ || !intent.upload)
            return nullptr;
        const auto selected = std::ranges::lower_bound(metadata.records, intent.slot, {}, &Runtime::SaveCloudGenerationRecord::slot);
        if (selected == metadata.records.end() || selected->slot != intent.slot || selected->generation != intent.generation ||
            selected->archive != intent.archive)
            return nullptr;
        return &*selected;
    }

    /** @copydoc CloudSaveTransport::ProbeUpload */
    Result<CloudSaveUploadProbe> CloudSaveTransport::ProbeUpload(const OperationId operation,
                                                                const CloudSaveDurableMutation &mutation) const {
        using Response = Result<CloudSaveUploadProbe>;
        if (mutation.partition_ != journalPartition_ || !mutation.intent_.upload)
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));
        const auto limits = frontend_->ServiceLimits(PlatformServiceKind::Cloud);
        if (limits.HasError())
            return Response::Failure(limits.ErrorValue());
        const auto maximumBytes = limits.Value().maxPayloadBytes;
        auto request = Read(operation, mutation.intent_.slot, maximumBytes);
        if (request.HasError())
            return Response::Failure(request.ErrorValue());
        return Response::Success(CloudSaveUploadProbe{mutation, subject_, maximumBytes, std::move(request).Value()});
    }

    /** @copydoc CloudSaveTransport::ObserveUpload */
    Result<std::optional<CloudMutationResult>> CloudSaveTransport::ObserveUpload(
        const CloudSaveUploadProbe &probe, const PlatformRequestSnapshot<CloudBlobReadResult> &completion) const {
        using Response = Result<std::optional<CloudMutationResult>>;
        if (probe.subject_ != subject_)
            return Response::Failure(MakeError(RequestErrors::Stale));
        if (const auto observed = CheckReadObservation(probe.Request().handle, completion); observed.HasError())
            return Response::Failure(observed.ErrorValue());

        const auto &mutation = probe.mutation_;
        const auto &intent = mutation.intent_;
        if (const auto *error = completion.terminal->ErrorValue()) {
            const auto &missing = CloudObjectErrors::NotFound;
            if (completion.state != PlatformRequestState::Failed || error->code.Value() != missing.code.Value() ||
                error->domain.Value() != missing.domain.Value())
                return Response::Failure(*error);
            if (intent.revision)
                return Response::Failure(MakeError(CloudObjectErrors::PreconditionFailed));
            return Response::Success(std::nullopt);
        }

        const auto *read = completion.terminal->Value();
        if (!read || completion.state != PlatformRequestState::Succeeded)
            return Response::Failure(MakeError(CloudObjectErrors::InvalidProviderResponse));
        const CloudBlobReadRequest request{subject_, mutation.key_, probe.maximumBytes_};
        if (const auto valid = ValidateCloudBlobReadCompletion(*read, request, subject_); valid.HasError())
            return Response::Failure(valid.ErrorValue());
        auto head = read->head;
        if (head.revision == intent.revision)
            return Response::Success(std::nullopt);
        if (head.sizeBytes != intent.upload->size)
            return Response::Failure(MakeError(CloudObjectErrors::PreconditionFailed));
        if (!head.transportDigest)
            head.transportDigest = ComputeSha256(read->bytes.Bytes());
        if (head.transportDigest != intent.upload->digest)
            return Response::Failure(MakeError(CloudObjectErrors::PreconditionFailed));
        return Response::Success(CloudMutationResult{.subject = read->subject,
                                                     .sessionGeneration = read->sessionGeneration,
                                                     .key = mutation.key_,
                                                     .mutation = MutationId(intent.mutation),
                                                     .committedObject = std::move(head)});
    }

    /** @copydoc CloudSaveTransport::ReconcileUpload */
    Result<CloudSaveUploadRecovery> CloudSaveTransport::ReconcileUpload(
        Runtime::SaveSlotLifecycle &lifecycle, const Runtime::SaveNamespaceAccessRequest &access,
        const CloudSaveUploadProbe &probe, const PlatformRequestSnapshot<CloudBlobReadResult> &completion,
        const CancellationToken &cancellation) const {
        using Response = Result<CloudSaveUploadRecovery>;
        const auto &mutation = probe.mutation_;
        if (!frontend_->IsOpen() || mutation.partition_ != journalPartition_)
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));
        const auto observed = ObserveUpload(probe, completion);
        if (observed.HasError())
            return Response::Failure(observed.ErrorValue());
        if (observed.Value()) {
            const auto published = PublishUploadConfirmation(lifecycle, access, mutation, *observed.Value());
            if (published.HasError())
                return Response::Failure(published.ErrorValue());
            return Response::Success({});
        }

        const auto current = lifecycle.ReadCloudRevision(access, scope_);
        if (current.HasError())
            return Response::Failure(current.ErrorValue());
        const auto *selected = FindUploadSelection(mutation, current.Value());
        if (!selected || !CanApplyUpload(*selected, mutation.key_, mutation.intent_.revision))
            return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));
        const Runtime::SaveSlotLifecycleTarget target{
            .address = {.namespaceAccess = access, .slot = selected->slot},
            .catalogRevision = current.Value().Index().revision,
            .generation = selected->generation};
        auto retry = RestageUpload(lifecycle, target, mutation, probe.maximumBytes_, cancellation);
        if (retry.HasError())
            return Response::Failure(retry.ErrorValue());
        return Response::Success({.retry = std::move(retry).Value()});
    }
}
