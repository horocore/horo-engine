#pragma once
/** @file VfxGpuVulkanCook.h @brief Authenticated offline SPIR-V family; no Apple/Metal tool is required or invoked. */
#include "AuthenticatedShaderCookConfiguration.h"
#include "Horo/Runtime/Render/VfxGpuKernel.h"

#include <array>

namespace Horo::Editor::Offline {
    struct VfxGpuVulkanCookFamily final {
        Render::ShaderTargetRequirement target;
        std::array<Render::ShaderManifest, 2> manifests;
        std::array<Render::ShaderCompilationBatch, 2> compiled;
    };

    [[nodiscard]] Render::ShaderTargetRequirement VfxGpuVulkanCookTarget();
    [[nodiscard]] Result<Render::ShaderCompilerTargetDescriptor> VfxGpuVulkanCookDescriptor(
        const Render::ShaderCompilerToolchainConfiguration &);
    [[nodiscard]] std::span<const std::string_view, 2> VfxGpuVulkanCookPrefixes() noexcept;
    [[nodiscard]] Result<std::filesystem::path> VfxGpuVulkanCookSourcePath(const std::filesystem::path &, std::size_t);
    [[nodiscard]] Result<void> ValidateVfxGpuVulkanCookPaths(const std::filesystem::path &, const std::filesystem::path &);
    [[nodiscard]] Result<void> PublishVfxGpuVulkanCook(const std::filesystem::path &, const VfxGpuVulkanCookFamily &,
                                                       const Application::Internal::AuthenticatedShaderCookConfiguration &);
    [[nodiscard]] Result<void> CookVfxGpuVulkanFamily(const std::filesystem::path &, const std::filesystem::path &,
                                                      const Application::Internal::AuthenticatedSpirvCookInputs &);
}  // namespace Horo::Editor::Offline
