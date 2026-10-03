#pragma once

#include "Horo/Release/BootstrapInstallation.h"

namespace Horo::Release {
    /** @brief Removes only authenticated portable package bytes and exact declared staged files. */
    [[nodiscard]] Result<void> RemoveVerifiedPortableVersion(const BootstrapInstallationRequest &request, NativeDurableFileSystem &files,
                                                             const Security::ArtifactVerifier &verifier);
}  // namespace Horo::Release
