#pragma once

#include "Horo/Release/UpdateActivation.h"

namespace Horo::Release::TestSupport {
    [[nodiscard]] Security::ArtifactVerifier MakeVerifier();
    [[nodiscard]] UpdateActivationVersion MakeVersion(const std::filesystem::path &root, NativeDurableFileSystem &files,
                                                      const Security::ArtifactVerifier &verifier);
}  // namespace Horo::Release::TestSupport
