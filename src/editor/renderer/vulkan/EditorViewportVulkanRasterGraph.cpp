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
        constexpr auto MaximumCanvasPasses = 1;
        constexpr auto MaximumCapturePasses = 1;
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
            std::array<RasterInputs, RenderGraphMaterialRaster::MaximumRetainedPasses> inputs;
            RasterInputs displayOverlay;
            RasterInputs canvas;
            EditorViewportColorGraph color;
            std::array<RenderGraphPassWorkload,
                       MaximumScenePasses + MaximumCanvasPasses + MaximumColorPasses + MaximumSpatialPasses + MaximumCapturePasses>
                workloads;
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
            RasterGraphAuthor(std::size_t draws, std::size_t passes, RenderQueueId graphics, bool color, bool spatial,
                              bool cameraCrossFade = false, bool capture = false)
                : ownedBuilder(Take(RenderGraphBuilder::Create(
                      {.maxPasses = passes + (color ? MaximumColorPasses : 0) + (spatial ? 1 : 0) + capture,
                       .maxResources = 3 + 2 * draws + draws * passes + (color ? MaximumColorImports : 0) + (spatial ? 1 : 0) +
                                       (cameraCrossFade ? 5 : 0) + capture,
                       .maxUsages = 3 * passes + 3 * draws * passes + (color ? MaximumColorUses : 0) + (spatial ? 2 : 0) +
                                    (cameraCrossFade ? 8 : 0) + (capture ? 2 : 0),
                       .maxDependencies = passes + (color ? MaximumColorPasses : 0) + (spatial ? 1 : 0) + capture}))),
                  builder(*ownedBuilder), queue(graphics), spatial_(spatial) {}

            /** @brief Appends the renderer-owned scene to the paint program's existing sampled canvas graph. */
            RasterGraphAuthor(RenderGraphBuilder &scene, RenderQueueId graphics) noexcept : builder(scene), queue(graphics) {}

            /** @brief Transfers cold imported state declarations to the combined graph's sole compiler. */
            void AppendInitialStates(std::vector<RenderGraphImportedState> &destination) const {
                destination.insert(destination.end(), initial.begin(), initial.end());
            }

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

            /** @brief Copies from the last authored presentation attachment into the exact cold staging buffer. */
            RenderGraphPassWorkload Capture(RenderGraphResourceId image, const VulkanViewportImageCapture::Copy &capture) {
                const auto target = Take(builder.ImportBuffer(capture.staging, RenderGraphResourceClass::Persistent));
                initial.push_back({target, {}, RenderGraphInitialContents::Discarded});
                const auto pass = Pass(RenderPassKind::Copy);
                Output(pass, image, RenderGraphUsageKind::CopySource, RenderGraphAccess::Read);
                Output(pass, target, RenderGraphUsageKind::CopyDestination, RenderGraphAccess::Write);
                const auto &region = capture.region;
                return {pass, RenderGraphColorReadback{image, target, region.x, region.y, {region.width, region.height}}};
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

            void Sample(RenderGraphPassRef pass, RasterInputs &input, RenderGraphResourceId texture, RenderTextureViewHandle view) {
                Take(builder.AddUsage({pass, texture, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled}));
                input.reads.push_back({texture, {}, view});
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

            std::optional<RenderGraphBuilder> ownedBuilder;
            RenderGraphBuilder &builder;
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
        /** @brief Identifies the existing graph output and the exact views underlying its attachments. */
        struct RasterOutput final {
            RenderGraphResourceId color;
            RenderGraphResourceId depth;
            RenderTextureViewHandle colorView;
            RenderTextureViewHandle depthView;
        };

        /** @brief Borrows the canvas writer output and the exact material sampling it in the current slot. */
        struct CanvasSample final {
            const OverlaySlot &quad;
            RenderGraphResourceId color;
            RenderTextureViewHandle view;
            bool world;
        };

        RasterAttachments attachments;
        std::vector<EditorViewportResources::MeshBinding> meshes;
        std::array<std::array<RasterVariant, 2>, 8> slots;
        std::unique_ptr<std::array<std::array<RasterVariant, 2>, 8>> cameraCrossFadeSlots;
        std::optional<EditorViewportResources::CameraCrossFadeAttachments> cameraCrossFadeAttachments;
        std::array<std::unique_ptr<std::array<std::array<RasterVariant, 2>, 8>>, EditorViewportColorPipeline::SlotCount> colorSlots;
        std::unique_ptr<std::array<std::array<RasterVariant, 2>, 8>> captureSlots;
        std::uint64_t captureId{};
        std::size_t captureColorSlot{};
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

        void AuthorAtlas(RasterGraphAuthor &author, RasterVariant &variant, const FrameSlot &slot, RenderGraphResourceId atlas,
                         std::size_t first = 0) {
            for (std::uint32_t index = 0; index < atlasPasses; ++index) {
                const auto pass = author.Pass();
                const auto clear = index == 0 ? std::optional{1.0F} : std::nullopt;
                author.Output(pass, atlas, RenderGraphUsageKind::DepthStencilAttachment,
                              clear ? RenderGraphAccess::Write : RenderGraphAccess::ReadWrite);
                auto &inputs = variant.inputs[first + index];
                if (shadowDraws) {
                    inputs.Reserve(meshes.size());
                    DrawMeshes(author, pass, inputs, slot.shadows[index]);
                }
                variant.workloads[first + index] = {pass, RenderGraphMaterialRaster{.depth = atlas,
                                                                                    .depthView = attachments.atlasView,
                                                                                    .extent = attachments.atlasExtent,
                                                                                    .depthClear = clear,
                                                                                    .draws = inputs.draws,
                                                                                    .reads = inputs.reads,
                                                                                    .depthOnly = true}};
            }
        }

        void AuthorScene(RasterGraphAuthor &author, RasterVariant &variant, const FrameSlot &slot, RenderGraphResourceId atlas,
                         const RasterOutput &output, bool overlay, std::size_t work) {
            const auto pass = author.Pass();
            author.Output(pass, output.color, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::Write);
            author.Output(pass, output.depth, RenderGraphUsageKind::DepthStencilAttachment, RenderGraphAccess::Write);
            auto &inputs = variant.inputs[work];
            inputs.Reserve(meshes.size() + (overlay ? 1 : 0));
            DrawMeshes(author, pass, inputs, slot.draws);
            if (overlay) {
                const auto &draw = slot.overlay;
                author.Draw(pass, inputs, draw.mesh, draw.vertices, draw.indices, draw.uniforms.material, draw.uniforms.buffer);
            }
            if (!inputs.draws.empty())
                author.Sample(pass, inputs, atlas, attachments.atlasView);
            variant.workloads[work] = {pass, RenderGraphMaterialRaster{.color = output.color,
                                                                       .depth = output.depth,
                                                                       .colorView = output.colorView,
                                                                       .depthView = output.depthView,
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
            author.Sample(pass, input, atlas, attachments.atlasView);
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

        /** @brief Appends an exact premultiplied canvas consumer using the opaque viewport attachments already authored. */
        RenderGraphPassRef AuthorCanvas(RasterGraphAuthor &author, RasterVariant &variant, const CanvasSample &source,
                                        const RasterOutput &output) const {
            const auto pass = author.Pass();
            author.Output(pass, output.color, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::ReadWrite);
            if (source.world)
                author.Output(pass, output.depth, RenderGraphUsageKind::DepthStencilAttachment, RenderGraphAccess::ReadWrite);
            auto &input = variant.canvas;
            input.Reserve(1);
            const auto &quad = source.quad;
            author.Draw(pass, input, quad.mesh, quad.vertices, quad.indices, quad.uniforms.material, quad.uniforms.buffer);
            author.Sample(pass, input, source.color, source.view);
            variant.workloads[variant.count++] =
                {pass, RenderGraphMaterialRaster{.color = output.color,
                                                 .depth = source.world ? output.depth : RenderGraphResourceId{},
                                                 .colorView = output.colorView,
                                                 .depthView = source.world ? output.depthView : RenderTextureViewHandle{},
                                                 .extent = attachments.extent,
                                                 .colorOperations = {.loadOperation = AttachmentLoadOperation::Load},
                                                 .depthClear = std::nullopt,
                                                 .draws = input.draws,
                                                 .reads = input.reads}};
            return pass;
        }

        /** @brief Appends the same acquired viewport output chain for ordinary and canvas-bearing scene variants. */
        RenderGraphResourceId AppendColorTail(RasterGraphAuthor &author, RasterVariant &variant, const OverlaySlot *overlay,
                                              const RasterOutput &scene, const RenderGraphResourceId atlas,
                                              const EditorViewportColorPipeline &pipeline,
                                              const std::shared_ptr<IResidentNativeReconstructionKernel> &kernel) {
            const auto displayResource = author.Attach(display);
            const auto output = author.Attach(presentation);
            auto toneMapInput = scene.color;
            if (kernel) {
                toneMapInput = author.Attach(spatial);
                variant.spatialWork = variant.count;
                variant.workloads[variant.count++] = author.Reconstruct(scene.color, scene.colorView, toneMapInput, spatialView, kernel);
            }
            author.ColorScene(variant.color, pipeline, toneMapInput, displayResource, displayView, attachments.extent);
            RenderGraphPassWorkload displayOverlay;
            if (overlay)
                displayOverlay = AuthorDisplayOverlay(author, variant, *overlay, displayResource, scene.depth, atlas);
            author.ColorEncode(variant.color, pipeline, output, presentationView);
            const auto work = variant.color.Workloads();
            for (const auto &pass : work.first(work.size() - 1))
                variant.workloads[variant.count++] = pass;
            if (overlay)
                variant.workloads[variant.count++] = std::move(displayOverlay);
            variant.workloads[variant.count++] = work.back();
            return output;
        }

        /** @brief Reuses the opaque, shadow and output owners on the paint program's single combined graph. */
        RenderGraphPassRef BuildCanvas(RasterVariant &variant, RenderGraphBuilder &builder, std::vector<RenderGraphImportedState> &initial,
                                       const FrameSlot &slot, RenderQueueId queue, const CanvasSample &source, const bool overlay,
                                       const VulkanViewportImageCapture::Copy *capture) {
            RasterGraphAuthor author{builder, queue};
            const auto atlas = author.Attach(attachments.atlas);
            const RasterOutput output{author.Attach(attachments.color), author.Attach(attachments.depth), attachments.colorView,
                                      attachments.depthView};
            variant.count = atlasPasses + 1;
            AuthorAtlas(author, variant, slot, atlas);
            AuthorScene(author, variant, slot, atlas, output, overlay, atlasPasses);
            const auto consumer = AuthorCanvas(author, variant, source, output);
            if (capture)
                variant.workloads[variant.count++] = author.Capture(output.color, *capture);
            author.AppendInitialStates(initial);
            return consumer;
        }

        void Build(RasterVariant &variant, const FrameSlot &slot, RenderQueueId queue, bool overlay,
                   const EditorViewportColorPipeline *pipeline = nullptr,
                   const std::shared_ptr<IResidentNativeReconstructionKernel> &kernel = {},
                   const VulkanViewportImageCapture::Copy *capture = nullptr) {
            variant.count = atlasPasses + 1;
            const auto draws = std::max<std::size_t>(1, meshes.size() + (overlay ? 1 : 0));
            RasterGraphAuthor author{draws, variant.count, queue, pipeline != nullptr, kernel != nullptr, false, capture != nullptr};
            const auto atlas = author.Attach(attachments.atlas);
            const RasterOutput output{author.Attach(attachments.color), author.Attach(attachments.depth), attachments.colorView,
                                      attachments.depthView};
            AuthorAtlas(author, variant, slot, atlas);
            AuthorScene(author, variant, slot, atlas, output, overlay && !pipeline, atlasPasses);
            const auto presentation =
                pipeline ? AppendColorTail(author, variant, overlay ? &slot.overlay : nullptr, output, atlas, *pipeline, kernel)
                         : output.color;
            if (capture)
                variant.workloads[variant.count++] = author.Capture(presentation, *capture);
            variant.graph.emplace(author.Compile());
        }

        /** @brief Authors two ordinary shadow/raster groups, then reads the exact spare into the opaque GUI output. */
        void BuildCameraCrossFade(RasterVariant &variant, const FrameSlot &sourceDraws, const FrameSlot &destinationDraws,
                                  const CameraCrossFadeProgram &program, const DrawSlot &composite, RenderQueueId queue, bool overlay,
                                  const EditorViewportResources::CameraCrossFadeAttachments &spare) {
            const auto groupCount = atlasPasses + 1;
            variant.count = 2 * groupCount + 1;
            const auto draws = std::max<std::size_t>(1, meshes.size() + (overlay ? 1 : 0));
            RasterGraphAuthor author{draws, variant.count, queue, false, false, true};
            const auto atlas = author.Attach(attachments.atlas);
            const auto source = author.Attach(spare.color);
            const auto sourceDepth = author.Attach(spare.depth);
            const auto destination = author.Attach(attachments.color);
            const auto destinationDepth = author.Attach(attachments.depth);
            AuthorAtlas(author, variant, sourceDraws, atlas);
            AuthorScene(author, variant, sourceDraws, atlas, {source, sourceDepth, spare.colorView, spare.depthView}, false, atlasPasses);
            AuthorAtlas(author, variant, destinationDraws, atlas, groupCount);
            AuthorScene(author, variant, destinationDraws, atlas,
                        {destination, destinationDepth, attachments.colorView, attachments.depthView}, overlay, groupCount + atlasPasses);
            const auto pass = author.Pass();
            author.Output(pass, destination, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::ReadWrite);
            auto &input = variant.inputs[variant.count - 1];
            input.Reserve(1);
            author.Draw(pass, input, program.triangle.mesh, program.triangle.vertices, program.triangle.indices, composite.material,
                        composite.buffer);
            author.Sample(pass, input, source, spare.colorView);
            variant.workloads[variant.count - 1] = {pass, RenderGraphMaterialRaster{.color = destination,
                                                                                    .colorView = attachments.colorView,
                                                                                    .extent = attachments.extent,
                                                                                    .colorOperations = {.loadOperation =
                                                                                                            AttachmentLoadOperation::Load},
                                                                                    .depthClear = std::nullopt,
                                                                                    .draws = input.draws,
                                                                                    .reads = input.reads}};
            variant.graph.emplace(author.Compile());
        }

        /** @brief Builds alternate compositor topology only when exact same-generation attachments change. */
        void PrepareCameraCrossFadeSlots(std::span<const FrameSlot> source, std::span<const FrameSlot> destination,
                                         const CameraCrossFadeProgram &program, RenderQueueId queue,
                                         const EditorViewportResources::CameraCrossFadeAttachments &spare) {
            if (cameraCrossFadeAttachments == spare)
                return;
            auto candidate = std::make_unique<std::array<std::array<RasterVariant, 2>, 8>>();
            for (std::size_t index = 0; index < source.size(); ++index) {
                BuildCameraCrossFade((*candidate)[index][0], source[index], destination[index], program, program.draws[index], queue, false,
                                     spare);
                if (meshes.size() < MaximumDraws)
                    BuildCameraCrossFade((*candidate)[index][1], source[index], destination[index], program, program.draws[index], queue,
                                         true, spare);
            }
            cameraCrossFadeSlots = std::move(candidate);
            cameraCrossFadeAttachments = spare;
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

        RasterVariant *Find(std::uint32_t slot, bool overlay, const EditorViewportColorPipeline *pipeline, bool crossFade) {
            const auto index = overlay ? 1 : 0;
            if (crossFade)
                return cameraCrossFadeSlots ? &(*cameraCrossFadeSlots)[slot][index] : nullptr;
            if (!pipeline)
                return &slots[slot][index];
            auto &prepared = colorSlots[pipeline->CandidateSlot()];
            return prepared ? &(*prepared)[slot][index] : nullptr;
        }

        /** @brief Builds exact staging bindings only while an approved copy needs its first native submission. */
        void PrepareCaptureSlots(std::span<const FrameSlot> nativeSlots, RenderQueueId queue, const EditorViewportColorPipeline *pipeline,
                                 const std::shared_ptr<IResidentNativeReconstructionKernel> &kernel,
                                 const VulkanViewportImageCapture::Copy &capture) {
            const std::size_t colorSlot = pipeline ? pipeline->CandidateSlot() : 0;
            if (captureSlots && captureId == capture.id && captureColorSlot == colorSlot &&
                (!pipeline || (*captureSlots)[0][0].color.MatchesInitialStates(*pipeline)))
                return;
            auto candidate = std::make_unique<std::array<std::array<RasterVariant, 2>, 8>>();
            for (std::size_t index = 0; index < nativeSlots.size(); ++index) {
                Build((*candidate)[index][0], nativeSlots[index], queue, false, pipeline, kernel, &capture);
                if (meshes.size() < MaximumDraws)
                    Build((*candidate)[index][1], nativeSlots[index], queue, true, pipeline, kernel, &capture);
            }
            captureSlots = std::move(candidate);
            captureId = capture.id;
            captureColorSlot = colorSlot;
        }
    };

    /** @brief Keeps all material spans and the authoritative producer alive until graph execution has taken its native pins. */
    struct EditorViewportRendererVulkan::ViewportCanvasGraph final {
        std::shared_ptr<ViewportRasterGraphs> topology;
        std::shared_ptr<ViewportCanvasGraph> captureVariant;
        std::uint64_t captureId{};
        std::unique_ptr<Render::UiPaintProgram> paint;
        std::unique_ptr<CanvasQuadProgram> quad;
        std::array<RasterVariant, std::tuple_size_v<FrameSlots>> slots;
        Sha256Digest writer;
        Sha256Digest consumer;
        RenderTextureFormat stencil{};
        RenderGraphLimits limits;
        bool overlay{};

        /** @brief Compares the stored artifact identities and exact authored graph bounds. */
        bool Matches(const ViewportCanvasPreparation &request) const noexcept {
            return writer == request.writer.artifact.reflection->cookedArtifactKey &&
                   consumer == request.consumer.metadata.cookedArtifactKey && stencil == request.writer.stencilFormat &&
                   quad->space == request.space && overlay == request.overlay && limits.maxPasses == request.limits.maxPasses &&
                   limits.maxResources == request.limits.maxResources && limits.maxUsages == request.limits.maxUsages &&
                   limits.maxDependencies == request.limits.maxDependencies &&
                   Detail::RenderFrontendResourceAccess::MatchesSceneCanvas(*paint, request.paint, request.prototype, request.textures);
        }
    };

    /** @copydoc EditorViewportRendererVulkan::SupportsViewportCanvas */
    bool EditorViewportRendererVulkan::SupportsViewportCanvas() const noexcept {
        using namespace Render;
        using enum RenderTextureUsage;
        const auto &backend = frontend_.Capabilities();
        const auto &usages = backend.support.formats.usages;
        const auto supports = [&usages](const RenderTextureFormat format, const RenderTextureUsage required) {
            return HasTextureUsage(usages[static_cast<std::size_t>(format)], required);
        };
        return initialized_ && !color_ && backend.supportsMaterialRasterGraph &&
               supports(RenderTextureFormat::Rgba16Float, Sampled | RenderAttachment) &&
               supports(RenderTextureFormat::Depth32FloatStencil8, RenderAttachment);
    }

    /** @copydoc EditorViewportRendererVulkan::CanComposeViewportCanvas */
    bool EditorViewportRendererVulkan::CanComposeViewportCanvas() const noexcept {
        return SupportsViewportCanvas() && !preparedCrossFade_ && !requestedGrid_.visible && !requestedLightVisualizer_.selectedLight;
    }

    /** @copydoc EditorViewportRendererVulkan::ReleaseViewportCanvas */
    void EditorViewportRendererVulkan::ReleaseViewportCanvas() noexcept {
        viewportCanvasGraph_.reset();
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareViewportCanvas */
    Result<void> EditorViewportRendererVulkan::PrepareViewportCanvas(const Render::StaticMeshPassDescriptor &pass,
                                                                     const ViewportCanvasPreparation &request,
                                                                     const CancellationToken &cancellation) {
        using namespace Render;
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::Cancelled));
        if (!pass.IsValid() || color_ || preparedCrossFade_ || request.paint.frameSlots != device_.frames ||
            (request.overlay && pass.scene.instances.size() >= MaximumDraws))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (const auto prepared = PrepareViewportRasterGraphs(pass); prepared.HasError())
            return prepared;
        if (!request.writer.artifact.reflection)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (!viewportCanvasGraph_ || viewportCanvasGraph_->topology != viewportRasterGraphs_ || !viewportCanvasGraph_->Matches(request)) {
            auto base = BuildViewportCanvasGraph(request, cancellation, nullptr);
            if (base.HasError())
                return Result<void>::Failure(base.ErrorValue());
            viewportCanvasGraph_ = std::move(base).Value();
        }
        auto &base = *viewportCanvasGraph_;
        const auto capture = imageCapture_.Pending();
        if (!capture) {
            base.captureVariant.reset();
        } else if (!base.captureVariant || base.captureId != capture->id) {
            auto variant = BuildViewportCanvasGraph(request, cancellation, &*capture);
            if (variant.HasError()) {
                imageCapture_.Fail();
                base.captureVariant.reset();
            } else {
                base.captureVariant = std::move(variant).Value();
                base.captureId = capture->id;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::BuildViewportCanvasGraph */
    Result<std::shared_ptr<EditorViewportRendererVulkan::ViewportCanvasGraph>> EditorViewportRendererVulkan::BuildViewportCanvasGraph(
        const ViewportCanvasPreparation &request, const CancellationToken &cancellation, const VulkanViewportImageCapture::Copy *capture) {
        using Outcome = Result<std::shared_ptr<ViewportCanvasGraph>>;
        try {
            auto candidate = std::make_shared<ViewportCanvasGraph>();
            candidate->topology = viewportRasterGraphs_;
            candidate->writer = request.writer.artifact.reflection->cookedArtifactKey;
            candidate->consumer = request.consumer.metadata.cookedArtifactKey;
            candidate->stencil = request.writer.stencilFormat;
            candidate->limits = request.limits;
            candidate->overlay = request.overlay;
            auto quad = CreateCanvasQuadProgram(request.consumer, request.space, RenderTextureFormat::Rgba8Unorm);
            if (quad.HasError())
                return Outcome::Failure(quad.ErrorValue());
            candidate->quad = std::move(quad).Value();

            const UiPaintSceneCanvasGraph scene{request.limits, device_.graphicsIdentity,
                                                [this, &candidate, capture](RenderGraphBuilder &builder,
                                                                            const UiPaintSceneCanvasSource &source,
                                                                            std::vector<RenderGraphImportedState> &initial) {
                auto &program = *candidate->quad;
                if (const auto bound = BindCanvasQuadSource(program, source.slot, source.surface.color); bound.HasError())
                    return Result<RenderGraphPassRef>::Failure(bound.ErrorValue());
                const ViewportRasterGraphs::CanvasSample sample{program.slots[source.slot].front(), source.color,
                                                                program.sampledViews[source.slot],
                                                                program.space == UiRenderCompositionSpace::World};
                return Result<RenderGraphPassRef>::Success(candidate->topology->BuildCanvas(candidate->slots[source.slot], builder, initial,
                                                                                            slots_[source.slot], device_.graphicsIdentity,
                                                                                            sample, candidate->overlay, capture));
            }};
            auto program = UiPaintProgram::CreateSceneCanvas(frontend_, request.paint, request.prototype, request.textures, request.writer,
                                                             scene, cancellation);
            if (program.HasError())
                return Outcome::Failure(program.ErrorValue());
            candidate->paint = std::move(program).Value();
            if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
                return Outcome::Failure(processed.ErrorValue());
            if (const auto checked = candidate->paint->ValidatePreparation(); checked.HasError())
                return Outcome::Failure(checked.ErrorValue());
            return Outcome::Success(std::move(candidate));
        } catch (const BuildFailure &failure) {
            return Outcome::Failure(failure.error);
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(RendererErrors::ViewportGeometryCreationFailed));
        }
    }

    /** @copydoc EditorViewportRendererVulkan::ExecuteViewportCanvas */
    Result<void> EditorViewportRendererVulkan::ExecuteViewportCanvas(Render::RenderFrameScope &frame,
                                                                     const Render::StaticMeshPassDescriptor &pass,
                                                                     const std::optional<Math::Mat4> &placement,
                                                                     const ViewportCanvasSource &source) {
        using namespace Render;
        if (!viewportCanvasGraph_ || !source || color_ || preparedCrossFade_ || viewportCanvasGraph_->topology != viewportRasterGraphs_)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        auto &base = *viewportCanvasGraph_;
        const auto capture = imageCapture_.Pending();
        const bool withCapture = capture && base.captureVariant && capture->id == base.captureId;
        auto &canvas = withCapture ? *base.captureVariant : base;
        const auto slot = PrepareViewportSlot(frame.Token(), pass);
        if (slot.HasError())
            return Result<void>::Failure(slot.ErrorValue());
        if (const auto lighting = forwardLighting_.Prepare(pass.scene); lighting.HasError())
            return lighting;
        if (const auto shadow = PrepareShadowFrame(frame.Token(), slot.Value(), pass); shadow.HasError())
            return shadow;
        const auto *shadow = preparedShadow_ ? &*preparedShadow_ : nullptr;
        if (!canvas.topology->Matches(resources_, pass.scene, shadow))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        const auto count = PrepareViewportOverlay(frame.Token(), slot.Value(), pass);
        if (count.HasError())
            return Result<void>::Failure(count.ErrorValue());
        if ((count.Value() > pass.scene.instances.size()) != canvas.overlay)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        if (const auto drawn = PrepareViewportDraws(frame.Token(), slot.Value(), pass); drawn.HasError())
            return drawn;
        if (const auto written = WriteCanvasQuad(frame.Token(), *canvas.quad, slot.Value(), pass, placement); written.HasError())
            return written;

        auto &variant = canvas.slots[slot.Value()];
        if (shadow)
            for (std::uint32_t index = 0; index < shadow->cascadeCount; ++index) {
                const auto &tile = shadow->cascades[index].viewport;
                std::get<RenderGraphMaterialRaster>(variant.workloads[index].workload).region =
                    RenderMaterialRegion{tile.x, tile.y, tile.width, tile.height};
            }
        std::get<RenderGraphMaterialRaster>(variant.workloads[canvas.topology->atlasPasses].workload).colorOperations.clearColor =
            pass.clearColor;
        auto submission = source(*canvas.paint, slot.Value());
        if (submission.HasError())
            return Result<void>::Failure(submission.ErrorValue());
        const auto frameInput = canvas.paint->PrepareSceneFrame(frame, slot.Value(), submission.Value());
        if (frameInput.HasError())
            return Result<void>::Failure(frameInput.ErrorValue());
        recordedEver_ = true;
        const auto workloads = std::span{variant.workloads}.first(variant.count);
        if (!withCapture)
            return canvas.paint->ExecuteSceneCanvas(frame, frameInput.Value(), std::move(submission).Value(), workloads);
        const auto recorded = Detail::RenderFrontendResourceAccess::RecordSubmittedSceneCanvas(*canvas.paint, frame, frameInput.Value(),
                                                                                               std::move(submission).Value(), workloads);
        if (recorded.HasError()) {
            imageCapture_.Fail();
            return Result<void>::Failure(recorded.ErrorValue());
        }
        imageCapture_.Recorded(recorded.Value());
        return Result<void>::Success();
    }

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
        std::optional<EditorViewportResources::CameraCrossFadeAttachments> crossFade;
        if (preparedCrossFade_) {
            crossFade = resources_.CameraCrossFadeImages();
            if (!crossFade || !crossFadeSourceSlots_ || preparedCrossFade_->target != pass.target || pass.clearColor.alpha != 1.0F ||
                (cast && cast->cascadeCount > crossFadeShadowBudget_) || pass.scene.instances.size() > crossFadeDrawCount_)
                return Result<void>::Failure(MakeError(RendererErrors::ViewportCrossFadeStale));
        }
        const auto prepareCamera = [&](ViewportRasterGraphs &graphs) {
            if (crossFade)
                graphs.PrepareCameraCrossFadeSlots(std::span{*crossFadeSourceSlots_}.first(device_.frames),
                                                   std::span{slots_}.first(device_.frames), crossFadeProgram_, device_.graphicsIdentity,
                                                   *crossFade);
        };
        try {
            if (viewportRasterGraphs_ && viewportRasterGraphs_->Matches(resources_, pass.scene, cast)) {
                if (color_)
                    viewportRasterGraphs_->PrepareColorSlot(std::span{slots_}.first(device_.frames), device_.graphicsIdentity, *color_,
                                                            spatial_);
                prepareCamera(*viewportRasterGraphs_);
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
            prepareCamera(*candidate);
            viewportRasterGraphs_ = std::move(candidate);
            return Result<void>::Success();
        } catch (const BuildFailure &failure) {
            return Result<void>::Failure(failure.error);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(RendererErrors::ViewportGeometryCreationFailed));
        }
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareImageCaptureGraph */
    void EditorViewportRendererVulkan::PrepareImageCaptureGraph() {
        const auto capture = imageCapture_.Pending();
        if (!capture || preparedCrossFade_) {
            viewportRasterGraphs_->captureSlots.reset();
            return;
        }
        try {
            viewportRasterGraphs_->PrepareCaptureSlots(std::span{slots_}.first(device_.frames), device_.graphicsIdentity, color_.get(),
                                                       spatial_, *capture);
        } catch (const BuildFailure &) {
            imageCapture_.Fail();
        } catch (const std::bad_alloc &) {
            imageCapture_.Fail();
        }
    }

    /** @copydoc EditorViewportRendererVulkan::ExecuteViewportRasterGraph */
    Result<void> EditorViewportRendererVulkan::ExecuteViewportRasterGraph(
        Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &pass, std::uint32_t slot, bool hasOverlay,
        const Render::DirectionalShadowFrame *shadow, const std::optional<Render::DirectionalShadowFrame> *sourceShadow) {
        using namespace Render;
        if (!viewportRasterGraphs_ || slot >= device_.frames || !viewportRasterGraphs_->Matches(resources_, pass.scene, shadow))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportStaleTarget));
        auto *prepared = viewportRasterGraphs_->Find(slot, hasOverlay, color_.get(), sourceShadow != nullptr);
        const auto capture = imageCapture_.Pending();
        const bool withCapture =
            !sourceShadow && capture && viewportRasterGraphs_->captureSlots && viewportRasterGraphs_->captureId == capture->id;
        if (withCapture)
            prepared = &(*viewportRasterGraphs_->captureSlots)[slot][hasOverlay ? 1 : 0];
        if (!prepared || !prepared->graph)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        auto &variant = *prepared;
        const auto atlasPasses = viewportRasterGraphs_->atlasPasses;
        const auto setRaster = [&](std::size_t first, const DirectionalShadowFrame *active) {
            if (active)
                for (std::uint32_t index = 0; index < active->cascadeCount; ++index) {
                    const auto &tile = active->cascades[index].viewport;
                    std::get<RenderGraphMaterialRaster>(variant.workloads[first + index].workload).region =
                        RenderMaterialRegion{tile.x, tile.y, tile.width, tile.height};
                }
            std::get<RenderGraphMaterialRaster>(variant.workloads[first + atlasPasses].workload).colorOperations.clearColor =
                pass.clearColor;
        };
        if (sourceShadow) {
            const auto *source = *sourceShadow ? &**sourceShadow : nullptr;
            if ((source != nullptr) != (shadow != nullptr) || AtlasPassCount(source) != atlasPasses ||
                viewportRasterGraphs_->cameraCrossFadeAttachments != resources_.CameraCrossFadeImages())
                return Result<void>::Failure(MakeError(RendererErrors::ViewportCrossFadeStale));
            setRaster(0, source);
        }
        setRaster(sourceShadow ? atlasPasses + 1 : 0, shadow);

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
        if (withCapture) {
            if (recorded.HasValue())
                imageCapture_.Recorded(reserved.Value().signal);
            else
                imageCapture_.Fail();
        }
        return recorded;
    }
}  // namespace Horo::Editor
