#pragma once

/** @file NativeCredentialBackend.h @brief Host composition for operating-system credential storage. */

#include "Horo/Security/CredentialStore.h"

#include <memory>

namespace Horo::Platform {
    /** @brief Create the native per-user, machine-local credential backend.
     * @return A native backend when one is configured for the build, or an empty pointer otherwise.
     * @details Creation performs no credential lookup. Hosts supply typed references and call backend operations off the UI lane.
     */
    [[nodiscard]] std::shared_ptr<Security::CredentialBackend> CreateNativeCredentialBackend();
}  // namespace Horo::Platform
