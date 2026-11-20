#include "EditorViewportRendererVulkan.h"
#include "RenderFrontendResourceAccess.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <limits>

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::ProgramResources::CreatePipeline */
    Result<void> EditorViewportRendererVulkan::ProgramResources::CreatePipeline(Render::RenderFrontend &frontend, ShaderResources &program,
                                                                                const std::uint32_t frames, bool displayLinearOverlay) {
        using namespace Render;
        const auto encoded = EncodeReflectedShaderArtifact(program.metadata);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        const auto checked = LoadEditorVulkanViewportCook(program.payload, encoded.Value());
        if (checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        if (auto stages = CreateProgramStages(frontend, program); stages.HasError())
            return stages;
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None, .depthFormat = RenderTextureFormat::Depth32Float};
        const auto pipeline = CreatePipelineState(frontend, program, EditorVulkanViewportShaderManifest(), state);
        if (pipeline.HasError())
            return Result<void>::Failure(pipeline.ErrorValue());
        program.pipeline = pipeline.Value();
        auto overlayState = state;
        overlayState.depthWrite = false;
        if (displayLinearOverlay)
            overlayState.colorFormat = RenderTextureFormat::Rgba16Float;
        const auto overlay = CreatePipelineState(frontend, program, EditorVulkanViewportShaderManifest(), overlayState);
        if (overlay.HasError())
            return Result<void>::Failure(overlay.ErrorValue());
        program.overlayPipeline = overlay.Value();
        auto sampler = frontend.CreateSampler({});
        if (sampler.HasError())
            return Result<void>::Failure(sampler.ErrorValue());
        program.shadowSampler = sampler.Value().handle;
        return CreateMaterialLayout(frontend, program, program.metadata.reflection, frames);
    }

    /** @copydoc EditorViewportRendererVulkan::ProgramResources::CreateSceneLinearPipeline */
    Result<void> EditorViewportRendererVulkan::ProgramResources::CreateSceneLinearPipeline(Render::RenderFrontend &frontend,
                                                                                           ShaderResources &program,
                                                                                           const std::uint32_t frames) {
        using namespace Render;
        const auto metadata = EncodeReflectedShaderArtifact(program.metadata);
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        const auto checked = LoadEditorVulkanSceneLinearCook(program.payload, metadata.Value());
        if (checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        if (const auto stages = CreateProgramStages(frontend, program, "PSSceneLinear"); stages.HasError())
            return stages;
        const auto pipeline =
            CreatePipelineState(frontend, program, EditorVulkanSceneLinearShaderManifest(), EditorVulkanSceneLinearPipelineState());
        if (pipeline.HasError())
            return Result<void>::Failure(pipeline.ErrorValue());
        program.pipeline = pipeline.Value();
        const auto sampler = frontend.CreateSampler({});
        if (sampler.HasError())
            return Result<void>::Failure(sampler.ErrorValue());
        program.shadowSampler = sampler.Value().handle;
        return CreateMaterialLayout(frontend, program, program.metadata.reflection, frames);
    }

    /** @copydoc EditorViewportRendererVulkan::ProgramResources::CreateProgramStages */
    Result<void> EditorViewportRendererVulkan::ProgramResources::CreateProgramStages(Render::RenderFrontend &frontend,
                                                                                     ShaderResources &program, const char *fragmentEntry,
                                                                                     const char *vertexEntry) {
        using namespace Render;
        const auto target = EditorVulkanViewportShaderTarget();
        const auto &reflection = program.metadata.reflection;
        const auto stage = [&](const ShaderStage kind, const char *entry) {
            return frontend.CreateShaderModule({target, kind, entry, program.metadata.cookedArtifactKey, reflection.interfaceCompatibility},
                                               program.payload);
        };
        const auto vertex = stage(ShaderStage::Vertex, vertexEntry);
        if (vertex.HasError())
            return Result<void>::Failure(vertex.ErrorValue());
        program.vertex = vertex.Value().handle;
        const auto fragment = stage(ShaderStage::Fragment, fragmentEntry);
        if (fragment.HasError())
            return Result<void>::Failure(fragment.ErrorValue());
        program.fragment = fragment.Value().handle;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ProgramResources::CreatePipelineState */
    Result<Render::RenderPipelineHandle> EditorViewportRendererVulkan::ProgramResources::CreatePipelineState(
        Render::RenderFrontend &frontend, const ShaderResources &program, const Render::ShaderManifest &manifest,
        const Render::RenderGraphicsPipelineState &state) {
        using namespace Render;
        const auto target = EditorVulkanViewportShaderTarget();
        const auto &reflection = program.metadata.reflection;
        const auto device = frontend.QueryPipelineDeviceIdentity();
        if (device.HasError())
            return Result<RenderPipelineHandle>::Failure(device.ErrorValue());
        const auto stateKey = ComputeRenderGraphicsPipelineStateDigest(state);
        if (stateKey.HasError())
            return Result<RenderPipelineHandle>::Failure(stateKey.ErrorValue());
        const auto &identity = device.Value();
        const PipelineCacheCompatibility compatibility{1,
                                                       identity.backend,
                                                       identity.backendModuleVersion,
                                                       identity.adapter,
                                                       identity.deviceCompatibilityDigest,
                                                       identity.driverIdentity,
                                                       identity.driverVersion,
                                                       target.backend,
                                                       target.payloadFormat,
                                                       program.metadata.cookedArtifactKey,
                                                       reflection.interfaceCompatibility,
                                                       stateKey.Value()};
        const auto pipeline =
            frontend.CreateGraphicsPipeline({program.vertex, program.fragment, manifest, target, reflection, state, compatibility});
        if (pipeline.HasError())
            return Result<RenderPipelineHandle>::Failure(pipeline.ErrorValue());
        return Result<RenderPipelineHandle>::Success(pipeline.Value().handle);
    }

    /** @copydoc EditorViewportRendererVulkan::ProgramResources::CreateMaterialLayout */
    Result<void> EditorViewportRendererVulkan::ProgramResources::CreateMaterialLayout(Render::RenderFrontend &frontend,
                                                                                      ShaderResources &program,
                                                                                      const Render::NormalizedShaderReflection &reflection,
                                                                                      const std::uint32_t frames) {
        using namespace Render;
        const auto layout = PrepareMaterialBindingLayout(reflection);
        if (layout.HasError())
            return Result<void>::Failure(layout.ErrorValue());
        program.layout = layout.Value();
        const auto bytes = ComputeMaterialParameterBufferByteSize(program.layout, {1});
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        if (bytes.Value() == 0 || bytes.Value() > MaximumUniformBytes)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        const auto nativeBytes = Render::Detail::RenderFrontendResourceAccess::MaterialParameterBufferBytes(frontend,
                                                                                                            {.pipeline = program.pipeline,
                                                                                                             .layout = program.layout},
                                                                                                            {1});
        if (nativeBytes.HasError())
            return Result<void>::Failure(nativeBytes.ErrorValue());
        if (nativeBytes.Value() < bytes.Value() || nativeBytes.Value() > MaximumUniformBytes)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        program.parameterBytes = bytes.Value();
        program.uniformBytes = nativeBytes.Value();
        auto materials = MaterialBindingTable::Create(program.pipeline.owner, frontend,
                                                      {.maximumGenerations = frames * (RenderMaterialPass::MaximumDraws + 1) * 8});
        if (materials.HasError())
            return Result<void>::Failure(materials.ErrorValue());
        program.materials = std::move(materials).Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ProgramResources::CreateDrawSlots */
    Result<void> EditorViewportRendererVulkan::ProgramResources::CreateDrawSlots(Render::RenderFrontend &frontend,
                                                                                 const ShaderResources &program, std::span<FrameSlot> slots,
                                                                                 const std::uint32_t frames,
                                                                                 const Render::RenderTextureViewHandle atlas) {
        for (std::uint32_t slot = 0; slot < frames; ++slot)
            for (std::size_t index = 0; index < Render::RenderMaterialPass::MaximumDraws; ++index) {
                const auto created =
                    CreateUniformSlot(frontend, program, slots[slot].draws[index],
                                      slot * (Render::RenderMaterialPass::MaximumDraws + 1) + index + 1, program.pipeline, atlas);
                if (created.HasError())
                    return created;
            }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::ProgramResources::CreateUniformSlot */
    Result<void> EditorViewportRendererVulkan::ProgramResources::CreateUniformSlot(
        Render::RenderFrontend &frontend, const ShaderResources &program, DrawSlot &draw, const std::uint64_t material,
        const Render::RenderPipelineHandle pipeline, const Render::RenderTextureViewHandle atlas,
        std::span<const Render::MaterialResourceBinding> sampled) {
        using namespace Render;
        const std::array<std::byte, MaximumUniformBytes> zero{};
        const auto buffer = frontend.CreateBuffer(RenderMemoryScopes::ViewportResources,
                                                  {program.uniformBytes, RenderBufferUsage::Uniform, RenderBufferAccess::HostVisible},
                                                  std::span{zero}.first(program.uniformBytes));
        if (buffer.HasError())
            return Result<void>::Failure(buffer.ErrorValue());
        draw.buffer = buffer.Value().handle;
        if (const auto processed = frontend.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        if (const auto ready = frontend.ResourceOperationResult(buffer.Value().operation); ready.HasError())
            return ready;
        const auto native = Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, draw.buffer);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        draw.instance = native.Value();
        draw.materialIdentity = material;
        if (program.shadowSampler.IsValid() && !atlas.IsValid())
            return Result<void>::Success();
        return BindUniformSlot(program, draw, pipeline, atlas, sampled);
    }

    Result<void> EditorViewportRendererVulkan::ProgramResources::BindUniformSlot(const ShaderResources &program, DrawSlot &draw,
                                                                                 Render::RenderPipelineHandle pipeline,
                                                                                 Render::RenderTextureViewHandle atlas,
                                                                                 std::span<const Render::MaterialResourceBinding> sampled) {
        using namespace Render;
        std::vector<MaterialResourceBinding> resources{{{1}, 0, MaterialBufferBinding{draw.buffer, 0, program.uniformBytes}}};
        if (program.shadowSampler.IsValid()) {
            resources.push_back({{2}, 0, atlas});
            resources.push_back({{3}, 0, program.shadowSampler});
        }
        resources.insert(resources.end(), sampled.begin(), sampled.end());
        const auto published = program.materials->Publish({.material = {draw.materialIdentity},
                                                           .revision = ++draw.materialRevision,
                                                           .pipeline = pipeline,
                                                           .layout = program.layout,
                                                           .resources = std::move(resources)});
        if (published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        const auto acquired = program.materials->Acquire(published.Value().binding);
        if (acquired.HasError())
            return Result<void>::Failure(acquired.ErrorValue());
        if (draw.material)
            static_cast<void>(program.materials->Release(draw.material->Id()));
        draw.material = acquired.Value();
        return Result<void>::Success();
    }

    /** @copydoc IEditorViewportRenderer::EnableCameraCrossFade */
    Result<void> EditorViewportRendererVulkan::EnableCameraCrossFade() {
        if (crossFadeSourceSlots_)
            return Result<void>::Success();
        const auto &profile = resources_.RequestedDirectionalShadowProfile();
        const auto shadows = profile.enabled ? profile.cascadeCount : 0;
        if (!initialized_ || !crossFadeCook_ || color_ || recordedEver_ || !frontend_.Capabilities().supportsMaterialRasterGraph ||
            shadows > CrossFadeShadowPasses || crossFadeGeneration_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportCrossFadeUnsupported));
        if (const auto boundary = Render::Detail::RenderFrontendResourceAccess::RequirePreparationBoundary(frontend_); boundary.HasError())
            return Result<void>::Failure(boundary.ErrorValue());
        CameraCrossFadeProgram program;
        program.shader.payload = crossFadeCook_->payload;
        program.shader.metadata = crossFadeCook_->metadata;
        auto slots = std::make_unique<FrameSlots>();
        if (const auto prepared = PrepareCameraCrossFadeProgram(program); prepared.HasError()) {
            RetireCameraCrossFadeProgram(program);
            return prepared;
        }
        if (const auto enabled = resources_.EnableCameraCrossFade(EditorViewportResources::CameraCrossFadeOutput::CopySource);
            enabled.HasError()) {
            RetireCameraCrossFadeProgram(program);
            return enabled;
        }
        crossFadeProgram_ = std::move(program);
        crossFadeSourceSlots_ = std::move(slots);
        crossFadeShadowBudget_ = shadows;
        ++crossFadeGeneration_;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareCameraCrossFadeProgram */
    Result<void> EditorViewportRendererVulkan::PrepareCameraCrossFadeProgram(CameraCrossFadeProgram &program) {
        using namespace Render;
        auto &shader = program.shader;
        const auto encoded = EncodeReflectedShaderArtifact(shader.metadata);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        if (const auto checked = LoadEditorVulkanCameraCrossFadeCook(shader.payload, encoded.Value()); checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        if (const auto stages = ProgramResources::CreateProgramStages(frontend_, shader); stages.HasError())
            return stages;
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None,
                                                .blend = RenderBlendMode::Alpha,
                                                .depthTest = false,
                                                .depthWrite = false,
                                                .depthFormat = std::nullopt};
        const auto pipeline = ProgramResources::CreatePipelineState(frontend_, shader, EditorVulkanCameraCrossFadeShaderManifest(), state);
        if (pipeline.HasError())
            return Result<void>::Failure(pipeline.ErrorValue());
        shader.pipeline = pipeline.Value();
        if (const auto layout = ProgramResources::CreateMaterialLayout(frontend_, shader, shader.metadata.reflection, device_.frames);
            layout.HasError())
            return layout;
        return CreateCameraCrossFadeTriangle(program);
    }

    /** @copydoc EditorViewportRendererVulkan::CreateCameraCrossFadeTriangle */
    Result<void> EditorViewportRendererVulkan::CreateCameraCrossFadeTriangle(CameraCrossFadeProgram &program) {
        using namespace Render;
        using enum RenderBufferUsage;
        constexpr std::array<MeshVertex, 3> vertices{MeshVertex{{-1, -1, 0}}, MeshVertex{{3, -1, 0}}, MeshVertex{{-1, 3, 0}}};
        constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
        const auto vertex =
            frontend_.CreateBuffer({sizeof(vertices), Vertex, RenderBufferAccess::HostVisible}, std::as_bytes(std::span{vertices}));
        if (vertex.HasError())
            return Result<void>::Failure(vertex.ErrorValue());
        program.vertices = vertex.Value().handle;
        const auto index =
            frontend_.CreateBuffer({sizeof(indices), Index, RenderBufferAccess::HostVisible}, std::as_bytes(std::span{indices}));
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        program.indices = index.Value().handle;
        if (auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        const auto mesh = frontend_.CreateMesh({.vertexBuffer = program.vertices,
                                                .indexBuffer = program.indices,
                                                .vertexStride = sizeof(MeshVertex),
                                                .vertexCount = 3,
                                                .indexCount = 3,
                                                .localBounds = {{-1, -1, 0}, {3, 3, 0}}});
        if (mesh.HasError())
            return Result<void>::Failure(mesh.ErrorValue());
        program.mesh = mesh.Value().handle;
        if (auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        return frontend_.ResourceOperationResult(mesh.Value().operation);
    }

    /** @copydoc EditorViewportRendererVulkan::BindCameraCrossFadeSource */
    Result<void> EditorViewportRendererVulkan::BindCameraCrossFadeSource() {
        const auto view = resources_.CameraCrossFadeColorView();
        if (!crossFadeSourceSlots_ || !view.IsValid() || crossFadeProgram_.boundSource == view)
            return Result<void>::Success();
        const auto &shader = crossFadeProgram_.shader;
        const std::array sampled{Render::MaterialResourceBinding{{2}, 0, view}};
        for (std::uint32_t slot = 0; slot < device_.frames; ++slot) {
            auto &draw = crossFadeProgram_.draws[slot];
            const auto bound = draw.buffer.IsValid()
                                   ? ProgramResources::BindUniformSlot(shader, draw, shader.pipeline, {}, sampled)
                                   : ProgramResources::CreateUniformSlot(frontend_, shader, draw, slot + 1, shader.pipeline, {}, sampled);
            if (bound.HasError())
                return bound;
        }
        crossFadeProgram_.boundSource = view;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareCameraCrossFadeScene */
    Result<void> EditorViewportRendererVulkan::PrepareCameraCrossFadeScene(const std::size_t draws) {
        if (!crossFadeSourceSlots_ || !crossFadeProgram_.boundSource.IsValid())
            return Result<void>::Success();
        crossFadeSceneReady_ = draws <= crossFadeDrawCount_;
        if (crossFadeSceneReady_) {
            declinedCrossFadeDrawCount_.reset();
            return Result<void>::Success();
        }
        if (declinedCrossFadeDrawCount_ == draws || draws > Render::RenderMaterialPass::MaximumDraws)
            return Result<void>::Success();
        const auto growth = draws - crossFadeDrawCount_;
        auto &slots = *crossFadeSourceSlots_;
        if (const auto prepared = PrepareCameraCrossFadeSlots(slots, shader_, shadowShader_, crossFadeDrawCount_, growth);
            prepared.HasError()) {
            RetireCameraCrossFadeSlots(slots, crossFadeDrawCount_, growth, &shader_, &shadowShader_);
            if (EditorViewportResources::IsSubmissionPinCapacityFailure(prepared.ErrorValue())) {
                declinedCrossFadeDrawCount_ = draws;
                return Result<void>::Success();
            }
            return prepared;
        }
        crossFadeDrawCount_ = draws;
        declinedCrossFadeDrawCount_.reset();
        crossFadeSceneReady_ = true;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::RetireCameraCrossFadeProgram */
    void EditorViewportRendererVulkan::RetireCameraCrossFadeProgram(CameraCrossFadeProgram &program) noexcept {
        for (auto &draw : program.draws)
            RetireDraw(draw);
        if (program.mesh.IsValid())
            static_cast<void>(frontend_.ReleaseMesh(program.mesh));
        for (const auto buffer : {program.vertices, program.indices})
            if (buffer.IsValid())
                static_cast<void>(frontend_.ReleaseBuffer(buffer));
        RetireProgram(program.shader, {});
        program = {};
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareCameraCrossFadeSlots */
    Result<void> EditorViewportRendererVulkan::PrepareCameraCrossFadeSlots(std::span<FrameSlot> slots, const ShaderResources &scene,
                                                                           const ShaderResources &shadow, const std::size_t first,
                                                                           const std::size_t count) {
        using Render::MaximumDirectionalShadowCascades;
        constexpr auto draws = Render::RenderMaterialPass::MaximumDraws;
        const auto atlas = resources_.ShadowTextureView();
        for (std::uint32_t frame = 0; frame < device_.frames; ++frame)
            for (std::size_t index = first; index < first + count; ++index) {
                auto &slot = slots[frame];
                const auto forward = (device_.frames + frame) * (draws + 1) + index + 1;
                if (const auto source =
                        ProgramResources::CreateUniformSlot(frontend_, scene, slot.draws[index], forward, scene.pipeline, atlas);
                    source.HasError())
                    return source;
                for (std::uint32_t cascade = 0; cascade < crossFadeShadowBudget_; ++cascade) {
                    const auto depth = ((device_.frames + frame) * MaximumDirectionalShadowCascades + cascade) * draws + index + 1;
                    if (const auto source =
                            ProgramResources::CreateUniformSlot(frontend_, shadow, slot.shadows[cascade][index], depth, shadow.pipeline);
                        source.HasError())
                        return source;
                }
            }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::RetireCameraCrossFadeSlots */
    void EditorViewportRendererVulkan::RetireCameraCrossFadeSlots(std::span<FrameSlot> slots, const std::size_t first,
                                                                  const std::size_t count, const ShaderResources *scene,
                                                                  const ShaderResources *shadow) noexcept {
        const auto release = [&](DrawSlot &draw, const ShaderResources *owner) {
            if (owner && owner->materials && draw.material)
                static_cast<void>(owner->materials->Release(draw.material->Id()));
            RetireDraw(draw);
        };
        for (auto &slot : slots) {
            for (auto &draw : std::span{slot.draws}.subspan(first, count))
                release(draw, scene);
            for (auto &cascade : slot.shadows)
                for (auto &draw : std::span{cascade}.subspan(first, count))
                    release(draw, shadow);
        }
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareXrProgram */
    Result<void> EditorViewportRendererVulkan::PrepareXrProgram(EditorVulkanMultiviewCook cooked,
                                                                const EditorVulkanMultiviewProgram program) {
        using namespace Render;
        if (owner_ != std::this_thread::get_id() || !initialized_ || recordedEver_ || xrShader_.vertex.IsValid())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        const auto encoded = EncodeReflectedShaderArtifact(cooked.metadata);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        using enum EditorVulkanMultiviewProgram;
        if (program != Projection && program != EnvironmentLighting)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        const bool environment = program == EnvironmentLighting;
        const auto admitted = environment ? LoadEditorVulkanMultiviewEnvironmentCook(cooked.payload, encoded.Value())
                                          : LoadEditorVulkanMultiviewCook(cooked.payload, encoded.Value());
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        xrShader_.payload = std::move(cooked.payload);
        xrShader_.metadata = std::move(cooked.metadata);
        if (auto stages = ProgramResources::CreateProgramStages(frontend_, xrShader_); stages.HasError())
            return stages;
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None, .depthFormat = RenderTextureFormat::Depth32Float};
        const auto manifest = environment ? EditorVulkanMultiviewEnvironmentShaderManifest() : EditorVulkanMultiviewShaderManifest();
        const auto pipeline = ProgramResources::CreatePipelineState(frontend_, xrShader_, manifest, state);
        if (pipeline.HasError())
            return Result<void>::Failure(pipeline.ErrorValue());
        xrShader_.pipeline = pipeline.Value();
        if (auto layout = ProgramResources::CreateMaterialLayout(frontend_, xrShader_, xrShader_.metadata.reflection, device_.frames);
            layout.HasError())
            return layout;
        for (std::uint32_t slot = 0; slot < device_.frames; ++slot)
            for (std::size_t index = 0; index < RenderMaterialPass::MaximumDraws; ++index)
                if (auto prepared =
                        ProgramResources::CreateUniformSlot(frontend_, xrShader_, xrDrawSlots_[slot][index],
                                                            slot * RenderMaterialPass::MaximumDraws + index + 1, xrShader_.pipeline);
                    prepared.HasError())
                    return prepared;
        xrEnvironmentLighting_ = environment;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::StageXrMotionProgram */
    void EditorViewportRendererVulkan::StageXrMotionProgram(EditorVulkanMultiviewCook cooked) noexcept {
        if (owner_ == std::this_thread::get_id() && initialized_ && !recordedEver_ && !xrMotionCook_)
            xrMotionCook_.emplace(std::move(cooked));
    }

    /** @copydoc EditorViewportRendererVulkan::StageXrCoverageProgram */
    void EditorViewportRendererVulkan::StageXrCoverageProgram(EditorVulkanMultiviewCook cooked) noexcept {
        if (owner_ == std::this_thread::get_id() && initialized_ && !recordedEver_ && !xrCoverageCook_)
            xrCoverageCook_.emplace(std::move(cooked));
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareXrCoveragePipeline */
    Result<void> EditorViewportRendererVulkan::PrepareXrCoveragePipeline() {
        using namespace Render;
        if (xrCoverageShader_.pipeline.IsValid())
            return Result<void>::Success();
        const auto metadata = EncodeReflectedShaderArtifact(xrCoverageCook_->metadata);
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        auto admitted = LoadEditorVulkanMultiviewCoverageCook(xrCoverageCook_->payload, metadata.Value());
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        auto cooked = std::move(admitted).Value();
        ShaderResources candidate{.payload = std::move(cooked.payload), .metadata = std::move(cooked.metadata)};
        const auto realize = [&]() -> Result<void> {
            if (auto stages = ProgramResources::CreateProgramStages(frontend_, candidate, "PSCoverage", "VSCoverage"); stages.HasError())
                return stages;
            const RenderGraphicsPipelineState state{.cull = RenderCullMode::None,
                                                    .colorFormat = RenderTextureFormat::R32Float,
                                                    .depthFormat = RenderTextureFormat::Depth32Float};
            const auto pipeline =
                ProgramResources::CreatePipelineState(frontend_, candidate, EditorVulkanMultiviewCoverageShaderManifest(), state);
            if (pipeline.HasError())
                return Result<void>::Failure(pipeline.ErrorValue());
            candidate.pipeline = pipeline.Value();
            return ProgramResources::CreateMaterialLayout(frontend_, candidate, candidate.metadata.reflection, device_.frames);
        };
        if (auto prepared = realize(); prepared.HasError()) {
            RetireProgram(candidate, {});
            return prepared;
        }
        xrCoverageShader_ = std::move(candidate);
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareXrMotionPipeline */
    Result<bool> EditorViewportRendererVulkan::PrepareXrMotionPipeline(const Render::RenderTextureFormat motion,
                                                                       const Render::RenderTextureFormat depth) {
        using namespace Render;
        using Outcome = Result<bool>;
        if (!xrMotionCook_)
            return Outcome::Success(false);
        if (const auto device = BorrowXrDevice(); device.HasError())
            return Outcome::Failure(device.ErrorValue());
        using enum RenderTextureFormat;
        if (xrGraph_ || (motion != Rgba16Float && motion != Rgba32Float) || (depth != Depth16Unorm && depth != Depth32Float))
            return Outcome::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        const auto formats = std::pair{motion, depth};
        if (xrMotionFormats_ == formats && xrMotionShader_.pipeline.IsValid() && xrMotionShader_.materials)
            return Outcome::Success(true);

        const auto metadata = EncodeReflectedShaderArtifact(xrMotionCook_->metadata);
        if (metadata.HasError())
            return Outcome::Failure(metadata.ErrorValue());
        const auto admitted = LoadEditorVulkanMultiviewMotionCook(xrMotionCook_->payload, metadata.Value());
        if (admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        ShaderResources candidate{.payload = std::move(admitted).Value().payload, .metadata = xrMotionCook_->metadata};
        std::array<DrawSlot, RenderMaterialPass::MaximumDraws> slots;
        const auto retireSlots = [&] {
            for (auto &draw : slots)
                RetireDraw(draw);
        };
        const auto retireFailure = [&](const Error &error) {
            retireSlots();
            RetireProgram(candidate, {});
            return Outcome::Failure(error);
        };
        if (auto stages = ProgramResources::CreateProgramStages(frontend_, candidate); stages.HasError())
            return retireFailure(stages.ErrorValue());
        const RenderGraphicsPipelineState state{.cull = RenderCullMode::None, .colorFormat = motion, .depthFormat = depth};
        const auto pipeline =
            ProgramResources::CreatePipelineState(frontend_, candidate, EditorVulkanMultiviewMotionShaderManifest(), state);
        if (pipeline.HasError())
            return retireFailure(pipeline.ErrorValue());
        candidate.pipeline = pipeline.Value();
        if (const auto layout = ProgramResources::CreateMaterialLayout(frontend_, candidate, candidate.metadata.reflection, device_.frames);
            layout.HasError())
            return retireFailure(layout.ErrorValue());
        for (std::size_t index = 0; index < slots.size(); ++index)
            if (auto allocated = ProgramResources::CreateUniformSlot(frontend_, candidate, slots[index], index + 1, candidate.pipeline);
                allocated.HasError())
                return retireFailure(allocated.ErrorValue());
        std::swap(xrMotionShader_, candidate);
        xrMotionDrawSlots_.swap(slots);
        xrMotionFormats_ = formats;
        retireSlots();
        RetireProgram(candidate, {});
        return Outcome::Success(true);
    }
}  // namespace Horo::Editor
