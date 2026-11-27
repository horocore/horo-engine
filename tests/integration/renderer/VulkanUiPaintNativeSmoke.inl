/** @file VulkanUiPaintNativeSmoke.inl @brief Native offscreen Vulkan paint, isolation and stencil pixels. */
namespace VulkanUiPaintSmoke {
    using VulkanVfxGpuSmoke::Accepted;
    using VulkanVfxGpuSmoke::Read;
    using VulkanVfxGpuSmoke::Value;
    using VulkanVfxInstancedSmoke::Owner;
    using VulkanVfxInstancedSmoke::Ready;
    constexpr FramebufferExtent Extent{64, 32};
    constexpr RenderTextureFormat Stencil = RenderTextureFormat::Depth32FloatStencil8;

    /** @brief Admits the exact production-cooked reflected UI kernel supplied by the native qualification lane. */
    CookedUiPaintKernel Load(const std::filesystem::path &root) {
        const auto target = UiPaintVulkanTarget();
        auto manifest = MakeUiPaintShaderManifest(target);
        auto payload = Read(root / "shaders/runtime/ui-paint-vulkan.horoshdr");
        const auto metadata = Read(root / "shaders/runtime/ui-paint-vulkan.reflection.json");
        auto reflection = Value(DecodeReflectedShaderArtifact(manifest, target, payload, metadata));
        CompiledShaderArtifact artifact{.backend = target.backend,
                                        .payloadFormat = target.payloadFormat,
                                        .artifactKey = reflection.compiledArtifactKey,
                                        .payload = std::move(payload),
                                        .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(reflection))};
        return {std::move(manifest), target, std::move(artifact), RenderTextureFormat::Rgba8Unorm, Stencil};
    }

    using Color = std::array<float, 4>;

    /** @brief Uses only author-space geometry. No expected pixel is inserted into any native image. */
    struct Geometry final {
        std::vector<UiPaintVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<UiPaintParameters> parameters;

        UiPaintDraw Quad(const UiPaintScissor region, const Color color) {
            const auto x = static_cast<float>(region.x);
            const auto y = static_cast<float>(region.y);
            const auto width = static_cast<float>(region.width);
            const auto height = static_cast<float>(region.height);
            const auto vertex = static_cast<std::uint32_t>(vertices.size());
            const auto first = static_cast<std::uint32_t>(indices.size());
            vertices.insert(vertices.end(), {{{x, y}, {0, 0}, color},
                                             {{x + width, y}, {1, 0}, color},
                                             {{x + width, y + height}, {1, 1}, color},
                                             {{x, y + height}, {0, 1}, color}});
            indices.insert(indices.end(), {vertex, vertex + 1, vertex + 2, vertex, vertex + 2, vertex + 3});
            return {.firstIndex = first, .indexCount = 6, .scissor = {0, 0, Extent.width, Extent.height}};
        }

        void Parameters(std::span<const UiPaintDraw> draws) {
            for (const auto &draw : draws)
                parameters.push_back({.extent = {Extent.width, Extent.height},
                                      .opacity = draw.opacity,
                                      .sampling = static_cast<std::uint32_t>(draw.sampling),
                                      .flags = draw.coverage != NoUiPaintTexture ? 1U : 0U,
                                      .maskExtent = draw.maskExtent,
                                      .cornerRadii = draw.cornerRadii});
        }
    };

    /** @brief Owns the portable closed graph and ordered raw paint programs until synchronous execution returns. */
    struct Recipe final {
        Geometry geometry;
        std::array<UiPaintDraw, 1> group, mask;
        std::array<UiPaintDraw, 4> root;
        std::array<RenderGraphResourceId, 1> neutralSampled;
        std::array<RenderGraphResourceId, 3> rootSampled;
        std::array<RenderGraphPassWorkload, 4> workloads;
        std::optional<CompiledRenderGraphExecution> plan;

        Recipe() {
            constexpr Color white{1, 1, 1, 1};
            constexpr UiPaintScissor whole{0, 0, Extent.width, Extent.height};
            group[0] = geometry.Quad(whole, {0, 1, 0, 1});
            mask[0] = geometry.Quad({16, 4, 40, 24}, white);
            mask[0].sampling = UiPaintSampling::RoundedRectangle;
            mask[0].maskExtent = {40, 24};
            mask[0].cornerRadii = {4, 4, 4, 4};
            root[0] = geometry.Quad({0, 8, 32, 20}, white);
            root[0].operation = UiPaintOperation::PushStencil;
            root[1] = geometry.Quad(whole, white);
            root[1].sampling = UiPaintSampling::Premultiplied;
            root[1].sampled = 1;
            root[1].coverage = 2;
            root[1].opacity = 0.5F;
            root[1].stencilDepth = 1;
            root[2] = root[0];
            root[2].operation = UiPaintOperation::PopStencil;
            root[2].stencilDepth = 1;
            root[3] = geometry.Quad({0, 16, Extent.width, 16}, {1, 0, 0, 1});
            geometry.Parameters(group);
            geometry.Parameters(mask);
            geometry.Parameters(root);
        }

        /** @brief Imports actual tracked render resources and states, including one exact cleared neutral texel. */
        void Build(Owner &owner, const UploadedUiPaintFrame &uploaded, std::shared_ptr<IResidentUiPaintKernel> kernel) {
            auto &frontend = *owner.frontend;
            auto builder = Value(RenderGraphBuilder::Create({4, 8, 32, 3}));
            const auto handles = uploaded.buffers;
            const std::array buffers{Value(builder.ImportBuffer(handles.vertices, RenderGraphResourceClass::Persistent)),
                                     Value(builder.ImportBuffer(handles.indices, RenderGraphResourceClass::Persistent)),
                                     Value(builder.ImportBuffer(handles.parameters, RenderGraphResourceClass::Persistent))};
            const auto color = [&](const FramebufferExtent extent, const bool copy) {
                auto usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled;
                if (copy)
                    usage = usage | RenderTextureUsage::CopySource;
                return Value(builder.ImportTexture(Ready(frontend, frontend.CreateTexture({.extent = extent,
                                                                                           .format = RenderTextureFormat::Rgba8Unorm,
                                                                                           .usage = usage},
                                                                                          {})),
                                                   RenderGraphResourceClass::Persistent));
            };
            const auto neutral = color({1, 1}, false);
            const auto groupColor = color(Extent, false);
            const auto maskColor = color(Extent, false);
            const auto output = color(Extent, true);
            const auto stencil =
                Value(builder.ImportTexture(Ready(frontend, frontend.CreateTexture({.extent = Extent,
                                                                                    .format = Stencil,
                                                                                    .usage = RenderTextureUsage::RenderAttachment},
                                                                                   {})),
                                            RenderGraphResourceClass::Persistent));
            neutralSampled = {neutral};
            rootSampled = {neutral, groupColor, maskColor};
            const auto next = [&] {
                return Value(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
            };
            const auto white = next();
            Accepted(builder.AddUsage({white, neutral, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}));
            workloads[0] = {white, RenderGraphColorAttachment{neutral, {.clearColor = {1, 1, 1, 1}}}};
            AddPaint(builder, next(), {groupColor, {}}, buffers, neutralSampled, group, 0, uploaded, kernel, 1);
            AddPaint(builder, next(), {maskColor, {}}, buffers, neutralSampled, mask, 1, uploaded, kernel, 2);
            AddPaint(builder, next(), {output, stencil}, buffers, rootSampled, root, 2, uploaded, kernel, 3);
            for (std::size_t index = 1; index < workloads.size(); ++index)
                Accepted(
                    builder.AddDependency({workloads[index - 1].pass, workloads[index].pass, RenderGraphDependencyKind::ResourceHazard}));
            const auto graph = Value(builder.Finalize());
            const auto ordered = Value(CompileRenderGraph(graph));
            std::vector<RenderGraphImportedState> initial;
            for (const auto buffer : buffers)
                initial.push_back({buffer,
                                   {RenderGraphSynchronizationAccess::Read,
                                    RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                    RenderGraphPipelineScope::Graphics,
                                    RenderGraphTextureLayout::NotApplicable,
                                    {1}}});
            for (const auto texture : {neutral, groupColor, maskColor, output, stencil})
                initial.push_back({texture, {}, RenderGraphInitialContents::Discarded});
            const auto states = Value(SynthesizeRenderGraphSynchronization(graph, ordered, owner.queues, initial));
            plan = Value(CompileRenderGraphExecution(graph, ordered, states, owner.queues));
        }

        void AddPaint(RenderGraphBuilder &builder, const RenderGraphPassRef pass, const std::array<RenderGraphResourceId, 2> target,
                      const std::array<RenderGraphResourceId, 3> buffers, const std::span<const RenderGraphResourceId> sampled,
                      const std::span<const UiPaintDraw> draws, const std::uint32_t first, const UploadedUiPaintFrame &uploaded,
                      const std::shared_ptr<IResidentUiPaintKernel> &kernel, const std::size_t output) {
            using enum RenderGraphUsageKind;
            using enum RenderGraphAccess;
            constexpr std::array usages{VertexBuffer, IndexBuffer, UniformBuffer};
            for (std::size_t index = 0; index < buffers.size(); ++index)
                Accepted(builder.AddUsage({pass, buffers[index], Read, usages[index]}));
            for (const auto sample : sampled)
                Accepted(builder.AddUsage({pass, sample, Read, Sampled}));
            Accepted(builder.AddUsage({pass, target[0], Write, ColorAttachment}));
            if (target[1].IsValid())
                Accepted(builder.AddUsage({pass, target[1], Write, DepthStencilAttachment}));
            workloads[output] = {pass, RenderGraphUiPaint{.color = target[0],
                                                          .stencil = target[1],
                                                          .vertices = buffers[0],
                                                          .indices = buffers[1],
                                                          .parameters = buffers[2],
                                                          .sampledResources = sampled,
                                                          .extent = Extent,
                                                          .vertexCount = static_cast<std::uint32_t>(geometry.vertices.size()),
                                                          .indexCount = static_cast<std::uint32_t>(geometry.indices.size()),
                                                          .draws = draws,
                                                          .kernel = kernel,
                                                          .clearColor = true,
                                                          .clearStencil = target[1].IsValid(),
                                                          .frameRevision = uploaded.revision,
                                                          .firstParameter = first,
                                                          .neutralSample = sampled.front()}};
        }
    };

    /** @brief Records or rejects the actual graph before issuing a bounded proof of completed GPU work. */
    void Execute(Owner &owner, const Recipe &recipe, const std::uint64_t number, const bool accept) {
        CAPTURE(number, accept);
        auto frame = Value(owner.frontend->BeginFrame({number, Extent}));
        const auto issued = Value(frame.ReserveSubmission(RenderQueueRole::Graphics));
        const std::array submissions{RenderGraphQueueSubmission{issued, {0, recipe.plan->Passes().size()}}};
        const auto recorded = frame.ExecuteSubmittedGraph(*recipe.plan, recipe.workloads, submissions);
        if (!accept) {
            REQUIRE(recorded.HasError());
            CHECK(recorded.ErrorValue().code.Value() == UiPaintFrameErrors::InvalidInput.code.Value());
            return;
        }
        Accepted(recorded);
        Accepted(frame.Present());
        RequireCompletion(*owner.frontend, issued.signal);
    }

    TEST_CASE("Vulkan UI paint draws an exact isolated group through coverage and balanced stencil",
              "[integration][renderer][vulkan][gpu][ui-paint-native]") {
        const auto *configured = std::getenv("HORO_VULKAN_UI_PAINT_ASSET_ROOT");
        if (!configured || !*configured)
            SKIP("Supply the genuine Vulkan offscreen UI paint package.");
        const std::filesystem::path root{configured};
        REQUIRE(root.is_absolute());
        auto cooked = Load(root);
        Owner owner;
        auto kernel = [&] {
            INFO("Realize the exact selected Vulkan UI pipeline and descriptor owners.");
            return Value(owner.frontend->RealizeUiPaintKernel(cooked));
        }();
        REQUIRE(kernel->RequiresGraphNeutralSample());
        Recipe recipe;
        auto &frontend = *owner.frontend;
        CHECK_FALSE(frontend.Capabilities().support.features.Supports(RenderCapability::UiPaint));
        const auto buffer = [&](const RenderBufferUsage usage, const std::size_t bytes) {
            return Ready(frontend,
                         frontend.CreateBuffer({.byteSize = bytes, .usage = usage, .access = RenderBufferAccess::HostVisible}, {}));
        };
        const UiPaintFrameBuffers buffers{buffer(RenderBufferUsage::Vertex, recipe.geometry.vertices.size() * sizeof(UiPaintVertex)),
                                          buffer(RenderBufferUsage::Index, recipe.geometry.indices.size() * sizeof(std::uint32_t)),
                                          buffer(RenderBufferUsage::Uniform,
                                                 recipe.geometry.parameters.size() * UiPaintFrameUpdate::ParameterStride)};
        std::uint64_t revisionNumber{};
        const auto revision = [&] {
            Accepted(frontend.UpdateUiPaintFrame(buffers, {++revisionNumber, recipe.geometry.vertices, recipe.geometry.indices,
                                                           recipe.geometry.parameters}));
            return UploadedUiPaintFrame{buffers, revisionNumber};
        };
        auto admitted = revision();
        recipe.Build(owner, admitted, kernel);

        auto &neutral = std::get<RenderGraphColorAttachment>(recipe.workloads[0].workload);
        neutral.operations.clearColor.red = 0;
        Execute(owner, recipe, 1, false);
        neutral.operations.clearColor.red = 1;

        Execute(owner, recipe, 2, true);
        using NativeReadback = Horo::Render::Test::VfxInstanced::NativeReadback;
        NativeReadback readback{owner.loader, NativeReadback::Capture::ViewportColor};
        readback.Prepare();
        const auto pixels = readback.ReadRgba8Attachment(owner.loader.OnlyRgba8CopySource());
        const auto pixel = [&](std::size_t x, std::size_t y) {
            return std::span{pixels}.subspan((y * Extent.width + x) * 4, 4);
        };
        const auto check = [&](std::size_t x, std::size_t y, const std::array<int, 4> expected) {
            CAPTURE(x, y);
            const auto actual = pixel(x, y);
            for (std::size_t component = 0; component < actual.size(); ++component)
                CHECK(std::abs(actual[component] - expected[component]) <= 1);
        };
        check(20, 12, {0, 128, 0, 128});
        check(8, 12, {0, 0, 0, 0});
        check(50, 12, {0, 0, 0, 0});
        check(20, 24, {255, 0, 0, 255});
        check(50, 24, {255, 0, 0, 255});
        check(5, 2, {0, 0, 0, 0});

        const auto old = admitted;
        admitted = revision();
        CHECK(admitted.revision > old.revision);
        Execute(owner, recipe, 3, false);
        Accepted(frontend.ReleaseBuffer(buffers.vertices));
        Accepted(frontend.ReleaseBuffer(buffers.indices));
        Accepted(frontend.ReleaseBuffer(buffers.parameters));
    }
}  // namespace VulkanUiPaintSmoke
