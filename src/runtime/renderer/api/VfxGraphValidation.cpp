#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"

#include <algorithm>
#include <array>

namespace Horo::Render {
    namespace {
        /** @brief Records one resource role; aliases with another role fail before native validation. */
        class VfxUses final {
        public:
            explicit VfxUses(const RenderGraphExecutionRequest &request) : request_(request) {}

            bool Add(const RenderGraphResourceId id, const RenderGraphUsageKind kind, const RenderGraphAccess access) {
                if (id.owner != request_.graph.Owner() || id.value == 0 || id.value > request_.graph.Resources().size())
                    return false;
                const auto texture = kind == RenderGraphUsageKind::ColorAttachment ||
                                     kind == RenderGraphUsageKind::DepthStencilAttachment || kind == RenderGraphUsageKind::Sampled;
                if (request_.graph.Resources()[id.value - 1].kind !=
                    (texture ? RenderGraphResourceKind::Texture : RenderGraphResourceKind::Buffer))
                    return false;
                const auto role = static_cast<std::uint8_t>(1 + static_cast<unsigned>(kind) * 3 + static_cast<unsigned>(access));
                auto &existing = roles_[id.value - 1];
                if (existing != 0 && existing != role)
                    return false;
                if (existing == 0)
                    ++count_;
                existing = role;
                return true;
            }

            bool Matches(const std::span<const RenderGraphResourceUsage> uses) const {
                if (uses.size() != count_)
                    return false;
                std::array<bool, RenderGraphLimits::HardMaxResources> seen{};
                for (const auto &use : uses) {
                    if (use.kind > RenderGraphUsageKind::UniformBuffer || use.access > RenderGraphAccess::ReadWrite ||
                        use.resource.owner != request_.graph.Owner() || use.resource.value == 0 ||
                        use.resource.value > request_.graph.Resources().size() || seen[use.resource.value - 1])
                        return false;
                    seen[use.resource.value - 1] = true;
                    const auto role =
                        static_cast<std::uint8_t>(1 + static_cast<unsigned>(use.kind) * 3 + static_cast<unsigned>(use.access));
                    if (roles_[use.resource.value - 1] != role)
                        return false;
                }
                return true;
            }

        private:
            const RenderGraphExecutionRequest &request_;
            std::array<std::uint8_t, RenderGraphLimits::HardMaxResources> roles_{};
            std::size_t count_{};
        };

        /** @brief Matches the dedicated canonical instance buffer to the imported graph identity. */
        bool MatchesParameters(const RenderGraphExecutionRequest &request, const VfxPreparedDraw &draw) {
            if (!draw.slot || draw.parameters.owner != request.graph.Owner() || draw.parameters.value == 0 ||
                draw.parameters.value > request.graph.Resources().size())
                return false;
            const auto *buffer = std::get_if<RenderBufferHandle>(&request.graph.Resources()[draw.parameters.value - 1].binding);
            const auto &resources = draw.slot->Material()->Descriptor().resources;
            const auto found = std::ranges::find(resources, VfxShaderParameterIds::Instances, &MaterialResourceBinding::binding);
            const auto *binding = found == resources.end() ? nullptr : std::get_if<MaterialBufferBinding>(&found->value);
            return buffer && binding && *buffer == binding->buffer && binding->offset == 0;
        }

        /** @brief Records one admitted draw's exact mesh and resource roles; short-circuit order preserves alias rejection. */
        bool AddDrawResourceRoles(const RenderGraphExecutionRequest &request, const VfxPreparedDraw &draw, VfxUses &uses) {
            const auto mesh = std::ranges::find(request.meshes, draw.mesh, &RenderGraphMeshInstance::mesh);
            return !(
                mesh == request.meshes.end() || mesh->instance == 0 ||
                std::ranges::count(request.meshes, draw.mesh, &RenderGraphMeshInstance::mesh) != 1 ||
                !uses.Add(draw.vertices, RenderGraphUsageKind::VertexBuffer, RenderGraphAccess::Read) ||
                !uses.Add(draw.indices, RenderGraphUsageKind::IndexBuffer, RenderGraphAccess::Read) ||
                !uses.Add(draw.parameters, RenderGraphUsageKind::UniformBuffer, RenderGraphAccess::Read) ||
                (draw.sampledTexture.IsValid() && !uses.Add(draw.sampledTexture, RenderGraphUsageKind::Sampled, RenderGraphAccess::Read)));
        }

        bool MatchesStage(const VfxGraphStage stage, const VfxParticlePass blend) noexcept {
            switch (stage) {
                case VfxGraphStage::OpaqueForward:
                    return blend == VfxParticlePass::Opaque || blend == VfxParticlePass::Masked;
                case VfxGraphStage::TranslucentForward:
                    return blend == VfxParticlePass::Translucent;
                case VfxGraphStage::AdditiveForward:
                    return blend == VfxParticlePass::Additive;
                default:
                    return false;
            }
        }
    }  // namespace

    /** @copydoc ValidateVfxGraphWorkload */
    Result<void> ValidateVfxGraphWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                          const RenderGraphVfx &workload) {
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        };
        if (workload.stage != VfxGraphStage::OpaqueForward && workload.stage != VfxGraphStage::TranslucentForward &&
            workload.stage != VfxGraphStage::AdditiveForward)
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
        if (request.graph.Resources().size() > RenderGraphLimits::HardMaxResources || pass.kind != RenderPassKind::Graphics ||
            !pass.usages.IsValidFor(request.graph.Usages().size()) || !workload.order || !workload.order->nativePrepared ||
            workload.order->preparingDraws || workload.order->draws.empty() || !workload.target.IsValid() ||
            workload.order->draws.size() > request.graph.Resources().size() || !workload.snapshot.Scene() || !workload.snapshot.Frame() ||
            !workload.snapshot.OriginRevision())
            return invalid();
        const auto target = std::ranges::find(request.targets, workload.target, &RenderGraphTargetInstance::target);
        if (target == request.targets.end() || target->instance == 0 ||
            std::ranges::count(request.targets, workload.target, &RenderGraphTargetInstance::target) != 1)
            return invalid();
        VfxUses uses{request};
        const auto depthAccess = workload.stage == VfxGraphStage::OpaqueForward ? RenderGraphAccess::ReadWrite : RenderGraphAccess::Read;
        if (!uses.Add(workload.color, RenderGraphUsageKind::ColorAttachment, RenderGraphAccess::ReadWrite) ||
            !uses.Add(workload.depth, RenderGraphUsageKind::DepthStencilAttachment, depthAccess))
            return invalid();
        std::array<bool, RenderGraphLimits::HardMaxResources> parameters{};
        std::size_t next{};
        for (const auto &draw : workload.order->draws) {
            if (!draw.slot || draw.firstOrder != next || draw.slot->ValidateDraw(workload, draw).HasError() ||
                !MatchesParameters(request, draw))
                return invalid();
            const auto &reference = workload.order->instances[draw.firstOrder];
            if (reference.batch >= workload.snapshot.Batches().size() ||
                !MatchesStage(workload.stage, workload.snapshot.Batches()[reference.batch].blend))
                return invalid();
            if (!AddDrawResourceRoles(request, draw, uses))
                return invalid();
            if (parameters[draw.parameters.value - 1])
                return invalid();
            parameters[draw.parameters.value - 1] = true;
            next += draw.instanceCount;
        }
        return next == workload.order->instances.size() &&
                       uses.Matches(request.graph.Usages().subspan(pass.usages.offset, pass.usages.count))
                   ? Result<void>::Success()
                   : invalid();
    }
}  // namespace Horo::Render
