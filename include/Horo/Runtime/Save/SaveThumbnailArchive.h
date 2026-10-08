#pragma once

/** @file SaveThumbnailArchive.h
 * @brief Existing-format optional presentation chunks and worker-side save composition.
 */

#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
#include "Horo/Runtime/Save/SaveThumbnailCapture.h"

namespace Horo::Runtime {
    /** @brief Reserved optional owner that never participates in gameplay restore or canonical state. @return Stable owner ID. */
    [[nodiscard]] SaveParticipantId SaveThumbnailArchiveOwner();
    /** @brief Reserved provenance record; collisions are rejected. @return Stable record ID. */
    [[nodiscard]] SaveRecordId SaveThumbnailMetadataRecord();
    /** @brief Reserved raw PNG record; collisions are rejected. @return Stable record ID. */
    [[nodiscard]] SaveRecordId SaveThumbnailImageRecord();

    /** @brief Owned worker input; logical chunks/state are produced independently before presentation attachment. */
    struct SavePresentationArchiveInput final {
        SaveArchiveHeader header;
        SaveGameManifest manifest; /**< Logical CanonicalStateHash is retained unchanged. */
        std::vector<PreservedSaveChunk> chunks;
        ArchiveFormatVersion version;
        SaveSlotPublicationMetadata publication;
        SaveSlotDisplayMetadata display;
        SaveThumbnailCaptureSnapshot capture;
        SaveArchiveReaderLimits limits;
    };

    /** @brief Finalizes existing v1/v2 framing with optional raw presentation records and exact catalog metadata.
     * @param input Bounded owned logical input and terminal capture. Encoding/hashing runs on a worker.
     * @return Finalized write or typed error before provider/commit admission. Omitted optional capture still builds a valid save.
     * @details The reserved owner is optional, schema 1, and excluded from logical state/restore. Old readers preserve it
     * as unknown optional data; only an explicit sealed drop policy may omit it. No archive/container wire version changes.
     */
    [[nodiscard]] Result<SaveStorageWrite> PrepareSavePresentationWrite(const SavePresentationArchiveInput &input);

    /** @brief Reads integrity-verified optional PNG/provenance without activating any gameplay participant.
     * @param archive Verified owned/borrowed archive. @param publication Exact committed slot/generation/thumbnail reference.
     * @param limits Product dimensions and encoded-byte ceilings. Timeout is ignored while loading completed data.
     * @return Detached CPU artifact, absence, or typed stale/malformed/limit error; callers may omit an invalid optional image.
     */
    [[nodiscard]] Result<std::optional<std::shared_ptr<const SaveThumbnailArtifact>>> ReadSaveThumbnail(
        const ValidatedSaveArchive &archive, const SaveSlotPublicationMetadata &publication, const SaveThumbnailLimits &limits = {});
}  // namespace Horo::Runtime
