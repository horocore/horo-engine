#pragma once

/**
 * @file CloudSaveTransport.h
 * @brief Host-only bridge between Save coordinator intents and opaque cloud transport.
 */

#include "Horo/Foundation/OperationStore.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"
#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"

#include <cstdint>
#include <optional>

namespace Horo::Application {
    class CloudSaveDurableMutation;
    class CloudSaveMutationJournal;

    /** @brief The exact platform request admitted for one owning Save operation. */
    template <typename T> struct CloudSaveRequest final {
        OperationId operation;                              /**< Save owner's correlation identity. */
        PlatformServices::PlatformRequestHandle<T> handle; /**< Frontend-issued cancellation and completion identity. */
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

        /** @brief Dispatches an exact durable mutation after checking its binding and payload.
         * @param operation Owning Save operation ID.
         * @param mutation Validated proof from the current binding's durable journal.
         * @param bytes Finalized archive bytes for upload. Deletion requires no payload.
         * @return An admitted mutation correlated to the Save owner, or a typed failure.
         */
        [[nodiscard]] Result<CloudSaveRequest<PlatformServices::CloudMutationResult>> DispatchMutation(
            OperationId operation, const CloudSaveDurableMutation &mutation,
            std::optional<PlatformServices::CloudBlobOwnedBytes> bytes = {}) const;

    private:
        friend class CloudSaveMutationJournal;
        /** @brief Derives the versioned opaque key for one typed slot inside this authenticated partition. */
        [[nodiscard]] Result<PlatformServices::CloudSaveObjectKey> Key(Runtime::SaveGameSlotId slot) const;

        CloudSaveTransport(const PlatformServices::PlatformServicesFrontend &frontend, PlatformServices::PlatformSubjectHandle subject,
                           PlatformServices::CloudSaveObjectPrefix prefix, Sha256Digest journalPartition) noexcept;

        const PlatformServices::PlatformServicesFrontend *frontend_;
        PlatformServices::PlatformSubjectHandle subject_;
        PlatformServices::CloudSaveObjectPrefix prefix_;
        Sha256Digest journalPartition_;
    };
}
