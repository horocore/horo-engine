#pragma once
/** @file RuntimeUiPaintProjectKernel.h @brief Private load-time preparation through the existing application shader producer. */
#include "RuntimeUiPaintProjectSources.h"

namespace Horo::Application::Internal {
    /** @brief Reads the bounded canonical source and joins one existing worker compiler invocation.
     * @param compiler Explicit admitted route and longer-lived application services.
     * @param cancellation Actual host shutdown ancestry.
     * @return Strict same-cook reflected kernel, or original typed I/O/tool/cancellation failure. */
    [[nodiscard]] Result<std::shared_ptr<const Render::CookedUiPaintKernel>> PrepareProjectUiKernel(
        const RuntimeUiPaintProjectCompiler &compiler, const CancellationToken &cancellation);
}  // namespace Horo::Application::Internal
