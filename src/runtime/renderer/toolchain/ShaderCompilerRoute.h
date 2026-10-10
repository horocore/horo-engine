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
}  // namespace Horo::Render::ShaderCompilerToolchainDetail
