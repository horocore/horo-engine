/** @file
 * @brief Private deterministic artifact-key serialization shared by the compiler pipeline implementation.
 */
#pragma once

#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

namespace Horo::Render::ShaderCompilerDetail {
    /** @brief Hashes the admitted source, interface and exact target/tool inputs without changing their canonical order.
     * @param request Validated immutable compilation inputs.
     * @param target Exact target and locked tool identities selected from the request.
     * @param sourceDigest Digest of the admitted source bytes.
     * @return Existing canonical artifact key, or an identity failure preserving its cause.
     */
    [[nodiscard]] Result<Sha256Digest> BuildArtifactKey(const ShaderCompilationRequest &request,
                                                        const ShaderCompilerTargetDescriptor &target, const Sha256Digest &sourceDigest);
}  // namespace Horo::Render::ShaderCompilerDetail
