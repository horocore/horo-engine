/** @file
 * @brief Private synchronous shader route execution within an adapter-owned scratch directory.
 */
#pragma once

#include "ShaderCompilerToolchain.h"

namespace Horo::Render::ShaderCompilerToolchainDetail {
    /**
     * @brief Execute one validated target without retaining borrowed invocation or scratch state.
     * @param configuration Verified tool installations and bounded process configuration.
     * @param processes Borrowed process runner retained by the calling adapter.
     * @param invocation Validated immutable target and optional synchronous diagnostic sink.
     * @param cancellation Live cancellation token for all route processes.
     * @param scratch Adapter-owned scratch directory that outlives this call.
     * @param sourcePath Materialized shader source within scratch.
     * @return Owned candidate output or a typed route failure.
     */
    [[nodiscard]] Result<ShaderCompilerAdapterOutput> CompileRoute(const ShaderCompilerToolchainConfiguration &configuration,
                                                                   IExternalProcessRunner &processes,
                                                                   const ShaderCompilerInvocation &invocation,
                                                                   const CancellationToken &cancellation,
                                                                   const std::filesystem::path &scratch,
                                                                   const std::filesystem::path &sourcePath);
    /** @brief Admits exact VFX interface revision/profile while leaving other typed raster routes unchanged.
     * @param invocation Immutable validated cook request. @return Its VFX role, or the original typed revision/profile failure.
     */
    [[nodiscard]] Result<bool> AdmitRasterCookInterface(const ShaderCompilerInvocation &invocation);
    /** @brief Builds the selected native-language arguments and the exact admitted VFX lowering/interface options.
     * @param entry Actual translated stage. @param spirvPath Same-operation validated intermediate.
     * @param nativePath Adapter-owned output path. @param metal Selected native language.
     * @param vfxInterface Exact admitted logical VFX identity. @return Deterministic shell-free translator arguments.
     */
    [[nodiscard]] std::vector<std::string> RasterTranslationArguments(const ShaderEntryPoint &entry, const std::filesystem::path &spirvPath,
                                                                      const std::filesystem::path &nativePath, bool metal,
                                                                      bool vfxInterface);
    /** @brief Selects the existing UI or VFX normalizer for actual same-operation captured evidence.
     * @param invocation Immutable validated cook request. @param stages Actual reflection JSON for each translated stage.
     * @param translations Actual corresponding native texts. @return Strictly normalized final-target evidence or a typed failure.
     */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeCapturedRasterReflection(const ShaderCompilerInvocation &invocation,
                                                                                       const std::vector<SpirvReflectionStage> &stages,
                                                                                       const std::vector<std::string> &translations);
    /** @brief Admits the fixed UI paint reflection names and the exact locked translation tool before any process runs.
     * @param request Explicit same-cook reflection identities supplied by the validated invocation.
     * @param cross Installed Cross tool, or null when that installation is unavailable.
     * @return Success, invalid-output failure for names, or missing-tool failure for identity mismatch.
     */
    [[nodiscard]] Result<void> ValidateUiPaintMetalReflection(const ShaderCompilerReflectionRequest &request,
                                                              const ShaderCompilerToolInstallation *cross);
    /** @brief Selects an admitted closed reflection interface before capturing the same-operation native evidence.
     * @param manifest Invocation's logical interface. @param target Exact selected target requirement.
     * @param request Explicit authored names/tool identities. @param cross Verified installed translator.
     * @return Success only for the exact UI or VFX interface and its complete authored-name map.
     */
    [[nodiscard]] Result<void> ValidateRasterReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                        const ShaderCompilerReflectionRequest &request,
                                                        const ShaderCompilerToolInstallation *cross);
    /** @brief Executes a distinct bounded reflection route inside the existing adapter-owned scratch lifetime. */
    [[nodiscard]] Result<std::vector<SpirvReflectionStage>> ReflectVulkanRoute(const ShaderCompilerToolchainConfiguration &configuration,
                                                                               IExternalProcessRunner &processes,
                                                                               const CompiledShaderArtifact &compiled,
                                                                               const ShaderCompilerToolIdentity &tool,
                                                                               const CancellationToken &cancellation,
                                                                               const std::filesystem::path &scratch);
}  // namespace Horo::Render::ShaderCompilerToolchainDetail
