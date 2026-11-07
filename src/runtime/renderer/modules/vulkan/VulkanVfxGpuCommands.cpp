#include "VulkanVfxGpuInternal.h"

#include <algorithm>
#include <cstring>

namespace Horo::Render::Detail {
    std::uint64_t VulkanVfxGpuInstance(RenderGraphResourceId id, const RenderGraphExecutionRequest &request) noexcept {
        const auto found = std::ranges::find(request.resources, id, &RenderGraphResourceInstance::resource);
        return found == request.resources.end() ? 0 : found->instance;
    }

    namespace {
        bool Matches(const VulkanVfxGpuBuffer &buffer, RenderGraphResourceId id, const RenderGraphExecutionRequest &request) noexcept {
            const auto found = std::ranges::find(request.graph.Resources(), id, &RenderGraphResource::id);
            if (found == request.graph.Resources().end())
                return false;
            const auto *handle = std::get_if<RenderBufferHandle>(&found->binding);
            return handle && *handle == buffer.capture.issued.handle && buffer.capture.issued.instance == VulkanVfxGpuInstance(id, request);
        }

        RenderTimelinePoint Signal(const RenderGraphVfxGpuCompute &work, const RenderGraphExecutionRequest &request) noexcept {
            std::size_t pass{};
            for (const auto &record : request.workloads) {
                if (std::get_if<RenderGraphVfxGpuCompute>(&record.workload) == &work)
                    break;
                ++pass;
            }
            for (const auto &record : request.submissions)
                if (pass >= record.passes.offset && pass - record.passes.offset < record.passes.count)
                    return record.submission.signal;
            return {};
        }

        bool Exact(const VulkanVfxGpuSimulation &simulation, const RenderGraphVfxGpuCompute &work) noexcept {
            return !simulation.stopped && work.work.step.simulation == simulation.id &&
                   work.work.step.deviceIncarnation == simulation.owner->incarnation &&
                   work.work.step.sourceGeneration == simulation.generation && work.work.step.tick == simulation.tick &&
                   SameVfxGpuDescriptor(simulation.descriptor, work.descriptor);
        }

        bool ValidComputeMetadata(const VulkanVfxGpuSimulation &simulation, const RenderGraphVfxGpuCompute &work,
                                  const RenderGraphExecutionRequest &request) {
            return work.kernel.get() == simulation.kernel.get() && Exact(simulation, work) &&
                   ValidateVfxGpuSimulationWork(simulation.descriptor, work.work) && Signal(work, request).IsValid();
        }

        bool Capacity(const VulkanVfxGpuSimulation &simulation) noexcept {
            return simulation.count + simulation.reserved <= simulation.receipts.size() - 2 &&
                   std::ranges::any_of(simulation.journals, [](const auto &journal) {
                return !journal.occupied;
            }) && std::ranges::none_of(simulation.journals, [](const auto &journal) {
                return journal.occupied && !journal.accepted;
            });
        }

        std::size_t JournalSlot(const VulkanVfxGpuSimulation &simulation) noexcept {
            for (std::size_t i = 0; i < simulation.journals.size(); ++i)
                if (!simulation.journals[i].occupied)
                    return i;
            return simulation.journals.size();
        }

        std::size_t OperationSlot(VulkanVfxGpuSimulation &simulation, VulkanCommandTicket ticket, RenderTimelinePoint point) noexcept {
            if (simulation.active.size() == simulation.active.capacity())
                return simulation.operations.size();
            for (std::size_t probe = 0; probe < simulation.operations.size(); ++probe) {
                const auto i = simulation.operationCursor++ % simulation.operations.size();
                if (simulation.operations[i].occupied)
                    continue;
                simulation.operations[i] = {.ticket = ticket, .point = point, .occupied = true};
                simulation.active.push_back(i);
                return i;
            }
            return simulation.operations.size();
        }

        void WriteParameters(VulkanVfxGpuSimulation &simulation, std::size_t operation, std::span<const std::uint32_t> words) noexcept {
            auto *destination = static_cast<std::byte *>(simulation.arena.mapped) + operation * simulation.plan.parameterStrideBytes;
            std::memcpy(destination, words.data(), words.size_bytes());
        }

