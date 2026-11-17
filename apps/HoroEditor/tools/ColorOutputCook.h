#pragma once
/** @file ColorOutputCook.h
 * @brief Authenticated offline compilation and atomic publication of the exact analytic SDR output product family.
 */
#include "AuthenticatedMetalCookConfiguration.h"
#include "ColorOutputPackage.h"
#include "Horo/Runtime/Render/ColorOutputProgram.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Editor::Offline {
    using ColorOutputAnalyticBundle = Render::Internal::ColorOutputAnalyticBundle;
    using ColorOutputCookPackage = Render::Internal::ColorOutputCookPackage;

    /** @brief Returns exact relative producer paths in manifest/processor/reference/table source/upload order.
     * @return Seven constant names; no directory discovery occurs.
     */
    [[nodiscard]] std::span<const std::string_view, 7> ColorOutputAnalyticFilenames() noexcept;
    /** @brief Returns scene/encode native package prefixes. @return Two exact relative identities. */
    [[nodiscard]] std::span<const std::string_view, 2> ColorOutputCookPrefixes() noexcept;
    /** @brief Returns the one explicit Metal target. @return Exact schema/payload and bounded resource support. */
    [[nodiscard]] Render::ShaderTargetRequirement ColorOutputMetalCookTarget();
    /** @brief Loads and checks the original exact finite producer generation with no source rewriting or trust inference.
     * @param root Absolute existing producer directory. @return Owned program/files or typed schema/path/hash failure.
     */
    [[nodiscard]] Result<ColorOutputAnalyticBundle> LoadColorOutputAnalyticBundle(const std::filesystem::path &root);
    /** @brief Decodes a complete atomically published product package and cross-checks its exact generation binding.
     * @param root Absolute package directory. @return Actual payload/reflection plus original analytic data; integrity is not cook trust.
     */
    [[nodiscard]] Result<ColorOutputCookPackage> LoadColorOutputCookPackage(const std::filesystem::path &root);
    /** @brief Authenticates tools, compiles actual scene/encode sources and atomically publishes the full generation.
     * @param repositoryRoot Absolute product source root. @param analyticRoot Exact original producer generation.
     * @param destination New absent package directory outside scratch. @param inputs Explicit independent authenticated tools/trust.
     * @param includeLookVariant Explicitly compile the additional creative-look variant; baseline remains default.
     * @return Success only after read-only decoding of the written complete generation; no runtime execution is implied.
     */
    [[nodiscard]] Result<void> CookColorOutputFamily(const std::filesystem::path &repositoryRoot, const std::filesystem::path &analyticRoot,
                                                     const std::filesystem::path &destination,
                                                     const Application::Internal::AuthenticatedMetalCookInputs &inputs,
                                                     bool includeLookVariant = false);
}  // namespace Horo::Editor::Offline
