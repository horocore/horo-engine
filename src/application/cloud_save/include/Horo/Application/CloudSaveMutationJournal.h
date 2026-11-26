#pragma once

/**
 * @file CloudSaveMutationJournal.h
 * @brief Immutable intent records owned by the application Save coordinator.
 */

#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/PlatformServices/PlatformCloudObjects.h"
#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"

#include <memory>
#include <optional>
#include <string>

namespace Horo::Application {
    class CloudSaveTransport;
    class CloudSaveMutationJournal;

    /** @brief Evidence for one exact finalized archive owned by local Save. */
    struct CloudSaveUploadEvidence final {
        PlatformServices::CloudBlobDigest digest;
        std::uint64_t size{};
    };

    /** @brief Durable facts for one exact upload or deletion. No live provider handle is serialized. */
    struct CloudSaveMutationIntent final {
        Runtime::SaveCloudMutationId mutation;
        Runtime::SaveGameSlotId slot;
        Runtime::SlotGenerationId generation;
        std::optional<Runtime::SlotGenerationId> parent;
        Runtime::ArchiveContentHash archive;
        std::optional<CloudSaveUploadEvidence> upload; /**< Empty for deletion. */
        std::optional<PlatformServices::ProviderObjectRevision> revision; /**< Empty only for conditional creation. */
    };

    /** @brief Proof that the journal has persisted and validated this exact mutation intent. */
    class CloudSaveDurableMutation final {
    public:
        /** @brief Returns generation and CAS evidence for owner-side reconciliation. @return Borrowed immutable intent. */
        [[nodiscard]] const CloudSaveMutationIntent &Intent() const noexcept { return intent_; }

    private:
        friend class CloudSaveMutationJournal;
        friend class CloudSaveTransport;
        CloudSaveDurableMutation(CloudSaveMutationIntent intent, Sha256Digest partition,
                                 PlatformServices::CloudSaveObjectKey key);
        CloudSaveMutationIntent intent_;
        Sha256Digest partition_;
        PlatformServices::CloudSaveObjectKey key_;
    };

    /** @brief Persists immutable coordinator intent entries below a host-granted durable Save root.
     * @details Store and Load perform blocking storage operations. The coordinator schedules them on its storage lane.
     */
    class CloudSaveMutationJournal final {
    public:
        /** @brief Binds storage to the transport's exact local and remote scope.
         * @param storage Dedicated durable Save storage, retained through journal operations.
         * @param transport The immutable authenticated transport binding, which must outlive this journal.
         * @return A journal, or typed invalid configuration.
         */
        [[nodiscard]] static Result<CloudSaveMutationJournal> Bind(std::shared_ptr<const ImmutableFileStorage> storage,
                                                                   CloudSaveTransport &transport);

        /** @brief Checks the local namespace before a coordinator obtains archive bytes.
         * @param name Exact selected local namespace. @return True for this journal's bound namespace.
         */
        [[nodiscard]] bool MatchesNamespace(const Runtime::SaveNamespaceId &name) const noexcept;

        /** @brief Durably publishes a complete immutable intent or verifies an identical replay.
         * @param intent Exact local generation, mutation identity, payload evidence and remote revision condition.
         * @param cancellation Cooperative storage stop token.
         * @return Dispatch proof only on durable success. A changed intent cannot reuse an existing ID.
         */
        [[nodiscard]] Result<CloudSaveDurableMutation> Store(CloudSaveMutationIntent intent,
                                                             const CancellationToken &cancellation = {}) const;

        /** @brief Reloads one exact local mutation without contacting a provider.
         * @param mutation ID discovered by the coordinator's durable pending-work index.
         * @param cancellation Cooperative storage stop token.
         * @return Validated dispatch proof, empty for an absent record, or typed corruption/authority/I/O failure.
         */
        [[nodiscard]] Result<std::optional<CloudSaveDurableMutation>> Load(
            Runtime::SaveCloudMutationId mutation, const CancellationToken &cancellation = {}) const;

    private:
        CloudSaveMutationJournal(std::shared_ptr<const ImmutableFileStorage> storage, const CloudSaveTransport &transport);
        /** @brief Resolves a mutation's private local entry name inside this journal's exact scope. */
        [[nodiscard]] std::string EntryName(Runtime::SaveCloudMutationId mutation) const;
        std::shared_ptr<const ImmutableFileStorage> storage_;
        const CloudSaveTransport *transport_;
    };
}
