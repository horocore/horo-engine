#include "ColorOutputPackage.h"
#include "ColorOutputShaderRecipe.h"
#include "EditorViewportColorInternal.h"
#include "RenderFrontendResourceAccess.h"

#include <algorithm>

namespace Horo::Editor {
    using namespace Render;
    using namespace ViewportColorDetail;

    namespace {
        /** @brief Builds real target-cache compatibility from the selected device and immutable reflection. */
        PipelineCacheCompatibility Compatibility(RenderFrontend &frontend, const ShaderTargetRequirement &target,
                                                 const ReflectedShaderArtifact &artifact, const RenderGraphicsPipelineState &state) {
            const auto device = Take(frontend.QueryPipelineDeviceIdentity());
            return {1,
                    device.backend,
                    device.backendModuleVersion,
                    device.adapter,
                    device.deviceCompatibilityDigest,
                    device.driverIdentity,
                    device.driverVersion,
                    target.backend,
                    target.payloadFormat,
                    artifact.cookedArtifactKey,
                    artifact.reflection.interfaceCompatibility,
                    Take(ComputeRenderGraphicsPipelineStateDigest(state))};
        }
    }  // namespace

    /** @brief Retires the exact original creation, including canonical already-rolled-back failure recognition. */
    template <class Handle> void EditorViewportColorPipeline::RetireOwned(Handle &handle, std::optional<Error> &first) {
        if (!handle.IsValid())
            return;
        const auto inventory = std::span{creations_}.first(creationCount_);
        const auto found = std::ranges::find(inventory, static_cast<const void *>(&handle), &CreationRecord::address);
        const auto operation = found == inventory.end() ? ResourceOperationId{} : found->operation;
        const auto result = Detail::RenderFrontendResourceAccess::RetireCreation(*frontend_, ResourceCreation<Handle>{handle, operation});
        Remember(first, result);
        if (result.HasValue())
            handle = {};
    }

