#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "MetalResourceRuntimeInternal.h"
#include "MetalVfxGpuGraphInternal.h"

namespace Horo::Render::Detail {
    namespace {
        Result<void> Invalid(const char *message) {
            return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid, message));
        }

        bool ObservationAvailable(const MetalVfxGpuBuffer &output, const bool observe) noexcept {
            return !observe || (!output.observationHeld && MetalVfxGpuQuiescent(*output.native));
        }

        /** @brief Exact native ring authority is checked independently of portable graph metadata. */
        Result<void> InventoryMatches(const MetalVfxGpuSimulation &simulation, const RenderGraphVfxGpuCompute &work,
                                      const RenderGraphExecutionRequest &request) {
            const auto source = MetalVfxStateIndex(simulation, work.source, request);
            const auto destination = MetalVfxStateIndex(simulation, work.destination, request);
            const auto observation = MetalVfxObservationIndex(simulation, work.observations, request);
            if (source != simulation.currentSource || destination == 0 || destination >= simulation.states.size() ||
                source == destination || observation >= simulation.observations.size() ||
                !MetalVfxBufferMatches(simulation.forces, work.forces, request))
                return Invalid("GPU VFX graph buffers differ from the exact prepared simulation inventory.");
            if (!MetalVfxGpuQuiescent(*simulation.states[destination].native))
                return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
            if (!ObservationAvailable(simulation.observations[observation], work.work.observe))
                return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
            return Result<void>::Success();
        }

        bool ExactStep(const MetalVfxGpuSimulation &simulation, const RenderGraphVfxGpuCompute &work) noexcept {
            return !simulation.stopped && work.work.step.simulation == simulation.simulationId &&
                   work.work.step.sourceGeneration == simulation.currentGeneration && work.work.step.tick == simulation.nextTick &&
                   SameVfxGpuDescriptor(simulation.descriptor, work.descriptor);
        }

        Result<void> StepMatches(const MetalVfxGpuSimulation &simulation, const RenderGraphVfxGpuCompute &work,
                                 const RenderGraphExecutionRequest &request) {
            if (!ExactStep(simulation, work) || !ValidateVfxGpuSimulationWork(simulation.descriptor, work.work) ||
                !MetalVfxStepSignal(work, request).IsValid())
                return Invalid("GPU VFX step is stale, cancelled, unissued or belongs to another simulation generation.");
            if (simulation.receiptCount + simulation.reservedReceipts > simulation.receipts.size() - 2)
                return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
            for (const auto &journal : simulation.journals)
                if (journal.occupied && !journal.accepted)
                    return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
            return Result<void>::Success();
        }

        /** @brief Reserves all receipt capacity before an encoder or GPU operation can observe the candidate. */
        MetalVfxGpuJournal *Reserve(MetalVfxGpuSimulation &simulation, const RenderGraphVfxGpuCompute &work,
                                    const RenderGraphExecutionRequest &request, id<MTLCommandBuffer> commands) {
            for (auto &journal : simulation.journals) {
                if (journal.occupied)
                    continue;
                journal = {.step = work.work.step,
                           .commands = commands,
                           .submission = MetalVfxStepSignal(work, request),
                           .source = MetalVfxStateIndex(simulation, work.source, request),
                           .destination = MetalVfxStateIndex(simulation, work.destination, request),
                           .observation = MetalVfxObservationIndex(simulation, work.observations, request),
                           .occupied = true,
                           .observe = work.work.observe};
                if (journal.observe) {
                    auto &output = simulation.observations[journal.observation];
                    output.observationHeld = true;
                    output.observationCompleted = output.observationFailed = false;
                    output.observationStep = journal.step;
                }
                simulation.reservedReceipts += 2;
                return &journal;
            }
            return nullptr;
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::ValidateVfxGpuCompute */
    Result<void> MetalResourceRuntime::ValidateVfxGpuCompute(const RenderGraphVfxGpuCompute &work,
                                                             const RenderGraphExecutionRequest &request) const {
        const auto simulation = std::dynamic_pointer_cast<MetalVfxGpuSimulation>(work.simulationLease);
        if (!simulation || simulation->owner != impl_->vfxGpuOwner || simulation->incarnation != impl_->lightKernelIncarnation ||
            work.work.step.deviceIncarnation != impl_->deviceIncarnation || work.kernel.get() != simulation->kernel.get())
            return Invalid("GPU VFX simulation or complete-family lease belongs to another native owner.");
        if (const auto valid = StepMatches(*simulation, work, request); valid.HasError())
            return valid;
        return InventoryMatches(*simulation, work, request);
    }

    /** @copydoc MetalResourceRuntime::ExecuteVfxGpuCompute */
    Result<void> MetalResourceRuntime::ExecuteVfxGpuCompute(void *nativeCommands, const RenderGraphVfxGpuCompute &work,
                                                            const RenderGraphExecutionRequest &request) {
        if (const auto valid = ValidateVfxGpuCompute(work, request); valid.HasError())
            return valid;
        id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)nativeCommands;
        if (!commands)
            return Invalid("GPU VFX dispatch has no actual owner command buffer.");
        const auto simulation = std::static_pointer_cast<MetalVfxGpuSimulation>(work.simulationLease);
        std::array<std::uint32_t, VfxGpuDispatchWords::Count> parameters{};
        if (!PackVfxGpuDispatchParameters(simulation->descriptor, work.work, parameters))
            return Invalid("GPU VFX canonical dispatch packing rejected the exact candidate.");
        const auto journal = Reserve(*simulation, work, request, commands);
        if (!journal)
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        TrackParallelGraphUse(nativeCommands, request.resources);
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        if (!encoder)
            return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
        [encoder setComputePipelineState:simulation->kernel->compute];
        const std::array<MetalBufferInstance *, 4> buffers{simulation->states[journal->source].native,
                                                           simulation->states[journal->destination].native, simulation->forces.native,
                                                           simulation->observations[journal->observation].native};
        for (std::size_t index = 0; index < buffers.size(); ++index)
            [encoder setBuffer:buffers[index]->buffer offset:0 atIndex:simulation->kernel->computeBindings[index]];
        [encoder setBytes:parameters.data() length:VfxGpuDispatchWords::Bytes atIndex:simulation->kernel->computeBindings[4]];
        [encoder dispatchThreads:MTLSizeMake(simulation->descriptor.layout.streamStrideWords, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
