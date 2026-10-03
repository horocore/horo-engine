#pragma once

/**
 * @file SaveCloudRevisionMetadata.h
 * @brief Generation-bound, provider-neutral cloud revision metadata beside the local slot index.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveNamespace.h"
#include "Horo/Runtime/Save/SaveSlotIndex.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::Runtime {
    struct SaveCloudProviderIdentityTag;
    struct SaveCloudAccountIdentityTag;
    struct SaveCloudMutationIdentityTag;

    /** @brief Stable product-assigned provider identity, not a provider display name. */
    using SaveCloudProviderId = PersistentSaveIdentity<SaveCloudProviderIdentityTag>;
    /** @brief Opaque account scope issued by the host; never a credential or native handle. */
    using SaveCloudAccountId = PersistentSaveIdentity<SaveCloudAccountIdentityTag>;
    /** @brief Durable idempotency/correlation identity of one confirmed cloud mutation. */
    using SaveCloudMutationId = PersistentSaveIdentity<SaveCloudMutationIdentityTag>;

    /** @brief Composite authority key; equal slot bytes in another scope never alias. */
    struct SaveCloudMetadataScope final {
        SaveNamespaceId localNamespace; /**< Product, environment, user and profile authority. */
        SaveCloudProviderId provider;   /**< Selected provider identity. */
        SaveCloudAccountId account;     /**< Selected provider-account scope. */

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SaveCloudMetadataScope &) const noexcept = default;
    };

    /** @brief Local coordinator state for one exact committed generation. */
    enum class SaveCloudGenerationState : std::uint8_t {
        Unknown,
        Clean,
        Dirty,
        Uploading,
        Downloading,
        Conflicted,
        Deleted, /**< Confirmed remote deletion for this local generation, never inferred from missing index rows. */
        Failed,
    };

    /** @brief One provider-scoped opaque object key and optional CAS revision. */
    struct SaveCloudObjectRef final {
        std::vector<std::byte> key;                     /**< Bounded provider-neutral object identity bytes. */
        std::optional<std::vector<std::byte>> revision; /**< Opaque write lock; absence means create-if-absent. */

        [[nodiscard]] bool operator==(const SaveCloudObjectRef &) const noexcept = default;
    };

    /** @brief State and remote evidence for exactly one local slot generation. */
    struct SaveCloudGenerationRecord final {
        SaveGameSlotId slot;                                               /**< Logical slot within the composite scope. */
        SlotGenerationId generation;                                       /**< Local generation this state describes. */
        ArchiveContentHash archive;                                        /**< Exact immutable archive identity. */
        SaveCloudGenerationState state{SaveCloudGenerationState::Unknown}; /**< Never a local-save success result. */
        std::optional<SaveCloudObjectRef> object;                          /**< Provider object evidence, if established. */
        std::optional<SaveCloudMutationId> lastConfirmed;                  /**< Last durably confirmed mutation, if any. */

        [[nodiscard]] bool operator==(const SaveCloudGenerationRecord &) const noexcept = default;
    };

    /** @brief Current sidecar schema; no field is serialized into a save archive. */
    inline constexpr std::uint32_t SaveCloudRevisionMetadataSchemaVersion = 1;

    /** @brief Qualified limits applied before copying or scanning untrusted decoded metadata. */
    struct SaveCloudRevisionMetadataLimits final {
        std::size_t maximumRecords{4'096};      /**< At most one record per current slot. */
        std::size_t maximumObjectKeyBytes{512}; /**< Hard provider-neutral object-key bound. */
        std::size_t maximumRevisionBytes{512};  /**< Hard opaque CAS-token bound. */
    };

    /**
     * @brief Versioned coordinator sidecar bound to one exact local index publication.
     *
     * A storage owner persists this together with the matching slot index through one atomic
     * catalog visibility gate. A reader must validate the pair before exposing any state.
     * The local index remains authoritative when this derived sidecar is absent or stale.
     */
    struct SaveCloudRevisionMetadata final {
        std::uint32_t schemaVersion{SaveCloudRevisionMetadataSchemaVersion}; /**< Exact sidecar schema. */
        std::uint64_t revision{};                                            /**< Non-zero coordinator snapshot revision. */
        std::uint64_t indexRevision{};                                       /**< Exact paired local slot-index revision. */
        SaveCloudMetadataScope scope;                                        /**< Full local and provider/account authority. */
        std::vector<SaveCloudGenerationRecord> records;                      /**< Canonical strict slot order. */
    };

    /**
     * @brief Immutable validated pair exposed to coordinator readers.
     *
     * It is constructed only after validating both halves; replacing the shared pointer is
     * the in-process publication gate. Durable stores must atomically replace the same pair.
     */
    class SaveCloudRevisionSnapshot final {
    public:
        /** @brief Validates and owns one complete catalog pair.
         * @param index Local authoritative index.
         * @param expectedScope Authority captured under the local namespace lease.
         * @param metadata Generation-bound sidecar.
         * @param limits Trusted metadata limits.
         * @return Immutable pair or a stable cloud-metadata error.
         */
        [[nodiscard]] static Result<SaveCloudRevisionSnapshot> Create(SaveSlotIndex index, const SaveCloudMetadataScope &expectedScope,
                                                                      SaveCloudRevisionMetadata metadata,
                                                                      SaveCloudRevisionMetadataLimits limits = {});

        /** @brief Returns the owned local index. @return Immutable catalog reference. */
        [[nodiscard]] const SaveSlotIndex &Index() const noexcept;
        /** @brief Returns the owned sidecar. @return Immutable metadata reference. */
        [[nodiscard]] const SaveCloudRevisionMetadata &Metadata() const noexcept;

    private:
        struct Data;
        explicit SaveCloudRevisionSnapshot(std::shared_ptr<const Data> data) noexcept;
        std::shared_ptr<const Data> data_;
    };

    /**
     * @brief Validates a decoded sidecar against its exact authoritative index.
     * @param index Current validated local slot index.
     * @param expectedScope Authority captured under the local namespace lease.
     * @param metadata Candidate metadata.
     * @param limits Trusted finite bounds.
     * @return Success or a stable malformed, stale, or limit error.
     */
    [[nodiscard]] Result<void> ValidateSaveCloudRevisionMetadata(const SaveSlotIndex &index, const SaveCloudMetadataScope &expectedScope,
                                                                 const SaveCloudRevisionMetadata &metadata,
                                                                 const SaveCloudRevisionMetadataLimits &limits = {});

    /**
     * @brief Rebuilds a stale or absent sidecar from current local publications.
     *
     * Exact slot/generation/hash matches in the same scope retain confirmed state and
     * provider evidence even when an unrelated slot advances the index. A changed row
     * becomes Unknown; a sidecar naming a newer index is rejected as stale input.
     * Tombstones require a separate durable sync journal and are not inferred from absence.
     * @param index Authoritative validated local index.
     * @param scope Exact local/provider/account authority for the replacement.
     * @param previous Optional decoded sidecar; invalid, foreign, or stale entries are ignored.
     * @param nextRevision Non-zero coordinator revision greater than a valid previous revision in the same scope.
     * @param limits Trusted finite bounds.
     * @return Complete candidate bound to index, or a stable error; no input is mutated.
     */
    [[nodiscard]] Result<SaveCloudRevisionMetadata> ReconcileSaveCloudRevisionMetadata(
        const SaveSlotIndex &index, const SaveCloudMetadataScope &scope, const std::optional<SaveCloudRevisionMetadata> &previous,
        std::uint64_t nextRevision, const SaveCloudRevisionMetadataLimits &limits = {});
}  // namespace Horo::Runtime
