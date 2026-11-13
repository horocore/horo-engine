#pragma once

/** @file ProjectMigrationErrors.h
 * @brief Narrow shared failures for independently composed candidate migration stages.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Application::ProjectErrors {
    /** @brief A stage cannot validate or transform its owned source without an explicit corrective action. */
    extern const ErrorCodeDescriptor MigrationStageFailed;
    /** @brief Cooperative cancellation stopped a migration before authoritative publication. */
    extern const ErrorCodeDescriptor MigrationCancelled;
}  // namespace Horo::Application::ProjectErrors
