#pragma once
/** @file MetalCommandExecution.h
 * @brief Private admission and translation of compiled single-queue Metal graph work.
 */
#include "MetalBackendInternal.h"

namespace Horo::Render::Detail {
    /** @brief Validates every graph binding without encoding; shared by serial and worker-native admission. */
    [[nodiscard]] Result<void> ValidateMetalRenderGraph(const IMetalRuntime &runtime, const RenderGraphExecutionRequest &request);
    /**
     * @brief Validates the entire graph before encoding its ordered single-queue workloads.
     * @param runtime Active owner-thread Metal runtime borrowed synchronously.
     * @param request Exact compiled graph, workload and resource bindings.
     * @return Success or typed admission/encoding failure; encoding failure aborts the frame.
     * Queue transfers and transient materialization are explicitly unsupported.
     */
    [[nodiscard]] Result<void> ExecuteMetalRenderGraph(IMetalRuntime &runtime, const RenderGraphExecutionRequest &request);
}  // namespace Horo::Render::Detail
