#pragma once

/** @file EditorPlaySaveHostArchive.h
 * @brief Detached PIE archive assembly through existing canonical-value and save-container contracts.
 */
#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"

namespace Horo::Application::Internal {
    inline constexpr std::size_t PlaySaveMaximumPayloadBytes = 8U << 20U;
    inline constexpr std::size_t PlaySaveMaximumArchiveBytes = 16U << 20U;
    inline constexpr std::size_t PlaySaveMaximumSlots = 16;

    /** @brief Returns finite bounds shared by actual worker publication and archive admission. */
    [[nodiscard]] Runtime::SaveArchiveReaderLimits PlaySaveArchiveLimits() noexcept;
    /** @brief Assembles actual detached participant records, preserving their identity and canonical bytes.
     * @param snapshot Actual canonical capture. @param header Host-issued sandbox/publication metadata.
     * @return Production-reader-verified immutable archive, or original bounded codec/container failure.
     * @details Uses generic save schema1. It does not claim the content-owned Scene schema2/dataset-absence authority.
     * No authoring serialization, live Scene access, callbacks, filesystem I/O or publication occurs here.
     */
    [[nodiscard]] Result<Runtime::FinalizedSaveArchive> BuildPlaySaveArchive(const Runtime::RuntimeSaveSnapshot &snapshot,
                                                                             const Runtime::SaveArchiveHeader &header);
    /** @brief Recomputes the exact schema1 logical-state hash from admitted actual archive records.
     * @param archive Production-reader integrity proof. @return Success or original malformed/hash/bounds failure.
     */
    [[nodiscard]] Result<void> ValidatePlaySaveArchiveState(const Runtime::ValidatedSaveArchive &archive);
}  // namespace Horo::Application::Internal
