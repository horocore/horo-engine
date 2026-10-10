#pragma once

/** @file SaveSlotRetention.h
 * @brief Bounded durable catalog retention policy and exact generation diagnostics.
 */

#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"

#include <array>

namespace Horo::Runtime {
    /** @brief Product-owned limits for one automatic slot category; manual slots are excluded. */
    struct SaveSlotRetentionLimits final {
        std::uint16_t maximumSlots{3};          /**< Normal visible capacity, at least two for safe rotation. */
        std::uint16_t lowSpaceSlots{2};         /**< Explicit low-space capacity, at least two. */
        std::uint16_t minimumSlots{1};          /**< Age pruning cannot go below this or remove the newest publication. */
        std::uint16_t backupGenerations{1};     /**< Last-known-good archives retained after retirement, at least one. */
        std::uint64_t maximumAgeMilliseconds{}; /**< Zero disables age pruning; uses host commit time, never archive time. */
    };

    /** @brief Immutable namespace policy; enabling it does not grant publication or retention capabilities. */
    struct SaveSlotRetentionPolicy final {
        bool enabled{};
        std::array<SaveSlotRetentionLimits, 2> kinds{}; /**< Auto then Checkpoint; limits apply independently. */
        bool cloudTombstones{}; /**< Retired generations remain physically held until exact host-confirmed remote deletion. */
    };

    /** @brief Reason selected deterministically before the same durable save publication gate. */
    enum class SaveSlotRetentionReason : std::uint8_t {
        Replacement,
        Capacity,
        Age,
        LowSpace
    };

    /** @brief Observable exact retirement; timestamps and identity bytes never determine order. */
    struct SaveSlotRetentionDecision final {
        SaveSlotCatalogEntry entry;
        SaveSlotRetentionReason reason{SaveSlotRetentionReason::Capacity};
        std::uint64_t sequence{}; /**< Catalog-issued publication order, not caller-supplied. */
    };

    /** @brief Exact remote deletion evidence retained before its local index row disappears. */
    struct SaveSlotRetentionCloudDeletion final {
        SaveCloudMetadataScope scope;
        SaveCloudGenerationRecord generation; /**< Provider object and CAS evidence; Unknown remains an explicit reconciliation task. */
    };

    /** @brief One authoritative catalog observation, suitable for recovery/remote coordinator handoff. */
    struct SaveSlotRetentionRecord final {
        SaveSlotCatalogEntry entry;
        std::uint64_t sequence{}; /**< Zero denotes legacy unknown order and is protected from automatic pruning. */
        std::uint64_t committedAtMilliseconds{};
        bool pinned{};
        bool deleted{};
        bool backup{};
        bool pendingCloudDelete{};                           /**< Explicit durable tombstone; never inferred from an absent listing row. */
        std::optional<SaveSlotRetentionCloudDeletion> cloud; /**< Exact scope/object evidence, preserved after confirmation for backups. */
    };

    /** @brief Complete bounded projection from the sole lifecycle catalog, never an alternate mutable index. */
    struct SaveSlotRetentionSnapshot final {
        std::uint64_t catalogRevision{};
        std::uint64_t lastCommitMilliseconds{}; /**< Persisted nondecreasing host clock. */
        std::vector<SaveSlotRetentionRecord> selected;
        std::vector<SaveSlotRetentionRecord> retained;
    };
}  // namespace Horo::Runtime
