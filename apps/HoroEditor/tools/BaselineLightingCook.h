#pragma once
/** @file BaselineLightingCook.h @brief Explicit authenticated offline generic Metal lit/grid/shadow production. */
#include "AuthenticatedMetalCookConfiguration.h"

#include <array>

namespace Horo::Editor::Offline {
    /** @brief Owns actual canonical manifests and compiler batches for exactly three raster roles; no compute entry exists. */
    struct BaselineLightingCookFamily final {
        std::array<Render::ShaderManifest, 3> manifests;
        std::array<Render::ShaderCompilationBatch, 3> compiled;
    };

    /** @brief Resolves one actual generic raster source. @param repositoryRoot Absolute repository. @param index Lit0/grid1/shadow2.
     * @return Exact assets path or typed invalid-request failure. */
    [[nodiscard]] Result<std::filesystem::path> BaselineLightingCookSourcePath(const std::filesystem::path &repositoryRoot,
                                                                               std::size_t index);
    /** @brief Checks a new owned destination and existing scratch without mutation. @param destination New package. @param scratch Existing
     * scratch.
     * @return Existing atomic publisher's exact filesystem admission. */
    [[nodiscard]] Result<void> ValidateBaselineLightingCookPaths(const std::filesystem::path &destination,
                                                                 const std::filesystem::path &scratch);
    /** @brief Atomically publishes real compiler batches after complete read-only decoder validation.
     * @param destination New package root. @param family Actual generic compilation. @param host Independently authenticated host.
     * @return Complete exclusive publication or original typed failure; no existing destination is overwritten. */
    [[nodiscard]] Result<void> PublishBaselineLightingCook(const std::filesystem::path &destination,
                                                           const BaselineLightingCookFamily &family,
                                                           const Application::Internal::AuthenticatedMetalCookConfiguration &host);
    /** @brief Authenticates explicit tools, compiles all current generic recipes, and exclusively publishes the complete family.
     * @param sourceRoot Absolute repository. @param destination New owned directory. @param inputs Exact approved host configuration.
     * @return Genuine complete offline generation or typed failure; no discovery or runtime compilation is supplied. */
    [[nodiscard]] Result<void> CookEditorBaselineLighting(const std::filesystem::path &sourceRoot, const std::filesystem::path &destination,
                                                          const Application::Internal::AuthenticatedMetalCookInputs &inputs);
}  // namespace Horo::Editor::Offline