        void UpdateCompute(VulkanVfxGpuSimulation &simulation, std::size_t journal) noexcept {
            const auto &record = simulation.journals[journal];
            const auto observationBytes = simulation.descriptor.readback.enabled
                                              ? std::size_t{simulation.descriptor.readback.maximumRecords} * VfxGpuObservationWords::Bytes
                                              : 4;
            const std::array<VkDescriptorBufferInfo, 5> buffers{
                {{simulation.states[record.source].capture.resource.buffer, 0, simulation.descriptor.layout.byteCount},
                 {simulation.states[record.destination].capture.resource.buffer, 0, simulation.descriptor.layout.byteCount},
                 {simulation.forces.capture.resource.buffer, 0,
                  std::max<std::size_t>(4, std::size_t{simulation.descriptor.forceCount} * GpuParticleForceWords::Bytes)},
                 {simulation.observations[record.observation].capture.resource.buffer, 0, observationBytes},
                 {simulation.arena.resource.buffer, 0, VfxGpuDispatchWords::Bytes}}};
            std::array<VkWriteDescriptorSet, 5> writes{};
            for (std::size_t i = 0; i < 5; ++i)
                writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                             .dstSet = simulation.computeSets[journal],
                             .dstBinding = simulation.kernel->computeBindings[i],
                             .descriptorCount = 1,
                             .descriptorType = i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                             .pBufferInfo = &buffers[i]};
            simulation.owner->native.updateSets(simulation.owner->device, 5, writes.data(), 0, nullptr);
        }

