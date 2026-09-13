#pragma once

/**
 * @file SaveSlotIndex.h
 * @brief Rebuildable, bounded save-slot index contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveSlotMetadata.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    /** @brief Current durable slot-index schema written by this runtime. */
    inline constexpr std::uint32_t SaveSlotIndexSchemaVersion = 1;

    /** @brief Explicit bounds for index admission and incremental reconstruction. */
    struct SaveSlotIndexLimits final {
        std::size_t maximumEntries{4'096};     /**< Maximum published slot records. */
        std::size_t maximumArtifacts{8'192};   /**< Maximum observations in one rebuild. */
        std::size_t maximumDiagnostics{8'192}; /**< Maximum retained rebuild diagnostics. */
    };

    /** @brief Derived catalog snapshot; committed archives remain authoritative. */
    struct SaveSlotIndex final {
        std::uint32_t schemaVersion{SaveSlotIndexSchemaVersion}; /**< Exact wire/model schema. */
        std::uint64_t revision{};                                /**< Non-zero monotonic catalog publication revision. */
        std::vector<SaveSlotCatalogEntry> entries;               /**< Unique entries in canonical slot-identity order. */
    };

    /** @brief Storage classification supplied by the qualified save storage scanner. */
    enum class SaveSlotArtifactState : std::uint8_t {
        Committed,
        Corrupt,
        Temporary,
    };

    /** @brief One bounded scan observation without exposing filesystem paths. */
    struct SaveSlotArtifactObservation final {
        SaveSlotArtifactState state{SaveSlotArtifactState::Corrupt}; /**< Scanner classification. */
        std::optional<SaveSlotCatalogEntry> entry;                   /**< Present only for decoded committed artifacts. */
        std::optional<SaveGameSlotId> suspectedSlot;                 /**< Optional safe identity for corrupt diagnostics. */
    };

    /** @brief Stable discrepancy categories emitted by index reconstruction. */
    enum class SaveSlotIndexDiagnosticKind : std::uint8_t {
        Missing,
        Stale,
        Duplicate,
        Orphaned,
        Corrupt,
    };

    /** @brief Bounded safe diagnostic for one index/storage discrepancy. */
    struct SaveSlotIndexDiagnostic final {
        SaveSlotIndexDiagnosticKind kind{SaveSlotIndexDiagnosticKind::Corrupt}; /**< Discrepancy classification. */
        std::optional<SaveGameSlotId> slot;                                     /**< Related slot when safely decoded. */
        std::optional<SlotGenerationId> generation;                             /**< Related committed generation, if known. */
    };

    /** @brief Immutable candidate returned only after a complete deterministic rebuild. */
    struct SaveSlotIndexRebuildResult final {
        SaveSlotIndex candidate;                          /**< Replacement index ready for atomic storage publication. */
        std::vector<SaveSlotIndexDiagnostic> diagnostics; /**< Stable-ordered discrepancy evidence. */
        std::size_t artifactsExamined{};                  /**< Total bounded observations consumed. */
    };

    /**
     * @brief Stateful bounded reconstruction of a derived slot index.
     *
     * The builder never mutates the prior index or committed artifacts. A caller publishes the
     * finalized candidate with its qualified atomic storage primitive only after Finalize succeeds.
     */
    class SaveSlotIndexRebuilder final {
    public:
        /**
         * @brief Creates an incremental rebuild.
         * @param previous Last decoded index, or no value when absent/corrupt.
         * @param nextRevision Non-zero revision for the candidate publication.
         * @param limits Trusted finite operation bounds.
         * @return A private builder or a stable index configuration/allocation error.
         */
        [[nodiscard]] static Result<SaveSlotIndexRebuilder> Create(std::optional<SaveSlotIndex> previous, std::uint64_t nextRevision,
                                                                   SaveSlotIndexLimits limits = {});

        /**
         * @brief Consumes at most one explicitly bounded scan page.
         * @param observations Detached scanner observations.
         * @param maximumToConsume Non-zero per-call work bound.
         * @return Number consumed, or a stable limit/state/allocation error.
         */
        [[nodiscard]] Result<std::size_t> Consume(std::span<const SaveSlotArtifactObservation> observations, std::size_t maximumToConsume);

        /**
         * @brief Seals deterministic diagnostics and the replacement index.
         * @return Complete candidate, or an error if already sealed or invalid.
         */
        [[nodiscard]] Result<SaveSlotIndexRebuildResult> Finalize();

    private:
        SaveSlotIndexRebuilder(std::optional<SaveSlotIndex> previous, std::uint64_t nextRevision, SaveSlotIndexLimits limits);

        std::optional<SaveSlotIndex> previous_;
        std::uint64_t nextRevision_{};
        SaveSlotIndexLimits limits_;
        std::vector<SaveSlotCatalogEntry> committed_;
        std::vector<SaveSlotIndexDiagnostic> diagnostics_;
        std::size_t artifactsExamined_{};
        bool sealed_{};
    };

    /**
     * @brief Validates index schema, revision, entry metadata, and canonical ordering.
     * @param index Candidate decoded or rebuilt index.
     * @param limits Trusted bounds.
     * @return Success or a stable slot-index error.
     */
    [[nodiscard]] Result<void> ValidateSaveSlotIndex(const SaveSlotIndex &index, const SaveSlotIndexLimits &limits = {});
}  // namespace Horo::Runtime