    /** @copydoc EditorViewportColorPipeline::DrainCreations */
    Result<void> EditorViewportColorPipeline::DrainCreations() {
        const auto processed = frontend_->ProcessResourceRequests();
        if (processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        for (const auto &record : std::span{creations_}.first(creationCount_)) {
            const auto result = frontend_->ResourceOperationResult(record.operation);
            if (result.HasError())
                return result;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::PrepareProgram */
    Result<void> EditorViewportColorPipeline::PrepareProgram(const EditorViewportColorProgram &input, std::size_t index) {
        try {
            const auto &artifact = input.artifact;
            Require(artifact.reflection && input.manifest.targets.size() == 1);
            const auto &metadata = *artifact.reflection;
            const auto &target = input.manifest.targets.front();
            Take(ValidateReflectedShaderArtifact(artifact, metadata));
            auto &program = programs_[index];
            Take(OwnCreation(frontend_->CreateShaderModule({target, ShaderStage::Vertex, "VSMain", metadata.cookedArtifactKey,
                                                            metadata.reflection.interfaceCompatibility},
                                                           artifact.payload),
                             program.vertex));
            Take(OwnCreation(frontend_->CreateShaderModule({target, ShaderStage::Fragment, "PSMain", metadata.cookedArtifactKey,
                                                            metadata.reflection.interfaceCompatibility},
                                                           artifact.payload),
                             program.fragment));
            const RenderGraphicsPipelineState state{.cull = RenderCullMode::None,
                                                    .depthTest = false,
                                                    .depthWrite = false,
                                                    .colorFormat =
                                                        index == 0 ? RenderTextureFormat::Rgba16Float : RenderTextureFormat::Rgba8Unorm,
                                                    .depthFormat = std::nullopt};
            Take(OwnCreation(frontend_->CreateGraphicsPipeline({program.vertex, program.fragment, input.manifest, target,
                                                                metadata.reflection, state,
                                                                Compatibility(*frontend_, target, metadata, state)}),
                             program.pipeline));
            Take(DrainCreations());
            program.layout = Take(PrepareMaterialBindingLayout(metadata.reflection));
            program.uniformBytes = Take(
                Detail::RenderFrontendResourceAccess::MaterialParameterBufferBytes(*frontend_,
                                                                                   {.pipeline = program.pipeline, .layout = program.layout},
                                                                                   {1}));
            Require(program.uniformBytes > 0 && program.uniformBytes <= 4096);
            program.materials = Take(MaterialBindingTable::Create(program.pipeline.owner, *frontend_, {.maximumGenerations = 64}));
            return Result<void>::Success();
        } catch (const Failure &failure) {
            return Result<void>::Failure(failure.error);
        }
    }

    /** @copydoc EditorViewportColorPipeline::PreparePrograms */
    Result<void> EditorViewportColorPipeline::PreparePrograms(const EditorViewportColorInputs &inputs) {
        for (std::size_t i = 0; i < programs_.size(); ++i) {
            const bool look = i == 0 && selection_->Mode() == ColorLookMode::Enabled;
            const auto stage = look ? ColorOutputShaderStage::SceneToDisplayLinearWithLook : static_cast<ColorOutputShaderStage>(i);
            const auto manifest = MakeColorOutputShaderManifest(stage, Internal::ColorOutputMetalCookTarget());
            if (manifest.HasError())
                return Result<void>::Failure(manifest.ErrorValue());
            const EditorViewportColorProgram input{manifest.Value(), inputs.package->programs[look ? 2 : i]};
            if (const auto prepared = PrepareProgram(input, i); prepared.HasError())
                return prepared;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::PrepareTables */
    Result<void> EditorViewportColorPipeline::PrepareTables(const CookedColorOutputProgram &input) {
        try {
            for (std::size_t i = 0; i < tables_.size(); ++i) {
                const auto *d = input.Table(i);
                Require(d != nullptr);
                Take(OwnCreation(frontend_->CreateTexture({.extent = d->extent,
                                                           .format = d->format,
                                                           .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination},
                                                          input.TableUpload(i)),
                                 tables_[i]));
                Take(DrainCreations());
                Take(OwnCreation(frontend_->CreateTextureView({.texture = tables_[i], .format = d->format}), tableViews_[i]));
                Take(OwnCreation(frontend_->CreateSampler(d->sampler), samplers_[i]));
            }
            Take(DrainCreations());
            return Result<void>::Success();
        } catch (const Failure &failure) {
            return Result<void>::Failure(failure.error);
        }
    }

    /** @copydoc EditorViewportColorPipeline::PrepareLook */
    Result<void> EditorViewportColorPipeline::PrepareLook() {
        if (selection_->Mode() != ColorLookMode::Enabled)
            return Result<void>::Success();
        try {
            const auto &look = *selection_->Look();
            Take(OwnCreation(frontend_->CreateTexture(look.AtlasDescriptor(), look.AtlasUpload()), lookTexture_));
            Take(DrainCreations());
            Take(OwnCreation(frontend_->CreateTextureView({.texture = lookTexture_, .format = RenderTextureFormat::Rgba32Float}),
                             lookView_));
            Take(DrainCreations());
            return Result<void>::Success();
        } catch (const Failure &failure) {
            return Result<void>::Failure(failure.error);
        }
    }

    /** @copydoc EditorViewportColorPipeline::PrepareGeometry */
    Result<void> EditorViewportColorPipeline::PrepareGeometry() {
        using enum Horo::Render::RenderBufferUsage;
        try {
            const std::array<MeshVertex, 3> vertices{};
            const std::array<std::uint32_t, 3> indices{0, 1, 2};
            Take(OwnCreation(frontend_->CreateBuffer({sizeof(vertices), Vertex | CopyDestination, RenderBufferAccess::DeviceLocal},
                                                     std::as_bytes(std::span{vertices})),
                             geometry_[0]));
            Take(OwnCreation(frontend_->CreateBuffer({sizeof(indices), Index | CopyDestination, RenderBufferAccess::DeviceLocal},
                                                     std::as_bytes(std::span{indices})),
                             geometry_[1]));
            Take(DrainCreations());
            Take(OwnCreation(frontend_->CreateMesh({.vertexBuffer = geometry_[0],
                                                    .indexBuffer = geometry_[1],
                                                    .vertexStride = sizeof(MeshVertex),
                                                    .vertexCount = 3,
                                                    .indexCount = 3,
                                                    .localBounds = {{-1, -1, 0}, {1, 1, 0}}}),
                             mesh_));
            for (auto &slot : slots_)
                for (std::size_t role = 0; role < 2; ++role)
                    Take(OwnCreation(frontend_->CreateBuffer({programs_[role].uniformBytes, Uniform, RenderBufferAccess::HostVisible}, {}),
                                     slot.uniform[role]));
            Take(DrainCreations());
            return Result<void>::Success();
        } catch (const Failure &failure) {
            return Result<void>::Failure(failure.error);
        }
    }

    /** @copydoc EditorViewportColorPipeline::PrepareSelection */
    Result<void> EditorViewportColorPipeline::PrepareSelection(const EditorViewportColorInputs &inputs) {
        if (!inputs.package || !inputs.package->analytic.program)
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        const auto selected = PrepareColorTransformSelection(inputs.package->analytic.program, inputs.lookMode, inputs.look, inputs.grading,
                                                             inputs.maximumTransformBytes);
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        if (inputs.lookMode == ColorLookMode::Enabled && !inputs.package->LookProgram())
            return Result<void>::Failure(MakeError(ColorPipelineErrors::InvalidTransform));
        selection_ = selected.Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::ValidateInitialization */
    Result<void> EditorViewportColorPipeline::ValidateInitialization() {
        if (status_ != EditorViewportColorStatus::Unavailable || kernel_ || !port_.bufferReadiness || !port_.writeUniform)
            return Result<void>::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        const auto frames = frontend_->Capabilities().support.limits.maxFramesInFlight;
        if (frames < 2 || frames > SlotCount)
            return Result<void>::Failure(MakeError(ExposureGpuErrors::Unsupported));
        activeSlots_ = frames;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::Initialize */
    Result<void> EditorViewportColorPipeline::Initialize(const EditorViewportColorInputs &inputs) {
        if (const auto valid = ValidateInitialization(); valid.HasError())
            return valid;
        if (const auto selected = PrepareSelection(inputs); selected.HasError())
            return selected;
        status_ = EditorViewportColorStatus::Preparing;
        const auto kernel = frontend_->RealizeExposureGpuKernel(inputs.exposure);
        if (kernel.HasError()) {
            status_ = EditorViewportColorStatus::Unavailable;
            return Result<void>::Failure(kernel.ErrorValue());
        }
        kernel_ = kernel.Value();
        try {
            Take(PreparePrograms(inputs));
            Take(PrepareTables(*inputs.package->analytic.program));
            Take(PrepareStorage());
            Take(PrepareGeometry());
            Take(PrepareLook());
            status_ = EditorViewportColorStatus::Ready;
            return Result<void>::Success();
        } catch (const Failure &failure) {
            const auto rollback = Close();
            status_ = EditorViewportColorStatus::Failed;
            return Result<void>::Failure(rollback.HasError() ? WithCause(failure.error, rollback.ErrorValue()) : failure.error);
        }
    }

    /** @copydoc EditorViewportColorPipeline::ClosePrograms */
    Result<void> EditorViewportColorPipeline::ClosePrograms() {
        std::optional<Error> first;
        for (std::size_t role = 0; role < programs_.size(); ++role) {
            auto &program = programs_[role];
            const auto retained = std::ranges::any_of(slots_, [role](const auto &slot) {
                return std::ranges::any_of(slot.bindings[role], [](const auto &binding) {
                    return binding != nullptr;
                });
            });
            if (retained)
                continue;
            if (program.materials) {
                const auto closed = program.materials->Shutdown();
                Remember(first, closed);
                if (closed.HasError())
                    continue;
                program.materials.reset();
            }
            RetireOwned(program.pipeline, first);
            if (program.pipeline.IsValid())
                continue;
            for (auto *shaderModule : {&program.vertex, &program.fragment})
                RetireOwned(*shaderModule, first);
        }
        return Outcome(first);
    }

    /** @copydoc EditorViewportColorPipeline::CloseStorage */
    Result<void> EditorViewportColorPipeline::CloseStorage() {
        std::optional<Error> first;
        bool bindingsRetained = false;
        for (auto &slot : slots_)
            for (std::size_t role = 0; role < 2; ++role)
                for (auto &binding : slot.bindings[role]) {
                    if (!binding)
                        continue;
                    const auto released = programs_[role].materials->Release(binding->Id());
                    Remember(first, released);
                    if (released.HasValue())
                        binding.reset();
                    else
                        bindingsRetained = true;
                }
        for (auto &slot : slots_) {
            RetireOwned(slot.histogram, first);
            if (bindingsRetained)
                continue;
            for (auto *buffer : {&slot.seed, &slot.output, &slot.uniform[0], &slot.uniform[1]})
                RetireOwned(*buffer, first);
        }
        return Outcome(first);
    }

    /** @copydoc EditorViewportColorPipeline::CloseGeometry */
    Result<void> EditorViewportColorPipeline::CloseGeometry() {
        std::optional<Error> first;
        RetireOwned(mesh_, first);
        if (!mesh_.IsValid())
            for (auto &buffer : geometry_)
                RetireOwned(buffer, first);
        return Outcome(first);
    }

    /** @copydoc EditorViewportColorPipeline::CloseTables */
    Result<void> EditorViewportColorPipeline::CloseTables() {
        std::optional<Error> first;
        if (std::ranges::any_of(programs_, [](const auto &program) {
            return program.materials != nullptr;
        }))
            return Result<void>::Success();
        for (std::size_t i = 0; i < 2; ++i) {
            RetireOwned(tableViews_[i], first);
            if (!tableViews_[i].IsValid())
                RetireOwned(tables_[i], first);
            RetireOwned(samplers_[i], first);
        }
        RetireOwned(lookView_, first);
        if (!lookView_.IsValid())
            RetireOwned(lookTexture_, first);
        return Outcome(first);
    }

    /** @copydoc EditorViewportColorPipeline::Close */
    Result<void> EditorViewportColorPipeline::Close() {
        candidate_ = false;
        encoded_ = false;
        std::optional<Error> first;
        Remember(first, CloseStorage());
        Remember(first, ClosePrograms());
        Remember(first, CloseGeometry());
        Remember(first, CloseTables());
        if (first)
            return Outcome(first);
        kernel_.reset();
        selection_.reset();
        previous_ = {};
        creationCount_ = 0;
        acceptedStateCount_ = 0;
        pendingStateCount_ = 0;
        tablesUsed_ = false;
        boundScene_ = {};
        boundDisplay_ = {};
        status_ = EditorViewportColorStatus::Closed;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
