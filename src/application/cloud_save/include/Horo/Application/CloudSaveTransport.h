#pragma once

/**
 * @file CloudSaveTransport.h
 * @brief Host-only bridge between Save coordinator intents and opaque cloud transport.
 */

#include "Horo/Application/CloudSaveUpload.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"
#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace Horo::Application {
    /** @brief The exact platform request admitted for one owning Save operation. */
    template <typename T> struct CloudSaveRequest final {
        OperationId operation;                              /**< Save owner's correlation identity. */
        PlatformServices::PlatformRequestHandle<T> handle; /**< Frontend-issued cancellation and completion identity. */
    };

    /** @brief An exact cloud revision selected for local validation. */
    class CloudSaveDownload final {
    public:
        /** @brief Returns the correlation and observation identity for the owning Save operation. */
        [[nodiscard]] const CloudSaveRequest<PlatformServices::CloudBlobReadResult> &Request() const noexcept { return admission_; }

    private:
        friend class CloudSaveTransport;
        CloudSaveDownload(Sha256Digest partition, Runtime::SaveGameSlotId slot, PlatformServices::ProviderObjectRevision revision,
                          PlatformServices::CloudBlobReadRequest read,
                          CloudSaveRequest<PlatformServices::CloudBlobReadResult> admission);
        Sha256Digest partition_;
        Runtime::SaveGameSlotId slot_;
        PlatformServices::ProviderObjectRevision revision_;
        PlatformServices::CloudBlobReadRequest read_;
        CloudSaveRequest<PlatformServices::CloudBlobReadResult> admission_;
    };

    /** @brief An absent destination slot and its host-authorized archive source. */
    struct CloudSaveImportTarget final {
        Runtime::SaveSlotLifecycleTarget destination;
        std::size_t sourceScope; /**< Index of a host-owned import policy entry. */
        Runtime::SaveSlotDisplayMetadata display;
    };

    /** @brief Bounded, private retention grant for rejected complete downloads in one account partition.
     * @details Host policy fixes these bounds for the lifetime of the dedicated storage root.
     * When full, existing evidence is retained until its host storage owner retires it.
     */
    struct CloudSaveQuarantinePolicy final {
        const ImmutableFileStorage &storage;
        std::size_t maximumArchiveBytes{};
        std::size_t maximumEntries{};
    };

    /** @brief A bounded read admitted for one immutable, uncertain upload intent. */
    class CloudSaveUploadProbe final {
    public:
        /** @brief Returns correlation and observation identity for the application owner. */
        [[nodiscard]] const CloudSaveRequest<PlatformServices::CloudBlobReadResult> &Request() const noexcept {
            return request_;
        }

    private:
        friend class CloudSaveTransport;
        CloudSaveUploadProbe(CloudSaveDurableMutation mutation,
                             PlatformServices::PlatformSubjectHandle subject,
                             std::uint64_t maximumBytes,
                             CloudSaveRequest<PlatformServices::CloudBlobReadResult> request);

        CloudSaveDurableMutation mutation_;
        PlatformServices::PlatformSubjectHandle subject_;
        std::uint64_t maximumBytes_;
        CloudSaveRequest<PlatformServices::CloudBlobReadResult> request_;
    };

    /** @brief A published upload confirmation or the exact restaged intent and bytes for a conditional retry. */
    struct CloudSaveUploadRecovery final {
        std::optional<StagedCloudSaveUpload> retry; /**< Empty after durable local confirmation. */
    };

    /**
     * @brief Translates typed intents from one authenticated client-profile partition.
     * @details The application owner keeps the frontend alive and serialized on its owning thread. The transport owns no catalog,
     * archive files, callbacks, retry policy or durable journal. Remote outcomes never directly change local Save state.
     */
    class CloudSaveTransport final {
    public:
        /**
         * @brief Binds a validated host-issued account scope and current provider subject.
         * @param frontend Live platform frontend. It must outlive the returned transport.
         * @param scope Private save scope authenticated by the application owner.
         * @param subject Live capability for that same authenticated provider account.
         * @return A transport with its stable partition prefix, or a typed validation error.
         */
        [[nodiscard]] static Result<CloudSaveTransport> Create(PlatformServices::PlatformServicesFrontend &frontend,
                                                               const Runtime::SaveCloudMetadataScope &scope,
                                                               PlatformServices::PlatformSubjectHandle subject);

        /** @brief Lists only the authenticated Save partition.
         * @param operation Owning Save operation ID. @param pageSize Bounded number of results. @param cursor Optional continuation.
         * @return An admitted cloud request correlated to the Save owner, or a typed failure.
         */
        [[nodiscard]] Result<CloudSaveRequest<PlatformServices::CloudObjectPage>> List(
            OperationId operation, std::uint32_t pageSize, std::optional<PlatformServices::CloudListCursor> cursor = {}) const;

        /** @brief Reads a complete, bounded object into opaque owned bytes.
         * @param operation Owning Save operation ID. @param slot Logical slot. @param maximumBytes Exact caller-imposed read bound.
         * @return An admitted read correlated to the Save owner, or a typed failure.
         */
        [[nodiscard]] Result<CloudSaveRequest<PlatformServices::CloudBlobReadResult>> Read(
            OperationId operation, Runtime::SaveGameSlotId slot, std::uint64_t maximumBytes) const;

        /** @brief Starts a bounded read for an explicitly selected remote revision.
         * @param operation Owning Save operation. @param slot Remote logical slot.
         * @param revision Exact selected remote revision. @param maximumBytes Caller-imposed complete read bound.
         * @return A download retaining the selection and exact request, or a typed admission failure.
         */
        [[nodiscard]] Result<CloudSaveDownload> DownloadRevision(
            OperationId operation, Runtime::SaveGameSlotId slot, PlatformServices::ProviderObjectRevision revision,
            std::uint64_t maximumBytes) const;

        /** @brief Validates the received archive and durably imports it into a distinct absent slot.
         * @param lifecycle Local catalog authority. @param download Admitted remote selection.
         * @param completion Terminal observation of that exact read. @param target Approved destination and source scope.
         * @param cancellation Observed until local manifest publication.
         * @param quarantine Optional host grant for retaining rejected archive evidence.
         * @return Normal local import result or a typed rejection. The caller retains immutable received evidence.
         * @details Runs on the serialized storage lane under the live frontend binding.
         */
        [[nodiscard]] Result<Runtime::SaveSlotLifecycleResult> ImportDownload(
            Runtime::SaveSlotLifecycle &lifecycle, const CloudSaveDownload &download,
            const PlatformServices::PlatformRequestSnapshot<PlatformServices::CloudBlobReadResult> &completion,
            const CloudSaveImportTarget &target, const CancellationToken &cancellation = {},
            const CloudSaveQuarantinePolicy *quarantine = nullptr) const;

        /** @brief Applies a direct descendant to the same logical slot without rewriting its verified archive.
         * @param lifecycle Local publication authority. @param download Admitted remote selection.
         * @param completion Terminal observation of that exact read. @param target Existing local slot consent.
         * @param expected Exact catalog and cloud evidence captured before transfer.
         * @param cancellation Observed until local manifest publication.
         * @param quarantine Optional host grant for retaining rejected archive evidence.
         * @return Durable local result or typed rejection. Runs on the serialized storage lane.
         */
        [[nodiscard]] Result<Runtime::SaveSlotLifecycleResult> ApplyDownload(
            Runtime::SaveSlotLifecycle &lifecycle, const CloudSaveDownload &download,
            const PlatformServices::PlatformRequestSnapshot<PlatformServices::CloudBlobReadResult> &completion,
            const Runtime::SaveSlotLifecycleTarget &target, const Runtime::SaveCloudRevisionSnapshot &expected,
            const CancellationToken &cancellation = {}, const CloudSaveQuarantinePolicy *quarantine = nullptr) const;

        /** @brief Dispatches an exact durable mutation after checking its binding and payload.
         * @param operation Owning Save operation ID.
         * @param mutation Validated proof from the current binding's durable journal.
         * @param bytes Finalized archive bytes for upload. Deletion requires no payload.
         * @return An admitted mutation correlated to the Save owner, or a typed failure.
         */
        [[nodiscard]] Result<CloudSaveRequest<PlatformServices::CloudMutationResult>> DispatchMutation(
            OperationId operation, const CloudSaveDurableMutation &mutation,
            std::optional<PlatformServices::CloudBlobOwnedBytes> bytes = {}) const;

        /**
         * @brief Routes a deferred terminal observation to its owning Save operation.
         * @tparam T Cloud list, complete read or mutation result.
         * @param request Original admission returned by this transport.
         * @param observer Owner callback receiving operation correlation and the typed terminal snapshot.
         * @return A suppression token retained by the owner or a typed registration failure.
         */
        template <typename T>
        [[nodiscard]] Result<PlatformServices::PlatformRequestSubscription> OnComplete(
            const CloudSaveRequest<T> &request,
            std::type_identity_t<std::function<void(OperationId, const PlatformServices::PlatformRequestSnapshot<T> &)>> observer) const {
            using Response = Result<PlatformServices::PlatformRequestSubscription>;
            if (request.operation == 0 || !observer)
                return Response::Failure(MakeError(Runtime::SaveErrors::OperationInvalid));
            return frontend_->OnCloudComplete(
                request.handle,
                [operation = request.operation, observer = std::move(observer)](const auto &snapshot) { observer(operation, snapshot); });
        }

        /** @brief Prepares a sidecar candidate from exact provider commit evidence.
         * @param mutation Original durable upload. @param completion Provider's complete commit result.
         * @param current Current validated catalog pair under the owner's binding lease.
         * @return Candidate for conditional publication, the unchanged pair for an identical confirmation, or a typed rejection.
         * @details The coordinator still owns durable publication against this exact current pair.
         */
        [[nodiscard]] Result<Runtime::SaveCloudRevisionSnapshot> ConfirmUpload(
            const CloudSaveDurableMutation &mutation, const PlatformServices::CloudMutationResult &completion,
            const Runtime::SaveCloudRevisionSnapshot &current) const;

        /** @brief Confirms an upload through the durable local catalog transaction.
         * @param lifecycle Local catalog authority. @param access Exact bound namespace revision.
         * @param mutation Original durable upload. @param completion Valid provider commit evidence.
         * @return Durable success, stale selection or account, or typed I/O failure.
         * @details Runs on the serialized coordinator's storage lane, with the frontend binding retained until the call finishes.
         */
        [[nodiscard]] Result<void> PublishUploadConfirmation(Runtime::SaveSlotLifecycle &lifecycle,
                                                             const Runtime::SaveNamespaceAccessRequest &access,
                                                             const CloudSaveDurableMutation &mutation,
                                                             const PlatformServices::CloudMutationResult &completion) const;

        /** @brief Reads the exact key of an upload whose provider commit may have begun.
         * @param operation Owning Save operation. @param mutation Original durable upload.
         * @return A probe retaining the immutable intent and this read's correlation, or a typed admission failure.
         */
        [[nodiscard]] Result<CloudSaveUploadProbe> ProbeUpload(OperationId operation,
                                                              const CloudSaveDurableMutation &mutation) const;

        /** @brief Recovers an uncertain upload from the probe's correlated terminal observation.
         * @param lifecycle Local catalog authority. @param access Exact bound namespace revision.
         * @param probe Admitted read and original durable intent. @param completion Observation from that exact request.
         * @param cancellation Cooperative stop while restaging the original archive.
         * @return Durable confirmation, exact restaged retry for the original conditional mutation, or typed failure.
         * @details Runs on the serialized storage lane while the authenticated frontend binding remains valid.
         * Dispatch of a restaged retry remains on the frontend owner lane and uses the retained immutable bytes.
         */
        [[nodiscard]] Result<CloudSaveUploadRecovery> ReconcileUpload(
            Runtime::SaveSlotLifecycle &lifecycle, const Runtime::SaveNamespaceAccessRequest &access,
            const CloudSaveUploadProbe &probe,
            const PlatformServices::PlatformRequestSnapshot<PlatformServices::CloudBlobReadResult> &completion,
            const CancellationToken &cancellation = {}) const;

    private:
        friend class CloudSaveMutationJournal;
        /** @brief Derives the versioned opaque key for one typed slot inside this authenticated partition. */
        [[nodiscard]] Result<PlatformServices::CloudSaveObjectKey> Key(Runtime::SaveGameSlotId slot) const;
        /** @brief Checks that a selected revision is still attributed to this exact account binding. */
        [[nodiscard]] bool OwnsDownload(const CloudSaveDownload &download) const noexcept;

        /** @brief Validates the exact selected local generation in the current binding. */
        [[nodiscard]] const Runtime::SaveCloudGenerationRecord *FindUploadSelection(
            const CloudSaveDurableMutation &mutation, const Runtime::SaveCloudRevisionSnapshot &current) const;

        /** @brief Classifies a correlated read as a proven commit, a possible retry, or a failure. */
        [[nodiscard]] Result<std::optional<PlatformServices::CloudMutationResult>> ObserveUpload(
            const CloudSaveUploadProbe &probe,
            const PlatformServices::PlatformRequestSnapshot<PlatformServices::CloudBlobReadResult> &completion) const;

        CloudSaveTransport(const PlatformServices::PlatformServicesFrontend &frontend, PlatformServices::PlatformSubjectHandle subject,
                           PlatformServices::CloudSaveObjectPrefix prefix, Sha256Digest journalPartition,
                           Runtime::SaveCloudMetadataScope scope) noexcept;

        const PlatformServices::PlatformServicesFrontend *frontend_;
        PlatformServices::PlatformSubjectHandle subject_;
        PlatformServices::CloudSaveObjectPrefix prefix_;
        Sha256Digest journalPartition_;
        Runtime::SaveCloudMetadataScope scope_;
    };
}
