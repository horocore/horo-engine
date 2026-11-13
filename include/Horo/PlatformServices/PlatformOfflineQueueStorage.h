#pragma once

/**
 * @file PlatformOfflineQueueStorage.h
 * @brief Versioned, integrity-checked and atomically published offline intent storage.
 */

#include "Horo/Foundation/Result.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace Horo {
    class DurableFileSystem;
}

namespace Horo::PlatformOfflineQueue {
    inline constexpr std::uint32_t PlatformOfflineQueueSchemaVersion = 1;
    inline constexpr std::size_t PlatformOfflineQueueDefaultMaximumRecords = 1024;
    inline constexpr std::size_t PlatformOfflineQueueDefaultMaximumPayloadBytes = 64 * 1024;
    inline constexpr std::size_t PlatformOfflineQueueDefaultMaximumDocumentBytes = 4 * 1024 * 1024;
    inline constexpr std::size_t PlatformOfflineQueueHardMaximumRecords = 4096;
    inline constexpr std::size_t PlatformOfflineQueueHardMaximumPayloadBytes = 256 * 1024;
    inline constexpr std::size_t PlatformOfflineQueueHardMaximumDocumentBytes = 16 * 1024 * 1024;

    /** @brief Stable non-sensitive identity of one durable offline intent. */
    struct PlatformOfflineIntentId final {
        std::array<std::byte, 16> bytes{}; /**< Opaque identity; zero is reserved. */

        /** @brief Checks the reserved all-zero representation. @return True when valid. */
        [[nodiscard]] bool IsValid() const noexcept;

        [[nodiscard]] auto operator<=>(const PlatformOfflineIntentId &) const noexcept = default;
    };

    /** @brief Pseudonymous storage partition for one protected user binding. */
    struct PlatformOfflineSubjectPartition final {
        std::array<std::byte, 16> bytes{}; /**< Opaque same-binding partition; never a raw account ID. */

        /** @brief Checks the reserved all-zero representation. @return True when valid. */
        [[nodiscard]] bool IsValid() const noexcept;

        [[nodiscard]] auto operator<=>(const PlatformOfflineSubjectPartition &) const noexcept = default;
    };

    /** @brief Exhaustive durable state persisted independently of any live provider request. */
    enum class PlatformOfflineIntentState : std::uint8_t {
        Pending,
        Dispatching,
        Reconciling,
        Suspended,
        Succeeded,
        PermanentlyFailed,
        Expired,
        Superseded,
        Abandoned
    };

    /** @brief Provider-neutral operation classes admitted by the offline queue. */
    enum class PlatformOfflineOperationKind : std::uint8_t {
        ProgressionMutation,
        PresenceSet,
        PresenceClear
    };

    /** @brief One bounded provider-neutral durable intent record. */
    struct PlatformOfflineQueueRecord final {
        PlatformOfflineIntentId identity;
        PlatformOfflineSubjectPartition partition;
        PlatformOfflineIntentState state{PlatformOfflineIntentState::Pending};
        PlatformOfflineOperationKind operation{PlatformOfflineOperationKind::ProgressionMutation};
        std::uint64_t sequence{};       /**< Nonzero partition-local ordering sequence. */
        std::vector<std::byte> payload; /**< Bounded canonical Horo bytes supplied by the semantic owner. */

        /** @brief Checks identity, lifecycle, operation and sequence invariants. @return True when structurally valid. */
        [[nodiscard]] bool IsValid() const noexcept;

        [[nodiscard]] bool operator==(const PlatformOfflineQueueRecord &) const noexcept = default;
    };

    /** @brief Finite product-selected limits for one queue partition document. */
    struct PlatformOfflineQueueLimits final {
        std::size_t maximumRecords{PlatformOfflineQueueDefaultMaximumRecords};
        std::size_t maximumPayloadBytes{PlatformOfflineQueueDefaultMaximumPayloadBytes};
        std::size_t maximumDocumentBytes{PlatformOfflineQueueDefaultMaximumDocumentBytes};
    };

    /** @brief Opaque optimistic-publication token for one adapter lifetime and exact protected partition. */
    class PlatformOfflineQueueRevision final {
    public:
        PlatformOfflineQueueRevision() = default;

    private:
        friend class PlatformOfflineQueueStorage;
        std::weak_ptr<const void> storage_;
        PlatformOfflineSubjectPartition partition_;
        std::array<std::uint8_t, 32> digest_{};
        bool documentExists_{};
    };

    /** @brief Validated detached records paired with their exact publication precondition. */
    struct PlatformOfflineQueueSnapshot final {
        std::vector<PlatformOfflineQueueRecord> records;
        PlatformOfflineQueueRevision revision;
    };

    /** @brief Stable storage, schema and integrity failures for the offline queue. */
    namespace PlatformOfflineQueueErrors {
        extern const ErrorCodeDescriptor InvalidConfiguration;
        extern const ErrorCodeDescriptor InvalidPartition;
        extern const ErrorCodeDescriptor InvalidRecord;
        extern const ErrorCodeDescriptor UnsupportedVersion;
        extern const ErrorCodeDescriptor PayloadTooLarge;
        extern const ErrorCodeDescriptor CapacityExceeded;
        extern const ErrorCodeDescriptor Corrupt;
        extern const ErrorCodeDescriptor DurableUnavailable;
        extern const ErrorCodeDescriptor StorageUnknown;
        extern const ErrorCodeDescriptor IdentityConflict;
        extern const ErrorCodeDescriptor RevisionConflict;
    }  // namespace PlatformOfflineQueueErrors

