#include "EditorViewportBaselineDrawParameters.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "MetalViewportClusteredFrame.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"

#include <algorithm>
#include <cstring>

namespace Horo::Editor {
    Result<void> MetalViewportClusteredFrame::WriteDrawUniform(Slot &slot, const std::size_t pass, const std::size_t draw,
                                                               const Render::StaticMeshPassDescriptor &descriptor) {
        const auto &instance = descriptor.scene.instances[draw];
        const auto aspect = static_cast<float>(descriptor.extent.width) / descriptor.extent.height;
        const auto matrix = BuildRenderMvp(descriptor.scene.camera, instance.localToWorld, aspect, Math::ClipDepthRange::ZeroToOne);
        if (matrix.HasError())
            return Result<void>::Failure(matrix.ErrorValue());
        auto &uniform = slot.uniforms[pass * MaximumDraws + draw];
        if (pass != 5) {
            const auto mvp = pass == 0 ? matrix.Value() : EditorViewportShadowMvp(*shadow_, pass - 1, instance.localToWorld);
            const auto packed = PackEditorViewportShadow(programs_->Role(2).layout, mvp, uniform.bytes);
            return packed.HasError() ? Result<void>::Failure(packed.ErrorValue()) : Result<void>::Success();
        }
        if (!programs_->UsesCulling())
            return WriteBaselineLit(slot, draw, descriptor, matrix.Value());
        EditorViewportClusteredShaderParameters parameters;
        if (const auto scene = parameters.SetScene(descriptor.scene, grid_, shadow_, profile_.budget.referencesPerCluster);
            scene.HasError())
            return scene;
        parameters.common.mvp = matrix.Value();
        parameters.common.model = instance.localToWorld;
        parameters.common.unexposedAcesCg = color_ != nullptr;
        const auto &style = instance.presentation;
        parameters.common.selection = {style.tint.x, style.tint.y, style.tint.z, style.tintStrength};
        const auto packed = parameters.PackLit(programs_->Role(0).layout, uniform.bytes);
        return packed.HasError() ? Result<void>::Failure(packed.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc MetalViewportClusteredFrame::WriteBaselineLit */
    Result<void> MetalViewportClusteredFrame::WriteBaselineLit(Slot &slot, std::size_t draw,
                                                               const Render::StaticMeshPassDescriptor &descriptor,
                                                               const Math::Mat4 &matrix) {
        const auto &instance = descriptor.scene.instances[draw];
        EditorViewportShaderParameters parameters;
        const auto prepared = PrepareEditorViewportBaselineDrawParameters(forward_, descriptor.scene, instance, shadow_,
                                                                          Render::ShaderTargetBackend::Metal, parameters);
        if (prepared.HasError())
            return prepared;
        parameters.mvp = matrix;
        parameters.model = instance.localToWorld;
        parameters.unexposedAcesCg = color_ != nullptr;
        const auto &style = instance.presentation;
        parameters.selection = {style.tint.x, style.tint.y, style.tint.z, style.tintStrength};
        auto &uniform = slot.uniforms[5 * MaximumDraws + draw];
        const auto packed = parameters.PackLit(programs_->Role(0).layout, uniform.bytes);
        return packed.HasError() ? Result<void>::Failure(packed.ErrorValue()) : Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::WriteUniforms(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor) {
        std::size_t count{};
        const std::size_t cascadeCount = shadow_ ? shadow_->cascadeCount : 0;
        for (std::size_t pass = 0; pass < 6; ++pass) {
            if (pass >= 1 && pass <= 4 && pass > cascadeCount)
                continue;
            for (std::size_t draw = 0; draw < descriptor.scene.instances.size(); ++draw) {
                if (const auto written = WriteDrawUniform(slot, pass, draw, descriptor); written.HasError())
                    return written;
                const auto &uniform = slot.uniforms[pass * MaximumDraws + draw];
                const auto role = pass == 5 ? 0 : 2;
                updates_[count++] = {uniform.buffer, std::span{uniform.bytes}.first(programs_->Role(role).uniformBytes), uniform.offset};
            }
        }
        return MetalViewportResourceBridge::UpdateBuffers(*frontend_, *bridge_, std::span{updates_}.first(count));
    }

    Result<void> MetalViewportClusteredFrame::Prepare(const Render::StaticMeshPassDescriptor &descriptor, const Render::RenderViewId view) {
        using namespace Render;
        ready_ = false;
        const auto extent = resources_->AllocatedExtent();
        if (!descriptor.IsValid() || descriptor.target != resources_->Target() ||
            descriptor.extent != FramebufferExtent{extent.width, extent.height})
            return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (const auto meshes = ValidateReadyMeshes(descriptor.scene); meshes.HasError())
            return meshes;
        if (const auto canonical = PrepareCanonical(descriptor.scene, view, descriptor.extent); canonical.HasError())
            return canonical;
        const auto aspect = static_cast<float>(extent.width) / extent.height;
        const auto shadows = BuildDirectionalShadowFrame(descriptor.scene, aspect, Math::ClipDepthRange::ZeroToOne,
                                                         resources_->ActiveDirectionalShadowProfile());
        if (shadows.HasError())
            return Result<void>::Failure(shadows.ErrorValue());
        shadow_ = shadows.Value();
        auto &slot = slots_[nextSlot_];
        if (const auto prepared = PrepareSlot(slot, descriptor); prepared.HasError())
            return prepared;
        activeSlot_ = nextSlot_;
        nextSlot_ = (nextSlot_ + 1) % programs_->SlotCount();
        preparedSignature_ = FrameSignature(descriptor);
        diagnostics_.sourceLightCount = static_cast<std::uint32_t>(descriptor.scene.lights.size());
        ready_ = true;
        return Result<void>::Success();
    }

    /** @copydoc MetalViewportClusteredFrame::UploadLights */
    Result<void> MetalViewportClusteredFrame::UploadLights(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor) {
        if (!programs_->UsesCulling())
            return Result<void>::Success();
        const auto uploaded = programs_->Frames().Update(static_cast<std::uint32_t>(nextSlot_),
                                                         std::span{packedLights_}.first(descriptor.scene.lights.size()),
                                                         std::span{packedClusters_}.first(grid_.clusterCount));
        if (uploaded.HasError())
            return Result<void>::Failure(uploaded.ErrorValue());
        slot.uploaded = uploaded.Value();
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::PrepareSlot(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor) {
        if (const auto collected = CollectDiagnostics(slot); collected.HasError())
            return collected;
        if (const auto uploaded = UploadLights(slot, descriptor); uploaded.HasError())
            return uploaded;
        slot.diagnosticsPending = false;
        if (!MatchesTopology(slot, descriptor.scene))
            if (const auto built = BuildGraph(slot, descriptor); built.HasError())
                return built;
        if (const auto written = WriteUniforms(slot, descriptor); written.HasError())
            return written;
        if (const auto overlays = WriteOverlays(slot, descriptor); overlays.HasError())
            return overlays;
        return WriteSelection(slot, descriptor);
    }

    Result<void> MetalViewportClusteredFrame::CollectDiagnostics(Slot &slot) {
        using namespace Render;
        if (!slot.diagnosticsPending)
            return Result<void>::Success();
        const auto bytes =
            MetalViewportResourceBridge::CompletedBufferBytes(*frontend_, slot.uploaded.buffers.membership, slot.recordedCommands);
        if (bytes.HasError()) {
            if (bytes.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value())
                return Result<void>::Success();
            return Result<void>::Failure(bytes.ErrorValue());
        }
        if (bytes.Value().size() < std::size_t{slot.recordedClusterCount} * sizeof(PackedLightMembership))
            return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
        std::uint64_t retained{}, omitted{};
        for (std::size_t index = 0; index < slot.recordedClusterCount; ++index) {
            PackedLightMembership membership;
            std::memcpy(&membership, bytes.Value().data() + index * sizeof(membership), sizeof(membership));
            if (membership.offset != index * profile_.budget.referencesPerCluster ||
                membership.count > profile_.budget.referencesPerCluster || membership.reserved != 0)
                return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
            retained += membership.count;
            omitted += membership.omitted;
        }
        diagnostics_.shadedReferences = retained;
        diagnostics_.omittedReferences = omitted;
        diagnostics_.completedLightRevision = slot.recordedLightRevision;
        diagnostics_.gpuDiagnosticsPending = false;
        slot.diagnosticsPending = false;
        return Result<void>::Success();
    }

    /** @copydoc MetalViewportClusteredFrame::RefreshColorWorkloads */
    void MetalViewportClusteredFrame::RefreshColorWorkloads(Slot::Variant &variant) noexcept {
        if (color_) {
            variant.colorGraph.Refresh(*color_);
            const auto colorWork = variant.colorGraph.Workloads();
            std::copy(colorWork.begin(), colorWork.end(), variant.workloads.begin() + BasePassCount());
        }
    }

    /** @copydoc MetalViewportClusteredFrame::OrderWorkloads */
    Result<void> MetalViewportClusteredFrame::OrderWorkloads(Slot::Variant &variant) {
        const auto passes = variant.graph->Passes();
        if (passes.size() > variant.orderedWorkloads.size())
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Capacity));
        const auto inventory = std::span{variant.workloads}.first(BasePassCount() + (color_ ? 4 : 0));
        for (std::size_t index = 0; index < passes.size(); ++index) {
            const auto found = std::ranges::find(inventory, passes[index].pass, &Render::RenderGraphPassWorkload::pass);
            if (found == inventory.end())
                return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
            variant.orderedWorkloads[index] = *found;
        }
        return Result<void>::Success();
    }

    /** @copydoc MetalViewportClusteredFrame::RefreshSceneWorkloads */
    void MetalViewportClusteredFrame::RefreshSceneWorkloads(Slot &slot, Slot::Variant &variant,
                                                            const Render::StaticMeshPassDescriptor &descriptor,
                                                            const Render::PrimaryOutputAttachment &output) {
        using namespace Render;
        const auto cascades = shadow_ ? shadow_->cascadeCount : 0;
        if (programs_->UsesCulling()) {
            auto &culling = std::get<RenderGraphLightCulling>(variant.workloads[1].workload);
            culling.dispatch = slot.uploaded.dispatch;
            culling.tableRevision = slot.uploaded.revision;
        }
        for (std::size_t cascade = 0; cascade < cascades; ++cascade) {
            auto &raster = std::get<RenderGraphMaterialRaster>(variant.workloads[RasterIndex(cascade + 1)].workload);
            const auto &region = shadow_->cascades[cascade].viewport;
            raster.region = RenderMaterialRegion{region.x, region.y, region.width, region.height};
        }
        auto &lit = std::get<RenderGraphMaterialRaster>(variant.workloads[RasterIndex(5)].workload);
        lit.colorOperations.clearColor = descriptor.clearColor;
        variant.workloads[BasePassCount() - 1].workload = output;
    }

    /** @copydoc MetalViewportClusteredFrame::RecordEncoding */
    void MetalViewportClusteredFrame::RecordEncoding(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor,
                                                     void *commands) noexcept {
        if (!programs_->UsesCulling())
            return;

        slot.recordedCommands = commands;
        slot.recordedLightRevision = descriptor.scene.lightSnapshotRevision;
        slot.recordedClusterCount = grid_.clusterCount;
        slot.diagnosticsPending = true;
        diagnostics_.gpuDiagnosticsPending = true;
    }

    Result<void> MetalViewportClusteredFrame::Execute(Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &descriptor,
                                                      const Render::PrimaryOutputAttachment &output) {
        using namespace Render;
        auto &slot = slots_[activeSlot_];
        if (!ready_ || !MatchesTopology(slot, descriptor.scene) || preparedSignature_ != FrameSignature(descriptor))
            return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
        ready_ = false;
        const std::size_t cascades = shadow_ ? shadow_->cascadeCount : 0;
        auto &variant = slot.variants[cascades];
        RefreshSceneWorkloads(slot, variant, descriptor, output);
        void *commands = bridge_->CurrentCommandBuffer();
        RefreshColorWorkloads(variant);
        if (const auto ordered = OrderWorkloads(variant); ordered.HasError())
            return ordered;
        const auto encoded = frame.ExecuteGraph(*variant.graph, std::span{variant.orderedWorkloads}.first(variant.graph->Passes().size()));
        if (color_ && encoded.HasValue())
            color_->Encoded();
        if (encoded.HasValue())
            RecordEncoding(slot, descriptor, commands);
        return encoded;
    }
}  // namespace Horo::Editor
