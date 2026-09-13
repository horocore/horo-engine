#pragma once

/**
 * @file SaveSlotMetadata.h
 * @brief Backend-neutral save-slot kinds and bounded catalog metadata contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveArchiveMetadata.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace Horo::Runtime {
    struct SaveCheckpointIdentityTag;
    struct SaveThumbnailIdentityTag;

    /** @brief Opaque product-owned checkpoint identity used only for catalog presentation. */
    using SaveCheckpointId = PersistentSaveIdentity<SaveCheckpointIdentityTag>;
    /** @brief Opaque thumbnail-content reference without renderer or filesystem ownership. */
    using SaveThumbnailId = PersistentSaveIdentity<SaveThumbnailIdentityTag>;

    /** @brief Stable built-in slot category used by capacity, rotation, and presentation policy. */
    enum class SaveSlotKind : std::uint8_t {
        Manual = 0,
        Quick = 1,
        Auto = 2,
        Checkpoint = 3,
        Recovery = 4,
        System = 5,
    };

    /** @brief Non-authoritative cloud synchronization summary for slot listings. */
    enum class SaveSlotCloudState : std::uint8_t {
        LocalOnly = 0,
        UploadPending = 1,
        Synchronized = 2,
        DownloadPending = 3,
        Conflict = 4,
    };

    /** @brief Explicit admission bounds for trusted and caller-owned slot metadata. */
    struct SaveSlotMetadataLimits final {
        std::size_t maximumBuildIdBytes{256};        /**< Maximum trusted build provenance bytes. */
        std::size_t maximumDisplayNameBytes{256};    /**< Maximum caller-owned display-name bytes. */
        std::size_t maximumDisplaySummaryBytes{512}; /**< Maximum caller-owned summary bytes. */
    };

    /**
     * @brief Trusted catalog facts describing one current committed slot publication.
     *
     * This value is suitable for a catalog listing without participant-payload decoding.
     * The generation identifies the publication independently from both hashes and time.
     * Kind reclassification is a catalog-only transaction and does not manufacture a new generation.
     */
    struct SaveSlotPublicationMetadata final {
        SaveGameSlotId slot;                                          /**< Logical slot stable across overwrites. */
        SlotGenerationId generation;                                  /**< Exact durable publication identity. */
        SaveSlotKind kind{SaveSlotKind::Manual};                      /**< Reclassifiable product catalog policy category. */
        std::uint64_t savedAtUnixMilliseconds{};                      /**< Presentation timestamp, never causality. */
        std::uint64_t playTimeNanoseconds{};                          /**< Accumulated semantic play duration. */
        SaveBaseSceneId baseScene;                                    /**< Stable compatible base-scene identity. */
        std::optional<SaveCheckpointId> checkpoint;                   /**< Optional product checkpoint reference. */
        std::optional<SaveThumbnailId> thumbnail;                     /**< Optional content reference; no GPU ownership. */
        ProductSaveCompatibilityVersion productCompatibility;         /**< Producing product policy version. */
        SaveSchemaVersion saveSchema;                                 /**< Producing canonical save schema. */
        std::string projectBuildId;                                   /**< Bounded trusted diagnostic provenance. */
        CanonicalStateHash canonicalState;                            /**< Logical-state equivalence identity. */
        ArchiveContentHash archiveContent;                            /**< Exact immutable archive-content identity. */
        SaveSlotCloudState cloudState{SaveSlotCloudState::LocalOnly}; /**< Non-causal listing summary. */

        [[nodiscard]] auto operator<=>(const SaveSlotPublicationMetadata &) const noexcept = default;
    };

    /**
     * @brief Caller-owned advisory slot presentation, excluded from identity and compatibility.
     *
     * Invalid presentation may be omitted or replaced by UI fallback text; it never makes a
     * validated archive or trusted publication metadata unrecoverable. Localization of slot kind
     * and cloud state belongs to the UI and is not stored here.
     */
    struct SaveSlotDisplayMetadata final {
        std::string displayName; /**< Optional bounded UTF-8 label; duplicates are allowed. */
        std::string summary;     /**< Optional bounded UTF-8 game-provided presentation text. */

        [[nodiscard]] auto operator<=>(const SaveSlotDisplayMetadata &) const noexcept = default;
    };

    /** @brief Complete catalog projection with independently validated trusted and display fields. */
    struct SaveSlotCatalogEntry final {
        SaveSlotPublicationMetadata publication; /**< Trusted committed publication facts. */
        SaveSlotDisplayMetadata display;         /**< Non-authoritative caller-owned presentation. */

        [[nodiscard]] auto operator<=>(const SaveSlotCatalogEntry &) const noexcept = default;
    };

    /**
     * @brief Validates trusted metadata for one committed slot publication.
     * @param metadata Trusted catalog facts to validate.
     * @param limits Explicit byte bounds.
     * @return Success, SaveErrors::SlotMetadataInvalid, or SaveErrors::SlotMetadataLimitExceeded.
     */
    [[nodiscard]] Result<void> ValidateSaveSlotPublicationMetadata(const SaveSlotPublicationMetadata &metadata,
                                                                   const SaveSlotMetadataLimits &limits = {});

    /**
     * @brief Validates caller-owned presentation without consulting trusted publication state.
     * @param metadata Advisory UTF-8 presentation to validate.
     * @param limits Explicit byte bounds applied before scanning text.
     * @return Success or SaveErrors::SlotDisplayMetadataInvalid.
     */
    [[nodiscard]] Result<void> ValidateSaveSlotDisplayMetadata(const SaveSlotDisplayMetadata &metadata,
                                                               const SaveSlotMetadataLimits &limits = {});

    /**
     * @brief Validates publication replacement for one logical slot.
     * @param previous Previously committed trusted publication.
     * @param replacement Candidate committed trusted publication.
     * @param limits Explicit byte bounds.
     * @return Success only when both records are valid, the slot is unchanged, and generation is new;
     * otherwise a stable slot metadata or generation error. Identical state/content hashes are allowed.
     */
    [[nodiscard]] Result<void> ValidateSaveSlotPublicationReplacement(const SaveSlotPublicationMetadata &previous,
                                                                      const SaveSlotPublicationMetadata &replacement,
                                                                      const SaveSlotMetadataLimits &limits = {});
}  // namespace Horo::Runtime