        void HostObservationBarrier(const VulkanVfxGpuSimulation &simulation, VkCommandBuffer commands, std::size_t observation) noexcept {
            const VkBufferMemoryBarrier2 buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
                                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                                .buffer = simulation.observations[observation].capture.resource.buffer,
                                                .offset = 0,
                                                .size = VK_WHOLE_SIZE};
            const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                              .bufferMemoryBarrierCount = 1,
                                              .pBufferMemoryBarriers = &buffer};
            simulation.owner->native.barrier(commands, &dependency);
        }

        bool Duplicate(const VulkanVfxGpuSimulation &simulation, const VfxGpuStepIdentity &step) noexcept {
            for (const auto &journal : simulation.journals)
                if (journal.occupied && journal.step == step)
                    return true;
            for (std::size_t i = 0; i < simulation.count; ++i)
                if (simulation.receipts[(simulation.head + i) % simulation.receipts.size()].step == step)
                    return true;
            return false;
        }

        void CompleteSubmission(VulkanVfxGpuSimulation &simulation, VulkanVfxGpuJournal &journal, bool accepted) noexcept {
            if (!simulation.Queue({journal.step, accepted ? VfxGpuSubmissionState::Accepted : VfxGpuSubmissionState::CancelledUnsubmitted,
                                   accepted ? journal.point : RenderTimelinePoint{}}))
                std::terminate();
            --simulation.reserved;
            if (!accepted) {
                if (journal.observe)
                    simulation.observations[journal.observation].held = false;
                --simulation.reserved;
                journal = {};
                return;
            }
            journal.accepted = true;
            simulation.source = journal.destination;
            simulation.generation = journal.step.targetGeneration;
            simulation.tick = journal.step.tick + 1;
            simulation.states[journal.destination].generation = simulation.generation;
        }

        void FinishJournals(VulkanVfxGpuSimulation &simulation, VulkanCommandTicket ticket, bool accepted) noexcept {
            for (auto &journal : simulation.journals)
                if (journal.occupied && !journal.accepted && !journal.uncertain && journal.ticket == ticket)
                    CompleteSubmission(simulation, journal, accepted);
        }
    }  // namespace

    std::size_t ReserveVulkanVfxGpuOperation(VulkanVfxGpuSimulation &simulation, VulkanCommandTicket ticket,
                                             RenderTimelinePoint point) noexcept {
        return OperationSlot(simulation, ticket, point);
    }

    void WriteVulkanVfxGpuParameters(VulkanVfxGpuSimulation &simulation, std::size_t operation,
                                     std::span<const std::uint32_t> words) noexcept {
        WriteParameters(simulation, operation, words);
    }

    std::size_t VulkanVfxGpuState(const VulkanVfxGpuSimulation &simulation, RenderGraphResourceId id,
                                  const RenderGraphExecutionRequest &request) noexcept {
        for (std::size_t i = 0; i < simulation.states.size(); ++i)
            if (Matches(simulation.states[i], id, request))
                return i;
        return simulation.states.size();
    }

    std::size_t VulkanVfxGpuObservation(const VulkanVfxGpuSimulation &simulation, RenderGraphResourceId id,
                                        const RenderGraphExecutionRequest &request) noexcept {
        for (std::size_t i = 0; i < simulation.observations.size(); ++i)
            if (Matches(simulation.observations[i], id, request))
                return i;
        return simulation.observations.size();
    }

    Result<void> VulkanVfxGpuRuntime::Impl::ValidateCompute(const RenderGraphVfxGpuCompute &work,
                                                            const RenderGraphExecutionRequest &request) const {
        auto found = Simulation(work.simulationLease);
        if (found.HasError())
            return Result<void>::Failure(found.ErrorValue());
        const auto &simulation = *found.Value();
        if (!ValidComputeMetadata(simulation, work, request))
            return Result<void>::Failure(VulkanVfxGpuInvalid("Particle compute work is not the exact next issued native step."));
        const auto source = VulkanVfxGpuState(simulation, work.source, request),
                   destination = VulkanVfxGpuState(simulation, work.destination, request);
        const auto observation = VulkanVfxGpuObservation(simulation, work.observations, request);
        const std::array valid{source == simulation.source,
                               destination > 0,
                               destination < simulation.states.size(),
                               source != destination,
                               observation < simulation.observations.size(),
                               Matches(simulation.forces, work.forces, request)};
        if (!std::ranges::all_of(valid, [](bool value) {
            return value;
        }))
            return Result<void>::Failure(VulkanVfxGpuInvalid("Particle compute buffers differ from its exact native inventory."));
        if (!Capacity(simulation))
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        const auto idle = Complete(simulation.states[destination].capture.resource.state->completion);
        if (idle.HasError())
            return Result<void>::Failure(idle.ErrorValue());
        if (!idle.Value() || (work.work.observe && simulation.observations[observation].held))
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        return Result<void>::Success();
    }

    Result<void> VulkanVfxGpuRuntime::Impl::EncodeCompute(VkCommandBuffer command, VulkanCommandTicket ticket,
                                                          const RenderQueueSubmission &submission, const RenderGraphVfxGpuCompute &work,
                                                          const RenderGraphExecutionRequest &request) {
        if (const auto valid = ValidateCompute(work, request); valid.HasError())
            return valid;
        auto simulation = std::static_pointer_cast<VulkanVfxGpuSimulation>(work.simulationLease);
        std::array<std::uint32_t, VfxGpuDispatchWords::Count> parameters{};
        if (!PackVfxGpuDispatchParameters(simulation->descriptor, work.work, parameters))
            return Result<void>::Failure(VulkanVfxGpuInvalid("Particle dispatch parameter packing failed."));
        const auto journal = JournalSlot(*simulation);
        if (journal == simulation->journals.size())
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        const auto operation = OperationSlot(*simulation, ticket, submission.signal);
        if (operation == simulation->operations.size())
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        auto &record = simulation->journals[journal];
        record = {.step = work.work.step,
                  .point = submission.signal,
                  .ticket = ticket,
                  .source = VulkanVfxGpuState(*simulation, work.source, request),
                  .destination = VulkanVfxGpuState(*simulation, work.destination, request),
                  .observation = VulkanVfxGpuObservation(*simulation, work.observations, request),
                  .operation = operation,
                  .occupied = true,
                  .observe = work.work.observe};
        simulation->operations[operation].journal = journal;
        simulation->reserved += 2;
        if (record.observe) {
            auto &output = simulation->observations[record.observation];
            output.held = true;
            output.completed = output.failed = false;
            output.observationStep = record.step;
        }
        WriteParameters(*simulation, operation, parameters);
        UpdateCompute(*simulation, journal);
        const auto offset = static_cast<std::uint32_t>(operation * simulation->plan.parameterStrideBytes);
        device->native.bindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, simulation->kernel->compute);
        device->native.bindSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, simulation->kernel->computeLayout, 0, 1,
                                &simulation->computeSets[journal], 1, &offset);
        device->native.dispatch(command,
                                static_cast<std::uint32_t>((std::uint64_t{simulation->descriptor.layout.streamStrideWords} + 63) / 64), 1,
                                1);
        if (record.observe)
            HostObservationBarrier(*simulation, command, record.observation);
        return Result<void>::Success();
    }

    Result<void> VulkanVfxGpuRuntime::Validate(const RenderGraphExecutionRequest &request, std::size_t pass,
                                               std::span<const VulkanGraphResource> bindings) {
        if (const auto *compute = std::get_if<RenderGraphVfxGpuCompute>(&request.workloads[pass].workload))
            return impl_->ValidateCompute(*compute, request);
        if (const auto *draw = std::get_if<RenderGraphVfxGpuDraw>(&request.workloads[pass].workload))
            return impl_->ValidateDraw(*draw, request, bindings);
        return Result<void>::Success();
    }

    Result<void> VulkanVfxGpuRuntime::Encode(VkCommandBuffer command, VulkanCommandTicket ticket, const RenderQueueSubmission &submission,
                                             const RenderGraphExecutionRequest &request, std::size_t pass,
                                             std::span<const VulkanGraphResource> bindings) {
        if (const auto *compute = std::get_if<RenderGraphVfxGpuCompute>(&request.workloads[pass].workload))
            return impl_->EncodeCompute(command, ticket, submission, *compute, request);
        if (const auto *draw = std::get_if<RenderGraphVfxGpuDraw>(&request.workloads[pass].workload))
            return impl_->EncodeDraw(command, ticket, submission, *draw, request, bindings);
        return Result<void>::Failure(VulkanVfxGpuInvalid("Not a typed native particle pass."));
    }

    void VulkanVfxGpuRuntime::Impl::Finish(VulkanVfxGpuSimulation &simulation, VulkanCommandTicket ticket, bool accepted) noexcept {
        FinishJournals(simulation, ticket, accepted);
        std::size_t index{};
        while (index < simulation.active.size()) {
            auto &op = simulation.operations[simulation.active[index]];
            if (op.ticket != ticket || op.accepted || op.uncertain) {
                ++index;
                continue;
            }
            if (accepted) {
                op.accepted = true;
                ++index;
                continue;
            }
            op = {};
            simulation.active[index] = simulation.active.back();
            simulation.active.pop_back();
        }
    }

    void VulkanVfxGpuRuntime::Finish(VulkanCommandTicket ticket, bool accepted) noexcept {
        for (const auto &simulation : impl_->simulations)
            impl_->Finish(*simulation, ticket, accepted);
    }

    void VulkanVfxGpuRuntime::Uncertain(VulkanCommandTicket ticket) noexcept {
        for (const auto &simulation : impl_->simulations) {
            for (auto &journal : simulation->journals) {
                if (!journal.occupied || journal.accepted || journal.ticket != ticket)
                    continue;
                if (!simulation->Queue({journal.step, VfxGpuSubmissionState::SubmissionUncertain, journal.point}))
                    std::terminate();
                --simulation->reserved;
                journal.uncertain = true;
                simulation->stopped = true;
            }
            for (const auto i : simulation->active)
                if (simulation->operations[i].ticket == ticket)
                    simulation->operations[i].uncertain = true;
        }
    }

    void VulkanVfxGpuRuntime::Abort() noexcept {
        for (const auto &simulation : impl_->simulations) {
            std::size_t index{};
            while (index < simulation->active.size()) {
                const auto op = simulation->operations[simulation->active[index]];
                if (op.accepted || op.uncertain) {
                    ++index;
                    continue;
                }
                impl_->Finish(*simulation, op.ticket, false);
            }
        }
    }

    void VulkanVfxGpuRuntime::Impl::Reject(VulkanVfxGpuSimulation &simulation, const RenderGraphVfxGpuCompute &work) noexcept {
        auto candidate = work.work;
        candidate.callerCancellation = {};
        candidate.lifetimeCancellation = {};
        if (!Exact(simulation, work) || !ValidateVfxGpuSimulationWork(simulation.descriptor, candidate) ||
            Duplicate(simulation, candidate.step) || simulation.count + simulation.reserved == simulation.receipts.size())
            return;
        const bool cancelled =
            work.work.callerCancellation.IsCancellationRequested() || work.work.lifetimeCancellation.IsCancellationRequested();
        if (!simulation.Queue(
                {work.work.step, cancelled ? VfxGpuSubmissionState::CancelledUnsubmitted : VfxGpuSubmissionState::RejectedUnsubmitted, {}}))
            std::terminate();
    }

    void VulkanVfxGpuRuntime::Reject(const RenderGraphExecutionRequest &request) noexcept {
        for (const auto &entry : request.workloads) {
            const auto *work = std::get_if<RenderGraphVfxGpuCompute>(&entry.workload);
            if (!work)
                continue;
            const auto simulation = std::dynamic_pointer_cast<VulkanVfxGpuSimulation>(work->simulationLease);
            if (simulation && simulation->owner == impl_->device && Signal(*work, request).IsValid())
                impl_->Reject(*simulation, *work);
        }
    }

    bool ContainsVulkanVfxGpuBuffer(const VulkanVfxGpuSimulation &simulation, VkBuffer buffer, bool arena) noexcept {
        if (!buffer)
            return false;
        if (arena && simulation.arena.resource.buffer == buffer)
            return true;
        if (simulation.forces.capture.resource.buffer == buffer)
            return true;
        for (const auto &state : simulation.states)
            if (state.capture.resource.buffer == buffer)
                return true;
        for (const auto &observation : simulation.observations)
            if (observation.capture.resource.buffer == buffer)
                return true;
        return false;
    }

    bool VulkanVfxGpuRuntime::Owns(VkBuffer buffer) const noexcept {
        return std::ranges::any_of(impl_->simulations, [buffer](const auto &simulation) {
            return ContainsVulkanVfxGpuBuffer(*simulation, buffer);
        });
    }
}  // namespace Horo::Render::Detail
