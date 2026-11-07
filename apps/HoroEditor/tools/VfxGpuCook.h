#pragma once
/** @file VfxGpuCook.h @brief Authenticated offline canonical GPU particle compute and direct-draw family production. */
#include "AuthenticatedMetalCookConfiguration.h"
#include "Horo/Runtime/Render/VfxGpuKernel.h"

#include <array>

namespace Horo::Editor::Offline {
    struct VfxGpuCookFamily final {
        Render::ShaderTargetRequirement target;
        std::array<Render::ShaderManifest, 2> manifests;
        std::array<Render::ShaderCompilationBatch, 2> compiled;
    };

    [[nodiscard]] Render::ShaderTargetRequirement VfxGpuMetalCookTarget();
    [[nodiscard]] std::span<const std::string_view, 2> VfxGpuCookPrefixes() noexcept;
    [[nodiscard]] Result<std::filesystem::path> VfxGpuCookSourcePath(const std::filesystem::path &, std::size_t);
    [[nodiscard]] Result<void> ValidateVfxGpuCookPaths(const std::filesystem::path &, const std::filesystem::path &);
    [[nodiscard]] Result<void> PublishVfxGpuCook(const std::filesystem::path &, const VfxGpuCookFamily &,
                                                 const Application::Internal::AuthenticatedMetalCookConfiguration &);
    [[nodiscard]] Result<void> CookVfxGpuFamily(const std::filesystem::path &, const std::filesystem::path &,
                                                const Application::Internal::AuthenticatedMetalCookInputs &);
}  // namespace Horo::Editor::Offline
