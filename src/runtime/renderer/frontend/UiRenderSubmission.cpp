#include "Horo/Runtime/Render/UiRenderSubmission.h"

#include "Horo/Runtime/Render/RenderFrontend.h"

#include <utility>

namespace Horo::Render {
    /** @copydoc RenderFrontend::SubmitUiGraph */
    Result<void> RenderFrontend::SubmitUiGraph(const FrameDescriptor &descriptor, const CompiledRenderGraphExecution &graph,
                                               const std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission ui) {
        auto begun = BeginFrame(descriptor);
        if (begun.HasError())
            return Result<void>::Failure(begun.ErrorValue());
        auto frame = std::move(begun).Value();
        if (const auto executed = frame.ExecuteGraph(graph, workloads, std::move(ui)); executed.HasError())
            return executed;
        return frame.Present();
    }
}  // namespace Horo::Render
