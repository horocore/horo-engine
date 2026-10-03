#pragma once

/**
 * @file BootstrapInstallationErrors.h
 * @brief Stable first-install admission, activation, and recovery failures.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::BootstrapInstallationErrors {
    /** @brief Destination, package identity, or immutable version layout is invalid. */
    extern const ErrorCodeDescriptor InvalidLayout;
    /** @brief Another product or version already owns the active installation. */
    extern const ErrorCodeDescriptor AlreadyInstalled;
    /** @brief An interrupted first install has unexpected journal or active state. */
    extern const ErrorCodeDescriptor PendingMismatch;
    /** @brief Platform permission, capacity, OS, architecture, or conflict preflight failed. */
    extern const ErrorCodeDescriptor PreflightFailed;
    /** @brief Operating-system integration could not be registered. */
    extern const ErrorCodeDescriptor IntegrationFailed;
    /** @brief First-launch health failed and the new active pointer was removed. */
    extern const ErrorCodeDescriptor HealthFailed;
    /** @brief The active pointer or integration could not be safely undone. */
    extern const ErrorCodeDescriptor RecoveryFailed;
    /** @brief Repair cannot prove that the current active version is healthy. */
    extern const ErrorCodeDescriptor RepairFailed;
    /** @brief Uninstall cannot prove that integration or owned files were removed. */
    extern const ErrorCodeDescriptor UninstallFailed;
}  // namespace Horo::Release::BootstrapInstallationErrors
