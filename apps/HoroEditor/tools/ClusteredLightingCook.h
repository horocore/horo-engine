#pragma once
/** @file ClusteredLightingCook.h @brief Explicit offline Metal clustered-family production and atomic publication. */
#include "AuthenticatedMetalCookConfiguration.h"

#include <array>

namespace Horo::Editor::Offline {
    /** @brief Actual compiler pipeline outputs and their canonical source manifests, never native handles. */
    struct ClusteredLightingCookFamily final {
        std::array<Render::ShaderManifest, 4> manifests;
        std::array<Render::ShaderCompilationBatch, 4> compiled;
    };

    /** @brief Resolves one canonical program beneath an explicit repository root's assets directory.
     * @param repositoryRoot Absolute repository root. @param programIndex Lit/grid/shadow/kernel index in [0,4).
     * @return Exact source path consumed by compilation, or typed invalid-input failure. */
    [[nodiscard]] Result<std::filesystem::path> ClusteredLightingCookSourcePath(const std::filesystem::path &repositoryRoot,
                                                                                std::size_t programIndex);
    /** @brief Checks the new absolute destination without creating scratch or package state. */
    [[nodiscard]] Result<void> ValidateClusteredLightingCookDestination(const std::filesystem::path &destination);
    /** @brief Checks an existing absolute scratch directory and rejects output within that actual directory tree.
     * @details Resolves existing directory identities and ancestors, including symlink/case aliases. Creates nothing.
     * The host must independently own scratch and the output parent throughout the offline operation. */
    [[nodiscard]] Result<void> ValidateClusteredLightingCookPaths(const std::filesystem::path &destination,
                                                                  const std::filesystem::path &scratch);
    /** @brief Writes a private candidate, reuses the real runtime decoder, then renames the complete new package directory.
     * @pre The caller exclusively owns the existing destination parent for this offline operation. Destination must not exist.
     * @details Failure removes only the directory claimed by this operation. No existing package or unrelated candidate is overwritten. */
    [[nodiscard]] Result<void> PublishClusteredLightingCook(const std::filesystem::path &destination,
                                                            const ClusteredLightingCookFamily &family,
                                                            const Application::Internal::AuthenticatedMetalCookConfiguration &host);
    /** @brief Authenticates explicit host inputs, compiles current canonical sources with the existing adapter, then publishes.
     * @details A signed catalog must independently approve all four actual host-native tools. No discovery or runtime fallback exists. */
    [[nodiscard]] Result<void> CookEditorClusteredLighting(const std::filesystem::path &sourceRoot,
                                                           const std::filesystem::path &destination,
                                                           const Application::Internal::AuthenticatedMetalCookInputs &inputs);
}  // namespace Horo::Editor::Offline
