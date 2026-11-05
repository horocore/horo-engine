#pragma once

/** @file ExposureGpuCook.h
 * @brief Authenticated offline production of the closed exposure compute family.
 */
#include "AuthenticatedMetalCookConfiguration.h"
#include "Horo/Runtime/Render/ExposureGpuKernel.h"

#include <array>
#include <filesystem>
#include <span>
#include <string_view>

namespace Horo::Editor::Offline {
    /** @brief Genuine compiler outputs and original manifests in clear/build/adapt order. */
    struct ExposureGpuCookFamily final {
        Render::ShaderTargetRequirement target;
        std::array<Render::ShaderManifest, 3> manifests;
        std::array<Render::ShaderCompilationBatch, 3> compiled;
    };

    /** @brief Returns the fixed Metal compute target. @return Exact payload/schema and finite interface limits. */
    [[nodiscard]] Render::ShaderTargetRequirement ExposureGpuMetalCookTarget();
    /** @brief Returns canonical package-relative identities. @return Clear/build/adapt names, not file discovery. */
    [[nodiscard]] std::span<const std::string_view, 3> ExposureGpuCookPrefixes() noexcept;
    /** @brief Resolves the one canonical source file. @param repositoryRoot Absolute repository root.
     * @return Source path, or a typed invalid-root error. No files are read here.
     */
    [[nodiscard]] Result<std::filesystem::path> ExposureGpuCookSourcePath(const std::filesystem::path &repositoryRoot);
    /** @brief Validates every genuine compiled stage and publishes one complete new package atomically.
     * @param destination New package directory, absent beneath an exclusively owned parent.
     * @param family Actual compilation results. @param host Independently authenticated host configuration.
     * @return Success after decoding written artifacts, or preserved validation/storage failure.
     */
    [[nodiscard]] Result<void> PublishExposureGpuCook(const std::filesystem::path &destination, const ExposureGpuCookFamily &family,
                                                      const Application::Internal::AuthenticatedMetalCookConfiguration &host);
    /** @brief Authenticates explicit tools and compiles all three current entry points before atomic publication.
     * @param repositoryRoot Absolute repository source root. @param destination New package path.
     * @param inputs Signed catalog, signature, independent trust, existing scratch and exact executable paths.
     * @return Success or preserved typed authentication/compiler/reflection/publication failure; no fallback.
     */
    [[nodiscard]] Result<void> CookExposureGpuFamily(const std::filesystem::path &repositoryRoot, const std::filesystem::path &destination,
                                                     const Application::Internal::AuthenticatedMetalCookInputs &inputs);
}  // namespace Horo::Editor::Offline
