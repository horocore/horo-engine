#include "EditorViewportColorGraph.h"
#include "EditorViewportRendererVulkan.h"
#include "RenderFrontendResourceAccess.h"

#include <algorithm>
#include <array>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        using namespace Render;
        constexpr auto MaximumDraws = RenderMaterialPass::MaximumDraws;
        constexpr auto MaximumScenePasses = MaximumDirectionalShadowCascades + 1;
        constexpr auto MaximumSpatialPasses = 1;
        constexpr auto MaximumColorPasses = 5;    // Two uploads, analytic output, overlay, SDR encode.
        constexpr auto MaximumColorImports = 10;  // Two outputs, four analytic table/staging resources and four fullscreen buffers.
        constexpr auto MaximumColorUses = 32;     // Fixed transfer and fullscreen shader reads, including the optional overlay.

        /** @brief One complete resident attachment generation of the ordinary desktop viewport. */
        struct RasterAttachments final {
            FramebufferExtent extent;
            FramebufferExtent atlasExtent;
            RenderTextureHandle color, depth, atlas;
            RenderTextureViewHandle colorView, depthView, atlasView;

            bool operator==(const RasterAttachments &) const noexcept = default;
        };

        /** @brief Captures one immutable topology signature from the existing viewport attachment owner. */
        RasterAttachments Attachments(const EditorViewportResources &resources) {
            const auto size = resources.AllocatedExtent();
            const auto atlas = resources.ShadowExtent();
            return {{size.width, size.height},    {atlas.width, atlas.height},    resources.ColorTexture(),
                    resources.DepthTexture(),     resources.ShadowDepthTexture(), resources.ColorTextureView(),
                    resources.DepthTextureView(), resources.ShadowTextureView()};
        }

        /** @brief One pass's owned backing for synchronous draw and read borrows. */
        struct RasterInputs final {
            std::vector<RenderGraphMaterialDraw> draws;
            std::vector<RenderGraphMaterialRead> reads;
            std::vector<RenderGraphResourceId> used;

            void Reserve(std::size_t count) {
                draws.reserve(count);
                reads.reserve(count + 1);  // One distinct uniform per draw and the exact shared atlas.
                used.reserve(3 * count + 1);
            }
        };

        struct RasterVariant final {
            std::optional<CompiledRenderGraphExecution> graph;
            std::array<RasterInputs, MaximumScenePasses> inputs;
            RasterInputs displayOverlay;
            EditorViewportColorGraph color;
            std::array<RenderGraphPassWorkload, MaximumScenePasses + MaximumColorPasses + MaximumSpatialPasses> workloads;
            std::optional<std::size_t> spatialWork;
            std::size_t count{};
        };

        struct BuildFailure final {
            Error error;
        };

        /** @brief Keeps partially authored graphs private and carries the first typed failure to the owning cold boundary. */
        template <typename T> T Take(Result<T> result) {
            if (result.HasError())
                throw BuildFailure{std::move(result).ErrorValue()};
            return std::move(result).Value();
        }

        void Take(Result<void> result) {
            if (result.HasError())
                throw BuildFailure{std::move(result).ErrorValue()};
        }

        /** @brief Authors exact scene resources and pass inputs into the canonical graph compiler. */
        class RasterGraphAuthor final {
        public:
            RasterGraphAuthor(std::size_t draws, std::size_t passes, RenderQueueId graphics, bool color, bool spatial)
                : builder(Take(RenderGraphBuilder::Create(
                      {.maxPasses = passes + (color ? MaximumColorPasses : 0) + (spatial ? 1 : 0),
                       .maxResources = 3 + 2 * draws + draws * passes + (color ? MaximumColorImports : 0) + (spatial ? 1 : 0),
                       .maxUsages = 3 * passes + 3 * draws * passes + (color ? MaximumColorUses : 0) + (spatial ? 2 : 0),
                       .maxDependencies = passes + (color ? MaximumColorPasses : 0) + (spatial ? 1 : 0)}))),
                  queue(graphics), spatial_(spatial) {}

            RenderGraphResourceId Attach(RenderTextureHandle texture) {
                const auto resource = Take(builder.ImportTexture(texture, RenderGraphResourceClass::Persistent));
                initial.push_back({resource, {}, RenderGraphInitialContents::Discarded});
                return resource;
            }

            RenderGraphPassRef Pass(RenderPassKind kind = RenderPassKind::Graphics, RenderQueueRole role = RenderQueueRole::Graphics) {
                const auto pass = Take(builder.AddPass(kind, role));
                if (last.IsValid())
                    Take(builder.AddDependency({last, pass, RenderGraphDependencyKind::ExecutionOrder}));
                last = pass;
                return pass;
            }

            void Output(RenderGraphPassRef pass, RenderGraphResourceId resource, RenderGraphUsageKind kind, RenderGraphAccess access) {
                Take(builder.AddUsage({pass, resource, access, kind}));
            }

            void Draw(RenderGraphPassRef pass, RasterInputs &input, RenderMeshHandle mesh, RenderBufferHandle vertices,
                      RenderBufferHandle indices, const std::shared_ptr<const ResidentMaterialBinding> &material,
                      RenderBufferHandle uniform) {
                if (!material)
                    throw BuildFailure{MakeError(RendererErrors::ViewportInvalidScene)};
                const auto vertex = BufferRead(pass, input, vertices, RenderGraphUsageKind::VertexBuffer);
                const auto index = BufferRead(pass, input, indices, RenderGraphUsageKind::IndexBuffer);
                const auto parameter = BufferRead(pass, input, uniform, RenderGraphUsageKind::UniformBuffer);
                input.reads.push_back({parameter, uniform, {}});
                input.draws.push_back({mesh, vertex, index, material});
            }

            void SampleAtlas(RenderGraphPassRef pass, RasterInputs &input, RenderGraphResourceId atlas, RenderTextureViewHandle view) {
                Take(builder.AddUsage({pass, atlas, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled}));
                input.reads.push_back({atlas, {}, view});
            }

            RenderGraphPassWorkload Reconstruct(RenderGraphResourceId scene, RenderTextureViewHandle sceneView,
                                                RenderGraphResourceId resolved, RenderTextureViewHandle resolvedView,
                                                const std::shared_ptr<IResidentNativeReconstructionKernel> &kernel) {
                const auto pass = Pass(RenderPassKind::Compute, RenderQueueRole::Compute);
                Output(pass, scene, RenderGraphUsageKind::Sampled, RenderGraphAccess::Read);
                Output(pass, resolved, RenderGraphUsageKind::Storage, RenderGraphAccess::Write);
                RenderGraphNativeReconstruction work{.kernel = kernel};
                using enum NativeReconstructionImage;
                work.images[static_cast<std::size_t>(CurrentColor)] = {scene, sceneView};
                work.images[static_cast<std::size_t>(OutputColor)] = {resolved, resolvedView};
                return {pass, std::move(work)};
            }

            void ColorScene(EditorViewportColorGraph &graph, const EditorViewportColorPipeline &color, RenderGraphResourceId scene,
                            RenderGraphResourceId display, RenderTextureViewHandle displayView, FramebufferExtent extent) {
                last = Take(graph.AppendScene(builder, color, queue, initial, scene, display, displayView, last, extent));
            }

            void ColorEncode(EditorViewportColorGraph &graph, const EditorViewportColorPipeline &color, RenderGraphResourceId output,
                             RenderTextureViewHandle outputView) {
                last = Take(graph.AppendEncode(builder, color, output, outputView, last));
            }

            CompiledRenderGraphExecution Compile() {
                const auto graph = Take(builder.Finalize());
                const auto schedule = Take(CompileRenderGraph(graph));
                const std::array assignments{RenderQueueAssignment{RenderQueueRole::Graphics, queue},
                                             RenderQueueAssignment{RenderQueueRole::Compute, queue}};
                const auto queues = std::span{assignments}.first(spatial_ ? 2 : 1);
                const auto synchronized = Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial));
                return Take(CompileRenderGraphExecution(graph, schedule, synchronized, queues));
            }

        private:
            RenderGraphResourceId BufferRead(RenderGraphPassRef pass, RasterInputs &input, RenderBufferHandle buffer,
                                             RenderGraphUsageKind kind) {
                const auto found = std::ranges::find(buffers, buffer, &std::pair<RenderBufferHandle, RenderGraphResourceId>::first);
                const auto resource = found == buffers.end() ? ImportBuffer(buffer) : found->second;
                if (std::ranges::find(input.used, resource) == input.used.end()) {
                    Take(builder.AddUsage({pass, resource, RenderGraphAccess::Read, kind}));
                    input.used.push_back(resource);
                }
                return resource;
            }

            RenderGraphResourceId ImportBuffer(RenderBufferHandle buffer) {
                const auto resource = Take(builder.ImportBuffer(buffer, RenderGraphResourceClass::Persistent));
                buffers.emplace_back(buffer, resource);
                initial.push_back({resource,
                                   {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                    RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::NotApplicable, queue}});
                return resource;
            }

            RenderGraphBuilder builder;
            RenderQueueId queue;
            bool spatial_{};
            RenderGraphPassRef last;
            std::vector<std::pair<RenderBufferHandle, RenderGraphResourceId>> buffers;
            std::vector<RenderGraphImportedState> initial;
        };

        /** @brief Preserves the required atlas initialization pass for frames without a shadow caster. */
        std::uint32_t AtlasPassCount(const DirectionalShadowFrame *shadow) noexcept {
            return shadow ? shadow->cascadeCount : 1;
        }
    }  // namespace

    /** @brief Caches exact graphics topology by renderer slot and optional overlay draw. Native recording owns later completion. */
    struct EditorViewportRendererVulkan::ViewportRasterGraphs final {
        RasterAttachments attachments;
        std::vector<EditorViewportResources::MeshBinding> meshes;
        std::array<std::array<RasterVariant, 2>, 8> slots;
        std::array<std::unique_ptr<std::array<std::array<RasterVariant, 2>, 8>>, EditorViewportColorPipeline::SlotCount> colorSlots;
        RenderTextureHandle display, presentation, spatial;
        RenderTextureViewHandle displayView, presentationView, spatialView;
        std::uint32_t atlasPasses{};
        bool shadowDraws{};

        bool Matches(const EditorViewportResources &resources, const RenderSceneView &scene, const DirectionalShadowFrame *shadow) const {
            if (attachments != Attachments(resources) || display != resources.DisplayTexture() ||
                presentation != resources.PresentationTexture() || spatial != resources.SpatialTexture() ||
                meshes.size() != scene.instances.size() || shadowDraws != (shadow != nullptr) || atlasPasses != AtlasPassCount(shadow))
                return false;
            for (std::size_t index = 0; index < meshes.size(); ++index) {
                const auto source = scene.instances[index].mesh;
                const auto current = resources.FindMesh(source.id.value);
                if (!current || !current->ready || current->sourceGeneration != source.generation || current->mesh != meshes[index].mesh)
                    return false;
            }
            return true;
        }

        /** @brief Maps only the exact mesh and uniform generations already owned by the normal renderer. */
        void DrawMeshes(RasterGraphAuthor &author, RenderGraphPassRef pass, RasterInputs &inputs, std::span<const DrawSlot> draws) {
            for (std::size_t index = 0; index < meshes.size(); ++index) {
                const auto &mesh = meshes[index];
                const auto &draw = draws[index];
                author.Draw(pass, inputs, mesh.mesh, mesh.descriptor.vertexBuffer, mesh.descriptor.indexBuffer, draw.material, draw.buffer);
            }
        }

        void AuthorAtlas(RasterGraphAuthor &author, RasterVariant &variant, const FrameSlot &slot, RenderGraphResourceId atlas) {
            for (std::uint32_t index = 0; index < atlasPasses; ++index) {
                const auto pass = author.Pass();
                const auto clear = index == 0 ? std::optional{1.0F} : std::nullopt;
                author.Output(pass, atlas, RenderGraphUsageKind::DepthStencilAttachment,
                              clear ? RenderGraphAccess::Write : RenderGraphAccess::ReadWrite);
                auto &inputs = variant.inputs[index];
                if (shadowDraws) {
                    inputs.Reserve(meshes.size());
                    DrawMeshes(author, pass, inputs, slot.shadows[index]);
                }
                variant.workloads[index] = {pass, RenderGraphMaterialRaster{.depth = atlas,
                                                                            .depthView = attachments.atlasView,
                                                                            .extent = attachments.atlasExtent,
                                                                            .depthClear = clear,
                                                                            .draws = inputs.draws,
                                                                            .reads = inputs.reads,
                                                                            .depthOnly = true}};
            }
        }

        void AuthorScene(RasterGraphAuthor &author, RasterVariant &variant, const FrameSlot &slot, RenderGraphResourceId atlas,
                         RenderGraphResourceId color, RenderGraphResourceId depth, bool overlay) {
            const auto pass = author.Pass();
            author.Output(pass, color, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::Write);
            author.Output(pass, depth, RenderGraphUsageKind::DepthStencilAttachment, RenderGraphAccess::Write);
            auto &inputs = variant.inputs[atlasPasses];
            inputs.Reserve(meshes.size() + (overlay ? 1 : 0));
            DrawMeshes(author, pass, inputs, slot.draws);
            if (overlay) {
                const auto &draw = slot.overlay;
                author.Draw(pass, inputs, draw.mesh, draw.vertices, draw.indices, draw.uniforms.material, draw.uniforms.buffer);
            }
            if (!inputs.draws.empty())
                author.SampleAtlas(pass, inputs, atlas, attachments.atlasView);
            variant.workloads[atlasPasses] = {pass, RenderGraphMaterialRaster{.color = color,
                                                                              .depth = depth,
                                                                              .colorView = attachments.colorView,
                                                                              .depthView = attachments.depthView,
                                                                              .extent = attachments.extent,
                                                                              .draws = inputs.draws,
                                                                              .reads = inputs.reads}};
        }

        RenderGraphPassWorkload AuthorDisplayOverlay(RasterGraphAuthor &author, RasterVariant &variant, const OverlaySlot &overlay,
                                                     RenderGraphResourceId displayResource, RenderGraphResourceId depth,
                                                     RenderGraphResourceId atlas) {
            const auto pass = author.Pass();
            author.Output(pass, displayResource, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::ReadWrite);
            author.Output(pass, depth, RenderGraphUsageKind::DepthStencilAttachment, RenderGraphAccess::ReadWrite);
            auto &input = variant.displayOverlay;
            input.Reserve(1);
            author.Draw(pass, input, overlay.mesh, overlay.vertices, overlay.indices, overlay.uniforms.material, overlay.uniforms.buffer);
            author.SampleAtlas(pass, input, atlas, attachments.atlasView);
            return {pass, RenderGraphMaterialRaster{.color = displayResource,
                                                    .depth = depth,
                                                    .colorView = displayView,
                                                    .depthView = attachments.depthView,
                                                    .extent = attachments.extent,
                                                    .colorOperations = {.loadOperation = AttachmentLoadOperation::Load},
                                                    .depthClear = std::nullopt,
                                                    .draws = input.draws,
                                                    .reads = input.reads}};
        }

        void Build(RasterVariant &variant, const FrameSlot &slot, RenderQueueId queue, bool overlay,
                   const EditorViewportColorPipeline *pipeline = nullptr,
                   const std::shared_ptr<IResidentNativeReconstructionKernel> &kernel = {}) {
            variant.count = atlasPasses + 1;
            const auto draws = std::max<std::size_t>(1, meshes.size() + (overlay ? 1 : 0));
            RasterGraphAuthor author{draws, variant.count, queue, pipeline != nullptr, kernel != nullptr};
            const auto atlas = author.Attach(attachments.atlas);
            const auto scene = author.Attach(attachments.color);
            const auto depth = author.Attach(attachments.depth);
            AuthorAtlas(author, variant, slot, atlas);
            AuthorScene(author, variant, slot, atlas, scene, depth, overlay && !pipeline);
            if (pipeline) {
                const auto displayResource = author.Attach(display);
                const auto output = author.Attach(presentation);
                auto toneMapInput = scene;
                if (kernel) {
                    toneMapInput = author.Attach(spatial);
                    variant.spatialWork = variant.count;
                    variant.workloads[variant.count++] =
                        author.Reconstruct(scene, attachments.colorView, toneMapInput, spatialView, kernel);
                }
                author.ColorScene(variant.color, *pipeline, toneMapInput, displayResource, displayView, attachments.extent);
                RenderGraphPassWorkload displayOverlay;
                if (overlay)
                    displayOverlay = AuthorDisplayOverlay(author, variant, slot.overlay, displayResource, depth, atlas);
                author.ColorEncode(variant.color, *pipeline, output, presentationView);
                const auto work = variant.color.Workloads();
                for (const auto &pass : work.first(work.size() - 1))
                    variant.workloads[variant.count++] = pass;
                if (overlay)
                    variant.workloads[variant.count++] = std::move(displayOverlay);
                variant.workloads[variant.count++] = work.back();
            }
            variant.graph.emplace(author.Compile());
        }

        /** @brief Warms each bounded native-slot pairing only for the current material pair and its actual table state. */
        void PrepareColorSlot(std::span<const FrameSlot> nativeSlots, RenderQueueId queue, const EditorViewportColorPipeline &pipeline,
                              const std::shared_ptr<IResidentNativeReconstructionKernel> &kernel) {
            auto &slot = colorSlots[pipeline.CandidateSlot()];
            if (slot && (*slot)[0][0].color.MatchesInitialStates(pipeline))
                return;
            auto candidate = std::make_unique<std::array<std::array<RasterVariant, 2>, 8>>();
            for (std::size_t index = 0; index < nativeSlots.size(); ++index) {
                Build((*candidate)[index][0], nativeSlots[index], queue, false, &pipeline, kernel);
                if (meshes.size() < MaximumDraws)
                    Build((*candidate)[index][1], nativeSlots[index], queue, true, &pipeline, kernel);
            }
            slot = std::move(candidate);
        }

        RasterVariant *Find(std::uint32_t slot, bool overlay, const EditorViewportColorPipeline *pipeline) {
            const auto index = overlay ? 1 : 0;
            if (!pipeline)
                return &slots[slot][index];
            auto &prepared = colorSlots[pipeline->CandidateSlot()];
            return prepared ? &(*prepared)[slot][index] : nullptr;
        }
    };

    /** @copydoc EditorViewportRendererVulkan::PrepareViewportRasterGraphs */
    Result<void> EditorViewportRendererVulkan::PrepareViewportRasterGraphs(const Render::StaticMeshPassDescriptor &pass) {
        using namespace Render;
        if (const auto boundary = Detail::RenderFrontendResourceAccess::RequirePreparationBoundary(frontend_); boundary.HasError())
            return Result<void>::Failure(boundary.ErrorValue());
        const auto attachments = Attachments(resources_);
        if (!initialized_ || pass.target != resources_.Target() || !pass.extent.IsValid() || pass.extent != attachments.extent ||
            pass.scene.instances.size() > MaximumDraws || !device_.graphicsIdentity.IsValid())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        auto shadow = BuildDirectionalShadowFrame(pass.scene, static_cast<float>(pass.extent.width) / pass.extent.height,
                                                  Math::ClipDepthRange::ZeroToOne, resources_.ActiveDirectionalShadowProfile());
        if (shadow.HasError())
            return Result<void>::Failure(shadow.ErrorValue());
        const auto *cast = shadow.Value() ? &*shadow.Value() : nullptr;
        if (AtlasPassCount(cast) > MaximumDirectionalShadowCascades)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        try {
            if (viewportRasterGraphs_ && viewportRasterGraphs_->Matches(resources_, pass.scene, cast)) {
                if (color_)
                    viewportRasterGraphs_->PrepareColorSlot(std::span{slots_}.first(device_.frames), device_.graphicsIdentity, *color_,
                                                            spatial_);
                return Result<void>::Success();
            }
            auto candidate = std::make_shared<ViewportRasterGraphs>();
            candidate->attachments = attachments;
            candidate->display = resources_.DisplayTexture();
            candidate->displayView = resources_.DisplayTextureView();
            candidate->presentation = resources_.PresentationTexture();
            candidate->presentationView = resources_.PresentationTextureView();
            candidate->spatial = resources_.SpatialTexture();
            candidate->spatialView = resources_.SpatialTextureView();
            candidate->atlasPasses = AtlasPassCount(cast);
            candidate->shadowDraws = cast != nullptr;
            candidate->meshes.reserve(pass.scene.instances.size());
            for (const auto &instance : pass.scene.instances) {
                const auto mesh = resources_.FindMesh(instance.mesh.id.value);
                if (!mesh || !mesh->ready || mesh->sourceGeneration != instance.mesh.generation)
                    return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleMeshResource));
                candidate->meshes.push_back(*mesh);
            }
            if (color_)
                candidate->PrepareColorSlot(std::span{slots_}.first(device_.frames), device_.graphicsIdentity, *color_, spatial_);
            else
                for (std::uint32_t index = 0; index < device_.frames; ++index) {
                    candidate->Build(candidate->slots[index][0], slots_[index], device_.graphicsIdentity, false);
                    if (candidate->meshes.size() < MaximumDraws)
                        candidate->Build(candidate->slots[index][1], slots_[index], device_.graphicsIdentity, true);
                }
            viewportRasterGraphs_ = std::move(candidate);
            return Result<void>::Success();
        } catch (const BuildFailure &failure) {
            return Result<void>::Failure(failure.error);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(RendererErrors::ViewportGeometryCreationFailed));
        }
    }

    /** @copydoc EditorViewportRendererVulkan::ExecuteViewportRasterGraph */
    Result<void> EditorViewportRendererVulkan::ExecuteViewportRasterGraph(Render::RenderFrameScope &frame,
                                                                          const Render::StaticMeshPassDescriptor &pass, std::uint32_t slot,
                                                                          bool hasOverlay, const Render::DirectionalShadowFrame *shadow) {
        using namespace Render;
        if (!viewportRasterGraphs_ || slot >= device_.frames || !viewportRasterGraphs_->Matches(resources_, pass.scene, shadow))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        auto *prepared = viewportRasterGraphs_->Find(slot, hasOverlay, color_.get());
        if (!prepared || !prepared->graph)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        auto &variant = *prepared;
        if (shadow)
            for (std::uint32_t index = 0; index < shadow->cascadeCount; ++index) {
                const auto &tile = shadow->cascades[index].viewport;
                std::get<RenderGraphMaterialRaster>(variant.workloads[index].workload).region =
                    RenderMaterialRegion{tile.x, tile.y, tile.width, tile.height};
            }
        std::get<RenderGraphMaterialRaster>(variant.workloads[viewportRasterGraphs_->atlasPasses].workload).colorOperations.clearColor =
            pass.clearColor;

        if (variant.spatialWork) {
            const auto spatial = PrepareSpatialDispatch(frame.Token(), pass);
            if (spatial.HasError())
                return Result<void>::Failure(spatial.ErrorValue());
            std::get<RenderGraphNativeReconstruction>(variant.workloads[*variant.spatialWork].workload).dispatch = spatial.Value();
        }
        if (color_)
            variant.color.Refresh(*color_);
        const auto reserved = frame.ReserveSubmission(RenderQueueRole::Graphics);
        if (reserved.HasError())
            return Result<void>::Failure(reserved.ErrorValue());
        const std::array submissions{RenderGraphQueueSubmission{reserved.Value(), {0, variant.count}}};
        recordedEver_ = true;
        const auto recorded = frame.ExecuteSubmittedGraph(*variant.graph, std::span{variant.workloads}.first(variant.count), submissions);
        if (recorded.HasValue() && color_)
            RecordViewportColor(reserved.Value().signal);
        return recorded;
    }
}  // namespace Horo::Editor
