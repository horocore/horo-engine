#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <algorithm>
#include <array>

namespace Horo::Render {
    namespace {
        using ParticleResources = std::array<RenderGraphResourceId, 4>;

        /** @brief Resolves a complete nonzero buffer identity without trusting caller indexing. */
        bool ValidBuffer(const RenderGraphExecutionRequest &request, const RenderGraphResourceId id) noexcept {
            if (!id.IsValid() || id.owner != request.graph.Owner() || id.value > request.graph.Resources().size() ||
                id.value > request.resources.size())
                return false;
            const auto &resolved = request.resources[id.value - 1];
            return request.graph.Resources()[id.value - 1].kind == RenderGraphResourceKind::Buffer && resolved.resource == id &&
                   resolved.instance != 0;
        }

        /** @brief Rejects both logical aliases and distinct imported IDs naming the same physical buffer. */
        bool DistinctBuffers(const RenderGraphExecutionRequest &request, const ParticleResources &ids) noexcept {
            for (std::size_t index = 0; index < ids.size(); ++index) {
                if (!ValidBuffer(request, ids[index]))
                    return false;
                const auto instance = request.resources[ids[index].value - 1].instance;
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (request.resources[ids[previous].value - 1].instance == instance)
                        return false;
            }
            return true;
        }

        /** @brief Matches exactly one whole-buffer storage use to each immutable input or candidate output. */
        bool ExactStorageUses(const std::span<const RenderGraphResourceUsage> uses, const ParticleResources &ids) noexcept {
            if (uses.size() != ids.size())
                return false;
            for (std::size_t index = 0; index < ids.size(); ++index) {
                const auto found = std::ranges::find(uses, ids[index], &RenderGraphResourceUsage::resource);
                const auto access = index == 0 || index == 2 ? RenderGraphAccess::Read : RenderGraphAccess::Write;
                if (found == uses.end() || found->kind != RenderGraphUsageKind::Storage || found->access != access)
                    return false;
            }
            return true;
        }

        /** @brief Rejects invalid generations without mistaking representation checks for registry issuance. */
        bool ValidDrawGeneration(const RenderGraphVfxGpuDraw &draw) noexcept {
            return draw.simulation.IsValid() && draw.deviceIncarnation != 0 && draw.generation != 0 &&
                   draw.generation <= std::uint64_t{draw.descriptor.arithmetic.maximumSteps} + 1;
        }

        /** @brief Checks one complete resolved attachment identity before indexing its metadata. */
        bool ValidTexture(const RenderGraphExecutionRequest &request, const RenderGraphResourceId id) noexcept {
            if (!id.IsValid() || id.owner != request.graph.Owner() || id.value > request.graph.Resources().size() ||
                id.value > request.resources.size())
                return false;
            return request.graph.Resources()[id.value - 1].kind == RenderGraphResourceKind::Texture &&
                   request.resources[id.value - 1].resource == id && request.resources[id.value - 1].instance != 0;
        }

        /** @brief Allows an optional depth attachment while excluding physical color/depth aliasing. */
        bool ValidDrawResources(const RenderGraphExecutionRequest &request, const RenderGraphVfxGpuDraw &draw) noexcept {
            if (!ValidBuffer(request, draw.particles) || !ValidTexture(request, draw.color))
                return false;
            if (!draw.depth.IsValid())
                return draw.depth == RenderGraphResourceId{};
            return ValidTexture(request, draw.depth) &&
                   request.resources[draw.depth.value - 1].instance != request.resources[draw.color.value - 1].instance;
        }

        /** @brief Matches one exact attachment or storage use, without accepting undeclared additional reads. */
        bool HasUse(const std::span<const RenderGraphResourceUsage> uses, const RenderGraphResourceId id, const RenderGraphUsageKind kind,
                    const RenderGraphAccess access) noexcept {
            const auto found = std::ranges::find(uses, id, &RenderGraphResourceUsage::resource);
            return found != uses.end() && found->kind == kind && found->access == access;
        }

        /** @brief Preserves existing color contents when requested and never permits depth mutation. */
        bool ExactDrawUses(const std::span<const RenderGraphResourceUsage> uses, const RenderGraphVfxGpuDraw &draw) noexcept {
            const auto colorAccess = draw.colorOperations.loadOperation == AttachmentLoadOperation::Load ? RenderGraphAccess::ReadWrite
                                                                                                         : RenderGraphAccess::Write;
            if (uses.size() != (draw.depth.IsValid() ? 3U : 2U) ||
                !HasUse(uses, draw.particles, RenderGraphUsageKind::Storage, RenderGraphAccess::Read) ||
                !HasUse(uses, draw.color, RenderGraphUsageKind::ColorAttachment, colorAccess))
                return false;
            return !draw.depth.IsValid() || HasUse(uses, draw.depth, RenderGraphUsageKind::DepthStencilAttachment, RenderGraphAccess::Read);
        }

        /** @brief Only whole graph color attachments with preserved results are part of this closed draw operation. */
        bool ValidColorOperations(const PrimaryOutputAttachment &operations) noexcept {
            return operations.IsValid() && !operations.outputColor && operations.loadOperation != AttachmentLoadOperation::DontCare &&
                   operations.storeOperation == AttachmentStoreOperation::Store;
        }

        /** @brief Validates retained logical draw inputs independently of graph attachment declarations. */
        bool ValidDrawInputs(const RenderGraphVfxGpuDraw &draw) noexcept {
            std::array<std::uint32_t, VfxGpuDrawWords::Count> parameters{};
            return draw.kernel && draw.simulationLease && !draw.lifetimeCancellation.IsCancellationRequested() &&
                   ValidDrawGeneration(draw) && PackVfxGpuDrawParameters(draw.descriptor, draw.view, parameters);
        }
    }  // namespace

    /** @copydoc ValidateVfxGpuComputeGraphWorkload */
    Result<void> ValidateVfxGpuComputeGraphWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                    const RenderGraphVfxGpuCompute &workload) {
        const ParticleResources ids{workload.source, workload.destination, workload.forces, workload.observations};
        if (pass.kind != RenderPassKind::Compute || !pass.usages.IsValidFor(request.graph.Usages().size()) || !workload.kernel ||
            !workload.simulationLease || !ValidateVfxGpuSimulationWork(workload.descriptor, workload.work) ||
            !DistinctBuffers(request, ids))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (!ExactStorageUses(request.graph.Usages().subspan(pass.usages.offset, pass.usages.count), ids))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        return Result<void>::Success();
    }

    /** @copydoc ValidateVfxGpuDrawGraphWorkload */
    Result<void> ValidateVfxGpuDrawGraphWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                 const RenderGraphVfxGpuDraw &workload) {
        if (pass.kind != RenderPassKind::Graphics || !pass.usages.IsValidFor(request.graph.Usages().size()) || !ValidDrawInputs(workload) ||
            !ValidColorOperations(workload.colorOperations) || !ValidDrawResources(request, workload))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        if (!ExactDrawUses(request.graph.Usages().subspan(pass.usages.offset, pass.usages.count), workload))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        return Result<void>::Success();
    }
}  // namespace Horo::Render
