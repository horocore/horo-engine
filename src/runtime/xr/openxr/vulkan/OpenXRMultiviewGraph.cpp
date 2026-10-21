#include "host/OpenXRMultiviewGraph.h"

#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"
#include "RenderExternalImages.h"

#include <algorithm>
#include <array>
#include <optional>
#include <thread>
#include <vector>

namespace Horo::XR::OpenXRInternal {
    using namespace Render;

    namespace {
        Error Invalid() {
            return MakeError(MultiviewExtractionErrors::InvalidInput);
        }

        Error Stale() {
            return MakeError(MultiviewExtractionErrors::StalePublication);
        }

        /** @brief Requires already resident inputs; upload progress belongs to the frontend owner. */
        template <typename Handle> Result<void> Ready(RenderFrontend &frontend, const Handle handle) {
            const auto state = frontend.ResourceState(handle);
            if (state.HasError())
                return Result<void>::Failure(state.ErrorValue());
            if (state.Value() != RenderResourceState::Ready)
                return Result<void>::Failure(Invalid());
            return Result<void>::Success();
        }
    }  // namespace

    struct OpenXRMultiviewGraph::Impl {
        struct Acquisition {
            OpenXRSwapchain *owner{};
            NativeSwapchainAcquisition current;
            RenderGraphResourceId resource;
        };

        struct Buffer {
            RenderBufferHandle handle;
            RenderGraphResourceId resource;
            RenderGraphUsageKind role;
        };

        RenderFrontend *frontend;
        XRMultiviewScene xr;
        std::shared_ptr<const MultiviewSceneSnapshot> scene;
        RenderQueueId queue;
        std::thread::id thread{std::this_thread::get_id()};
        std::optional<CompiledRenderGraphExecution> graph;
        std::array<Acquisition, XRViewRenderHardLimits::MaximumTargets> acquisitions{};
        std::size_t acquisitionCount{};
        std::array<MultiviewGraphTarget, MultiviewExtractionLimits::HardMaximumViews> targets{};
        std::array<MultiviewGraphGeometry, MultiviewExtractionLimits::HardMaximumInstances> geometry{};
        std::array<Buffer, MultiviewExtractionLimits::HardMaximumInstances * 3> buffers{};
        std::size_t bufferCount{};
        MultiviewNativeWorkLimits limits;

        Impl(RenderFrontend &owner, const XRMultiviewScene &source, const RenderQueueId graphics,
             const MultiviewNativeWorkLimits &workLimits)
            : frontend(&owner), xr(source), scene(std::make_shared<const MultiviewSceneSnapshot>(source.Scene())), queue(graphics),
              limits(workLimits) {}

        /** @brief Rejects invented renderer capability/device facts and revoked real XR acquisitions. */
        Result<void> Validate(const XRMultiviewOwnerState &active) const {
            if (thread != std::this_thread::get_id())
                return Result<void>::Failure(Invalid());
            if (const auto valid = xr.Validate(active); valid.HasError())
                return valid;
            const auto &actual = frontend->Capabilities().support;
            const auto support = ResolveMultiviewCapabilities(actual);
            if (!actual.IsValid() || actual.deviceIncarnation != active.renderer.deviceIncarnation ||
                actual.capabilityRevision != active.renderer.capabilityRevision || !support.multipass || !support.indexedShaders ||
                scene->Views().size() > support.maximumViews ||
                (scene->Mode() == MultiviewExecutionMode::Instanced && !support.instancedTargetRouting))
                return Result<void>::Failure(Stale());
            for (const auto &entry : std::span{acquisitions}.first(acquisitionCount)) {
                const auto current = entry.owner->Current();
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                if (current.Value().image != entry.current.image || current.Value().texture != entry.current.texture)
                    return Result<void>::Failure(Stale());
            }
            return Result<void>::Success();
        }

