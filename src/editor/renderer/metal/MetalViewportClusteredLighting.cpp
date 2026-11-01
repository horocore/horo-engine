#include "MetalViewportClusteredLighting.h"

#include "EditorViewportShaderRecipe.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "RenderFrontendResourceAccess.h"
#include "editor/renderer/EditorRendererErrors.h"

namespace Horo::Editor {
    namespace {
        using namespace Render;

        bool ClusteredProgramEnvelope(const ShaderHotReloadProgramView &view, const std::size_t role) {
            return view.manifest && view.compiled && view.id.value == role + 1 && view.manifest->targets.size() == 1 &&
                   view.compiled->artifacts.size() == 1;
        }

        bool ClusteredProfileAdmitted(const EditorViewportLightingProfile &profile) noexcept {
            return profile.IsValid() && profile.requested == LightPreparationMode::ClusteredForwardNative &&
                   (profile.fallback == LightPreparationFallback::Forbidden ||
                    profile.fallback == LightPreparationFallback::AllowBaselineForwardCpu);
        }

        Result<const CompiledShaderArtifact *> ClusteredArtifact(const ShaderHotReloadProgramView &view, const std::size_t role) {
            using Outcome = Result<const CompiledShaderArtifact *>;
            if (!ClusteredProgramEnvelope(view, role))
                return Outcome::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidRequest));
            const auto &artifact = view.compiled->artifacts.front();
            if (artifact.backend != ShaderTargetBackend::Metal || !artifact.reflection)
                return Outcome::Failure(MakeError(ShaderCompilerPipelineErrors::InvalidAdapterOutput));
            const auto canonical = MakeEditorViewportClusteredShaderManifest(static_cast<EditorViewportShaderProgram>(role + 1),
                                                                             view.manifest->targets.front());
            if (canonical.HasError())
                return Outcome::Failure(canonical.ErrorValue());
            const auto expected = ComputeShaderInterfaceCompatibilityId(canonical.Value());
            if (expected.HasError())
                return Outcome::Failure(expected.ErrorValue());
            if (expected.Value() != artifact.reflection->reflection.interfaceCompatibility)
                return Outcome::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
            if (const auto valid = ValidateReflectedShaderArtifact(artifact, *artifact.reflection); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            return Outcome::Success(&artifact);
        }

        Result<PipelineCacheCompatibility> ClusteredCompatibility(RenderFrontend &frontend, const ShaderTargetRequirement &target,
                                                                  const ReflectedShaderArtifact &artifact,
                                                                  const RenderGraphicsPipelineState &state) {
            const auto device = frontend.QueryPipelineDeviceIdentity();
            if (device.HasError())
                return Result<PipelineCacheCompatibility>::Failure(device.ErrorValue());
            const auto digest = ComputeRenderGraphicsPipelineStateDigest(state);
            if (digest.HasError())
                return Result<PipelineCacheCompatibility>::Failure(digest.ErrorValue());
            const auto &identity = device.Value();
            return Result<PipelineCacheCompatibility>::Success(
                {1, identity.backend, identity.backendModuleVersion, identity.adapter, identity.deviceCompatibilityDigest,
                 identity.driverIdentity, identity.driverVersion, target.backend, target.payloadFormat, artifact.cookedArtifactKey,
                 artifact.reflection.interfaceCompatibility, digest.Value()});
        }

