#pragma once

/**
 * @file UpdateDeltaStaging.h
 * @brief Private reconstruction of an exact target tree from verified base and delta trees.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Release/UpdateFileDelta.h"

namespace Horo::Release {
    /**
     * @brief Copies only planned files into a new private tree and verifies its complete target inventory.
     * @param plan Canonical base, patch, and target inventory plan; revalidated before use.
     * @param baseRoot Quiescent previously authenticated base tree.
     * @param deltaRoot Quiescent independently authenticated delta tree.
     * @param stageRoot Absent, host-owned private destination outside both source trees.
     * @param limits Archive and total-byte limits for every source and target inventory.
     * @param files Durable filesystem retained for the entire operation.
     * @param cancellation Cooperative cancellation before each file copy and final verification.
     * @return Success only for an exact, complete target tree; failure removes the private destination.
     * @note This operation does not publish a ready marker or grant activation authority. The host must bind the
     *       reconstructed tree to signed full-target metadata before activation.
     */
    [[nodiscard]] Result<void> ReconstructUpdateFileDeltaStage(const UpdateFileDeltaPlan &plan, const std::filesystem::path &baseRoot,
                                                               const std::filesystem::path &deltaRoot,
                                                               const std::filesystem::path &stageRoot, const UpdateArchiveLimits &limits,
                                                               NativeDurableFileSystem &files, CancellationToken cancellation);
}  // namespace Horo::Release
