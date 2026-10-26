#include "UiPaintProgramInternal.h"

#include <algorithm>

namespace Horo::Render {
    namespace {
        using namespace Detail;
        using namespace Runtime::Ui;

        /** @brief The already resident buffers and physical target extent for one slot graph. */
        struct ProgramGraphTarget final {
            const UiPaintFrameBuffers &buffers;
            FramebufferExtent extent;
        };

        /** @brief Load-time graph assembly over one already owned resident slot; no source or native authority is duplicated. */
        class ProgramGraph final {
        public:
            ProgramGraph(UiPaintSlotProgram &slot, std::span<const RenderTextureHandle> external, std::span<const UiPaintMaskScope> masks,
                         std::span<const UiOpacityGroup> groups, ProgramGraphTarget target, std::shared_ptr<IResidentUiPaintKernel> primary,
                         std::shared_ptr<IResidentUiPaintKernel> offscreen)
                : slot_(slot), external_(external), masks_(masks), groups_(groups), buffers_(target.buffers), extent_(target.extent),
                  primary_(std::move(primary)), offscreen_(std::move(offscreen)) {}

            Result<void> Build() {
                const auto passes = masks_.size() + groups_.size() + 1;
                auto created = RenderGraphBuilder::Create(
                    {static_cast<std::uint32_t>(passes), static_cast<std::uint32_t>(4 + external_.size() + slot_.surfaces.size()),
                     RenderGraphLimits::HardMaxUsages, static_cast<std::uint32_t>(std::max<std::size_t>(1, passes - 1))});
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                auto builder = std::move(created).Value();
                if (const auto imported = Import(builder); imported.HasError())
                    return imported;
                slot_.sampled.resize(passes);
                slot_.workloads.resize(passes);
                RenderGraphPassRef previous;
                for (std::size_t index = 0; index < passes; ++index) {
                    if (const auto added = Pass(builder, index, previous); added.HasError())
                        return added;
                }
                auto graph = builder.Finalize();
                if (graph.HasError())
                    return Result<void>::Failure(graph.ErrorValue());
                auto schedule = CompileRenderGraph(graph.Value());
                if (schedule.HasError())
                    return Result<void>::Failure(schedule.ErrorValue());
                const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
                auto synchronization = SynthesizeRenderGraphSynchronization(graph.Value(), schedule.Value(), queues, initial_);
                if (synchronization.HasError())
                    return Result<void>::Failure(synchronization.ErrorValue());
                auto execution = CompileRenderGraphExecution(graph.Value(), schedule.Value(), synchronization.Value(), queues);
                if (execution.HasError())
                    return Result<void>::Failure(execution.ErrorValue());
                slot_.graph = std::move(execution).Value();
                return Result<void>::Success();
            }

        private:
            Result<void> Import(RenderGraphBuilder &builder) {
                const std::array handles{buffers_.vertices, buffers_.indices, buffers_.parameters};
                for (std::size_t index = 0; index < handles.size(); ++index) {
                    const auto imported = builder.ImportBuffer(handles[index], RenderGraphResourceClass::Persistent);
                    if (imported.HasError())
                        return Result<void>::Failure(imported.ErrorValue());
                    bufferIds_[index] = imported.Value();
                    initial_.push_back({imported.Value(),
                                        {RenderGraphSynchronizationAccess::Read,
                                         RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                         RenderGraphPipelineScope::Graphics,
                                         RenderGraphTextureLayout::NotApplicable,
                                         {1}}});
                }
                slot_.textures.reserve(external_.size() + slot_.surfaces.size());
                for (const auto handle : external_)
                    if (const auto imported = Texture(builder, handle, false); imported.HasError())
                        return imported;
                for (const auto &surface : slot_.surfaces)
                    if (const auto imported = Texture(builder, surface.handle, true); imported.HasError())
                        return imported;
                const auto stencil = builder.ImportTexture(slot_.stencil.handle, RenderGraphResourceClass::Persistent);
                if (stencil.HasError())
                    return Result<void>::Failure(stencil.ErrorValue());
                stencil_ = stencil.Value();
                initial_.push_back({stencil_, {}, RenderGraphInitialContents::Discarded});
                return Result<void>::Success();
            }

            Result<void> Texture(RenderGraphBuilder &builder, RenderTextureHandle handle, bool discard) {
                const auto imported = builder.ImportTexture(handle, RenderGraphResourceClass::Persistent);
                if (imported.HasError())
                    return Result<void>::Failure(imported.ErrorValue());
                slot_.textures.push_back(imported.Value());
                if (discard)
                    initial_.push_back({imported.Value(), {}, RenderGraphInitialContents::Discarded});
                else
                    initial_.push_back({imported.Value(),
                                        {RenderGraphSynchronizationAccess::Read,
                                         RenderGraphSynchronizationOperation::Sampled,
                                         RenderGraphPipelineScope::Graphics,
                                         RenderGraphTextureLayout::ShaderReadOnly,
                                         {1}}});
                return Result<void>::Success();
            }

            void Samples(std::size_t pass, std::uint32_t group) {
                auto &samples = slot_.sampled[pass];
                samples.assign(slot_.textures.begin(), slot_.textures.begin() + external_.size());
                if (pass < masks_.size()) {
                    if (masks_[pass].parent != NoPaintRole)
                        samples.push_back(slot_.textures[external_.size() + masks_[pass].parent]);
                } else {
                    for (std::size_t index = 0; index < masks_.size(); ++index)
                        samples.push_back(slot_.textures[external_.size() + index]);
                    for (std::size_t index = 0; index < groups_.size(); ++index)
                        if (groups_[index].parent == group)
                            samples.push_back(slot_.textures[external_.size() + masks_.size() + index]);
                }
            }