        Result<void> CloseClusteredProgram(RenderFrontend &frontend, MetalViewportClusteredLighting::Program &program) {
            program.materials.reset();
            if (program.pipeline.IsValid()) {
                if (auto released = frontend.ReleaseGraphicsPipeline(program.pipeline); released.HasError())
                    return released;
                program.pipeline = {};
            }
            for (auto *stage : {&program.vertex, &program.fragment}) {
                if (!stage->IsValid())
                    continue;
                if (auto released = frontend.ReleaseShaderModule(*stage); released.HasError())
                    return released;
                *stage = {};
            }
            return Result<void>::Success();
        }
    }  // namespace

    MetalViewportClusteredLighting::MetalViewportClusteredLighting(Render::RenderFrontend &frontend) noexcept : frontend_(&frontend) {}

    MetalViewportClusteredLighting::~MetalViewportClusteredLighting() {
        static_cast<void>(Close());
    }

    Result<void> MetalViewportClusteredLighting::Prepare(const std::span<const Render::ShaderHotReloadProgramView> family,
                                                         const Render::CookedLightCullingKernel &kernel,
                                                         const EditorViewportLightingProfile &profile) {
        if (family.size() != programs_.size() || !ClusteredProfileAdmitted(profile) || frames_)
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        slots_ = frontend_->Capabilities().support.limits.maxFramesInFlight;
        for (std::size_t role = 0; role < programs_.size(); ++role)
            if (const auto ready = PrepareProgram(family[role], role); ready.HasError())
                return ready;
        const auto kernelResult = frontend_->RealizeLightCullingKernel(kernel);
        if (kernelResult.HasError())
            return Result<void>::Failure(kernelResult.ErrorValue());
        kernel_ = kernelResult.Value();
        return PrepareStorage(profile);
    }

    Result<void> MetalViewportClusteredLighting::PrepareReplacement(const std::span<const Render::ShaderHotReloadProgramView> family,
                                                                    const MetalViewportClusteredLighting &previous,
                                                                    const EditorViewportLightingProfile &profile) {
        if (frontend_ != previous.frontend_ || !previous.kernel_ || family.size() != programs_.size() ||
            !ClusteredProfileAdmitted(profile) || frames_)
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        slots_ = previous.slots_;
        for (std::size_t role = 0; role < programs_.size(); ++role)
            if (const auto ready = PrepareProgram(family[role], role); ready.HasError())
                return ready;
        kernel_ = previous.kernel_;
        return PrepareStorage(profile);
    }

    Result<void> MetalViewportClusteredLighting::PrepareStorage(const EditorViewportLightingProfile &profile) {
        auto frames = Render::LightFrameBufferPool::Create(*frontend_, profile.budget, slots_);
        if (frames.HasError())
            return Result<void>::Failure(frames.ErrorValue());
        frames_ = std::move(frames).Value();
        const auto sampler = frontend_->CreateSampler({});
        if (sampler.HasError())
            return Result<void>::Failure(sampler.ErrorValue());
        sampler_ = sampler.Value().handle;
        if (const auto processed = frontend_->ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredLighting::PrepareProgram(const Render::ShaderHotReloadProgramView &view, const std::size_t role) {
        const auto admitted = ClusteredArtifact(view, role);
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        const auto &artifact = *admitted.Value();
        const auto &reflection = *artifact.reflection;
        auto &program = programs_[role];
        const auto stage = [&](const Render::ShaderStage kind, const char *entry) {
            return frontend_->CreateShaderModule({view.manifest->targets.front(), kind, entry, reflection.cookedArtifactKey,
                                                  reflection.reflection.interfaceCompatibility},
                                                 artifact.payload);
        };
        const auto vertex = stage(Render::ShaderStage::Vertex, "VSMain");
        if (vertex.HasError())
            return Result<void>::Failure(vertex.ErrorValue());
        program.vertex = vertex.Value().handle;
        const auto fragment = stage(Render::ShaderStage::Fragment, "PSMain");
        if (fragment.HasError())
            return Result<void>::Failure(fragment.ErrorValue());
        program.fragment = fragment.Value().handle;
        if (const auto pipeline = PreparePipeline(program, view, artifact, role); pipeline.HasError())
            return pipeline;
        return PrepareMaterialOwner(program, reflection.reflection);
    }

    Result<void> MetalViewportClusteredLighting::PreparePipeline(Program &program, const Render::ShaderHotReloadProgramView &view,
                                                                 const Render::CompiledShaderArtifact &artifact, const std::size_t role) {
        Render::RenderGraphicsPipelineState state{.cull = Render::RenderCullMode::None,
                                                  .depthWrite = role != 1,
                                                  .depthFormat = Render::RenderTextureFormat::Depth32Float,
                                                  .depthOnly = role == 2};
        const auto &target = view.manifest->targets.front();
        const auto &metadata = *artifact.reflection;
        const auto compatibility = ClusteredCompatibility(*frontend_, target, metadata, state);
        if (compatibility.HasError())
            return Result<void>::Failure(compatibility.ErrorValue());
        const auto pipeline = frontend_->CreateGraphicsPipeline(
            {program.vertex, program.fragment, *view.manifest, target, metadata.reflection, state, compatibility.Value()});
        if (pipeline.HasError())
            return Result<void>::Failure(pipeline.ErrorValue());
        program.pipeline = pipeline.Value().handle;
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredLighting::PrepareMaterialOwner(Program &program,
                                                                      const Render::NormalizedShaderReflection &reflection) {
        const auto layout = Render::PrepareMaterialBindingLayout(reflection);
        if (layout.HasError())
            return Result<void>::Failure(layout.ErrorValue());
        program.layout = layout.Value();
        const auto bytes = Render::Detail::RenderFrontendResourceAccess::MaterialParameterBufferBytes(*frontend_,
                                                                                                      {.pipeline = program.pipeline,
                                                                                                       .layout = program.layout},
                                                                                                      {1});
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        if (bytes.Value() == 0 || bytes.Value() > 4096)
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShaderContractInvalid));
        program.uniformBytes = bytes.Value();
        auto materials = Render::MaterialBindingTable::Create(program.pipeline.owner, *frontend_,
                                                              {.maximumGenerations =
                                                                   slots_ * Render::RenderGraphMaterialRaster::MaximumRetainedDraws * 2});
        if (materials.HasError())
            return Result<void>::Failure(materials.ErrorValue());
        program.materials = std::move(materials).Value();
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredLighting::Close() {
        for (auto &program : programs_) {
            if (auto closed = CloseClusteredProgram(*frontend_, program); closed.HasError())
                return closed;
        }
        kernel_.reset();
        if (frames_) {
            if (auto closed = frames_->Shutdown(); closed.HasError())
                return closed;
            frames_.reset();
        }
        if (!sampler_.IsValid())
            return Result<void>::Success();
        if (auto released = frontend_->ReleaseSampler(sampler_); released.HasError())
            return released;
        sampler_ = {};
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
