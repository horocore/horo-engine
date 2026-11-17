#pragma once
/** @file EditorBaselineLightingAssets.h @brief Explicit read-only generic Metal lit/grid/shadow data; no compute or runtime compiler. */
#include "Horo/Runtime/Render/ShaderHotReloadTarget.h"

#include <array>
#include <filesystem>
#include <span>
#include <string_view>

namespace Horo::Editor {
    namespace BaselineLightingAssetErrors {
        inline const ErrorCodeDescriptor
            Unavailable{ErrorDomainId{"horo.editor.renderer"},
                        ErrorCode{"editor.renderer.baseline_lighting_assets_unavailable"},
                        ErrorSeverity::Error,
                        "The complete packaged Metal baseline lighting family is unavailable.",
                        "Cook and install all three current generic lit/grid/shadow programs using approved tools.",
                        false,
                        false};
    }

    /** @brief Owns real decoded raster data; no culling kernel or substituted clustered interface is present. */
    struct EditorBaselineLightingAssets final {
        std::array<Render::ShaderManifest, 3> manifests;
        std::array<Render::ShaderCompilationBatch, 3> programs;
        /** @brief Borrows stable logical roles1/2/3. @return Views valid only while this exact complete owner remains unmoved. */
        [[nodiscard]] std::array<Render::ShaderHotReloadProgramView, 3> ProgramViews() const noexcept;
    };

    /** @brief Returns the exact three baseline package prefixes. @return Lit/grid/shadow names, without discovery or fallback. */
    [[nodiscard]] std::span<const std::string_view, 3> PackagedMetalBaselineLightingPrefixes() noexcept;
    /** @brief Returns generic raster-only Metal2.4 metadata. @return Fixed target, without compute/storage/tool activation. */
    [[nodiscard]] Render::ShaderTargetRequirement EditorBaselineLightingMetalTarget();
    /** @brief Inspects only a known absolute package root and its exact six ordinary data files.
     * @param assetRoot Explicit package root. @return False iff all six files absent; partial/symlink/error returns typed failure.
     * @details Static path inspection is not race-free filesystem authority; host owns package paths throughout cold loading. */
    [[nodiscard]] Result<bool> HasPackagedMetalBaselineLightingInputs(const std::filesystem::path &assetRoot);
    /** @brief Reads the exact bounded family and validates actual normalized metadata and standard VS/PS payload envelopes.
     * @param assetRoot Explicit absolute package root. @return Complete immutable data candidate or original typed failure.
     * @details No native realization, provenance authentication, compiler process or alternate package search occurs. */
    [[nodiscard]] Result<EditorBaselineLightingAssets> LoadPackagedMetalBaselineLighting(const std::filesystem::path &assetRoot);
}  // namespace Horo::Editor
