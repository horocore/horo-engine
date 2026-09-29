#pragma once

/** @file UserStateMigrationErrors.h @brief Stable post-start user-state migration errors. */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::UserStateMigrationErrors {
    extern const ErrorCodeDescriptor InvalidPlan;
    extern const ErrorCodeDescriptor SourceChanged;
    extern const ErrorCodeDescriptor BackupRequiresRepair;
    extern const ErrorCodeDescriptor TransformFailed;
    extern const ErrorCodeDescriptor UnsafePath;
}  // namespace Horo::Release::UserStateMigrationErrors
