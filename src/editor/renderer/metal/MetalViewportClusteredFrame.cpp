#include "MetalViewportClusteredFrame.h"

#include "EditorViewportMeshReadiness.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>
#include <bit>

namespace Horo::Editor {
    namespace {
        void MixFrameWord(std::uint64_t &signature, const std::uint64_t word) noexcept {
            signature ^= word;
            signature *= 1099511628211ULL;
        }

        std::uint64_t MatrixSignature(const Math::Mat4 &matrix) noexcept {
            std::uint64_t signature = 14695981039346656037ULL;
            for (const auto value : matrix.values)
                MixFrameWord(signature, std::bit_cast<std::uint32_t>(value));
            return signature == 0 ? 1 : signature;
        }

        std::size_t UniformRole(const std::size_t pass) noexcept {
            return pass == 5 ? 0 : (pass == 6 ? 1 : 2);
        }

        std::size_t UniformOffset(const std::size_t pass, const std::size_t draw) noexcept {
            const auto block = pass < 5 ? pass : 0;
            return (block * MetalViewportClusteredFrame::MaximumDraws + draw) * MetalViewportClusteredFrame::MaximumUniformBytes;
        }
    }  // namespace

    MetalViewportClusteredFrame::MetalViewportClusteredFrame(Render::RenderFrontend &frontend, MetalViewportClusteredLighting &programs,
                                                             EditorViewportResources &resources, Render::MetalEditorGraphicsBridge &bridge,
                                                             const EditorViewportLightingProfile &profile)
        : frontend_(&frontend), programs_(&programs), resources_(&resources), bridge_(&bridge), profile_(profile),
          slots_(std::make_unique<Slot[]>(programs.SlotCount())) {
        diagnostics_.selection = {Render::LightPreparationMode::ClusteredForwardNative, 0};
    }

    MetalViewportClusteredFrame::~MetalViewportClusteredFrame() {
        for (std::size_t index = 0; index < programs_->SlotCount(); ++index)
            CloseSlot(slots_[index]);
    }

    void MetalViewportClusteredFrame::CloseSlot(Slot &slot) noexcept {
        for (auto &variant : slot.variants)
            variant.graph.reset();
        for (std::size_t index = 0; index < slot.uniforms.size(); ++index) {
            auto &draw = slot.uniforms[index];
            if (draw.material)
                static_cast<void>(programs_->Role(UniformRole(index / MaximumDraws)).materials->Release(draw.material->Id()));
            draw.material.reset();
        }
        for (auto buffer : slot.uniformBuffers)
            if (buffer.IsValid())
                static_cast<void>(frontend_->ReleaseBuffer(buffer));
        for (auto &overlay : slot.overlays) {
            if (overlay.mesh.IsValid())
                static_cast<void>(frontend_->ReleaseMesh(overlay.mesh));
            for (const auto buffer : {overlay.descriptor.vertexBuffer, overlay.descriptor.indexBuffer})
                if (buffer.IsValid())
                    static_cast<void>(frontend_->ReleaseBuffer(buffer));
        }
    }

