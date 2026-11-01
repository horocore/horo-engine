#pragma once
/** @file EditorClusteredLightingAssets.h @brief Cold loading of the complete offline-cooked Metal clustered lighting family. */
#include "Horo/Runtime/Render/LightCullingKernel.h"
#include "Horo/Runtime/Render/ShaderHotReloadTarget.h"

#include <array>
#include <filesystem>
#include <span>
#include <string_view>

namespace Horo::Editor {
    namespace ClusteredLightingAssetErrors {
        inline const ErrorCodeDescriptor
            Unavailable{ErrorDomainId{"horo.editor.renderer"},
                        ErrorCode{"editor.renderer.clustered_lighting_assets_unavailable"},
                        ErrorSeverity::Error,
                        "The complete packaged Metal clustered lighting shader family is unavailable.",
                        "Rebuild and install the complete clustered lighting shader bundle using approved tools.",
                        false,
                        false};
    }

    /** @brief Owned immutable payloads and actual decoded reflection, retained through synchronous native cold preparation. */
    struct EditorClusteredLightingAssets final {
        std::array<Render::ShaderManifest, 3> manifests;
        std::array<Render::ShaderCompilationBatch, 3> programs;
        Render::CookedLightCullingKernel kernel;
        /** @brief Borrows this complete object's stable roles 1/2/3; views must not outlive or move their owner. */
        [[nodiscard]] std::array<Render::ShaderHotReloadProgramView, 3> ProgramViews() const noexcept;
    };

    /** @brief Borrows the immutable four-prefix package inventory used by both offline producer and runtime loader. */
    [[nodiscard]] std::span<const std::string_view, 4> PackagedMetalClusteredLightingPrefixes() noexcept;
    /** @brief Canonical Metal 2.4 target shared by raster family and compute kernel; no tools are selected. */
    [[nodiscard]] Render::ShaderTargetRequirement EditorClusteredLightingMetalTarget();
    /** @brief Inspects the exact four-pair inventory without reading or validating shader contents.
     * @return False only when all eight files are absent; true means a complete regular-file candidate.
     * Partial, symlink and filesystem failures remain typed errors, never missing-family fallback. */
    [[nodiscard]] Result<bool> HasPackagedMetalClusteredLightingInputs(const std::filesystem::path &assetRoot);
    /** @brief Reads only the four exact bounded packaged pairs and validates their same-artifact metadata.
     * @details Cold host operation; never compiles, chooses a fallback, publishes partial resources or searches alternate paths.
     * CI verifies the complete data-only bundle's exact source/run provenance before packaging. */
    [[nodiscard]] Result<EditorClusteredLightingAssets> LoadPackagedMetalClusteredLighting(const std::filesystem::path &assetRoot);
}  // namespace Horo::Editor
