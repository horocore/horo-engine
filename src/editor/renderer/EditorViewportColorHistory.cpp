#include "EditorViewportColorPipeline.h"

#include <cassert>

namespace Horo::Editor {
    using namespace Render;

    /** @copydoc EditorViewportColorPipeline::BufferInitialState */
    RenderGraphLogicalState EditorViewportColorPipeline::BufferInitialState(RenderBufferHandle buffer, RenderQueueId queue) const noexcept {
        for (std::size_t i = 0; i < acceptedStateCount_; ++i)
            if (acceptedStates_[i].buffer == buffer)
                return acceptedStates_[i].state;
        for (const auto &slot : slots_)
            if (buffer == slot.uniform[0] || buffer == slot.uniform[1])
                return {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                        RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::NotApplicable, queue};
        // Device-local geometry/history has exact owned initial upload bytes. Actual queue ordering/completion is checked separately.
        return {RenderGraphSynchronizationAccess::Write, RenderGraphSynchronizationOperation::CopyDestination,
                RenderGraphPipelineScope::Transfer, RenderGraphTextureLayout::NotApplicable, queue};
    }

    /** @copydoc EditorViewportColorPipeline::TableInitialState */
    RenderGraphLogicalState EditorViewportColorPipeline::TableInitialState(RenderQueueId queue) const noexcept {
        if (tablesUsed_)
            return {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::Sampled,
                    RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::ShaderReadOnly, queue};
        return {RenderGraphSynchronizationAccess::Write, RenderGraphSynchronizationOperation::CopyDestination,
                RenderGraphPipelineScope::Transfer, RenderGraphTextureLayout::CopyDestination, queue};
    }

    /** @copydoc EditorViewportColorPipeline::StageBufferState */
    void EditorViewportColorPipeline::StageBufferState(RenderBufferHandle buffer, RenderGraphLogicalState state) noexcept {
        for (std::size_t i = 0; i < pendingStateCount_; ++i)
            if (pendingStates_[i].buffer == buffer) {
                pendingStates_[i].state = state;
                return;
            }
        // One closed graph contributes at most eight buffers; all four slots plus immutable geometry fit 32 records.
        assert(pendingStateCount_ < pendingStates_.size());
        pendingStates_[pendingStateCount_++] = {buffer, state};
    }

    /** @copydoc EditorViewportColorPipeline::PublishBufferStates */
    void EditorViewportColorPipeline::PublishBufferStates() noexcept {
        for (std::size_t i = 0; i < pendingStateCount_; ++i) {
            std::size_t destination = 0;
            while (destination < acceptedStateCount_ && acceptedStates_[destination].buffer != pendingStates_[i].buffer)
                ++destination;
            if (destination == acceptedStateCount_) {
                assert(acceptedStateCount_ < acceptedStates_.size());
                ++acceptedStateCount_;
            }
            acceptedStates_[destination] = pendingStates_[i];
        }
    }
}  // namespace Horo::Editor