        /** @brief Imports exact owner-issued image registrations and actual acquired/final states once. */
        Result<RenderGraphResourceId> Texture(RenderGraphBuilder &builder, const XRExternalRenderTargetDescriptor &target,
                                              std::span<OpenXRSwapchain *const> owners, std::vector<RenderGraphImportedState> &initial,
                                              std::vector<RenderGraphFinalState> &final) {
            for (const auto &entry : std::span{acquisitions}.first(acquisitionCount))
                if (entry.current.image == target.image)
                    return Result<RenderGraphResourceId>::Success(entry.resource);
            OpenXRSwapchain *owner = nullptr;
            NativeSwapchainAcquisition current{};
            for (auto *candidate : owners) {
                if (!candidate)
                    return Result<RenderGraphResourceId>::Failure(Invalid());
                const auto value = candidate->Current();
                if (value.HasError())
                    return Result<RenderGraphResourceId>::Failure(value.ErrorValue());
                if (value.Value().image == target.image) {
                    if (owner)
                        return Result<RenderGraphResourceId>::Failure(Invalid());
                    owner = candidate;
                    current = value.Value();
                }
            }
            if (!owner)
                return Result<RenderGraphResourceId>::Failure(Stale());
            const auto facts = Render::Detail::RenderExternalTextureAccess::Describe(*frontend, current.texture);
            if (facts.HasError())
                return Result<RenderGraphResourceId>::Failure(facts.ErrorValue());
            const auto &f = facts.Value();
            const auto &d = f.descriptor;
            if (f.deviceIncarnation != scene->Publication().deviceIncarnation || d.extent != target.allocationExtent ||
                d.format != target.format || d.sampleCount != target.sampleCount || d.layerCount != target.arrayLayerCount ||
                d.dimension != RenderTextureDimension::TwoD || d.mipCount != 1 ||
                (static_cast<unsigned>(d.usage) & static_cast<unsigned>(target.usage)) != static_cast<unsigned>(target.usage) ||
                f.acquired.queue != queue || f.requiredFinal.queue != queue)
                return Result<RenderGraphResourceId>::Failure(Invalid());
            const auto id = builder.ImportTexture(current.texture, RenderGraphResourceClass::External);
            if (id.HasError())
                return id;
            if (const auto exported = builder.ExportResource(id.Value()); exported.HasError())
                return Result<RenderGraphResourceId>::Failure(exported.ErrorValue());
            initial.push_back({id.Value(), f.acquired, RenderGraphInitialContents::Preserved});
            final.push_back({id.Value(), f.requiredFinal});
            acquisitions[acquisitionCount++] = {owner, current, id.Value()};
            return id;
        }

        /** @brief Shares immutable ready input buffers without inventing upload or completion state. */
        Result<RenderGraphResourceId> Input(RenderGraphBuilder &builder, const RenderBufferHandle handle, const RenderGraphUsageKind role,
                                            std::vector<RenderGraphImportedState> &initial) {
            for (const auto &entry : std::span{buffers}.first(bufferCount))
                if (entry.handle == handle)
                    return entry.role == role ? Result<RenderGraphResourceId>::Success(entry.resource)
                                              : Result<RenderGraphResourceId>::Failure(Invalid());
            if (const auto ready = Ready(*frontend, handle); ready.HasError())
                return Result<RenderGraphResourceId>::Failure(ready.ErrorValue());
            const auto id = builder.ImportBuffer(handle, RenderGraphResourceClass::Persistent);
            if (id.HasError())
                return id;
            initial.push_back({id.Value(),
                               {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::NotApplicable, queue},
                               RenderGraphInitialContents::Preserved});
            buffers[bufferCount++] = {handle, id.Value(), role};
            return id;
        }

        static constexpr std::size_t MaximumResources =
            XRViewRenderHardLimits::MaximumTargets + MultiviewExtractionLimits::HardMaximumInstances * 3;

        /** @brief Bounded temporary authoring facts, released after immutable graph compilation. */
        struct GraphInputs {
            std::vector<RenderGraphImportedState> initial;
            std::vector<RenderGraphFinalState> final;

            GraphInputs() {
                initial.reserve(MaximumResources);
                final.reserve(XRViewRenderHardLimits::MaximumTargets);
            }
        };