            Result<void> Pass(RenderGraphBuilder &builder, std::size_t index, RenderGraphPassRef &previous) {
                const auto created = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                const auto pass = created.Value();
                if (previous.IsValid()) {
                    const auto ordered = builder.AddDependency({previous, pass, RenderGraphDependencyKind::ResourceHazard});
                    if (ordered.HasError())
                        return ordered;
                }
                previous = pass;
                const bool root = index == slot_.workloads.size() - 1;
                const bool mask = index < masks_.size();
                const auto group = mask || root ? NoPaintRole : static_cast<std::uint32_t>(groups_.size() - 1 - (index - masks_.size()));
                Samples(index, group);
                const auto surface = mask ? index : masks_.size() + group;
                RenderGraphUiPaint paint{.color = root ? RenderGraphResourceId{} : slot_.textures[external_.size() + surface],
                                         .stencil = mask ? RenderGraphResourceId{} : stencil_,
                                         .vertices = bufferIds_[0],
                                         .indices = bufferIds_[1],
                                         .parameters = bufferIds_[2],
                                         .sampledResources = slot_.sampled[index],
                                         .extent = extent_,
                                         .kernel = root ? primary_ : offscreen_,
                                         .clearColor = !root,
                                         .clearStencil = !mask,
                                         .activity = UiPaintPassActivity::Hidden};
                slot_.workloads[index] = {pass, std::move(paint)};
                return Usages(builder, pass, std::get<RenderGraphUiPaint>(slot_.workloads[index].workload));
            }

            Result<void> Usages(RenderGraphBuilder &builder, RenderGraphPassRef pass, const RenderGraphUiPaint &paint) const {
                using enum RenderGraphAccess;
                using enum RenderGraphUsageKind;
                std::vector<RenderGraphResourceUsage> uses{{pass, paint.vertices, Read, VertexBuffer},
                                                           {pass, paint.indices, Read, IndexBuffer},
                                                           {pass, paint.parameters, Read, UniformBuffer}};
                if (paint.color.IsValid())
                    uses.emplace_back(pass, paint.color, Write, ColorAttachment);
                if (paint.stencil.IsValid())
                    uses.emplace_back(pass, paint.stencil, Write, DepthStencilAttachment);
                for (const auto texture : paint.sampledResources)
                    uses.emplace_back(pass, texture, Read, Sampled);
                std::ranges::sort(uses, {}, [](const RenderGraphResourceUsage &use) {
                    return use.resource.value;
                });
                for (const auto &use : uses)
                    if (const auto added = builder.AddUsage(use); added.HasError())
                        return added;
                return Result<void>::Success();
            }

            UiPaintSlotProgram &slot_;
            std::span<const RenderTextureHandle> external_;
            std::span<const UiPaintMaskScope> masks_;
            std::span<const UiOpacityGroup> groups_;
            UiPaintFrameBuffers buffers_;
            FramebufferExtent extent_;
            std::shared_ptr<IResidentUiPaintKernel> primary_;
            std::shared_ptr<IResidentUiPaintKernel> offscreen_;
            std::array<RenderGraphResourceId, 3> bufferIds_;
            RenderGraphResourceId stencil_;
            std::vector<RenderGraphImportedState> initial_;
        };
    }  // namespace

    /** @copydoc UiPaintProgram::Storage::CreateSlot */
    Result<void> UiPaintProgram::Storage::CreateSlot(RenderFrontend &renderer, std::uint32_t index, const UiPaintFrameBuffers &buffers,
                                                     RenderTextureFormat stencilFormat) {
        auto &slot = native.slots[index];
        const FramebufferExtent extent{descriptor.projection.pixelExtent.width, descriptor.projection.pixelExtent.height};
        slot.surfaces.resize(source.masks.size() + source.prototype.Groups().size());
        for (auto &surface : slot.surfaces) {
            const auto created =
                renderer.CreateOwnedUiPaintTexture(renderer.memoryConfig_.defaultResourceScope,
                                                   {.extent = extent,
                                                    .format = RenderTextureFormat::Rgba16Float,
                                                    .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment},
                                                   {}, retirement);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            surface = created.Value();
        }
        const auto stencil =
            renderer.CreateOwnedUiPaintTexture(renderer.memoryConfig_.defaultResourceScope,
                                               {.extent = extent, .format = stencilFormat, .usage = RenderTextureUsage::RenderAttachment},
                                               {}, retirement);
        if (stencil.HasError())
            return Result<void>::Failure(stencil.ErrorValue());
        slot.stencil = stencil.Value();
        if (const auto compiled = CompileSlot(index, buffers); compiled.HasError())
            return compiled;
        for (auto &frame : slot.frames) {
            frame.vertices.reserve(descriptor.capacity.vertices);
            frame.indices.reserve(descriptor.capacity.indices);
            frame.parameters.reserve(descriptor.capacity.parameters);
            frame.draws.reserve(descriptor.capacity.parameters);
            frame.ranges.resize(slot.workloads.size());
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPaintProgram::Storage::CompileSlot */
    Result<void> UiPaintProgram::Storage::CompileSlot(std::uint32_t index, const UiPaintFrameBuffers &buffers) {
        const FramebufferExtent extent{descriptor.projection.pixelExtent.width, descriptor.projection.pixelExtent.height};
        ProgramGraph graph{native.slots[index], source.external, source.masks,    source.prototype.Groups(),
                           {buffers, extent},   native.primary,  native.offscreen};
        return graph.Build();
    }
}  // namespace Horo::Render
