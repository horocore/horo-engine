#pragma once
/** @file VfxGpuSpirvEvidence.h @brief One bounded backend-neutral access/layout proof for canonical VFX SPIR-V. */
#include "Horo/Runtime/Render/VfxGpuKernel.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Render::VfxGpuCookDetail {
    struct InvalidEvidence {};

    struct BinaryEvidence final {
        bool rowMajor{};
        std::vector<std::string> blockNames;
        std::vector<std::vector<std::string>> memberNames;
    };

    [[nodiscard]] VfxGpuProgramRole Role(const ShaderManifest &manifest);
    [[nodiscard]] bool Compute(VfxGpuProgramRole role) noexcept;
    [[nodiscard]] std::string_view EntryName(VfxGpuProgramRole role, ShaderStage stage);
    [[nodiscard]] std::span<const std::string_view> ResourceNames(VfxGpuProgramRole role);
    [[nodiscard]] std::span<const std::string_view> UniformNames(VfxGpuProgramRole role);
    [[nodiscard]] bool CanonicalInterface(const ShaderManifest &, const ShaderTargetRequirement &,
                                          std::span<const ShaderReflectionBindingName>);
    /** @brief Proves actual selected compute/vertex entry, roots/layout and no writes/escapes to readonly buffers; unknown ops reject. */
    [[nodiscard]] BinaryEvidence ProveSpirv(std::span<const std::uint8_t> bytes, VfxGpuProgramRole role);
}  // namespace Horo::Render::VfxGpuCookDetail
