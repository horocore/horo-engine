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
    /** @brief Admits the fixed UI paint reflection names and the exact locked translation tool before any process runs.
     * @param request Explicit same-cook reflection identities supplied by the validated invocation.
     * @param cross Installed Cross tool, or null when that installation is unavailable.
     * @return Success, invalid-output failure for names, or missing-tool failure for identity mismatch.
     */
    [[nodiscard]] Result<void> ValidateUiPaintMetalReflection(const ShaderCompilerReflectionRequest &request,
                                                              const ShaderCompilerToolInstallation *cross);
    /** @brief Executes a distinct bounded reflection route inside the existing adapter-owned scratch lifetime. */
    [[nodiscard]] Result<std::vector<SpirvReflectionStage>> ReflectVulkanRoute(const ShaderCompilerToolchainConfiguration &configuration,
                                                                               IExternalProcessRunner &processes,
                                                                               const CompiledShaderArtifact &compiled,
                                                                               const ShaderCompilerToolIdentity &tool,
                                                                               const CancellationToken &cancellation,
                                                                               const std::filesystem::path &scratch);
}  // namespace Horo::Render::ShaderCompilerToolchainDetail