    Result<void> MetalViewportClusteredFrame::ValidateCanonical(const Render::RenderSceneView &scene) const {
        if (!scene.IsValid() || scene.instances.size() > MaximumDraws || scene.lights.size() > profile_.budget.maximumLights)
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Capacity));
        if (profile_.budget.requireCompleteCoverage && scene.lights.size() > profile_.budget.referencesPerCluster)
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Coverage));
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::ValidateReadyMeshes(const Render::RenderSceneView &scene) const {
        return ValidateEditorViewportMeshReadiness(*resources_, scene.instances);
    }

    Result<void> MetalViewportClusteredFrame::PrepareCanonical(const Render::RenderSceneView &scene, const Render::RenderViewId view,
                                                               const Render::FramebufferExtent extent) {
        using namespace Render;
        if (const auto valid = ValidateCanonical(scene); valid.HasError())
            return valid;
        for (std::size_t index = 0; index < scene.lights.size(); ++index)
            lights_[index] = {{scene.lights[index].stableIdentity}, scene.lights[index]};
        std::ranges::sort(std::span{lights_}.first(scene.lights.size()), {}, &IdentifiedRenderLight::identity);
        const auto aspect = static_cast<float>(extent.width) / extent.height;
        const auto matrix = BuildRenderMvp(scene.camera, Math::Mat4::Identity(), aspect, Math::ClipDepthRange::ZeroToOne);
        if (matrix.HasError())
            return Result<void>::Failure(matrix.ErrorValue());
        const auto projection = MatrixSignature(matrix.Value());
        std::uint64_t gridSignature = projection;
        for (const auto value : {extent.width, extent.height, profile_.gridX, profile_.gridY, profile_.gridZ})
            MixFrameWord(gridSignature, value);
        const ClusteredLightGridDescriptor descriptor{{view, scene.lightSceneDomain, scene.lightSnapshotRevision, projection,
                                                       gridSignature == 0 ? 1 : gridSignature},
                                                      scene.camera,
                                                      extent,
                                                      profile_.gridX,
                                                      profile_.gridY,
                                                      profile_.gridZ};
        const auto built = BuildClusteredLightGrid(descriptor, profile_.budget, clusters_);
        if (built.HasError())
            return Result<void>::Failure(built.ErrorValue());
        grid_ = built.Value();
        return PackLightFrame(std::span{lights_}.first(scene.lights.size()), std::span{clusters_}.first(grid_.clusterCount), packedLights_,
                              packedClusters_);
    }

    Result<void> MetalViewportClusteredFrame::PrepareUniforms(Slot &slot) {
        using namespace Render;
        constexpr std::array<std::size_t, 3> counts{MaximumDraws, 3, MaximumDraws * 5};
        for (std::size_t role = 0; role < slot.uniformBuffers.size(); ++role) {
            if (slot.uniformBuffers[role].IsValid())
                continue;
            const auto created = frontend_->CreateBuffer({.byteSize = counts[role] * MaximumUniformBytes,
                                                          .usage = RenderBufferUsage::Uniform,
                                                          .access = RenderBufferAccess::HostVisible},
                                                         {});
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            slot.uniformBuffers[role] = created.Value().handle;
        }
        if (const auto processed = frontend_->ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        for (std::size_t pass = 0; pass < 7; ++pass)
            for (std::size_t draw = 0; draw < MaximumDraws; ++draw) {
                auto &uniform = slot.uniforms[pass * MaximumDraws + draw];
                uniform.buffer = slot.uniformBuffers[UniformRole(pass)];
                uniform.offset = UniformOffset(pass, draw);
            }
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::PrepareMaterial(Slot &slot, const std::size_t pass, const std::size_t draw) {
        using namespace Render;
        const auto role = UniformRole(pass);
        const auto &program = programs_->Role(role);
        auto &uniform = slot.uniforms[pass * MaximumDraws + draw];
        std::vector<MaterialResourceBinding> resources{
            {{1}, 0, MaterialBufferBinding{uniform.buffer, uniform.offset, program.uniformBytes}}};
        if (role == 0) {
            resources.push_back({{2}, 0, resources_->ShadowTextureView()});
            resources.push_back({{3}, 0, programs_->Sampler()});
            resources.push_back({{4},
                                 0,
                                 MaterialBufferBinding{slot.uploaded.buffers.lights, 0,
                                                       std::size_t{profile_.budget.maximumLights} * sizeof(PackedRenderLight)}});
            resources.push_back({{5},
                                 0,
                                 MaterialBufferBinding{slot.uploaded.buffers.membership, 0,
                                                       std::size_t{profile_.budget.maximumClusters} * sizeof(PackedLightMembership)}});
            resources.push_back({{6},
                                 0,
                                 MaterialBufferBinding{slot.uploaded.buffers.references, 0,
                                                       std::size_t{profile_.budget.maximumClusters} * profile_.budget.referencesPerCluster *
                                                           sizeof(std::uint32_t)}});
        }
        const auto identity = static_cast<std::uint64_t>(&slot - slots_.get()) * MaximumUniformDraws + pass * MaximumDraws + draw + 1;
        const auto published = program.materials->Publish({.material = {identity},
                                                           .revision = ++slot.materialRevision,
                                                           .pipeline = program.pipeline,
                                                           .layout = program.layout,
                                                           .resources = std::move(resources)});
        if (published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        const auto acquired = program.materials->Acquire(published.Value().binding);
        if (acquired.HasError())
            return Result<void>::Failure(acquired.ErrorValue());
        if (uniform.material)
            static_cast<void>(program.materials->Release(uniform.material->Id()));
        uniform.material = acquired.Value();
        return Result<void>::Success();
    }

    bool MetalViewportClusteredFrame::MatchesTopology(const Slot &slot, const Render::RenderSceneView &scene) const noexcept {
        if (!slot.variants.front().graph || slot.color != resources_->ColorTexture() || slot.depth != resources_->DepthTexture() ||
            slot.atlas != resources_->ShadowDepthTexture() || slot.meshCount != scene.instances.size())
            return false;
        for (std::size_t draw = 0; draw < slot.meshCount; ++draw) {
            const auto mesh = resources_->FindMesh(scene.instances[draw].mesh.id.value);
            if (!mesh || slot.meshes[draw] != mesh->mesh ||
                !IsEditorViewportMeshGenerationReady(*mesh, scene.instances[draw].mesh.generation))
                return false;
        }
        return true;
    }

    std::uint64_t MetalViewportClusteredFrame::FrameSignature(const Render::StaticMeshPassDescriptor &descriptor) const noexcept {
        auto signature = grid_.clusterSignature;
        MixFrameWord(signature, descriptor.scene.lightSceneDomain);
        MixFrameWord(signature, ComputeEditorViewportLightRevision(descriptor.scene.lights));
        for (const auto &instance : descriptor.scene.instances) {
            MixFrameWord(signature, instance.mesh.id.value);
            MixFrameWord(signature, instance.mesh.generation);
            MixFrameWord(signature, MatrixSignature(instance.localToWorld));
            for (const auto value : {instance.presentation.tint.x, instance.presentation.tint.y, instance.presentation.tint.z,
                                     instance.presentation.tintStrength})
                MixFrameWord(signature, std::bit_cast<std::uint32_t>(value));
        }
        const auto extent = resources_->AllocatedExtent();
        const auto matrix = BuildRenderMvp(descriptor.scene.camera, Math::Mat4::Identity(),
                                           static_cast<float>(extent.width) / extent.height, Math::ClipDepthRange::ZeroToOne);
        if (matrix.HasError())
            return 0;
        MixFrameWord(signature, MatrixSignature(matrix.Value()));
        return signature == 0 ? 1 : signature;
    }

    EditorViewportLightingDiagnostics MetalViewportClusteredFrame::Diagnostics() const noexcept {
        return diagnostics_;
    }

    bool MetalViewportClusteredFrame::MatchesProfile(const EditorViewportLightingProfile &profile) const noexcept {
        const std::array same{profile.requested == profile_.requested,
                              profile.fallback == profile_.fallback,
                              profile.budget.maximumLights == profile_.budget.maximumLights,
                              profile.budget.maximumClusters == profile_.budget.maximumClusters,
                              profile.budget.referencesPerCluster == profile_.budget.referencesPerCluster,
                              profile.budget.requireCompleteCoverage == profile_.budget.requireCompleteCoverage,
                              profile.gridX == profile_.gridX,
                              profile.gridY == profile_.gridY,
                              profile.gridZ == profile_.gridZ,
                              profile.maximumForwardLightTests == profile_.maximumForwardLightTests};
        return std::ranges::all_of(same, [](bool value) {
            return value;
        });
    }
}  // namespace Horo::Editor