        /** @brief Resolves complete runtime-ordered targets and ready prepared views before graph publication. */
        Result<void> PrepareTargets(RenderGraphBuilder &builder, const std::span<OpenXRSwapchain *const> owners,
                                    const std::span<const OpenXRMultiviewTargetViews> views, GraphInputs &inputs) {
            for (std::size_t index = 0; index < views.size(); ++index) {
                auto &target = targets[index];
                for (const auto &source : xr.Plan().Targets()) {
                    if (source.view != xr.Plan().Views()[index].id)
                        continue;
                    auto resource = Texture(builder, source, owners, inputs.initial, inputs.final);
                    if (resource.HasError())
                        return Result<void>::Failure(resource.ErrorValue());
                    if (source.role == XRExternalRenderTargetRole::ProjectionColor) {
                        target.color = resource.Value();
                        target.arrayLayer = source.arrayLayer;
                        target.rectangle = {source.renderRectangle.x, source.renderRectangle.y, source.renderRectangle.width,
                                            source.renderRectangle.height};
                    } else {
                        target.depth = resource.Value();
                        target.depthArrayLayer = source.arrayLayer;
                    }
                }
                target.colorView = views[index].color;
                target.depthView = views[index].depth;
                if (const auto ready = Ready(*frontend, target.colorView); ready.HasError())
                    return Result<void>::Failure(ready.ErrorValue());
                if (target.depth.IsValid())
                    if (const auto ready = Ready(*frontend, target.depthView); ready.HasError())
                        return Result<void>::Failure(ready.ErrorValue());
            }
            if (acquisitionCount != owners.size())
                return Result<void>::Failure(Invalid());
            return Result<void>::Success();
        }

        /** @brief Resolves visible canonical geometry and its exact material uniform dependency. */
        Result<void> PrepareGeometry(RenderGraphBuilder &builder, const std::span<const OpenXRMultiviewGeometry> source,
                                     GraphInputs &inputs) {
            for (std::size_t index = 0; index < source.size(); ++index) {
                const auto &input = source[index];
                if (input.instanceIndex != scene->Draws()[index].instanceIndex || !input.mesh.IsValid() || !input.material ||
                    input.material->Id() != scene->Instances()[input.instanceIndex].material)
                    return Result<void>::Failure(Invalid());
                if (const auto ready = Ready(*frontend, scene->Instances()[input.instanceIndex].mesh); ready.HasError())
                    return ready;
                const auto &resources = input.material->Descriptor().resources;
                const auto *uniform = resources.size() == 1 ? std::get_if<MaterialBufferBinding>(&resources.front().value) : nullptr;
                if (!uniform)
                    return Result<void>::Failure(Invalid());
                auto vertex = Input(builder, input.mesh.vertexBuffer, RenderGraphUsageKind::VertexBuffer, inputs.initial);
                if (vertex.HasError())
                    return Result<void>::Failure(vertex.ErrorValue());
                auto indices = Input(builder, input.mesh.indexBuffer, RenderGraphUsageKind::IndexBuffer, inputs.initial);
                if (indices.HasError())
                    return Result<void>::Failure(indices.ErrorValue());
                auto parameters = Input(builder, uniform->buffer, RenderGraphUsageKind::UniformBuffer, inputs.initial);
                if (parameters.HasError())
                    return Result<void>::Failure(parameters.ErrorValue());
                geometry[index] = {input.instanceIndex, vertex.Value(), indices.Value(), parameters.Value(), input.material};
            }
            return Result<void>::Success();
        }

