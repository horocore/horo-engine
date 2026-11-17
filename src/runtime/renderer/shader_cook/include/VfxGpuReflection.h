#pragma once
/** @file VfxGpuReflection.h @brief Closed same-operation Metal evidence for canonical GPU VFX compute and direct-draw roles. */
#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "ShaderReflectionCook.h"
#include "VfxGpuSpirvEvidence.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Render {
    /** @brief Admits only the two canonical manifests, authored names and Metal 2.4 target; no native capability is implied. */
    [[nodiscard]] Result<void> ValidateVfxGpuMetalReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                             std::span<const ShaderReflectionBindingName> names);
    /** @brief Qualifies only proven read-only selected-entry buffer arguments before actual Apple compilation.
     * @details The binary is the same validated operation input fed to Cross. Unknown pointer operations fail closed;
     * every non-qualifier source byte, including the complete shader body, is retained.
     */
    [[nodiscard]] Result<std::string> QualifyVfxGpuMetalInputs(const ShaderManifest &manifest, const ShaderEntryPoint &entry,
                                                               std::string_view source, std::span<const std::uint8_t> validatedSpirv);
    /** @brief Reconciles actual bounded Cross JSON, actual translated/qualified MSL and same-operation validated binary evidence. */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeVfxGpuMetalReflection(const ShaderManifest &manifest,
                                                                                    const ShaderTargetRequirement &target,
                                                                                    std::span<const SpirvReflectionStage> stages,
                                                                                    std::span<const std::string> translations,
                                                                                    std::span<const ShaderReflectionBindingName> names,
                                                                                    std::span<const std::uint8_t> validatedSpirv);
}  // namespace Horo::Render

namespace Horo::Render::VfxGpuCookDetail {
    struct Field final {
        std::string name;
        std::string type;
        std::uint32_t offset{};
        std::uint32_t bytes{};
        std::uint32_t matrixStride{};
    };

    struct Buffer final {
        std::string name;
        std::string wrapperMember;
        std::uint32_t slot{};
        std::uint32_t bytes{};
        bool constant{};
        bool readonly{};
        std::vector<Field> fields;
    };

    struct NativeEvidence final {
        std::vector<Buffer> buffers;
        std::vector<std::size_t> qualificationOffsets;
        std::vector<ShaderReflectedInterfaceVariable> interfaces;
    };

    [[nodiscard]] NativeEvidence ParseNative(std::string_view source, VfxGpuProgramRole role, ShaderStage stage, bool provenReadonly);
}  // namespace Horo::Render::VfxGpuCookDetail