    /**
     * @brief Owns one partition's versioned queue document and publishes it atomically.
     * @details The adapter stores opaque Horo identities and treats bounded payload bytes as opaque. Callers must provide canonical Horo
     * intent bytes and must not supply provider-native data or cloud archive bytes. The adapter does not inspect payload contents and
     * never accepts a provider account identifier, live subject handle, credential or callback.
     * Public failures preserve their typed identity and severity, but omit protected partition locators and unrestricted
     * filesystem messages, diagnostics and cause text. The storage owner already possesses its private root for authorized recovery.
     */
    class PlatformOfflineQueueStorage final {
    public:
        /**
         * @brief Validates and creates a storage adapter.
         * @param files Host-owned durable filesystem primitives that outlive this adapter and all its copies.
         * @param root Product state root supplied by the composition root.
         * @param limits Finite record, payload and document bounds.
         * @return Adapter or a typed configuration failure.
         * @throws std::bad_alloc If the adapter's shared lifetime marker cannot be allocated.
         */
        [[nodiscard]] static Result<PlatformOfflineQueueStorage> Create(DurableFileSystem &files, std::filesystem::path root,
                                                                        const PlatformOfflineQueueLimits &limits = {});

        /**
         * @brief Loads one opaque subject partition without invoking a provider.
         * @param partition Pseudonymous same-binding partition.
         * @return Validated records, an empty vector for a new partition, or a typed recovery failure.
         * @details Read-only convenience. Mutating owners must use LoadSnapshot and retain its exact revision through publication.
         */
        [[nodiscard]] Result<std::vector<PlatformOfflineQueueRecord>> Load(const PlatformOfflineSubjectPartition &partition) const;

        /**
         * @brief Loads validated records and an opaque revision for conditional publication.
         * @param partition Exact protected same-binding partition.
         * @return Detached snapshot, including an absence token for a new partition, or typed recovery failure.
         * @details Corrupt/quarantined documents never issue a token. The token is not persisted, logged, or transferred to a
         * newly created adapter. A caller must merge its mutation into these records, preserving unrelated durable intent.
         */
        [[nodiscard]] Result<PlatformOfflineQueueSnapshot> LoadSnapshot(const PlatformOfflineSubjectPartition &partition) const;

        /**
         * @brief Durably replaces one complete partition document.
         * @param partition Pseudonymous same-binding partition.
         * @param records Complete bounded replacement snapshot; every record must use @p partition.
         * @param expected Revision returned by LoadSnapshot on this adapter (or its copy) for this exact partition.
         * @return Success only after lock, durable preparation and the host atomic replacement's directory synchronization.
         * RevisionConflict means that no replacement was attempted: reload and revalidate instead of retrying a stale snapshot.
         * @details The complete document digest and presence are compared while holding the publication lock. There is no
         * unconditional replacement overload; corruption cannot be overwritten using a token captured before it occurred.
         */
        [[nodiscard]] Result<void> Publish(const PlatformOfflineSubjectPartition &partition,
                                           std::span<const PlatformOfflineQueueRecord> records,
                                           const PlatformOfflineQueueRevision &expected) const;

        /**
         * @brief Durably fences a corrupt partition while preserving its original document for explicit recovery.
         * @param partition Exact protected partition diagnosed by the owner; identity is never inferred from file contents.
         * @return Success when an invalid document is quarantined, or a typed healthy/I/O/publication failure.
         * @details Load and Publish fail closed after quarantine, including restart. Other partitions remain independent.
         * No automatic migration, deletion, empty replacement, provider call or credential fallback occurs.
         */
        [[nodiscard]] Result<void> Quarantine(const PlatformOfflineSubjectPartition &partition) const;

    private:
        /** @brief Lifetime scope for publication tokens, never account or replay authority. */
        struct Identity final {};

        PlatformOfflineQueueStorage(DurableFileSystem &files, std::filesystem::path root, PlatformOfflineQueueLimits limits);

        [[nodiscard]] std::filesystem::path PartitionPath(const PlatformOfflineSubjectPartition &partition) const;
        /** @brief Checks the durable quarantine fence without disclosing its private locator. @return Success only when absent. */
        [[nodiscard]] Result<void> CheckQuarantine(const PlatformOfflineSubjectPartition &partition) const;
        /** @brief Compares current bounded bytes and presence with a validated publication precondition.
         * @param path Exact partition document. @param expected Adapter/partition-scoped revision.
         * @pre The caller holds the partition publication lock and has checked its quarantine fence.
         * @return Success for unchanged storage, otherwise a typed conflict or unavailable failure. */
        [[nodiscard]] Result<void> CheckRevision(const std::filesystem::path &path, const PlatformOfflineQueueRevision &expected) const;
        [[nodiscard]] Result<std::vector<std::byte>> Encode(const PlatformOfflineSubjectPartition &partition,
                                                            std::span<const PlatformOfflineQueueRecord> records) const;

        DurableFileSystem *files_{};
        std::filesystem::path root_;
        PlatformOfflineQueueLimits limits_;
        std::shared_ptr<const Identity> identity_;
    };
}  // namespace Horo::PlatformOfflineQueue
