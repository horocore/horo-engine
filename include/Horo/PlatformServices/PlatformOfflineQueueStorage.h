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
    }  // namespace PlatformOfflineQueueErrors

    /**
     * @brief Owns one partition's versioned queue document and publishes it atomically.
     * @details The adapter stores opaque Horo identities and treats bounded payload bytes as opaque. Callers must provide canonical Horo
     * intent bytes and must not supply provider-native data or cloud archive bytes. The adapter does not inspect payload contents and
     * never accepts a provider account identifier, live subject handle, credential or callback.
     */
    class PlatformOfflineQueueStorage final {
    public:
        /**
         * @brief Validates and creates a storage adapter.
         * @param files Host-owned durable filesystem primitives.
         * @param root Product state root supplied by the composition root.
         * @param limits Finite record, payload and document bounds.
         * @return Adapter or a typed configuration failure.
         */
        [[nodiscard]] static Result<PlatformOfflineQueueStorage> Create(DurableFileSystem &files, std::filesystem::path root,
                                                                        const PlatformOfflineQueueLimits &limits = {});

        /**
         * @brief Loads one opaque subject partition without invoking a provider.
         * @param partition Pseudonymous same-binding partition.
         * @return Validated records, an empty vector for a new partition, or a typed recovery failure.
         */
        [[nodiscard]] Result<std::vector<PlatformOfflineQueueRecord>> Load(const PlatformOfflineSubjectPartition &partition) const;

        /**
         * @brief Durably replaces one complete partition document.
         * @param partition Pseudonymous same-binding partition.
         * @param records Complete bounded replacement snapshot; every record must use @p partition.
         * @return Success only after lock, durable preparation and the host atomic replacement's directory synchronization.
         */
        [[nodiscard]] Result<void> Publish(const PlatformOfflineSubjectPartition &partition,
                                           std::span<const PlatformOfflineQueueRecord> records) const;

    private:
        PlatformOfflineQueueStorage(DurableFileSystem &files, std::filesystem::path root, PlatformOfflineQueueLimits limits) noexcept;

        [[nodiscard]] std::filesystem::path PartitionPath(const PlatformOfflineSubjectPartition &partition) const;
        [[nodiscard]] Result<std::vector<std::byte>> Encode(const PlatformOfflineSubjectPartition &partition,
                                                            std::span<const PlatformOfflineQueueRecord> records) const;

        DurableFileSystem *files_{};
        std::filesystem::path root_;
        PlatformOfflineQueueLimits limits_;
    };
}  // namespace Horo::PlatformOfflineQueue
