#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "MetalClusteredMaterialGraphFixture.h"
#include "RenderFrontendResourceAccess.h"
#include "renderer/RenderBackendDrainTestSupport.h"
#include "runtime/renderer/modules/metal/MetalPreparedMaterialState.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"
#include "runtime/renderer/modules/metal/MetalUniformShaderAbi.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace Horo::Render::Test {
    namespace {
        using namespace Detail;

        /** @brief Reports exact native admission failures without replacing the original error. */
        template <typename T> T RequireValue(Result<T> result) {
            INFO(
                (result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Native operation accepted."));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Preserves typed native validation failures under the exact graph-pass context. */
        void RequireSuccess(const Result<void> &result) {
            INFO(
                (result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Native operation accepted."));
            REQUIRE(result.HasValue());
        }

        /** @brief Looks up only a typed generation issued by the fixture's engine registry. */
        template <typename Handle> RenderResourceIdentity Identity(const Handle handle) {
            return {handle.owner, handle.slot, handle.generation};
        }

        /** @brief Creates a typed alias only after a successful engine registry publication. */
        template <typename Handle> Handle IssuedHandle(const RenderResourceIdentity id) {
            return {id.owner, id.slot, id.generation};
        }

        /** @brief Uses the same canonical MeshVertex layout when requesting fresh actual native type reflection. */
        MTLVertexDescriptor *QualificationVertexDescriptor() {
            auto *vertices = [[MTLVertexDescriptor alloc] init];
            constexpr std::array offsets{offsetof(MeshVertex, position), offsetof(MeshVertex, normal), offsetof(MeshVertex, uv)};
            for (NSUInteger index = 0; index < offsets.size(); ++index) {
                vertices.attributes[index].format = index == 2 ? MTLVertexFormatFloat2 : MTLVertexFormatFloat3;
                vertices.attributes[index].offset = offsets[index];
                vertices.attributes[index].bufferIndex = MetalMeshVertexBufferIndex;
            }
            vertices.layouts[MetalMeshVertexBufferIndex].stride = sizeof(MeshVertex);
            vertices.layouts[MetalMeshVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
            return vertices;
        }

        /** @brief Creates real driver reflection from the fixture's genuine already-admitted library functions. */
        id<MTLBufferBinding> QualificationUniformReflection(id<MTLDevice> device, id<MTLFunction> vertex, id<MTLFunction> fragment,
                                                            const std::uint32_t binding) {
            auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
            descriptor.vertexFunction = vertex;
            descriptor.fragmentFunction = fragment;
            descriptor.vertexDescriptor = QualificationVertexDescriptor();
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            MTLRenderPipelineReflection *reflection = nil;
            NSError *error = nil;
            id<MTLRenderPipelineState> pipeline =
                [device newRenderPipelineStateWithDescriptor:descriptor
                                                     options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
                                                  reflection:&reflection
                                                       error:&error];
            INFO((error ? error.localizedDescription.UTF8String : "Actual native uniform reflection requested."));
            REQUIRE(pipeline);
            REQUIRE(reflection);
            for (id<MTLBinding> argument in reflection.vertexBindings)
                if (argument.used && argument.type == MTLBindingTypeBuffer && argument.index == binding)
                    return (id<MTLBufferBinding>)argument;
            FAIL("The actual vertex shader lacks its reflected viewport uniform binding.");
            return nil;
        }

        /** @brief Copies one genuine cooked member for negative checks without changing artifact or material authority. */
        ShaderReflectedParameter QualificationParameter(const NormalizedShaderReflection &reflection, const std::uint32_t id) {
            const auto member = std::ranges::find(reflection.parameters, ShaderParameterId{id}, &ShaderReflectedParameter::id);
            REQUIRE(member != reflection.parameters.end());
            return *member;
        }

        void CheckUniformMatrixMutations(MTLStructType *structure, const ShaderReflectedParameter &original) {
            auto parameter = original;
            parameter.byteOffset += 4;
            REQUIRE_FALSE(HasMetalUniformParameter(structure, parameter));
            parameter = original;
            parameter.matrixStride = 32;
            REQUIRE_FALSE(HasMetalUniformParameter(structure, parameter));
            parameter = original;
            parameter.columnMajor = false;
            REQUIRE_FALSE(HasMetalUniformParameter(structure, parameter));
            parameter = original;
            parameter.rows = 3;
            parameter.columns = 3;
            REQUIRE_FALSE(HasMetalUniformParameter(structure, parameter));
        }

        void CheckUniformArrayMutations(MTLStructType *structure, const ShaderReflectedParameter &original) {
            auto parameter = original;
            ++parameter.arrayCount;
            REQUIRE_FALSE(HasMetalUniformParameter(structure, parameter));
            parameter = original;
            parameter.arrayStride += 16;
            REQUIRE_FALSE(HasMetalUniformParameter(structure, parameter));
            parameter = original;
            parameter.byteOffset += 16;
            REQUIRE_FALSE(HasMetalUniformParameter(structure, parameter));
        }

        /** @brief Observes a real command's terminal status within the test-only bounded readback budget. */
        bool AwaitTerminal(id<MTLCommandBuffer> commands) noexcept {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (commands && commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            return !commands || commands.status == MTLCommandBufferStatusCompleted || commands.status == MTLCommandBufferStatusError;
        }

        /** @brief Keeps cleanup from masking an earlier Catch failure while honoring actual owner drain. */
        bool DrainQualificationFrontend(RenderFrontend &frontend) noexcept {
            try {
                if (frontend.BeginShutdown().HasError())
                    return false;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                for (;;) {
                    const auto progress = frontend.PollDrain();
                    if (progress.HasError())
                        return false;
                    if (progress.Value() == RenderBackendDrainProgress::Drained)
                        return true;
                    if (std::chrono::steady_clock::now() >= deadline)
                        return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            } catch (...) {
                return false;
            }
        }

        /** @brief Authors exact whole-resource access and honest initial contents for this cold test graph. */
        struct QualificationGraph final {
            QualificationGraph(RenderGraphBuilder &source, const RenderQueueId graphics) : builder(source), queue(graphics) {}

            RenderGraphBuilder &builder;
            RenderQueueId queue;
            std::vector<RenderGraphImportedState> initial;

            RenderGraphResourceId Buffer(RenderBufferHandle handle, RenderGraphSynchronizationOperation operation, bool initialized) {
                const auto id = RequireValue(builder.ImportBuffer(handle, RenderGraphResourceClass::Persistent));
                RenderGraphLogicalState state;
                const auto scope = operation == RenderGraphSynchronizationOperation::GraphicsInputBuffer
                                       ? RenderGraphPipelineScope::Graphics
                                       : RenderGraphPipelineScope::External;
                if (initialized)
                    state = {RenderGraphSynchronizationAccess::Read, operation, scope, RenderGraphTextureLayout::NotApplicable, queue};
                initial.push_back({id, state, initialized ? RenderGraphInitialContents::Preserved : RenderGraphInitialContents::Discarded});
                return id;
            }

            RenderGraphResourceId Texture(RenderTextureHandle handle) {
                const auto id = RequireValue(builder.ImportTexture(handle, RenderGraphResourceClass::Persistent));
                initial.push_back({id, {}, RenderGraphInitialContents::Discarded});
                return id;
            }

            void Use(RenderGraphPassRef pass, RenderGraphResourceId id, RenderGraphAccess access, RenderGraphUsageKind kind) {
                REQUIRE(builder.AddUsage({pass, id, access, kind}).HasValue());
            }

            std::array<RenderGraphPassRef, 4> Passes() {
                std::array<RenderGraphPassRef, 4> passes;
                for (std::size_t index = 0; index < passes.size(); ++index) {
                    passes[index] = RequireValue(
                        builder.AddPass(index == 0 ? RenderPassKind::Compute : RenderPassKind::Graphics, RenderQueueRole::Graphics));
                    if (index != 0)
                        REQUIRE(builder.AddDependency({passes[index - 1], passes[index], RenderGraphDependencyKind::ExecutionOrder})
                                    .HasValue());
                }
                return passes;
            }
        };
    }  // namespace

    void MetalClusteredMaterialGraphFixture::Initialize(const Editor::EditorClusteredLightingAssets &assets) {
        CreateOwners();
        view_ = RequireValue(frontend_->CreateRenderViewIdentity());
        PreparePrograms(assets);
        PrepareScene();
        Upload(32, 1);
        PrepareMaterials();
        PrepareGraph();
    }

    void MetalClusteredMaterialGraphFixture::PrepareScene() {
        constexpr std::array<MeshVertex, 3> vertices{
            {{{-1, -1, 0}, {0, 0, 1}, {}}, {{1, -1, 0}, {0, 0, 1}, {}}, {{0, 1, 0}, {0, 0, 1}, {}}}};
        constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
        vertices_ = Buffer({.byteSize = sizeof(vertices), .usage = RenderBufferUsage::Vertex, .access = RenderBufferAccess::HostVisible},
                           std::as_bytes(std::span{vertices}));
        indices_ = Buffer({.byteSize = sizeof(indices), .usage = RenderBufferUsage::Index, .access = RenderBufferAccess::HostVisible},
                          std::as_bytes(std::span{indices}));
        const std::array dependencies{Identity(vertices_), Identity(indices_)};
        const auto record = Reserve(RenderResourceClass::Mesh, dependencies);
        const RenderMeshDescriptor mesh{vertices_,
                                        indices_,
                                        sizeof(MeshVertex),
                                        3,
                                        RenderIndexFormat::UInt32,
                                        3,
                                        RenderPrimitiveTopology::Triangles,
                                        {{-1, -1, -0.01F}, {1, 1, 0.01F}}};
        mesh_ = IssuedHandle<RenderMeshHandle>(
            Publish(record, RequireValue(runtime_.CreateMesh(mesh, Native(RenderResourceClass::Buffer, Identity(vertices_)),
                                                             Native(RenderResourceClass::Buffer, Identity(indices_))))));
        color_ = Texture({.extent = {64, 64},
                          .format = RenderTextureFormat::Rgba8Unorm,
                          .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource});
        depth_ = Texture({.extent = {64, 64}, .format = RenderTextureFormat::Depth32Float, .usage = RenderTextureUsage::RenderAttachment});
        atlas_ = Texture({.extent = {1, 1},
                          .format = RenderTextureFormat::Depth32Float,
                          .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled});
        colorView_ = View(color_, RenderTextureFormat::Rgba8Unorm, RenderTextureAspect::Color);
        depthView_ = View(depth_, RenderTextureFormat::Depth32Float, RenderTextureAspect::Depth);
        atlasView_ = View(atlas_, RenderTextureFormat::Depth32Float, RenderTextureAspect::Depth);
        litUniform_ =
            Buffer({.byteSize = programs_[0].uniformBytes, .usage = RenderBufferUsage::Uniform, .access = RenderBufferAccess::HostVisible});
        depthUniform_ =
            Buffer({.byteSize = programs_[1].uniformBytes, .usage = RenderBufferUsage::Uniform, .access = RenderBufferAccess::HostVisible});
        buffers_.lights =
            Buffer({.byteSize = sizeof(packedLights_), .usage = RenderBufferUsage::Storage, .access = RenderBufferAccess::HostVisible});
        buffers_.clusters =
            Buffer({.byteSize = sizeof(packedClusters_), .usage = RenderBufferUsage::Storage, .access = RenderBufferAccess::HostVisible});
        buffers_.membership = Buffer({.byteSize = 16 * sizeof(PackedLightMembership),
                                      .usage = RenderBufferUsage::Storage,
                                      .access = RenderBufferAccess::HostVisible});
        buffers_.references = Buffer(
            {.byteSize = 16 * 32 * sizeof(std::uint32_t), .usage = RenderBufferUsage::Storage, .access = RenderBufferAccess::HostVisible});
        readback_ = Buffer({.byteSize = 256, .usage = RenderBufferUsage::CopyDestination, .access = RenderBufferAccess::HostVisible});
    }

    NativeLightFrameUpdate MetalClusteredMaterialGraphFixture::UpdateRequest(const std::uint64_t revision) const {
        const std::array handles{buffers_.lights, buffers_.clusters, buffers_.membership, buffers_.references};
        NativeLightFrameUpdate result;
        for (std::size_t index = 0; index < handles.size(); ++index)
            result.instances[index] = Native(RenderResourceClass::Buffer, Identity(handles[index]));
        result.update = {.budget = budget_,
                         .revision = revision,
                         .lights = std::span{packedLights_}.first(count_),
                         .clusters = packedClusters_};
        return result;
    }

    void MetalClusteredMaterialGraphFixture::ConfigureSample(const ClusteredQualificationSample sample) {
        camera_.position.z = 3;
        pixelY_ = 40;
        switch (sample) {
            case ClusteredQualificationSample::BottomRight:
                pixelX_ = 40;
                expectedCells_ = {7, 7};
                break;
            case ClusteredQualificationSample::BottomLeft:
                pixelX_ = 24;
                expectedCells_ = {6, 6};
                break;
            case ClusteredQualificationSample::TopFar:
                pixelX_ = 32;
                pixelY_ = 24;
                camera_.position.z = 10;
                expectedCells_ = {8, 9};
                break;
            default:
                FAIL("Unknown authored clustered qualification sample");
        }
    }

    void MetalClusteredMaterialGraphFixture::Upload(const std::uint32_t count, const std::uint64_t revision,
                                                    const ClusteredQualificationSample sample) {
        REQUIRE(count <= lights_.size());
        if (committed_)
            RequireTerminal();
        ConfigureSample(sample);
        count_ = count;
        revision_ = revision;
        const float worldX = ((static_cast<float>(pixelX_) + 0.5F) / 64.0F * 2.0F - 1.0F) * 2.0F;
        const float worldY = -((static_cast<float>(pixelY_) + 0.5F) / 64.0F * 2.0F - 1.0F) * 2.0F;
        for (std::size_t index = 0; index < lights_.size(); ++index) {
            lights_[index] = {.kind = RenderLightKind::Point,
                              .position = {worldX, worldY, 0.3F},
                              .intensity = 0.2F,
                              .range = 0.4F,
                              .stableIdentity = index + 1};
            identified_[index] = {{lights_[index].stableIdentity}, lights_[index]};
        }
        const std::uint64_t gridGeneration = sample == ClusteredQualificationSample::TopFar ? 2 : 1;
        grid_ = RequireValue(
            BuildClusteredLightGrid({{view_, 382, revision, 1, gridGeneration}, camera_, {64, 64}, 2, 2, 4}, budget_, clusters_));
        // Authored spheres have >0.06 world-unit clearance from nonintersecting XY/Z splits.
        REQUIRE(
            CullLightsCpu(std::span{identified_}.first(count), clusters_, budget_, expectedMembership_, expectedReferences_).HasValue());
        const auto selected = RequireValue(
            MapClusteredLightViewDepth(grid_, static_cast<float>(pixelX_) + 0.5F, static_cast<float>(pixelY_) + 0.5F, camera_.position.z));
        REQUIRE(selected);
        REQUIRE(std::ranges::find(expectedCells_, *selected) != expectedCells_.end());
        REQUIRE(PackLightFrame(std::span{identified_}.first(count), clusters_, packedLights_, packedClusters_).HasValue());
        REQUIRE(runtime_.UpdateLightFrame(UpdateRequest(revision)).HasValue());
        WriteParameters(count);
        if (graph_) {
            auto &culling = std::get<RenderGraphLightCulling>(workloads_[0].workload);
            culling.dispatch = {count, 16, 32, 0};
            culling.tableRevision = revision;
        }
    }

    void MetalClusteredMaterialGraphFixture::WriteParameters(const std::uint32_t count) {
        std::array<std::byte, 4096> bytes{};
        REQUIRE(programs_[0].uniformBytes <= bytes.size());
        REQUIRE(programs_[1].uniformBytes <= bytes.size());
        Editor::EditorViewportClusteredShaderParameters parameters;
        const RenderSceneView scene{.camera = camera_,
                                    .lights = std::span{lights_}.first(count),
                                    .lightSceneDomain = 382,
                                    .lightSnapshotRevision = revision_};
        REQUIRE(parameters.SetScene(scene, grid_, std::nullopt, 32).HasValue());
        parameters.common.mvp = grid_.viewProjection;
        REQUIRE(parameters.PackLit(programs_[0].layout, bytes).HasValue());
        auto *lit = reinterpret_cast<MetalBufferInstance *>(Native(RenderResourceClass::Buffer, Identity(litUniform_)));
        std::memcpy(lit->buffer.contents, bytes.data(), programs_[0].uniformBytes);
        REQUIRE(Editor::PackEditorViewportShadow(programs_[1].layout, parameters.common.mvp, bytes).HasValue());
        auto *depth = reinterpret_cast<MetalBufferInstance *>(Native(RenderResourceClass::Buffer, Identity(depthUniform_)));
        std::memcpy(depth->buffer.contents, bytes.data(), programs_[1].uniformBytes);
    }

    void MetalClusteredMaterialGraphFixture::PrepareMaterials() {
        const MaterialBindingDescriptor
            lit{.material = {1},
                .revision = 1,
                .pipeline = programs_[0].pipeline,
                .layout = programs_[0].layout,
                .resources = {{{1}, 0, MaterialBufferBinding{litUniform_, 0, programs_[0].uniformBytes}},
                              {{2}, 0, atlasView_},
                              {{3}, 0, sampler_},
                              {{4}, 0, MaterialBufferBinding{buffers_.lights, 0, sizeof(packedLights_)}},
                              {{5}, 0, MaterialBufferBinding{buffers_.membership, 0, 16 * sizeof(PackedLightMembership)}},
                              {{6}, 0, MaterialBufferBinding{buffers_.references, 0, 16 * 32 * sizeof(std::uint32_t)}}}};
        litMaterial_ = RequireValue(materials_->Acquire(RequireValue(materials_->Publish(lit)).binding));
        const MaterialBindingDescriptor depth{.material = {2},
                                              .revision = 1,
                                              .pipeline = programs_[1].pipeline,
                                              .layout = programs_[1].layout,
                                              .resources = {{{1}, 0, MaterialBufferBinding{depthUniform_, 0, programs_[1].uniformBytes}}}};
        depthMaterial_ = RequireValue(materials_->Acquire(RequireValue(materials_->Publish(depth)).binding));
    }

    void MetalClusteredMaterialGraphFixture::PrepareGraph() {
        const auto queues = RequireValue(RenderFrontendResourceAccess::QueueAssignments(*frontend_));
        const auto graphics = std::ranges::find(queues, RenderQueueRole::Graphics, &RenderQueueAssignment::role);
        REQUIRE(graphics != queues.end());
        auto builder =
            RequireValue(RenderGraphBuilder::Create({.maxPasses = 4, .maxResources = 12, .maxUsages = 24, .maxDependencies = 4}));
        QualificationGraph author{builder, graphics->queue};
        const std::array handles{buffers_.lights, buffers_.clusters, buffers_.membership, buffers_.references};
        std::array<RenderGraphResourceId, 4> light;
        for (std::size_t index = 0; index < light.size(); ++index)
            light[index] = author.Buffer(handles[index], RenderGraphSynchronizationOperation::Storage, index < 2);
        const auto vb = author.Buffer(vertices_, RenderGraphSynchronizationOperation::GraphicsInputBuffer, true);
        const auto ib = author.Buffer(indices_, RenderGraphSynchronizationOperation::GraphicsInputBuffer, true);
        const auto litUniform = author.Buffer(litUniform_, RenderGraphSynchronizationOperation::GraphicsInputBuffer, true);
        const auto depthUniform = author.Buffer(depthUniform_, RenderGraphSynchronizationOperation::GraphicsInputBuffer, true);
        const auto color = author.Texture(color_), depth = author.Texture(depth_), atlas = author.Texture(atlas_);
        const auto pass = author.Passes();
        for (std::size_t index = 0; index < light.size(); ++index)
            author.Use(pass[0], light[index], index < 2 ? RenderGraphAccess::Read : RenderGraphAccess::Write,
                       RenderGraphUsageKind::Storage);
        workloads_[0] = {pass[0], RenderGraphLightCulling{light[0], light[1], light[2], light[3], {count_, 16, 32, 0}, revision_, kernel_}};
        author.Use(pass[1], atlas, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment);
        workloads_[1] = {pass[1], RenderGraphDepthAttachment{atlas, 1}};
        depthDraw_[0] = {mesh_, vb, ib, depthMaterial_};
        litDraw_[0] = {mesh_, vb, ib, litMaterial_};
        depthReads_[0] = {depthUniform, depthUniform_, {}};
        litReads_ = {{{litUniform, litUniform_, {}},
                      {atlas, {}, atlasView_},
                      {light[0], handles[0], {}},
                      {light[2], handles[2], {}},
                      {light[3], handles[3], {}}}};
        workloads_[2] = {pass[2], RenderGraphMaterialRaster{.depth = depth,
                                                            .depthView = depthView_,
                                                            .extent = {64, 64},
                                                            .depthClear = 1,
                                                            .draws = depthDraw_,
                                                            .reads = depthReads_,
                                                            .depthOnly = true}};
        workloads_[3] = {pass[3], RenderGraphMaterialRaster{.color = color,
                                                            .depth = depth,
                                                            .colorView = colorView_,
                                                            .depthView = depthView_,
                                                            .extent = {64, 64},
                                                            .depthClear = std::nullopt,
                                                            .draws = litDraw_,
                                                            .reads = litReads_}};
        for (const auto rasterPass : {2U, 3U}) {
            author.Use(pass[rasterPass], depth, rasterPass == 2 ? RenderGraphAccess::Write : RenderGraphAccess::ReadWrite,
                       RenderGraphUsageKind::DepthStencilAttachment);
            author.Use(pass[rasterPass], vb, RenderGraphAccess::Read, RenderGraphUsageKind::VertexBuffer);
            author.Use(pass[rasterPass], ib, RenderGraphAccess::Read, RenderGraphUsageKind::IndexBuffer);
            author.Use(pass[rasterPass], rasterPass == 2 ? depthUniform : litUniform, RenderGraphAccess::Read,
                       RenderGraphUsageKind::UniformBuffer);
        }
        author.Use(pass[3], color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment);
        author.Use(pass[3], atlas, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled);
        for (const auto index : {0U, 2U, 3U})
            author.Use(pass[3], light[index], RenderGraphAccess::Read, RenderGraphUsageKind::Storage);
        const auto graph = RequireValue(builder.Finalize());
        const auto schedule = RequireValue(CompileRenderGraph(graph));
        const auto synchronization = RequireValue(SynthesizeRenderGraphSynchronization(graph, schedule, queues, author.initial));
        graph_.emplace(RequireValue(CompileRenderGraphExecution(graph, schedule, synchronization, queues)));
        ResolveGraphInstances();
    }

    void MetalClusteredMaterialGraphFixture::ResolveGraphInstances() {
        for (const auto &resource : graph_->Resources()) {
            if (const auto *buffer = std::get_if<RenderBufferHandle>(&resource.binding))
                resources_.push_back({resource.id, Native(RenderResourceClass::Buffer, Identity(*buffer))});
            else
                resources_.push_back(
                    {resource.id, Native(RenderResourceClass::Texture, Identity(std::get<RenderTextureHandle>(resource.binding)))});
        }
        meshes_[0] = {mesh_, Native(RenderResourceClass::Mesh, Identity(mesh_))};
        const std::array viewHandles{colorView_, depthView_, atlasView_};
        for (std::size_t index = 0; index < views_.size(); ++index)
            views_[index] = {viewHandles[index], Native(RenderResourceClass::TextureView, Identity(viewHandles[index]))};
    }

    void MetalClusteredMaterialGraphFixture::Execute(const bool commit) {
        id<MTLDevice> device = (__bridge id<MTLDevice>)bridge_.Device();
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)bridge_.CommandQueue();
        gate_ = [device newSharedEvent];
        REQUIRE(gate_);
        commands_ = [queue commandBuffer];
        REQUIRE(commands_);
        [commands_ encodeWaitForEvent:gate_ value:1];
        // This is explicitly private native qualification: no public frontend frame/admission is claimed.
        const RenderGraphExecutionRequest request{.frame = {},
                                                  .graph = *graph_,
                                                  .workloads = workloads_,
                                                  .resources = resources_,
                                                  .meshes = meshes_,
                                                  .views = views_};
        CHECK_FALSE(request.frame.IsValid());
        for (std::size_t index = 0; index < workloads_.size(); ++index) {
            const auto &workload = workloads_[index].workload;
            if (const auto *raster = std::get_if<RenderGraphMaterialRaster>(&workload)) {
                INFO("Private graph pass " << index << " depthOnly=" << raster->depthOnly << " extent=" << raster->extent.width << "x"
                                           << raster->extent.height << " color=" << raster->color.value << " depth=" << raster->depth.value
                                           << " draws=" << raster->draws.size() << " reads=" << raster->reads.size());
                RequireSuccess(ValidateMaterialRasterWorkload(request, graph_->Passes()[index], *raster));
                RequireSuccess(runtime_.ValidateMaterialRaster(*raster, request));
                RequireSuccess(runtime_.ExecuteMaterialRaster((__bridge void *)commands_, *raster, request));
            } else {
                INFO("Private graph pass " << index << " workload=" << workload.index());
                RequireSuccess(runtime_.ValidateGraphWorkload(workload, resources_));
                RequireSuccess(runtime_.ExecuteGraphWorkload((__bridge void *)commands_, workload, resources_));
            }
        }
        const auto pending = runtime_.UpdateLightFrame(UpdateRequest(revision_ + 1));
        REQUIRE(pending.HasError());
        CHECK(pending.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value());
        CheckAttachmentJournal(true);
        committed_ = false;
        if (commit) {
            [commands_ commit];
            committed_ = true;
            runtime_.FinishGraphCommands((__bridge void *)commands_, true);
            const auto submitted = runtime_.UpdateLightFrame(UpdateRequest(revision_ + 1));
            REQUIRE(submitted.HasError());
            CHECK(submitted.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value());
            gate_.signaledValue = 1;
        } else {
            runtime_.FinishGraphCommands((__bridge void *)commands_, false);
            CheckAttachmentJournal(false);
            commands_ = nil;
        }
    }

    void MetalClusteredMaterialGraphFixture::CheckAttachmentJournal(const bool recorded) {
        for (const auto handle : {color_, depth_, atlas_}) {
            const auto *texture = reinterpret_cast<MetalTextureInstance *>(Native(RenderResourceClass::Texture, Identity(handle)));
            CHECK((texture->use.last == commands_) == recorded);
        }
    }

    void MetalClusteredMaterialGraphFixture::RequireTerminal() {
        REQUIRE(AwaitTerminal(commands_));
        REQUIRE(commands_.status == MTLCommandBufferStatusCompleted);
    }

    void MetalClusteredMaterialGraphFixture::ReadPixel() {
        auto *texture = reinterpret_cast<MetalTextureInstance *>(Native(RenderResourceClass::Texture, Identity(color_)));
        auto *buffer = reinterpret_cast<MetalBufferInstance *>(Native(RenderResourceClass::Buffer, Identity(readback_)));
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)bridge_.CommandQueue();
        id<MTLCommandBuffer> copy = [queue commandBuffer];
        // Real builder-issued identities describe this explicit test-only native readback journal.
        auto readback = RequireValue(RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 2, .maxUsages = 1, .maxDependencies = 1}));
        const auto source = RequireValue(readback.ImportTexture(color_, RenderGraphResourceClass::Persistent));
        const auto destination = RequireValue(readback.ImportBuffer(readback_, RenderGraphResourceClass::Persistent));
        const std::array uses{RenderGraphResourceInstance{source, Native(RenderResourceClass::Texture, Identity(color_))},
                              RenderGraphResourceInstance{destination, Native(RenderResourceClass::Buffer, Identity(readback_))}};
        runtime_.TrackParallelGraphUse((__bridge void *)copy, uses);
        commands_ = copy;
        committed_ = false;
        id<MTLBlitCommandEncoder> blit = [copy blitCommandEncoder];
        [blit copyFromTexture:texture->texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(pixelX_, pixelY_, 0)
                          sourceSize:MTLSizeMake(1, 1, 1)
                            toBuffer:buffer->buffer
                   destinationOffset:0
              destinationBytesPerRow:256
            destinationBytesPerImage:256];
        [blit endEncoding];
        [copy commit];
        committed_ = true;
        runtime_.FinishGraphCommands((__bridge void *)copy, true);
        REQUIRE(AwaitTerminal(copy));
        REQUIRE(copy.status == MTLCommandBufferStatusCompleted);
        const auto *pixel = static_cast<const std::uint8_t *>(buffer->buffer.contents);
        CHECK(pixel[0] > 20);
        if (count_ == 0)
            CHECK(pixel[0] < 100);
        else {
            // Sixteen contributions cannot exceed 164 here; all 32 produce approximately 190.
            CHECK(pixel[0] > 180);
        }
        CHECK(pixel[3] == 255);
    }

    void MetalClusteredMaterialGraphFixture::CheckCompleted(const std::uint32_t count) {
        RequireTerminal();
        CHECK(frontend_->Capabilities().supportsMaterialRasterGraph == productionGraphCapability_);
        CHECK(frontend_->Capabilities().supportsMaterialPassBatches == productionBatchCapability_);
        const auto *membership =
            reinterpret_cast<MetalBufferInstance *>(Native(RenderResourceClass::Buffer, Identity(buffers_.membership)));
        const auto *references =
            reinterpret_cast<MetalBufferInstance *>(Native(RenderResourceClass::Buffer, Identity(buffers_.references)));
        const auto *records = static_cast<const PackedLightMembership *>(membership->buffer.contents);
        const auto *indices = static_cast<const std::uint32_t *>(references->buffer.contents);
        for (std::size_t cell = 0; cell < clusters_.size(); ++cell) {
            const auto spatialCount = std::ranges::find(expectedCells_, cell) != expectedCells_.end() ? count : 0;
            REQUIRE(expectedMembership_[cell].count == spatialCount);
            REQUIRE(records[cell].offset == expectedMembership_[cell].offset);
            REQUIRE(records[cell].count == expectedMembership_[cell].count);
            CHECK(records[cell].omitted == 0);
            CHECK(records[cell].reserved == 0);
            for (std::uint32_t source = 0; source < records[cell].count; ++source)
                CHECK(indices[records[cell].offset + source] == expectedReferences_[expectedMembership_[cell].offset + source]);
        }
        ReadPixel();
    }

    void MetalClusteredMaterialGraphFixture::CheckUniformAbi(const Editor::EditorClusteredLightingAssets &assets) {
        const auto *vertex =
            reinterpret_cast<MetalShaderModule *>(Native(RenderResourceClass::ShaderModule, Identity(programs_[0].vertex)));
        const auto *fragment =
            reinterpret_cast<MetalShaderModule *>(Native(RenderResourceClass::ShaderModule, Identity(programs_[0].fragment)));
        const auto &layout = programs_[0].layout;
        const auto mapping = std::ranges::find(layout.targetBindings, ShaderBindingId{1}, &ShaderTargetBindingMapEntry::id);
        REQUIRE(mapping != layout.targetBindings.end());
        const auto buffer = QualificationUniformReflection((__bridge id<MTLDevice>)bridge_.Device(), vertex->function, fragment->function,
                                                           mapping->nativeBinding);
        REQUIRE(buffer.bufferStructType);
        REQUIRE(buffer.bufferDataSize == programs_[0].uniformBytes);
        const auto &reflection = assets.programs[0].artifacts.front().reflection->reflection;
        for (const auto &parameter : reflection.parameters) {
            if (parameter.binding != ShaderBindingId{1} || !parameter.active)
                continue;
            INFO("Actual uniform parameter " << parameter.id.value);
            REQUIRE(HasMetalUniformParameter(buffer.bufferStructType, parameter));
            REQUIRE(MetalUniformParameterFits(parameter, buffer.bufferDataSize));
        }
        CheckUniformMatrixMutations(buffer.bufferStructType, QualificationParameter(reflection, 1));
        CheckUniformArrayMutations(buffer.bufferStructType, QualificationParameter(reflection, 3));
        CheckUniformArrayMutations(buffer.bufferStructType, QualificationParameter(reflection, 12));
        auto outside = QualificationParameter(reflection, 1);
        outside.byteOffset = static_cast<std::uint32_t>(buffer.bufferDataSize);
        REQUIRE_FALSE(MetalUniformParameterFits(outside, buffer.bufferDataSize));
    }

    void MetalClusteredMaterialGraphFixture::CheckLastGoodMaterial() {
        const auto original = litMaterial_->Id();
        const auto retained = materials_->Snapshot().retainedGenerations;
        auto invalid = litMaterial_->Descriptor();
        ++invalid.revision;
        std::get<MaterialBufferBinding>(invalid.resources.front().value).offset = 1;
        REQUIRE(materials_->Publish(std::move(invalid)).HasError());
        const auto prior = RequireValue(materials_->Acquire(original));
        CHECK(prior.get() == litMaterial_.get());
        CHECK(materials_->Snapshot().retainedGenerations == retained);
    }

    void MetalClusteredMaterialGraphFixture::ReleaseOwners() noexcept {
        graph_.reset();
        for (auto &workload : workloads_)
            workload.workload = std::monostate{};
        litDraw_[0].material.reset();
        depthDraw_[0].material.reset();
        litMaterial_.reset();
        depthMaterial_.reset();
        materials_.reset();
        kernel_.reset();
        if (registry_) {
            registry_->Shutdown(BackendResourceReleaseMode::DestroyNative);
            runtime_.DrainGraphRetirements();
            registry_->DrainRetirements();
            registry_.reset();
        }
        runtime_.Shutdown();
        if (memory_)
            memory_->Shutdown();
        memory_.reset();
        commands_ = nil;
        gate_ = nil;
        closed_ = true;
    }

    void MetalClusteredMaterialGraphFixture::Close() {
        if (committed_)
            RequireTerminal();
        ReleaseOwners();
        RequireDrainedOwner(*frontend_);
        frontend_.reset();
    }

    MetalClusteredMaterialGraphFixture::~MetalClusteredMaterialGraphFixture() {
        if (closed_)
            return;
        gate_.signaledValue = 1;
        if (committed_ && !AwaitTerminal(commands_))
            std::terminate();
        if (commands_ && !committed_)
            runtime_.FinishGraphCommands((__bridge void *)commands_, false);
        ReleaseOwners();
        if (frontend_ && !DrainQualificationFrontend(*frontend_))
            std::terminate();
    }

    TEST_CASE("Configured Metal private runtime qualifies actual clustered compute to material raster before advertisement",
              "[integration][renderer][metal][lighting][gpu][configured-artifact][private-runtime]") {
        const char *root = std::getenv("HORO_CLUSTERED_LIGHTING_ASSET_ROOT");
        if (!root)
            SKIP("Missing genuine approved-tool bundle; this private-runtime test claims no frontend capability admission.");
        const auto assets = RequireValue(Editor::LoadPackagedMetalClusteredLighting(root));
        MetalClusteredMaterialGraphFixture fixture;
        fixture.Initialize(assets);
        fixture.CheckUniformAbi(assets);
        fixture.Execute(false);
        fixture.Upload(0, 2);
        fixture.Execute(true);
        fixture.CheckCompleted(0);
        fixture.Upload(32, 3);
        fixture.CheckLastGoodMaterial();
        fixture.Execute(true);
        fixture.CheckCompleted(32);
        fixture.Upload(32, 4, ClusteredQualificationSample::BottomLeft);
        fixture.Execute(true);
        fixture.CheckCompleted(32);
        fixture.Upload(32, 5, ClusteredQualificationSample::TopFar);
        fixture.Execute(true);
        fixture.CheckCompleted(32);
        fixture.Close();
    }
}  // namespace Horo::Render::Test