        /** @brief Declares exhaustive semantic uses and compiles exact queue/acquired/final-state evidence. */
        Result<void> Compile(RenderGraphBuilder &builder, const RenderGraphPassRef pass, const GraphInputs &inputs) {
            for (const auto &entry : std::span{buffers}.first(bufferCount))
                if (auto use = builder.AddUsage({pass, entry.resource, RenderGraphAccess::Read, entry.role}); use.HasError())
                    return Result<void>::Failure(use.ErrorValue());
            for (const auto &entry : std::span{acquisitions}.first(acquisitionCount)) {
                const auto depth = std::ranges::any_of(xr.Plan().Targets(), [&](const auto &target) {
                    return target.image == entry.current.image && target.role == XRExternalRenderTargetRole::ProjectionDepth;
                });
                if (auto use =
                        builder.AddUsage({pass, entry.resource, RenderGraphAccess::Write,
                                          depth ? RenderGraphUsageKind::DepthStencilAttachment : RenderGraphUsageKind::ColorAttachment});
                    use.HasError())
                    return Result<void>::Failure(use.ErrorValue());
            }
            const auto authored = builder.Finalize();
            if (authored.HasError())
                return Result<void>::Failure(authored.ErrorValue());
            const auto schedule = CompileRenderGraph(authored.Value());
            if (schedule.HasError())
                return Result<void>::Failure(schedule.ErrorValue());
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, queue}};
            const auto sync =
                SynthesizeRenderGraphSynchronization(authored.Value(), schedule.Value(), queues, inputs.initial, inputs.final);
            if (sync.HasError())
                return Result<void>::Failure(sync.ErrorValue());
            auto compiled = CompileRenderGraphExecution(authored.Value(), schedule.Value(), sync.Value(), queues);
            if (compiled.HasError())
                return Result<void>::Failure(compiled.ErrorValue());
            graph.emplace(std::move(compiled).Value());
            return Result<void>::Success();
        }
    };

    OpenXRMultiviewGraph::OpenXRMultiviewGraph(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    OpenXRMultiviewGraph::~OpenXRMultiviewGraph() = default;
    OpenXRMultiviewGraph::OpenXRMultiviewGraph(OpenXRMultiviewGraph &&) noexcept = default;
    OpenXRMultiviewGraph &OpenXRMultiviewGraph::operator=(OpenXRMultiviewGraph &&) noexcept = default;

    /** @copydoc OpenXRMultiviewGraph::Create */
    Result<OpenXRMultiviewGraph> OpenXRMultiviewGraph::Create(RenderFrontend &frontend, const XRMultiviewScene &scene,
                                                              const XRMultiviewOwnerState &active,
                                                              const std::span<OpenXRSwapchain *const> owners,
                                                              const std::span<const OpenXRMultiviewGeometry> geometry,
                                                              const std::span<const OpenXRMultiviewTargetViews> views,
                                                              const RenderQueueId queue, const MultiviewNativeWorkLimits &limits) {
        if (!queue.IsValid() || !limits.IsValid() || owners.empty() || owners.size() > XRViewRenderHardLimits::MaximumTargets ||
            views.size() != scene.Scene().Views().size() || geometry.size() != scene.Scene().Draws().size())
            return Result<OpenXRMultiviewGraph>::Failure(Invalid());
        try {
            auto impl = std::make_unique<Impl>(frontend, scene, queue, limits);
            if (const auto live = impl->Validate(active); live.HasError())
                return Result<OpenXRMultiviewGraph>::Failure(live.ErrorValue());
            auto builder = RenderGraphBuilder::Create({1, Impl::MaximumResources, Impl::MaximumResources, 1});
            if (builder.HasError())
                return Result<OpenXRMultiviewGraph>::Failure(builder.ErrorValue());
            auto pass = builder.Value().AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
            if (pass.HasError())
                return Result<OpenXRMultiviewGraph>::Failure(pass.ErrorValue());
            Impl::GraphInputs inputs;
            if (const auto targets = impl->PrepareTargets(builder.Value(), owners, views, inputs); targets.HasError())
                return Result<OpenXRMultiviewGraph>::Failure(targets.ErrorValue());
            if (const auto meshes = impl->PrepareGeometry(builder.Value(), geometry, inputs); meshes.HasError())
                return Result<OpenXRMultiviewGraph>::Failure(meshes.ErrorValue());
            if (const auto compiled = impl->Compile(builder.Value(), pass.Value(), inputs); compiled.HasError())
                return Result<OpenXRMultiviewGraph>::Failure(compiled.ErrorValue());
            return Result<OpenXRMultiviewGraph>::Success(OpenXRMultiviewGraph{std::move(impl)});
        } catch (const std::bad_alloc &) {
            return Result<OpenXRMultiviewGraph>::Failure(MakeError(MultiviewExtractionErrors::CapacityExceeded));
        }
    }

    /** @copydoc OpenXRMultiviewGraph::Execute */
    Result<void> OpenXRMultiviewGraph::Execute(RenderFrameScope &frame, const XRMultiviewOwnerState &active,
                                               const RenderQueueSubmission &submission) {
        if (!impl_ || !submission.IsValid() || submission.queue != impl_->queue)
            return Result<void>::Failure(Invalid());
        if (auto live = impl_->Validate(active); live.HasError())
            return live;
        const std::array workloads{
            RenderGraphPassWorkload{impl_->graph->Passes().front().pass,
                                    RenderGraphMultiview{impl_->scene,
                                                         active.renderer,
                                                         std::span{impl_->targets}.first(impl_->scene->Views().size()),
                                                         std::span{impl_->geometry}.first(impl_->scene->Draws().size()),
                                                         {},
                                                         1.0F,
                                                         impl_->limits}}};
        const std::array submissions{RenderGraphQueueSubmission{submission, {0, 1}}};
        return frame.ExecuteSubmittedGraph(*impl_->graph, workloads, submissions);
    }

    /** @copydoc OpenXRMultiviewGraph::Graph */
    const CompiledRenderGraphExecution &OpenXRMultiviewGraph::Graph() const noexcept {
        return *impl_->graph;
    }
}  // namespace Horo::XR::OpenXRInternal
