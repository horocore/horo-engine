#pragma once

#include "ShaderCompilerToolchain.h"

namespace Horo::Render::ShaderCompilerToolchainDetail {
    [[nodiscard]] bool IsApproved(const ShaderCompilerToolchainConfiguration &configuration, const ShaderCompilerToolInstallation &tool);
}  // namespace Horo::Render::ShaderCompilerToolchainDetail
