#include "EditorViewportRendererVulkan.h"
#include "EditorViewportVulkanCanvasProjection.h"
#include "RenderFrontendResourceAccess.h"
#include "editor/renderer/EditorRendererErrors.h"

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::CanvasQuadProgram::CanvasQuadProgram */
    EditorViewportRendererVulkan::CanvasQuadProgram::CanvasQuadProgram(EditorViewportRendererVulkan &viewport, std::uint32_t views)
        : owner(viewport), viewCount(views) {}

    /** @copydoc EditorViewportRendererVulkan::CanvasQuadProgram::~CanvasQuadProgram */
    EditorViewportRendererVulkan::CanvasQuadProgram::~CanvasQuadProgram() {
        for (auto &slot : slots)
            for (auto &view : std::span{slot}.first(viewCount))
                owner.RetireDraw(view.uniforms);
        owner.RetireProgram(shader, {});
        for (const auto view : sampledViews)
            if (view.IsValid())
                static_cast<void>(owner.frontend_.ReleaseTextureView(view));
        if (sampler.IsValid())
            static_cast<void>(owner.frontend_.ReleaseSampler(sampler));
        if (mesh.IsValid())
            static_cast<void>(owner.frontend_.ReleaseMesh(mesh));
        for (const auto buffer : {vertices, indices})
            if (buffer.IsValid())
                static_cast<void>(owner.frontend_.ReleaseBuffer(buffer));
    }

    /** @copydoc EditorViewportRendererVulkan::CreateCanvasQuadProgram */
    Result<std::unique_ptr<EditorViewportRendererVulkan::CanvasQuadProgram>> EditorViewportRendererVulkan::CreateCanvasQuadProgram(
        const EditorVulkanWorldCanvasCook &cook, const Render::UiRenderCompositionSpace space,
        const Render::RenderTextureFormat colorFormat, const std::uint32_t viewCount) {
        using namespace Render;
        using Outcome = Result<std::unique_ptr<CanvasQuadProgram>>;
        const bool world = space == UiRenderCompositionSpace::World;
        if (!initialized_ || !frontend_.Capabilities().supportsMaterialRasterGraph || (!world && space != UiRenderCompositionSpace::Camera))
            return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (viewCount == 0 || viewCount > CanvasQuadProgram::StereoViews || (viewCount > 1 && !world))
            return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (const auto boundary = Detail::RenderFrontendResourceAccess::RequirePreparationBoundary(frontend_); boundary.HasError())
            return Outcome::Failure(boundary.ErrorValue());
        const auto metadata = EncodeReflectedShaderArtifact(cook.metadata);
        if (metadata.HasError())
            return Outcome::Failure(metadata.ErrorValue());
        if (const auto admitted = LoadEditorVulkanWorldCanvasCook(cook.payload, metadata.Value()); admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());

        auto program = std::make_unique<CanvasQuadProgram>(*this, viewCount);
        auto &shader = program->shader;
        program->space = space;
        shader.payload = cook.payload;
        shader.metadata = cook.metadata;
        if (const auto stages = ProgramResources::CreateProgramStages(frontend_, shader); stages.HasError())
            return Outcome::Failure(stages.ErrorValue());
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None,
                                                .blend = RenderBlendMode::PremultipliedAlpha,
                                                .depthTest = world,
                                                .depthWrite = false,
                                                .colorFormat = colorFormat,
                                                .depthFormat = world ? std::optional{RenderTextureFormat::Depth32Float} : std::nullopt};
        const auto pipeline = ProgramResources::CreatePipelineState(frontend_, shader, EditorVulkanWorldCanvasShaderManifest(), state);
        if (pipeline.HasError())
            return Outcome::Failure(pipeline.ErrorValue());
        shader.pipeline = pipeline.Value();
        const auto sampler = frontend_.CreateSampler({});
        if (sampler.HasError())
            return Outcome::Failure(sampler.ErrorValue());
        program->sampler = sampler.Value().handle;
        if (const auto layout = ProgramResources::CreateMaterialLayout(frontend_, shader, shader.metadata.reflection, device_.frames);
            layout.HasError())
            return Outcome::Failure(layout.ErrorValue());
        if (const auto mesh = CreateCanvasQuadMesh(*program); mesh.HasError())
            return Outcome::Failure(mesh.ErrorValue());
        if (const auto ready = frontend_.ResourceOperationResult(sampler.Value().operation); ready.HasError())
            return Outcome::Failure(ready.ErrorValue());
        return Outcome::Success(std::move(program));
    }

    /** @copydoc EditorViewportRendererVulkan::CreateCanvasQuadMesh */
    Result<void> EditorViewportRendererVulkan::CreateCanvasQuadMesh(CanvasQuadProgram &program) {
        using namespace Render;
        constexpr Math::Vec3 normal{0, 0, 1};
        constexpr std::array vertices{MeshVertex{{-0.5F, 0.5F, 0}, normal, {0, 0}}, MeshVertex{{0.5F, 0.5F, 0}, normal, {1, 0}},
                                      MeshVertex{{-0.5F, -0.5F, 0}, normal, {0, 1}}, MeshVertex{{0.5F, -0.5F, 0}, normal, {1, 1}}};
        constexpr std::array<std::uint32_t, 6> indices{0, 2, 1, 1, 2, 3};
        const auto vertex = frontend_.CreateBuffer({sizeof(vertices), RenderBufferUsage::Vertex, RenderBufferAccess::HostVisible},
                                                   std::as_bytes(std::span{vertices}));
        if (vertex.HasError())
            return Result<void>::Failure(vertex.ErrorValue());
        program.vertices = vertex.Value().handle;
        const auto index = frontend_.CreateBuffer({sizeof(indices), RenderBufferUsage::Index, RenderBufferAccess::HostVisible},
                                                  std::as_bytes(std::span{indices}));
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        program.indices = index.Value().handle;
        if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        const auto mesh = frontend_.CreateMesh({.vertexBuffer = program.vertices,
                                                .indexBuffer = program.indices,
                                                .vertexStride = sizeof(MeshVertex),
                                                .vertexCount = vertices.size(),
                                                .indexCount = indices.size(),
                                                .localBounds = {{-0.5F, -0.5F, 0}, {0.5F, 0.5F, 0}}});
        if (mesh.HasError())
            return Result<void>::Failure(mesh.ErrorValue());
        program.mesh = mesh.Value().handle;
        if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        return frontend_.ResourceOperationResult(mesh.Value().operation);
    }

    /** @copydoc EditorViewportRendererVulkan::BindCanvasQuadSource */
    Result<void> EditorViewportRendererVulkan::BindCanvasQuadSource(CanvasQuadProgram &program, const std::uint32_t slot,
                                                                    const Render::RenderTextureHandle canvas) {
        using namespace Render;
        if (&program.owner != this || slot >= device_.frames || program.sampledViews[slot].IsValid())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        const auto sampled = frontend_.CreateTextureView({.texture = canvas, .format = RenderTextureFormat::Rgba16Float});
        if (sampled.HasError())
            return Result<void>::Failure(sampled.ErrorValue());
        const auto view = sampled.Value().handle;
        program.sampledViews[slot] = view;
        if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        if (const auto ready = frontend_.ResourceOperationResult(sampled.Value().operation); ready.HasError())
            return ready;
        const std::array inputs{MaterialResourceBinding{{2}, 0, view}, MaterialResourceBinding{{3}, 0, program.sampler}};
        for (std::uint32_t eye = 0; eye < program.viewCount; ++eye) {
            auto &quad = program.slots[slot][eye];
            quad.mesh = program.mesh;
            quad.vertices = program.vertices;
            quad.indices = program.indices;
            const auto identity = slot * program.viewCount + eye + 1;
            if (auto created = ProgramResources::CreateUniformSlot(frontend_, program.shader, quad.uniforms, identity,
                                                                   program.shader.pipeline, {}, inputs);
                created.HasError())
                return created;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::WriteCanvasQuad */
    Result<void> EditorViewportRendererVulkan::WriteCanvasQuad(const Render::FrameToken frame, const CanvasQuadProgram &program,
                                                               const std::uint32_t slot, const Render::StaticMeshPassDescriptor &pass,
                                                               const std::optional<Math::Mat4> &placement) const {
        const auto mvp = BuildVulkanEditorCanvasMvp(program.space, placement, pass.scene.camera, pass.extent);
        if (mvp.HasError())
            return Result<void>::Failure(mvp.ErrorValue());
        return WriteCanvasQuadViews(frame, program, slot, std::span{&mvp.Value(), 1});
    }

    /** @copydoc EditorViewportRendererVulkan::WriteCanvasQuadViews */
    Result<void> EditorViewportRendererVulkan::WriteCanvasQuadViews(const Render::FrameToken frame, const CanvasQuadProgram &program,
                                                                    const std::uint32_t slot,
                                                                    const std::span<const Math::Mat4> clips) const {
        if (&program.owner != this || slot >= device_.frames || program.viewCount != clips.size())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        for (std::size_t view = 0; view < clips.size(); ++view) {
            const auto written = WriteCanvasQuadClip(frame, program.shader, program.slots[slot][view].uniforms, clips[view]);
            if (written.HasError())
                return written;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::WriteCanvasQuadClip */
    Result<void> EditorViewportRendererVulkan::WriteCanvasQuadClip(const Render::FrameToken frame, const ShaderResources &program,
                                                                   const DrawSlot &draw, const Math::Mat4 &clip) const {
        if (!draw.material || !Math::IsFinite(clip))
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        std::array<std::byte, MaximumUniformBytes> storage{};
        const auto bytes = std::span{storage}.first(program.uniformBytes);
        const auto columns = std::span{clip.values};
        for (std::uint32_t column = 0; column < 4; ++column) {
            const auto written = ProgramResources::WriteParameter(program.layout, bytes, {column + 1}, columns.subspan(column * 4, 4));
            if (written.HasError())
                return written;
        }
        return bridge_.Port()->WriteFrameBuffer(frame, draw.instance, bytes);
    }
}  // namespace Horo::Editor
