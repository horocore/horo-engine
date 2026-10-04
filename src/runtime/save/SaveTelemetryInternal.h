#pragma once

/** @file SaveTelemetryInternal.h
 * @brief Target-private terminal observation after the Save operation lock is released.
 */
namespace Horo::Runtime {
    struct SaveOperationSnapshot;
    /** @brief Emits closed terminal evidence without changing callbacks or retaining operation state.
     * @param snapshot Immutable winning terminal snapshot, borrowed for this synchronous call only.
     */
    void RecordSaveOperationTerminal(const SaveOperationSnapshot &snapshot) noexcept;
}  // namespace Horo::Runtime
